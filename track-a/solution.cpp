#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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
constexpr uint32_t kMaxWorkers = 12;
constexpr uint32_t kEmptySlot = std::numeric_limits<uint32_t>::max();

struct MappedFile {
    const char* data = nullptr;
    size_t size = 0;

#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;

    bool open(const std::string& path) {
        close();
        file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER file_size{};
        if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0 ||
            static_cast<unsigned long long>(file_size.QuadPart) >
                static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
            close();
            return false;
        }
        size = static_cast<size_t>(file_size.QuadPart);
        if (size == 0) return true;

        mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) {
            close();
            return false;
        }
        data = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!data) {
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (data) {
            UnmapViewOfFile(data);
            data = nullptr;
        }
        if (mapping) {
            CloseHandle(mapping);
            mapping = nullptr;
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
        size = 0;
    }
#else
    int fd = -1;

    bool open(const std::string& path) {
        close();
        int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
        open_flags |= O_CLOEXEC;
#endif
        fd = ::open(path.c_str(), open_flags);
        if (fd < 0) return false;

        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size < 0 ||
            static_cast<uintmax_t>(st.st_size) >
                static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
            close();
            return false;
        }
        size = static_cast<size_t>(st.st_size);
        if (size == 0) return true;

        void* mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) {
            close();
            return false;
        }
        data = static_cast<const char*>(mapped);
        return true;
    }

    void close() {
        if (data && size != 0) {
            munmap(const_cast<char*>(data), size);
            data = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        size = 0;
    }
#endif

    ~MappedFile() { close(); }
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

// The competition input has a fixed 72-hour UTC window. Keeping this mapping
// branch-free in the hot parser avoids date-library work for every record.
static inline int parse_hour_id(const char* ts) noexcept {
    const int month = (ts[5] - '0') * 10 + (ts[6] - '0');
    const int day = (ts[8] - '0') * 10 + (ts[9] - '0');
    const int hour = (ts[11] - '0') * 10 + (ts[12] - '0');

    if (month == 10) {
        if (day == 30) return hour - 18;
        if (day == 31) return 6 + hour;
    } else if (month == 11) {
        if (day == 1) return 30 + hour;
        if (day == 2) return 54 + hour;
    }
    return -1;
}

static std::string make_hour_string(uint32_t hour_id) {
    if (hour_id >= kHours) return {};
    const int hour_index = static_cast<int>(hour_id);
    int month, day, hour;
    if (hour_index < 6) {
        month = 10; day = 30; hour = 18 + hour_index;
    } else if (hour_index < 30) {
        month = 10; day = 31; hour = hour_index - 6;
    } else if (hour_index < 54) {
        month = 11; day = 1; hour = hour_index - 30;
    } else {
        month = 11; day = 2; hour = hour_index - 54;
    }
    char buffer[21];
    std::snprintf(buffer, sizeof(buffer), "2026-%02d-%02dT%02d:00:00Z", month, day, hour);
    return std::string(buffer, 20);
}

static inline void append_uint(std::string& out, uint64_t value) {
    char buffer[20];
    char* cursor = buffer + sizeof(buffer);
    do {
        *--cursor = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    out.append(cursor, static_cast<size_t>((buffer + sizeof(buffer)) - cursor));
}

static inline void compute_percentiles(uint32_t* values, size_t n,
                                       uint32_t& p50, uint32_t& p95,
                                       uint32_t& p99) {
    if (n == 1) {
        p50 = p95 = p99 = values[0];
        return;
    }
    if (n == 2) {
        if (values[0] > values[1]) std::swap(values[0], values[1]);
        p50 = values[0];
        p95 = p99 = values[1];
        return;
    }

    const size_t r50 = (50 * n + 99) / 100 - 1;
    const size_t r95 = (95 * n + 99) / 100 - 1;
    const size_t r99 = (99 * n + 99) / 100 - 1;

    if (n <= 24) {
        std::sort(values, values + n);
        p50 = values[r50];
        p95 = values[r95];
        p99 = values[r99];
        return;
    }

    std::nth_element(values, values + r95, values + n);
    p95 = values[r95];

    if (r50 == r95) p50 = p95;
    else {
        std::nth_element(values, values + r50, values + r95);
        p50 = values[r50];
    }

    if (r99 == r95) p99 = p95;
    else {
        std::nth_element(values + r95 + 1, values + r99, values + n);
        p99 = values[r99];
    }
}

static inline size_t peak_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<size_t>(counters.PeakWorkingSetSize);
    return 0;
#else
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        return static_cast<size_t>(usage.ru_maxrss);
#else
        return static_cast<size_t>(usage.ru_maxrss) * 1024;
#endif
    }
    return 0;
#endif
}

// Inline latency slots eliminate most per-group heap allocations when
// requests are divided among several parser workers. Larger shards spill once
// into a geometrically grown heap buffer.
struct LatencyBuffer {
    static constexpr uint32_t kInlineCapacity = 8;
    std::array<uint32_t, kInlineCapacity> inline_values{};
    std::unique_ptr<uint32_t[]> heap_values;
    uint32_t length = 0;
    uint32_t allocated = kInlineCapacity;

    LatencyBuffer() = default;
    LatencyBuffer(const LatencyBuffer&) = delete;
    LatencyBuffer& operator=(const LatencyBuffer&) = delete;
    LatencyBuffer(LatencyBuffer&&) noexcept = default;
    LatencyBuffer& operator=(LatencyBuffer&&) noexcept = default;

    uint32_t* data() noexcept { return heap_values ? heap_values.get() : inline_values.data(); }
    const uint32_t* data() const noexcept { return heap_values ? heap_values.get() : inline_values.data(); }
    uint32_t size() const noexcept { return length; }
    uint32_t capacity() const noexcept { return allocated; }

    void reserve(uint32_t requested) {
        if (requested <= allocated) return;
        uint32_t next_capacity = allocated;
        while (next_capacity < requested) next_capacity *= 2;
        auto next = std::make_unique<uint32_t[]>(next_capacity);
        std::copy(data(), data() + length, next.get());
        heap_values = std::move(next);
        allocated = next_capacity;
    }

    void push_back(uint32_t value) {
        if (length == allocated) reserve(length + 1);
        data()[length++] = value;
    }

    void append_from(const LatencyBuffer& other) {
        if (other.length == 0) return;
        reserve(length + other.length);
        std::copy(other.data(), other.data() + other.length, data() + length);
        length += other.length;
    }

    void release() noexcept {
        heap_values.reset();
        length = 0;
        allocated = kInlineCapacity;
    }
};

struct LocalGroup {
    uint32_t count = 0;
    uint32_t errors = 0;
    LatencyBuffer latencies;

    LocalGroup() = default;
    LocalGroup(const LocalGroup&) = delete;
    LocalGroup& operator=(const LocalGroup&) = delete;
    LocalGroup(LocalGroup&&) noexcept = default;
    LocalGroup& operator=(LocalGroup&&) noexcept = default;
};

// Sparse group storage: a slot exists for every possible (local endpoint, hour),
// but LocalGroup and its latency vector are created only when that pair appears.
// This removes millions of eagerly constructed vector objects from the old table.
struct WorkerThread {
    std::unordered_map<std::string_view, uint32_t> endpoint_ids;
    std::vector<std::string> endpoint_names;
    std::vector<uint32_t> group_slot;
    std::vector<LocalGroup> groups;
    size_t endpoint_slot_capacity = 256;

    WorkerThread() {
        endpoint_ids.max_load_factor(0.70f);
        endpoint_ids.reserve(2048);
        endpoint_names.reserve(2048);
        group_slot.assign(endpoint_slot_capacity * kHours, kEmptySlot);
        groups.reserve(8192);
    }

    void ensure_endpoint_slot(uint32_t endpoint_id) {
        if (endpoint_id < endpoint_slot_capacity) return;
        size_t new_capacity = endpoint_slot_capacity;
        while (endpoint_id >= new_capacity) new_capacity *= 2;
        group_slot.resize(new_capacity * kHours, kEmptySlot);
        endpoint_slot_capacity = new_capacity;
    }

    inline LocalGroup& get_group(uint32_t endpoint_id, uint32_t hour_id) {
        const size_t slot_index = static_cast<size_t>(endpoint_id) * kHours + hour_id;
        uint32_t& group_id = group_slot[slot_index];
        if (group_id == kEmptySlot) {
            group_id = static_cast<uint32_t>(groups.size());
            groups.emplace_back();
        }
        return groups[group_id];
    }
};

static inline bool unsigned_bytewise_less(std::string_view lhs,
                                          std::string_view rhs) noexcept {
    const size_t common = std::min(lhs.size(), rhs.size());
    for (size_t i = 0; i < common; ++i) {
        const auto a = static_cast<unsigned char>(lhs[i]);
        const auto b = static_cast<unsigned char>(rhs[i]);
        if (a != b) return a < b;
    }
    return lhs.size() < rhs.size();
}

} // namespace

int main(int argc, char* argv[]) {
    const auto start_time = std::chrono::steady_clock::now();
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string data_dir = "/data";
    std::string out_dir = "/out";
    bool profile = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (arg == "--profile") profile = true;
    }

    const std::string input_path = data_dir + "/requests.log";
    const std::string output_path = out_dir + "/summary.txt";

    std::error_code fs_error;
    std::filesystem::create_directories(out_dir, fs_error);
    if (fs_error) {
        std::cerr << "FAIL cannot create output directory " << out_dir << ": "
                  << fs_error.message() << '\n';
        return 1;
    }

    MappedFile input;
    if (!input.open(input_path)) {
        const std::string alternate_path = data_dir + "\\requests.log";
        if (!input.open(alternate_path)) {
            std::cerr << "FAIL cannot open " << input_path << '\n';
            return 1;
        }
    }

    if (input.size == 0) {
        std::ofstream empty_output(output_path, std::ios::binary | std::ios::trunc);
        if (!empty_output) {
            std::cerr << "FAIL cannot write " << output_path << '\n';
            return 1;
        }
        return 0;
    }

    unsigned int worker_count = std::thread::hardware_concurrency();
    if (worker_count == 0) worker_count = 4;
#if defined(__linux__)
    // hardware_concurrency() may report the host total inside an affinity-
    // restricted process. Respect the CPUs this process can actually run on.
    cpu_set_t allowed_cpus;
    CPU_ZERO(&allowed_cpus);
    if (sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus) == 0) {
        const int allowed_count = CPU_COUNT(&allowed_cpus);
        if (allowed_count > 0)
            worker_count = std::min(worker_count, static_cast<unsigned int>(allowed_count));
    }
#endif
    worker_count = std::min(worker_count, kMaxWorkers);
    if (input.size < 2 * 1024 * 1024) worker_count = 1;

    std::vector<WorkerThread> workers(worker_count);

    struct Chunk { const char* begin; const char* end; };
    std::vector<Chunk> chunks(worker_count);
    const char* const file_begin = input.data;
    const char* const file_end = input.data + input.size;

    // Adjacent workers derive the same aligned cut for each nominal boundary.
    // Therefore every complete record belongs to exactly one worker.
    for (unsigned int t = 0; t < worker_count; ++t) {
        const size_t nominal_begin = (input.size * t) / worker_count;
        const size_t nominal_end = (input.size * (t + 1)) / worker_count;
        const char* begin = file_begin + nominal_begin;
        const char* end = file_begin + nominal_end;

        if (t != 0) {
            while (begin < file_end && *begin != '\n') ++begin;
            if (begin < file_end) ++begin;
        }
        if (t + 1 != worker_count) {
            while (end < file_end && *end != '\n') ++end;
            if (end < file_end) ++end;
        } else {
            end = file_end;
        }
        chunks[t] = {begin, end};
    }

#if defined(__linux__)
    const long system_page_size = sysconf(_SC_PAGESIZE);
#else
    const long system_page_size = 0;
#endif

    std::vector<std::thread> thread_pool;
    thread_pool.reserve(worker_count);

    for (unsigned int t = 0; t < worker_count; ++t) {
        thread_pool.emplace_back([t, &workers, &chunks, system_page_size]() {
            WorkerThread& worker = workers[t];
            const char* cursor = chunks[t].begin;
            const char* const end = chunks[t].end;

            while (cursor < end) {
                // The timestamp prefix is a fixed 27-byte field in the official
                // request-log format. Ignore a final incomplete fragment safely.
                if (static_cast<size_t>(end - cursor) < 27) break;

                const int hour_id = parse_hour_id(cursor);
                const char* p = cursor + 27;
                if (p < end && *p == ' ') ++p;

                // Method.
                while (p < end && *p != ' ') ++p;
                if (p == end) break;
                ++p;

                // Endpoint.
                const char* endpoint_begin = p;
                while (p < end && *p != ' ') ++p;
                if (p == end) break;
                const std::string_view endpoint(endpoint_begin,
                                                static_cast<size_t>(p - endpoint_begin));
                ++p;

                // Three-digit HTTP status.
                if (static_cast<size_t>(end - p) < 3) break;
                const bool is_5xx = (*p == '5');
                p += 3;
                if (p < end && *p == ' ') ++p;

                // Latency in microseconds.
                uint32_t latency = 0;
                bool has_latency = false;
                while (p < end && *p >= '0' && *p <= '9') {
                    has_latency = true;
                    latency = latency * 10u + static_cast<uint32_t>(*p - '0');
                    ++p;
                }
                if (!has_latency) break;

                // Advance to the next record, tolerating CRLF and optional suffixes.
                while (p < end && *p != '\n') ++p;
                if (p < end) ++p;
                cursor = p;

                if (hour_id < 0 || hour_id >= static_cast<int>(kHours)) continue;

                auto it = worker.endpoint_ids.find(endpoint);
                uint32_t endpoint_id;
                if (it != worker.endpoint_ids.end()) {
                    endpoint_id = it->second;
                } else {
                    // The usual endpoint catalogue fits in the initial reserve.
                    // If it grows, rebuild string_view keys after vector relocation.
                    if (worker.endpoint_names.size() == worker.endpoint_names.capacity()) {
                        const size_t old_size = worker.endpoint_names.size();
                        worker.endpoint_names.reserve(
                            std::max<size_t>(256, worker.endpoint_names.capacity() * 2));
                        worker.endpoint_ids.clear();
                        for (uint32_t i = 0; i < old_size; ++i)
                            worker.endpoint_ids.emplace(worker.endpoint_names[i], i);
                    }
                    endpoint_id = static_cast<uint32_t>(worker.endpoint_names.size());
                    worker.ensure_endpoint_slot(endpoint_id);
                    worker.endpoint_names.emplace_back(endpoint);
                    worker.endpoint_ids.emplace(worker.endpoint_names.back(), endpoint_id);
                }

                LocalGroup& group = worker.get_group(endpoint_id,
                                                    static_cast<uint32_t>(hour_id));
                ++group.count;
                if (is_5xx) ++group.errors;
                group.latencies.push_back(latency);
            }

#if defined(__linux__)
            // Endpoint names have already been copied into worker-owned storage.
            // Drop this worker's fully consumed, page-aligned mapping interior so
            // file-backed input pages do not accumulate in the process RSS.
            if (system_page_size > 0 && chunks[t].end > chunks[t].begin) {
                const uintptr_t page = static_cast<uintptr_t>(system_page_size);
                const uintptr_t begin_address = reinterpret_cast<uintptr_t>(chunks[t].begin);
                const uintptr_t end_address = reinterpret_cast<uintptr_t>(chunks[t].end);
                const uintptr_t advise_begin = ((begin_address + page - 1) / page) * page;
                const uintptr_t advise_end = (end_address / page) * page;
                if (advise_end > advise_begin) {
                    (void)madvise(reinterpret_cast<void*>(advise_begin),
                                  static_cast<size_t>(advise_end - advise_begin),
                                  MADV_DONTNEED);
                }
            }
#endif
        });
    }

    for (auto& thread : thread_pool) thread.join();
    thread_pool.clear();
    input.close();

    // All endpoint names are already owned by worker-local vectors, so global
    // catalogue views remain valid after the large input mmap is closed.
    size_t endpoint_upper_bound = 0;
    for (const WorkerThread& worker : workers)
        endpoint_upper_bound += worker.endpoint_names.size();

    std::vector<std::string_view> global_endpoint_names;
    global_endpoint_names.reserve(endpoint_upper_bound);
    std::unordered_map<std::string_view, uint32_t> global_endpoint_ids;
    global_endpoint_ids.max_load_factor(0.70f);
    global_endpoint_ids.reserve(endpoint_upper_bound);

    for (const WorkerThread& worker : workers) {
        for (const std::string& owned_endpoint : worker.endpoint_names) {
            const std::string_view endpoint(owned_endpoint);
            if (global_endpoint_ids.find(endpoint) == global_endpoint_ids.end()) {
                const uint32_t id = static_cast<uint32_t>(global_endpoint_names.size());
                global_endpoint_names.push_back(endpoint);
                global_endpoint_ids.emplace(endpoint, id);
            }
        }
    }

    const size_t global_endpoint_count = global_endpoint_names.size();
    std::vector<std::vector<uint32_t>> global_to_local(worker_count);
    for (unsigned int t = 0; t < worker_count; ++t) {
        auto& remap = global_to_local[t];
        remap.assign(global_endpoint_count, kEmptySlot);
        const WorkerThread& worker = workers[t];
        for (uint32_t local_id = 0; local_id < worker.endpoint_names.size(); ++local_id) {
            const std::string_view endpoint(worker.endpoint_names[local_id]);
            const auto it = global_endpoint_ids.find(endpoint);
            if (it != global_endpoint_ids.end()) remap[it->second] = local_id;
        }
    }

    // Per-worker lookup maps are no longer needed. Keep the owned endpoint
    // strings alive because global_endpoint_names references them.
    for (WorkerThread& worker : workers)
        std::unordered_map<std::string_view, uint32_t>().swap(worker.endpoint_ids);
    std::unordered_map<std::string_view, uint32_t>().swap(global_endpoint_ids);

    std::vector<uint32_t> sorted_endpoint_ids(global_endpoint_count);
    std::iota(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), 0u);
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(),
              [&](uint32_t lhs, uint32_t rhs) {
                  return unsigned_bytewise_less(global_endpoint_names[lhs],
                                                global_endpoint_names[rhs]);
              });

    std::array<std::string, kHours> hour_strings;
    for (uint32_t h = 0; h < kHours; ++h) hour_strings[h] = make_hour_string(h);
    std::array<std::string, kHours> hour_outputs;

    auto reduce_hour_range = [&](uint32_t first_hour, uint32_t last_hour) {
        std::array<LocalGroup*, kMaxWorkers> parts{};

        for (uint32_t hour = first_hour; hour < last_hour; ++hour) {
            std::string& output = hour_outputs[hour];
            output.reserve(global_endpoint_count * 80);

            for (const uint32_t global_id : sorted_endpoint_ids) {
                size_t part_count = 0;
                size_t largest_part = 0;
                uint64_t total_count = 0;
                uint64_t total_errors = 0;

                for (unsigned int t = 0; t < worker_count; ++t) {
                    const uint32_t local_id = global_to_local[t][global_id];
                    if (local_id == kEmptySlot) continue;

                    WorkerThread& worker = workers[t];
                    const uint32_t group_id = worker.group_slot[
                        static_cast<size_t>(local_id) * kHours + hour];
                    if (group_id == kEmptySlot) continue;

                    LocalGroup& group = worker.groups[group_id];
                    parts[part_count] = &group;
                    if (part_count == 0 ||
                        group.latencies.size() > parts[largest_part]->latencies.size()) {
                        largest_part = part_count;
                    }
                    ++part_count;
                    total_count += group.count;
                    total_errors += group.errors;
                }

                if (part_count == 0) continue;

                // Merge destructively into the largest shard. Each (worker,hour,
                // endpoint) group is owned by exactly one reduction thread, so
                // these appends and releases are race-free.
                LocalGroup* primary = parts[largest_part];
                LatencyBuffer& latencies = primary->latencies;
                if (latencies.capacity() < total_count)
                    latencies.reserve(static_cast<uint32_t>(total_count));

                for (size_t i = 0; i < part_count; ++i) {
                    if (i == largest_part) continue;
                    LatencyBuffer& source = parts[i]->latencies;
                    latencies.append_from(source);
                    source.release();
                }

                uint32_t p50, p95, p99;
                compute_percentiles(latencies.data(), latencies.size(), p50, p95, p99);
                const uint64_t ppm = (total_errors * 1000000ULL) / total_count;

                output.append(hour_strings[hour]);
                output.push_back(' ');
                output.append(global_endpoint_names[global_id]);
                output.push_back(' ');
                append_uint(output, total_count);
                output.push_back(' ');
                append_uint(output, p50);
                output.push_back(' ');
                append_uint(output, p95);
                output.push_back(' ');
                append_uint(output, p99);
                output.push_back(' ');
                append_uint(output, total_errors);
                output.push_back(' ');
                append_uint(output, ppm);
                output.push_back('\n');

                // Percentiles are now materialized in the output; release the
                // merged shard immediately instead of retaining all latencies.
                latencies.release();
            }
        }
    };

    const unsigned int reduction_threads = std::min(worker_count, 8u);
    thread_pool.reserve(reduction_threads);
    for (unsigned int t = 0; t < reduction_threads; ++t) {
        const uint32_t first_hour = (kHours * t) / reduction_threads;
        const uint32_t last_hour = (kHours * (t + 1)) / reduction_threads;
        thread_pool.emplace_back(reduce_hour_range, first_hour, last_hour);
    }
    for (auto& thread : thread_pool) thread.join();

    // Write hourly buffers directly in canonical order. Avoid constructing a
    // second 10+ MB aggregate output string and its corresponding peak-memory copy.
    std::ofstream output_file(output_path, std::ios::binary | std::ios::trunc);
    if (!output_file) {
        std::cerr << "FAIL cannot write to " << output_path << '\n';
        return 1;
    }
    for (const std::string& hour_output : hour_outputs) {
        if (!hour_output.empty())
            output_file.write(hour_output.data(),
                              static_cast<std::streamsize>(hour_output.size()));
    }
    output_file.flush();
    if (!output_file) {
        std::cerr << "FAIL writing output " << output_path << '\n';
        return 1;
    }
    output_file.close();
    if (!output_file) {
        std::cerr << "FAIL closing output " << output_path << '\n';
        return 1;
    }

    if (profile) {
        const auto end_time = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(end_time - start_time).count();
        std::cerr << "wall_time: " << seconds << " s\n";
        std::cerr << "peak_rss: "
                  << (static_cast<double>(peak_rss_bytes()) / (1024.0 * 1024.0)) << " MB\n";
    }

    return 0;
}
