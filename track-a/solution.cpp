#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
#define SPEED_DEMON_HAS_SSE42_TARGET 1
#include <nmmintrin.h>
#endif
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif
#endif

namespace {
constexpr uint32_t kHours = 72;
constexpr size_t kSmallInputBytes = 1u << 20;
constexpr uint32_t kMaxLocalEndpoints = std::numeric_limits<uint16_t>::max();

struct MappedFile {
    const char* data = nullptr;
    size_t size = 0;
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    bool open(const std::string& path) {
        file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER file_size{};
        if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0) return false;
        size = static_cast<size_t>(file_size.QuadPart);
        if (size == 0) return true;
        mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return false;
        data = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
#else
    int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size < 0) return false;
        size = static_cast<size_t>(st.st_size);
        if (size == 0) return true;
        void* p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) return false;
        data = static_cast<const char*>(p);
        return true;
    }
    ~MappedFile() {
        if (data && size) munmap(const_cast<char*>(data), size);
        if (fd >= 0) ::close(fd);
    }
#endif
};

struct Record {
    uint32_t latency;
    uint32_t key; // [22:16] hour, [15:0] local endpoint, bit 23 is 5xx during counting
};
static_assert(sizeof(Record) == 8, "Record must stay compact");

#if defined(SPEED_DEMON_HAS_SSE42_TARGET)
__attribute__((target("sse4.2")))
inline size_t crc32c_hash(std::string_view key) noexcept {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(key.data());
    size_t left = key.size();
    uint64_t crc = 0x9e3779b9u;
    while (left >= 8) {
        uint64_t word;
        std::memcpy(&word, p, sizeof(word));
        crc = _mm_crc32_u64(crc, word);
        p += 8;
        left -= 8;
    }
    while (left--) crc = _mm_crc32_u8(static_cast<uint32_t>(crc), *p++);
    const uint32_t low = static_cast<uint32_t>(crc) ^ static_cast<uint32_t>(key.size() * 0x9e3779b9u);
    const uint64_t hash = (static_cast<uint64_t>(static_cast<uint32_t>(crc)) << 32) | low;
    return hash ? static_cast<size_t>(hash) : 1u;
}
#endif

inline size_t endpoint_hash(std::string_view key) noexcept {
#if defined(SPEED_DEMON_HAS_SSE42_TARGET)
    static const bool has_crc = __builtin_cpu_supports("sse4.2");
    if (has_crc) return crc32c_hash(key);
#endif
    const size_t hash = std::hash<std::string_view>{}(key);
    return hash ? hash : 1u;
}

struct FlatStringMap {
    struct Slot { size_t hash = 0; uint16_t id = 0; };
    std::vector<Slot> slots;
    size_t mask = 0;
    size_t used = 0;

    void reserve(size_t expected) {
        size_t capacity = 8;
        while (capacity * 7 / 10 < expected) capacity <<= 1;
        slots.assign(capacity, Slot{});
        mask = capacity - 1;
    }

    int find(std::string_view key, const std::vector<std::string_view>& names) const noexcept {
        const size_t hash = endpoint_hash(key);
        size_t pos = hash & mask;
        for (;;) {
            const Slot& slot = slots[pos];
            if (!slot.hash) return -1;
            if (slot.hash == hash && names[slot.id] == key) return static_cast<int>(slot.id);
            pos = (pos + 1) & mask;
        }
    }

    void insert_hash(size_t hash, uint16_t id) {
        if (!hash) hash = 1;
        if ((used + 1) * 10 > slots.size() * 7) rehash(slots.size() * 2);
        size_t pos = hash & mask;
        while (slots[pos].hash) pos = (pos + 1) & mask;
        slots[pos] = Slot{hash, id};
        ++used;
    }

    void insert(std::string_view key, uint16_t id) {
        insert_hash(endpoint_hash(key), id);
    }

    void rehash(size_t capacity) {
        std::vector<Slot> next(capacity);
        const size_t next_mask = capacity - 1;
        for (const Slot& slot : slots) {
            if (!slot.hash) continue;
            size_t pos = slot.hash & next_mask;
            while (next[pos].hash) pos = (pos + 1) & next_mask;
            next[pos] = slot;
        }
        slots.swap(next);
        mask = next_mask;
    }
};

struct Worker {
    FlatStringMap ids;
    std::vector<std::string_view> names;
    std::vector<Record> records;
    bool ok = true;
    const char* error = nullptr;
};

struct Group {
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t errors = 0;
    uint32_t p50 = 0;
    uint32_t p95 = 0;
    uint32_t p99 = 0;
};
static_assert(sizeof(Group) == 24);

struct Chunk { size_t begin; size_t end; };

inline int hour_id(const char* ts) noexcept {
    const unsigned month = static_cast<unsigned>(ts[5] - '0') * 10u + static_cast<unsigned>(ts[6] - '0');
    const unsigned day = static_cast<unsigned>(ts[8] - '0') * 10u + static_cast<unsigned>(ts[9] - '0');
    const unsigned hour = static_cast<unsigned>(ts[11] - '0') * 10u + static_cast<unsigned>(ts[12] - '0');
    if (month == 10) {
        if (day == 30 && hour >= 18 && hour <= 23) return static_cast<int>(hour - 18);
        if (day == 31 && hour <= 23) return static_cast<int>(6 + hour);
    } else if (month == 11) {
        if (day == 1 && hour <= 23) return static_cast<int>(30 + hour);
        if (day == 2 && hour <= 17) return static_cast<int>(54 + hour);
    }
    return -1;
}

inline bool parse_line(const char*& cursor, const char* end, std::string_view& endpoint,
                       uint8_t& hour, uint8_t& is_5xx, uint32_t& latency) noexcept {
    const char* line = cursor;
    if (end - line < 38) return false;
    const int h = hour_id(line);
    if (h < 0) return false;

    const char* method = line + 28;
    unsigned method_length;
    switch (*method) {
        case 'G': method_length = 3; break;                         // GET
        case 'P': method_length = method[1] == 'O' ? 4u : method[1] == 'U' ? 3u : 5u; break;
        case 'D': method_length = 6; break;                         // DELETE
        case 'H': method_length = 4; break;                         // HEAD
        case 'O': method_length = 7; break;                         // OPTIONS
        default: return false;
    }
    const char* ep = method + method_length + 1;
    if (ep >= end || ep[-1] != ' ') return false;
    const char* ep_end = static_cast<const char*>(std::memchr(ep, ' ', static_cast<size_t>(end - ep)));
    if (!ep_end || ep_end == ep) return false;
    endpoint = std::string_view(ep, static_cast<size_t>(ep_end - ep));

    const char* status = ep_end + 1;
    if (end - status < 5 || status[3] != ' ') return false;
    is_5xx = static_cast<uint8_t>(status[0] == '5');
    const char* p = status + 4;
    uint32_t value = 0;
    const char* digits = p;
    while (p < end && static_cast<unsigned char>(*p) >= '0' && static_cast<unsigned char>(*p) <= '9') {
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (value > (UINT32_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
        ++p;
    }
    if (p == digits) return false;
    if (p < end && *p == '\r') ++p;
    if (p >= end || *p != '\n') return false;

    cursor = p + 1;
    hour = static_cast<uint8_t>(h);
    latency = value;
    return true;
}

inline uint32_t decimal_digits(uint32_t v) noexcept {
    if (v >= 1000000000u) return 10;
    if (v >= 100000000u) return 9;
    if (v >= 10000000u) return 8;
    if (v >= 1000000u) return 7;
    if (v >= 100000u) return 6;
    if (v >= 10000u) return 5;
    if (v >= 1000u) return 4;
    if (v >= 100u) return 3;
    if (v >= 10u) return 2;
    return 1;
}

inline void write_uint(char*& dst, uint32_t value) noexcept {
    const auto result = std::to_chars(dst, dst + 10, value);
    dst = result.ptr;
}

inline std::string make_hour(uint32_t id) {
    int month, day, hour;
    if (id < 6) { month = 10; day = 30; hour = 18 + static_cast<int>(id); }
    else if (id < 30) { month = 10; day = 31; hour = static_cast<int>(id) - 6; }
    else if (id < 54) { month = 11; day = 1; hour = static_cast<int>(id) - 30; }
    else { month = 11; day = 2; hour = static_cast<int>(id) - 54; }
    char text[21];
    std::snprintf(text, sizeof(text), "2026-%02d-%02dT%02d:00:00Z", month, day, hour);
    return std::string(text, 20);
}

inline void select_percentiles(uint32_t* first, uint32_t* last,
                               uint32_t& p50, uint32_t& p95, uint32_t& p99) {
    const size_t n = static_cast<size_t>(last - first);
    if (n == 1) { p50 = p95 = p99 = first[0]; return; }
    if (n == 2) {
        const uint32_t a = first[0], b = first[1];
        p50 = std::min(a, b);
        p95 = p99 = std::max(a, b);
        return;
    }
    const size_t r50 = (50u * n + 99u) / 100u - 1u;
    const size_t r95 = (95u * n + 99u) / 100u - 1u;
    const size_t r99 = (99u * n + 99u) / 100u - 1u;
    if (n <= 24) {
        std::sort(first, last);
        p50 = first[r50]; p95 = first[r95]; p99 = first[r99];
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

inline size_t peak_rss_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return static_cast<size_t>(pmc.PeakWorkingSetSize);
    return 0;
#else
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) return static_cast<size_t>(usage.ru_maxrss) * 1024u;
    return 0;
#endif
}

unsigned available_parallelism() noexcept {
    unsigned count = std::thread::hardware_concurrency();
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        const int affinity_count = CPU_COUNT(&allowed);
        if (affinity_count > 0) {
            count = count ? std::min(count, static_cast<unsigned>(affinity_count))
                          : static_cast<unsigned>(affinity_count);
        }
    }
#endif
    return count ? count : 4u;
}

std::vector<Chunk> partition(const char* data, size_t size, unsigned threads) {
    std::vector<size_t> bounds(threads + 1);
    bounds.front() = 0;
    bounds.back() = size;
    for (unsigned t = 1; t < threads; ++t) {
        size_t pos = size * t / threads;
        if (pos && data[pos - 1] != '\n') {
            const void* nl = std::memchr(data + pos, '\n', size - pos);
            pos = nl ? static_cast<size_t>(static_cast<const char*>(nl) - data) + 1 : size;
        }
        bounds[t] = std::max(bounds[t - 1], pos);
    }
    std::vector<Chunk> chunks(threads);
    for (unsigned t = 0; t < threads; ++t) chunks[t] = {bounds[t], bounds[t + 1]};
    return chunks;
}

} // namespace

int main(int argc, char** argv) {
    const auto started = std::chrono::steady_clock::now();
    std::string data_dir = "/data";
    std::string out_dir = "/out";
    bool profile = false;
    unsigned requested_threads = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (arg == "--threads" && i + 1 < argc) requested_threads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (arg == "--profile") profile = true;
    }

    std::error_code fs_error;
    std::filesystem::create_directories(out_dir, fs_error);
    if (fs_error) {
        std::cerr << "FAIL cannot create output directory: " << fs_error.message() << '\n';
        return 1;
    }

    MappedFile input;
    const std::string input_path = data_dir + "/requests.log";
    if (!input.open(input_path)) {
        std::cerr << "FAIL cannot open input: " << input_path << '\n';
        return 1;
    }
    const std::string output_path = out_dir + "/summary.txt";
    if (input.size == 0) {
        std::ofstream empty(output_path, std::ios::binary);
        if (!empty) { std::cerr << "FAIL cannot write summary.txt\n"; return 1; }
        return 0;
    }

    unsigned threads = requested_threads ? requested_threads : available_parallelism();
    threads = std::clamp(threads, 1u, 12u);
    if (input.size < kSmallInputBytes) threads = 1;

#if !defined(_WIN32)
    long detected_page_size = sysconf(_SC_PAGESIZE);
    const size_t page_size = detected_page_size > 0 ? static_cast<size_t>(detected_page_size) : 4096u;
#endif
    auto stage = std::chrono::steady_clock::now();
    double t_parse=0,t_merge=0,t_count=0,t_scatter=0,t_select=0,t_output=0;
    const std::vector<Chunk> chunks = partition(input.data, input.size, threads);
    std::vector<Worker> workers(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);

    // Phase 1: parallel, allocation-light parsing. Records are stored as 8 bytes each.
    for (unsigned t = 0; t < threads; ++t) {
        Worker& w = workers[t];
        w.ids.reserve(2048);
        w.names.reserve(2048);
        const size_t chunk_bytes = chunks[t].end - chunks[t].begin;
        w.records.reserve(chunk_bytes / 76u + 16u);
        pool.emplace_back([&, t]() {
            Worker& worker = workers[t];
            const char* cursor = input.data + chunks[t].begin;
            const char* const end = input.data + chunks[t].end;
#if !defined(_WIN32)
            const size_t page = page_size;
            size_t advised_end = ((chunks[t].begin + page - 1u) / page) * page;
            size_t next_advice = chunks[t].begin + (1u << 20);
#endif
            while (cursor < end) {
                std::string_view endpoint;
                uint8_t hour = 0, error = 0;
                uint32_t latency = 0;
                if (!parse_line(cursor, end, endpoint, hour, error, latency)) {
                    worker.ok = false;
                    worker.error = "malformed or out-of-window request record";
                    return;
                }
                const int found = worker.ids.find(endpoint, worker.names);
                uint16_t id;
                if (found < 0) {
                    if (worker.names.size() >= kMaxLocalEndpoints) {
                        worker.ok = false;
                        worker.error = "more than 65,535 distinct endpoints in one input partition";
                        return;
                    }
                    id = static_cast<uint16_t>(worker.names.size());
                    worker.names.push_back(endpoint);
                    worker.ids.insert(endpoint, id);
                } else {
                    id = static_cast<uint16_t>(found);
                }
                worker.records.push_back(Record{latency, static_cast<uint32_t>(id) | (static_cast<uint32_t>(hour) << 16) | (static_cast<uint32_t>(error) << 23)});
#if !defined(_WIN32)
                const size_t current = static_cast<size_t>(cursor - input.data);
                if (current >= next_advice) {
                    const size_t safe_end = (current / page) * page;
                    if (safe_end > advised_end) {
                        madvise(const_cast<char*>(input.data + advised_end), safe_end - advised_end, MADV_DONTNEED);
                        advised_end = safe_end;
                    }
                    next_advice = current + (1u << 20);
                }
#endif
            }
#if !defined(_WIN32)
            const size_t safe_end = (static_cast<size_t>(end - input.data) / page) * page;
            if (safe_end > advised_end) madvise(const_cast<char*>(input.data + advised_end), safe_end - advised_end, MADV_DONTNEED);
#endif
        });
    }
    for (auto& th : pool) th.join();
    pool.clear();
    auto mark=std::chrono::steady_clock::now(); t_parse=std::chrono::duration<double>(mark-stage).count(); stage=mark;

    size_t total_records = 0;
    size_t local_endpoint_total = 0;
    for (const Worker& w : workers) {
        if (!w.ok) {
            std::cerr << "FAIL parse: " << (w.error ? w.error : "unknown error") << '\n';
            return 1;
        }
        total_records += w.records.size();
        local_endpoint_total += w.names.size();
    }
    if (total_records > UINT32_MAX) {
        std::cerr << "FAIL record count exceeds 32-bit latency indexing\n";
        return 1;
    }

    // Phase 2: unify per-worker endpoint IDs without copying endpoint bytes.
    FlatStringMap global_ids;
    global_ids.reserve(std::min<size_t>(local_endpoint_total, kMaxLocalEndpoints));
    std::vector<std::string_view> endpoint_names;
    endpoint_names.reserve(std::min<size_t>(local_endpoint_total, kMaxLocalEndpoints));
    std::vector<std::vector<uint16_t>> local_to_global(threads);
    for (unsigned t = 0; t < threads; ++t) {
        const Worker& w = workers[t];
        auto& remap = local_to_global[t];
        remap.resize(w.names.size());
        for (size_t i = 0; i < w.names.size(); ++i) {
            const std::string_view name = w.names[i];
            const int found = global_ids.find(name, endpoint_names);
            if (found < 0) {
                if (endpoint_names.size() >= kMaxLocalEndpoints) {
                    std::cerr << "FAIL more than 65,535 distinct endpoints in input\n";
                    return 1;
                }
                const uint16_t id = static_cast<uint16_t>(endpoint_names.size());
                endpoint_names.push_back(name);
                global_ids.insert(name, id);
                remap[i] = id;
            } else {
                remap[i] = static_cast<uint16_t>(found);
            }
        }
    }
    // No endpoint lookup is needed after the local-to-global maps are built.
    FlatStringMap().slots.swap(global_ids.slots);
    for (Worker& w : workers) {
        FlatStringMap().slots.swap(w.ids.slots);
        std::vector<std::string_view>().swap(w.names);
    }
    mark=std::chrono::steady_clock::now(); t_merge=std::chrono::duration<double>(mark-stage).count(); stage=mark;
    const size_t endpoint_count = endpoint_names.size();
    const size_t group_count = static_cast<size_t>(kHours) * endpoint_count;
    if (group_count == 0 || group_count > UINT32_MAX) {
        std::cerr << "FAIL invalid group catalogue size\n";
        return 1;
    }

    // Phase 3: count records per worker/group. Each worker owns its counter row;
    // the only shared updates are 5xx counters, using relaxed atomic increments.
    std::vector<uint32_t> worker_cursors(static_cast<size_t>(threads) * group_count, 0u);
    std::vector<Group> groups(group_count);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t]() {
            const size_t row = static_cast<size_t>(t) * group_count;
            const auto& remap = local_to_global[t];
            for (Record& r : workers[t].records) {
                const uint32_t local_id = r.key & 0xffffu;
                const uint32_t hour = (r.key >> 16) & 0x7fu;
                const bool is_5xx = ((r.key >> 23) & 1u) != 0;
                const uint32_t g = static_cast<uint32_t>(static_cast<size_t>(hour) * endpoint_count + remap[local_id]);
                r.key = g;
                ++worker_cursors[row + g];
                if (is_5xx) {
                    std::atomic_ref<uint32_t>(groups[g].errors).fetch_add(1u, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    pool.clear();
    std::vector<std::vector<uint16_t>>().swap(local_to_global);

    mark=std::chrono::steady_clock::now(); t_count=std::chrono::duration<double>(mark-stage).count(); stage=mark;
    // Turn each worker's count cell into its exclusive write cursor. For each
    // (hour, endpoint), worker slices are adjacent, so all latencies for a group
    // become one contiguous range without a second compaction pass.
    uint64_t running = 0;
    for (size_t g = 0; g < group_count; ++g) {
        Group& group = groups[g];
        group.start = static_cast<uint32_t>(running);
        uint64_t count = 0;
        for (unsigned t = 0; t < threads; ++t) {
            uint32_t& cell = worker_cursors[static_cast<size_t>(t) * group_count + g];
            const uint32_t worker_count = cell;
            cell = static_cast<uint32_t>(running);
            running += worker_count;
            count += worker_count;
        }
        if (count > UINT32_MAX || running > total_records) {
            std::cerr << "FAIL group prefix overflow or count mismatch\n";
            return 1;
        }
        group.count = static_cast<uint32_t>(count);
    }
    if (running != total_records) {
        std::cerr << "FAIL parsed record count does not match group totals\n";
        return 1;
    }

    // Phase 4: scatter each record's latency exactly once into its final group slice.
    std::vector<uint32_t> latencies(total_records);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t]() {
            const size_t row = static_cast<size_t>(t) * group_count;
            for (const Record& r : workers[t].records) {
                latencies[worker_cursors[row + r.key]++] = r.latency;
            }
        });
    }
    for (auto& th : pool) th.join();
    pool.clear();

    mark=std::chrono::steady_clock::now(); t_scatter=std::chrono::duration<double>(mark-stage).count(); stage=mark;
    // Release the large parse-time buffers before percentile selection.
    for (Worker& w : workers) {
        std::vector<Record>().swap(w.records);
        FlatStringMap().slots.swap(w.ids.slots);
        std::vector<std::string_view>().swap(w.names);
    }
    std::vector<uint32_t>().swap(worker_cursors);

    mark=std::chrono::steady_clock::now(); t_select=std::chrono::duration<double>(mark-stage).count(); stage=mark;
    // Output order is chronological hour, then unsigned-bytewise endpoint order.
    std::vector<uint16_t> sorted_endpoints(endpoint_count);
    for (size_t i = 0; i < endpoint_count; ++i) sorted_endpoints[i] = static_cast<uint16_t>(i);
    std::sort(sorted_endpoints.begin(), sorted_endpoints.end(), [&](uint16_t a, uint16_t b) {
        return endpoint_names[a] < endpoint_names[b];
    });

    // Phase 5: percentile selection is independent by group and dynamically scheduled.
    std::atomic<size_t> next_group{0};
    constexpr size_t kGroupBatch = 128;
    const unsigned reduction_threads = std::min<unsigned>(threads, 12u);
    for (unsigned t = 0; t < reduction_threads; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                const size_t begin = next_group.fetch_add(kGroupBatch, std::memory_order_relaxed);
                if (begin >= group_count) break;
                const size_t end = std::min(group_count, begin + kGroupBatch);
                for (size_t g = begin; g < end; ++g) {
                    Group& group = groups[g];
                    if (!group.count) continue;
                    uint32_t* first = latencies.data() + group.start;
                    uint32_t* last = first + group.count;
                    select_percentiles(first, last, group.p50, group.p95, group.p99);
                    // The latency offset is dead after selection; reuse it for output ppm.
                    group.start = static_cast<uint32_t>((static_cast<uint64_t>(group.errors) * 1000000u) / group.count);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    pool.clear();

    // Phase 6: size output exactly, then format disjoint hour ranges in parallel.
    std::vector<std::string> hour_names(kHours);
    for (uint32_t h = 0; h < kHours; ++h) hour_names[h] = make_hour(h);
    std::vector<size_t> hour_offsets(kHours + 1, 0u);
    for (uint32_t h = 0; h < kHours; ++h) {
        size_t bytes = 0;
        const size_t base = static_cast<size_t>(h) * endpoint_count;
        for (uint16_t endpoint_id : sorted_endpoints) {
            const Group& group = groups[base + endpoint_id];
            if (!group.count) continue;
            const uint32_t ppm = group.start;
            bytes += 28u + endpoint_names[endpoint_id].size() +
                     decimal_digits(group.count) + decimal_digits(group.p50) +
                     decimal_digits(group.p95) + decimal_digits(group.p99) +
                     decimal_digits(group.errors) + decimal_digits(ppm);
        }
        hour_offsets[h + 1] = hour_offsets[h] + bytes;
    }
    std::string output(hour_offsets.back(), '\0');
    const unsigned output_threads = std::min<unsigned>(threads, kHours);
    for (unsigned t = 0; t < output_threads; ++t) {
        const uint32_t h_begin = static_cast<uint32_t>((kHours * t) / output_threads);
        const uint32_t h_end = static_cast<uint32_t>((kHours * (t + 1)) / output_threads);
        pool.emplace_back([&, h_begin, h_end]() {
            for (uint32_t h = h_begin; h < h_end; ++h) {
                char* dst = output.data() + hour_offsets[h];
                const size_t base = static_cast<size_t>(h) * endpoint_count;
                const std::string& timestamp = hour_names[h];
                for (uint16_t endpoint_id : sorted_endpoints) {
                    const Group& group = groups[base + endpoint_id];
                    if (!group.count) continue;
                    const std::string_view endpoint = endpoint_names[endpoint_id];
                    std::memcpy(dst, timestamp.data(), timestamp.size()); dst += timestamp.size();
                    *dst++ = ' ';
                    std::memcpy(dst, endpoint.data(), endpoint.size()); dst += endpoint.size();
                    *dst++ = ' '; write_uint(dst, group.count);
                    *dst++ = ' '; write_uint(dst, group.p50);
                    *dst++ = ' '; write_uint(dst, group.p95);
                    *dst++ = ' '; write_uint(dst, group.p99);
                    *dst++ = ' '; write_uint(dst, group.errors);
                    *dst++ = ' ';
                    write_uint(dst, group.start);
                    *dst++ = '\n';
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    pool.clear();

    {
        std::ofstream file(output_path, std::ios::binary);
        if (!file) { std::cerr << "FAIL cannot open output: " << output_path << '\n'; return 1; }
        file.write(output.data(), static_cast<std::streamsize>(output.size()));
        file.close();
        if (!file) { std::cerr << "FAIL writing output: " << output_path << '\n'; return 1; }
    }

    mark=std::chrono::steady_clock::now(); t_output=std::chrono::duration<double>(mark-stage).count();
    if (profile) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cerr << "records: " << total_records << '\n'
                  << "endpoints: " << endpoint_count << '\n'
                  << "groups: " << group_count << '\n'
                  << "threads: " << threads << '\n'
                  << "wall_time: " << elapsed << " s\n"
                  << "peak_rss_mb: " << (peak_rss_bytes() / (1024.0 * 1024.0)) << '\n'
                  << "stages: " << t_parse << ' ' << t_merge << ' ' << t_count << ' ' << t_scatter << ' ' << t_select << ' ' << t_output << '\n';
    }
    return 0;
}