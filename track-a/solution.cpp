#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
constexpr uint32_t kHours = 72;
constexpr uint32_t kMaxEndpoints = 1842;
constexpr uint32_t kMaxGroups = kHours * kMaxEndpoints;
constexpr uint32_t kHourBytes = 27;

struct TransparentHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const std::string& s) const noexcept {
        return (*this)(std::string_view{s});
    }
};
struct TransparentEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};
using EndpointMap = std::unordered_map<std::string, uint16_t, TransparentHash, TransparentEqual>;

struct GroupMeta {
    uint32_t count = 0;
    uint32_t errors = 0;
    uint32_t cursor = 0;
};
static_assert(sizeof(GroupMeta) == 12);

struct ParsedCountRecord {
    int hour = -1;
    std::string_view endpoint;
    bool is_5xx = false;
};
struct ParsedLatencyRecord {
    int hour = -1;
    std::string_view endpoint;
    uint32_t latency = 0;
};

inline int parse_hour_id(const char* ts) noexcept {
    const int month = (ts[5] - '0') * 10 + (ts[6] - '0');
    const int day = (ts[8] - '0') * 10 + (ts[9] - '0');
    const int hour = (ts[11] - '0') * 10 + (ts[12] - '0');
    if (month == 10) {
        if (day == 30 && hour >= 18) return hour - 18;
        if (day == 31) return 6 + hour;
    } else if (month == 11) {
        if (day == 1) return 30 + hour;
        if (day == 2 && hour <= 17) return 54 + hour;
    }
    return -1;
}

inline bool parse_common_fields(const char* first, const char* last,
                                int& hour, std::string_view& endpoint,
                                const char*& after_endpoint) noexcept {
    if (last > first && last[-1] == '\r') --last;
    if (static_cast<size_t>(last - first) < kHourBytes + 1 + 3 + 1 + 1) return false;
    hour = parse_hour_id(first);
    if (hour < 0 || hour >= static_cast<int>(kHours)) return false;

    const char* p = first + kHourBytes;
    if (p == last || *p++ != ' ') return false;
    while (p < last && *p != ' ') ++p; // method
    if (p == last) return false;
    ++p;

    const char* ep = p;
    while (p < last && *p != ' ') ++p;
    if (p == ep || p == last) return false;
    endpoint = std::string_view(ep, static_cast<size_t>(p - ep));
    after_endpoint = p + 1;
    return true;
}

inline bool parse_count_record(const char* first, const char* last,
                               ParsedCountRecord& out) noexcept {
    const char* p = nullptr;
    if (!parse_common_fields(first, last, out.hour, out.endpoint, p)) return false;
    if (last - p < 3) return false;
    out.is_5xx = (*p == '5');
    return p + 3 < last && p[3] == ' ';
}

inline bool parse_latency_record(const char* first, const char* last,
                                 ParsedLatencyRecord& out) noexcept {
    const char* p = nullptr;
    if (!parse_common_fields(first, last, out.hour, out.endpoint, p)) return false;
    if (last - p < 4 || p[3] != ' ') return false;
    p += 4;
    if (p == last) return false;
    uint64_t value = 0;
    for (; p < last; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value > UINT32_MAX) return false;
    }
    out.latency = static_cast<uint32_t>(value);
    return true;
}

inline uint16_t find_or_add_endpoint(EndpointMap& map, std::string_view endpoint) {
    auto it = map.find(endpoint);
    if (it != map.end()) return it->second;
    if (map.size() >= kMaxEndpoints) throw std::runtime_error("endpoint catalogue exceeds the Track A limit");
    const auto id = static_cast<uint16_t>(map.size());
    auto inserted = map.emplace(std::string(endpoint), id);
    return inserted.first->second;
}

inline void append_uint(std::string& out, uint64_t value) {
    char buf[24];
    char* p = buf + sizeof(buf);
    if (value == 0) {
        out.push_back('0');
        return;
    }
    while (value) {
        *--p = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    out.append(p, static_cast<size_t>(buf + sizeof(buf) - p));
}

inline std::string make_hour_string(uint32_t hour_id) {
    int month, day, hour;
    if (hour_id < 6) {
        month = 10; day = 30; hour = 18 + static_cast<int>(hour_id);
    } else if (hour_id < 30) {
        month = 10; day = 31; hour = static_cast<int>(hour_id) - 6;
    } else if (hour_id < 54) {
        month = 11; day = 1; hour = static_cast<int>(hour_id) - 30;
    } else {
        month = 11; day = 2; hour = static_cast<int>(hour_id) - 54;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "2026-%02d-%02dT%02d:00:00Z", month, day, hour);
    return std::string(buf);
}

inline void percentiles(uint32_t* first, uint32_t* last,
                        uint32_t& p50, uint32_t& p95, uint32_t& p99) {
    const size_t n = static_cast<size_t>(last - first);
    const size_t r50 = (50 * n + 99) / 100 - 1;
    const size_t r95 = (95 * n + 99) / 100 - 1;
    const size_t r99 = (99 * n + 99) / 100 - 1;

    if (n <= 24) {
        std::sort(first, last);
        p50 = first[r50];
        p95 = first[r95];
        p99 = first[r99];
        return;
    }

    std::nth_element(first, first + r95, last);
    p95 = first[r95];
    if (r50 == r95) p50 = p95;
    else {
        std::nth_element(first, first + r50, first + r95);
        p50 = first[r50];
    }
    if (r99 == r95) p99 = p95;
    else {
        std::nth_element(first + r95 + 1, first + r99, last);
        p99 = first[r99];
    }
}

template <class Record, class Parser, class Fn>
bool scan_file(const std::string& path, Parser parser, Fn&& fn, std::string& error) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = std::string("cannot open input: ") + std::strerror(errno);
        return false;
    }

    constexpr size_t kChunkBytes = 64u * 1024u;
    constexpr size_t kMaxLineBytes = 256u;
    std::array<char, kChunkBytes + kMaxLineBytes> buffer{};
    size_t carry = 0;
    Record record;

    for (;;) {
        ssize_t got;
        do {
            got = ::read(fd, buffer.data() + carry, kChunkBytes);
        } while (got < 0 && errno == EINTR);
        if (got < 0) {
            error = std::string("input read failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }

        char* p = buffer.data();
        char* const end = p + carry + static_cast<size_t>(got);
        while (p < end) {
            const char* nl = static_cast<const char*>(
                std::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!nl) break;
            if (!parser(p, nl, record)) {
                error = "malformed Track A record";
                ::close(fd);
                return false;
            }
            if (!fn(record)) {
                ::close(fd);
                return false;
            }
            p = const_cast<char*>(nl) + 1;
        }

        carry = static_cast<size_t>(end - p);
        if (carry >= kMaxLineBytes) {
            error = "record exceeds the Track A maximum line length";
            ::close(fd);
            return false;
        }
        if (carry) std::memmove(buffer.data(), p, carry);

        if (got == 0) {
            ::close(fd);
            if (carry != 0) {
                error = "input does not end with LF";
                return false;
            }
            return true;
        }
    }
}

size_t peak_rss_bytes() noexcept {
    struct rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return static_cast<size_t>(usage.ru_maxrss) * 1024u;
}

} // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::string data_dir = "/data";
    std::string out_dir = "/out";
    bool profile = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (arg == "--profile") profile = true;
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::cerr << "FAIL cannot create output directory: " << ec.message() << '\n';
        return 1;
    }

    const std::string input_path = data_dir + "/requests.log";

    EndpointMap endpoint_ids;
    endpoint_ids.max_load_factor(0.8f);
    endpoint_ids.reserve(kMaxEndpoints);
    std::vector<GroupMeta> groups(kMaxGroups);
    uint64_t total_records = 0;
    std::string error;

    // Pass 1: count records and 5xx responses per (hour, endpoint).
    // The fixed group table is only 72 x 1,842 x 12 bytes; no per-worker
    // vectors or per-group heap allocations are created.
    const bool first_ok = scan_file<ParsedCountRecord>(input_path, parse_count_record, [&](const ParsedCountRecord& record) {
        const uint16_t endpoint_id = find_or_add_endpoint(endpoint_ids, record.endpoint);
        GroupMeta& group = groups[static_cast<size_t>(record.hour) * kMaxEndpoints + endpoint_id];
        if (group.count == UINT32_MAX) {
            error = "group count exceeds uint32 range";
            return false;
        }
        ++group.count;
        if (record.is_5xx) ++group.errors;
        ++total_records;
        return true;
    }, error);
    if (!first_ok) {
        std::cerr << "FAIL first pass: " << error << '\n';
        return 1;
    }

    if (total_records == 0) {
        std::ofstream empty_out(out_dir + "/summary.txt", std::ios::binary);
        if (!empty_out) {
            std::cerr << "FAIL cannot write summary.txt\n";
            return 1;
        }
        return 0;
    }
    if (total_records > UINT32_MAX) {
        std::cerr << "FAIL total record count exceeds uint32 range\n";
        return 1;
    }

    // Turn the per-group counts into prefix cursors. Each group's cursor advances
    // during pass 2, then cursor-count gives its exact beginning in the flat array.
    uint32_t running_offset = 0;
    for (GroupMeta& group : groups) {
        group.cursor = running_offset;
        if (UINT32_MAX - running_offset < group.count) {
            std::cerr << "FAIL latency offsets exceed uint32 range\n";
            return 1;
        }
        running_offset += group.count;
    }
    if (running_offset != total_records) {
        std::cerr << "FAIL internal count/prefix mismatch\n";
        return 1;
    }

    // Exactly one uint32 per request; unlike vector-per-group storage, this has no
    // capacity slack, per-vector allocator metadata, or duplicated worker-local tables.
    std::unique_ptr<uint32_t[]> latencies(new (std::nothrow) uint32_t[static_cast<size_t>(total_records)]);
    if (!latencies) {
        std::cerr << "FAIL cannot allocate latency array\n";
        return 1;
    }

    // Pass 2: place each latency directly into its group's contiguous slice.
    // The cursor array was prepared by prefix-summing counts in pass 1.
    const bool second_ok = scan_file<ParsedLatencyRecord>(input_path, parse_latency_record, [&](const ParsedLatencyRecord& record) {
        const auto it = endpoint_ids.find(record.endpoint);
        if (it == endpoint_ids.end()) {
            error = "endpoint catalogue changed between input passes";
            return false;
        }
        GroupMeta& group = groups[static_cast<size_t>(record.hour) * kMaxEndpoints + it->second];
        if (group.cursor >= total_records) {
            error = "latency write cursor exceeds allocated array";
            return false;
        }
        latencies[group.cursor++] = record.latency;
        return true;
    }, error);
    if (!second_ok) {
        std::cerr << "FAIL second pass: " << error << '\n';
        return 1;
    }

    // Build a compact endpoint-name table. unordered_map element references remain
    // stable after rehash; no endpoint strings are copied into a second catalogue.
    const size_t endpoint_count = endpoint_ids.size();
    std::vector<std::string_view> endpoint_names(endpoint_count);
    for (const auto& entry : endpoint_ids) endpoint_names[entry.second] = entry.first;
    std::vector<uint16_t> sorted_endpoint_ids(endpoint_count);
    std::iota(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), uint16_t{0});
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(),
              [&](uint16_t a, uint16_t b) { return endpoint_names[a] < endpoint_names[b]; });

    std::array<std::string, kHours> hour_names;
    for (uint32_t h = 0; h < kHours; ++h) hour_names[h] = make_hour_string(h);

    // Stream output one line at a time; do not retain the 10 MB result in RAM.
    std::vector<char> output_buffer(8u * 1024u);
    std::ofstream output;
    output.rdbuf()->pubsetbuf(output_buffer.data(), static_cast<std::streamsize>(output_buffer.size()));
    output.open(out_dir + "/summary.txt", std::ios::binary);
    if (!output) {
        std::cerr << "FAIL cannot open summary.txt for writing\n";
        return 1;
    }
    std::string line;
    line.reserve(256);

    for (uint32_t h = 0; h < kHours; ++h) {
        for (const uint16_t endpoint_id : sorted_endpoint_ids) {
            GroupMeta& group = groups[static_cast<size_t>(h) * kMaxEndpoints + endpoint_id];
            const uint32_t n = group.count;
            if (n == 0) continue;

            const uint32_t end_offset = group.cursor;
            if (end_offset < n) {
                std::cerr << "FAIL invalid group latency range\n";
                return 1;
            }
            uint32_t* first = latencies.get() + (end_offset - n);
            uint32_t* last = first + n;
            uint32_t p50, p95, p99;
            percentiles(first, last, p50, p95, p99);

            const uint64_t error_ppm = (static_cast<uint64_t>(group.errors) * 1000000ULL) / n;
            line.clear();
            line.append(hour_names[h]);
            line.push_back(' ');
            line.append(endpoint_names[endpoint_id]);
            line.push_back(' ');
            append_uint(line, n);
            line.push_back(' ');
            append_uint(line, p50);
            line.push_back(' ');
            append_uint(line, p95);
            line.push_back(' ');
            append_uint(line, p99);
            line.push_back(' ');
            append_uint(line, group.errors);
            line.push_back(' ');
            append_uint(line, error_ppm);
            line.push_back('\n');
            output.write(line.data(), static_cast<std::streamsize>(line.size()));
        }
    }
    output.close();
    if (!output) {
        std::cerr << "FAIL writing summary.txt\n";
        return 1;
    }

    if (profile) {
        std::cerr << "records: " << total_records << '\n'
                  << "endpoints: " << endpoint_count << '\n'
                  << "peak_rss_mb: " << (peak_rss_bytes() / (1024.0 * 1024.0)) << '\n';
    }
    return 0;
}
