#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <nmmintrin.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
constexpr uint32_t kHours = 72, kMaxEndpoints = 1842, kHourBytes = 27;
constexpr uint32_t kMaxGroups = kHours * kMaxEndpoints;
constexpr size_t kEndpointTableSize = 4096;

struct EndpointSlot {
    uint64_t hash = 0;
    uint32_t str_offset = 0;
    uint16_t len = 0;
    uint16_t id = 0;
};
static_assert(sizeof(EndpointSlot) == 16);

struct EndpointCatalogue {
    std::vector<EndpointSlot> table = std::vector<EndpointSlot>(kEndpointTableSize);
    std::vector<char> arena;
    size_t count = 0;
};

struct Record {
    std::string_view endpoint;
    uint64_t ep_hash = 0;
    uint32_t latency = 0;
    int hour = -1;
    bool is_5xx = false;
};

__attribute__((target("sse4.2"))) inline uint64_t compute_crc32c(const char* s, size_t len) noexcept {
    uint64_t crc = 0xFFFFFFFF, v;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        std::memcpy(&v, s + i, 8);
        crc = _mm_crc32_u64(crc, v);
    }
    for (; i < len; ++i) crc = _mm_crc32_u8(static_cast<uint32_t>(crc), static_cast<unsigned char>(s[i]));
    return crc ? crc : 1;
}

inline bool inline_streq(const char* a, const char* b, size_t len) noexcept {
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t va, vb;
        std::memcpy(&va, a + i, 8);
        std::memcpy(&vb, b + i, 8);
        if (va != vb) return false;
    }
    for (; i < len; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

inline int parse_hour_id(const char* ts) noexcept {
    static constexpr int kDayBase[8] = {-18, 6, -999, -999, -999, 30, 54, -999};
    const size_t key = ((static_cast<size_t>(ts[6]) & 1u) << 2u) | (static_cast<size_t>(ts[9]) & 3u);
    const int h = kDayBase[key] + (ts[11] - '0') * 10 + (ts[12] - '0');
    return (h >= 0 && h < static_cast<int>(kHours)) ? h : -1;
}

template <bool ParseLatency>
inline bool parse_line(const char* first, const char* last, Record& out) noexcept {
    if (last > first && last[-1] == '\r') --last;
    if (static_cast<size_t>(last - first) < 36) return false;
    out.hour = parse_hour_id(first);
    if (out.hour < 0) return false;

    const char* p = first + kHourBytes;
    if (*p++ != ' ') return false;
    while (p < last && *p != ' ') ++p;
    if (++p >= last) return false;

    const char* ep = p;
    const char* sp = static_cast<const char*>(std::memchr(p, ' ', static_cast<size_t>(last - p)));
    if (!sp || sp == p) return false;
    const size_t ep_len = static_cast<size_t>(sp - p);
    out.ep_hash = compute_crc32c(ep, ep_len);
    out.endpoint = std::string_view(ep, ep_len);
    p = sp;

    if (++p >= last || last - p < 4 || p[3] != ' ') return false;
    if constexpr (!ParseLatency) {
        out.is_5xx = (*p == '5');
        return true;
    } else {
        p += 4;
        if (p == last) return false;
        uint64_t val = 0;
        for (; p < last; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < '0' || c > '9') return false;
            val = val * 10 + (c - '0');
        }
        if (val > UINT32_MAX) return false;
        out.latency = static_cast<uint32_t>(val);
        return true;
    }
}

inline size_t lookup_slot(const EndpointCatalogue& cat, std::string_view ep, uint64_t hash) noexcept {
    const uint16_t len = static_cast<uint16_t>(ep.size());
    size_t idx = hash & (kEndpointTableSize - 1);
    while (cat.table[idx].hash != 0) {
        if (cat.table[idx].hash == hash && cat.table[idx].len == len &&
            inline_streq(cat.arena.data() + cat.table[idx].str_offset, ep.data(), len)) {
            return idx;
        }
        idx = (idx + 1) & (kEndpointTableSize - 1);
    }
    return idx;
}

inline uint16_t find_or_add_endpoint(EndpointCatalogue& cat, std::string_view ep, uint64_t hash) {
    const size_t idx = lookup_slot(cat, ep, hash);
    if (cat.table[idx].hash != 0) return cat.table[idx].id;
    if (cat.count >= kMaxEndpoints) return UINT16_MAX;
    const auto id = static_cast<uint16_t>(cat.count++);
    cat.table[idx] = {hash, static_cast<uint32_t>(cat.arena.size()), static_cast<uint16_t>(ep.size()), id};
    cat.arena.insert(cat.arena.end(), ep.begin(), ep.end());
    return id;
}

inline uint16_t find_endpoint(const EndpointCatalogue& cat, std::string_view ep, uint64_t hash) noexcept {
    const size_t idx = lookup_slot(cat, ep, hash);
    return cat.table[idx].hash ? cat.table[idx].id : UINT16_MAX;
}

struct OutputWriter {
    int fd = -1;
    size_t pos = 0;
    static constexpr size_t kBufSize = 64 * 1024;
    char buffer[kBufSize];

    explicit OutputWriter(int f) : fd(f) {}
    ~OutputWriter() {
        flush();
        if (fd >= 0) ::close(fd);
    }

    void flush() {
        for (size_t off = 0; off < pos;) {
            ssize_t res = ::write(fd, buffer + off, pos - off);
            if (res < 0 && errno == EINTR) continue;
            if (res <= 0) break;
            off += static_cast<size_t>(res);
        }
        pos = 0;
    }

    void put_s(std::string_view sv, char sep = ' ') noexcept {
        std::memcpy(buffer + pos, sv.data(), sv.size());
        pos += sv.size();
        buffer[pos++] = sep;
    }

    void put_u(uint32_t val, char sep = ' ') noexcept {
        char temp[11], *p = temp + sizeof(temp);
        do {
            *--p = static_cast<char>('0' + (val % 10));
            val /= 10;
        } while (val);
        const size_t len = static_cast<size_t>(temp + sizeof(temp) - p);
        std::memcpy(buffer + pos, p, len);
        pos += len;
        buffer[pos++] = sep;
    }

    void write_line(std::string_view hr, std::string_view ep, uint32_t n,
                    uint32_t p50, uint32_t p95, uint32_t p99, uint32_t err,
                    uint32_t ppm) noexcept {
        if (pos + 256 > kBufSize) flush();
        put_s(hr);
        put_s(ep);
        for (uint32_t v : {n, p50, p95, p99, err}) put_u(v);
        put_u(ppm, '\n');
    }
};

inline void percentiles(uint32_t* first, uint32_t* last,
                        uint32_t& p50, uint32_t& p95, uint32_t& p99) {
    const size_t n = static_cast<size_t>(last - first);
    if (n <= 3) {
        if (n > 1 && first[0] > first[1]) std::swap(first[0], first[1]);
        if (n == 3 && first[1] > first[2]) std::swap(first[1], first[2]);
        if (n > 1 && first[0] > first[1]) std::swap(first[0], first[1]);
        p50 = (n == 2) ? first[0] : first[n / 2];
        p95 = p99 = first[n - 1];
        return;
    }
    const size_t r50 = (50 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
    if (n <= 24) {
        std::sort(first, last);
    } else {
        std::nth_element(first, first + r95, last);
        if (r50 != r95) std::nth_element(first, first + r50, first + r95);
        if (r99 != r95) std::nth_element(first + r95 + 1, first + r99, last);
    }
    p50 = first[r50];
    p95 = first[r95];
    p99 = first[r99];
}

template <bool ParseLatency, class Fn>
bool scan_file(const std::string& path, Fn&& fn) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    constexpr size_t kChunk = 256 * 1024;
    std::array<char, kChunk + 256> buf;
    size_t carry = 0;
    Record rec;

    for (;;) {
        ssize_t got = ::read(fd, buf.data() + carry, kChunk);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) break;

        char* p = buf.data();
        char* const end = p + carry + static_cast<size_t>(got);
        while (p < end) {
            char* nl = static_cast<char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!nl) break;
            if (!parse_line<ParseLatency>(p, nl, rec) || !fn(rec)) {
                ::close(fd);
                return false;
            }
            p = nl + 1;
        }

        carry = static_cast<size_t>(end - p);
        if (carry >= 256) break;
        if (carry) std::memmove(buf.data(), p, carry);

        if (got == 0) {
            ::close(fd);
            return carry == 0;
        }
    }
    ::close(fd);
    return false;
}

size_t peak_rss_bytes() noexcept {
    struct rusage usage{};
    return (::getrusage(RUSAGE_SELF, &usage) == 0) ? static_cast<size_t>(usage.ru_maxrss) * 1024u : 0;
}
} // namespace

int main(int argc, char** argv) {
    std::string data_dir = "/data", out_dir = "/out";
    bool profile = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--data" && i + 1 < argc) data_dir = argv[++i];
        if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
        if (arg == "--profile") profile = true;
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) return 1;

    const std::string input_path = data_dir + "/requests.log";
    const auto t_start = std::chrono::steady_clock::now();

    EndpointCatalogue catalogue;
    catalogue.arena.reserve(128 * 1024);
    std::vector<uint32_t> group_counts(kMaxGroups), group_errors(kMaxGroups), group_cursors(kMaxGroups);
    uint64_t total_records = 0;

    const bool first_ok = scan_file<false>(input_path, [&](const Record& r) {
        const uint16_t ep_id = find_or_add_endpoint(catalogue, r.endpoint, r.ep_hash);
        if (ep_id == UINT16_MAX) return false;
        const size_t g = static_cast<size_t>(r.hour) * kMaxEndpoints + ep_id;
        ++group_counts[g];
        if (r.is_5xx) ++group_errors[g];
        ++total_records;
        return true;
    });
    if (!first_ok) return 1;
    const auto t_pass1 = std::chrono::steady_clock::now();

    const std::string out_file = out_dir + "/summary.txt";
    const int out_fd = ::open(out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0 || total_records > UINT32_MAX) return 1;
    if (total_records == 0) { ::close(out_fd); return 0; }

    uint32_t running_offset = 0;
    for (size_t i = 0; i < kMaxGroups; ++i) {
        group_cursors[i] = running_offset;
        running_offset += group_counts[i];
    }

    auto latencies = std::make_unique_for_overwrite<uint32_t[]>(total_records);

    const bool second_ok = scan_file<true>(input_path, [&](const Record& r) {
        const uint16_t ep_id = find_endpoint(catalogue, r.endpoint, r.ep_hash);
        if (ep_id == UINT16_MAX) return false;
        const size_t g = static_cast<size_t>(r.hour) * kMaxEndpoints + ep_id;
        latencies[group_cursors[g]++] = r.latency;
        return true;
    });
    if (!second_ok) return 1;
    const auto t_pass2 = std::chrono::steady_clock::now();

    const size_t ep_cnt = catalogue.count;
    std::vector<std::string_view> ep_names(ep_cnt);
    for (const auto& s : catalogue.table) {
        if (s.hash) ep_names[s.id] = std::string_view(catalogue.arena.data() + s.str_offset, s.len);
    }
    std::vector<uint16_t> sorted_ep(ep_cnt);
    std::iota(sorted_ep.begin(), sorted_ep.end(), 0);
    std::sort(sorted_ep.begin(), sorted_ep.end(), [&](uint16_t a, uint16_t b) {
        return ep_names[a] < ep_names[b];
    });

    char hour_names[kHours][21];
    for (int h = 0; h < 72; ++h) {
        const int m = (h < 30) ? 10 : 11, d = (h < 6) ? 30 : (h < 30) ? 31 : (h < 54) ? 1 : 2;
        const int base = (h < 6) ? 18 : (h < 30) ? -6 : (h < 54) ? -30 : -54;
        std::snprintf(hour_names[h], 21, "2026-%02d-%02dT%02d:00:00Z", m, d, h + base);
    }

    OutputWriter writer(out_fd);

    for (uint32_t h = 0; h < kHours; ++h) {
        const std::string_view hr_sv(hour_names[h], 20);
        for (const uint16_t ep_id : sorted_ep) {
            const size_t g = static_cast<size_t>(h) * kMaxEndpoints + ep_id;
            const uint32_t n = group_counts[g];
            if (n == 0) continue;

            uint32_t* first = latencies.get() + (group_cursors[g] - n), p50, p95, p99;
            percentiles(first, first + n, p50, p95, p99);
            writer.write_line(hr_sv, ep_names[ep_id], n, p50, p95, p99, group_errors[g],
                              static_cast<uint32_t>((static_cast<uint64_t>(group_errors[g]) * 1000000ULL) / n));
        }
    }
    writer.flush();
    const auto t_end = std::chrono::steady_clock::now();

    if (profile) {
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::fprintf(stderr,
                     "records: %lu\nendpoints: %zu\npass1_ms: %f\npass2_ms: %f\noutput_ms: %f\ntotal_ms: %f\npeak_rss_mb: %f\n",
                     total_records, ep_cnt, ms(t_start, t_pass1), ms(t_pass1, t_pass2),
                     ms(t_pass2, t_end), ms(t_start, t_end), peak_rss_bytes() / (1024.0 * 1024.0));
    }
    return 0;
}
