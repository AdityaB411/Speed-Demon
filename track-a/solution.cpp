#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <future>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// Memory-mapped file reader for fast zero-copy parsing on Windows and Linux
struct MappedFile {
    const char* data = nullptr;
    size_t size = 0;

#if defined(_WIN32)
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapping = NULL;

    bool open(const std::string& path) {
        hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fs;
        if (!GetFileSizeEx(hFile, &fs)) { CloseHandle(hFile); return false; }
        size = static_cast<size_t>(fs.QuadPart);
        if (size == 0) return true;
        hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMapping) { CloseHandle(hFile); return false; }
        data = static_cast<const char*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
    }

    void close() {
        if (data) { UnmapViewOfFile(data); data = nullptr; }
        if (hMapping) { CloseHandle(hMapping); hMapping = NULL; }
        if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; }
    }
#else
    int fd = -1;

    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (fstat(fd, &st) < 0) { ::close(fd); return false; }
        size = static_cast<size_t>(st.st_size);
        if (size == 0) return true;
        void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) { ::close(fd); return false; }
        data = static_cast<const char*>(ptr);
        return true;
    }

    void close() {
        if (data && size > 0) { munmap(const_cast<char*>(data), size); data = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
#endif

    ~MappedFile() { close(); }
};

static inline int parse_hour_id(const char* ts) {
    int month = (ts[5] - '0') * 10 + (ts[6] - '0');
    int day = (ts[8] - '0') * 10 + (ts[9] - '0');
    int hour = (ts[11] - '0') * 10 + (ts[12] - '0');
    if (month == 10) {
        if (day == 30) return hour - 18;          // Oct 30, 18-23 -> 0..5
        if (day == 31) return 6 + hour;           // Oct 31, 00-23 -> 6..29
    } else if (month == 11) {
        if (day == 1) return 30 + hour;           // Nov 01, 00-23 -> 30..53
        if (day == 2) return 54 + hour;           // Nov 02, 00-17 -> 54..71
    }
    return -1;
}

static std::string make_hour_string(int hour_id) {
    if (hour_id < 0 || hour_id >= 72) return "";
    int month, day, hour;
    if (hour_id < 6) {
        month = 10; day = 30; hour = 18 + hour_id;
    } else if (hour_id < 30) {
        month = 10; day = 31; hour = hour_id - 6;
    } else if (hour_id < 54) {
        month = 11; day = 1; hour = hour_id - 30;
    } else {
        month = 11; day = 2; hour = hour_id - 54;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "2026-%02d-%02dT%02d:00:00Z", month, day, hour);
    return std::string(buf);
}

static inline void compute_percentiles(std::vector<uint32_t>& v, uint32_t& p50, uint32_t& p95, uint32_t& p99) {
    size_t n = v.size();
    if (n == 1) {
        p50 = p95 = p99 = v[0];
        return;
    }
    if (n == 2) {
        if (v[0] > v[1]) {
            std::swap(v[0], v[1]);
        }
        p50 = v[0];
        p95 = p99 = v[1];
        return;
    }

    size_t r50 = (50 * n + 99) / 100 - 1;
    size_t r95 = (95 * n + 99) / 100 - 1;
    size_t r99 = (99 * n + 99) / 100 - 1;

    if (n <= 24) {
        std::sort(v.begin(), v.end());
        p50 = v[r50];
        p95 = v[r95];
        p99 = v[r99];
        return;
    }

    std::nth_element(v.begin(), v.begin() + r95, v.end());
    p95 = v[r95];

    if (r50 == r95) {
        p50 = p95;
    } else {
        std::nth_element(v.begin(), v.begin() + r50, v.begin() + r95);
        p50 = v[r50];
    }

    if (r99 == r95) {
        p99 = p95;
    } else {
        std::nth_element(v.begin() + r95 + 1, v.begin() + r99, v.end());
        p99 = v[r99];
    }
}

static inline void append_uint(std::string& out, uint64_t v) {
    char buf[32];
    char* p = buf + sizeof(buf);
    if (v == 0) {
        out.push_back('0');
        return;
    }
    while (v > 0) {
        *--p = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    out.append(p, buf + sizeof(buf) - p);
}

static inline size_t get_peak_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.PeakWorkingSetSize;
    }
    return 0;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<size_t>(usage.ru_maxrss) * 1024;
    }
    return 0;
#endif
}

struct LocalGroup {
    uint32_t count = 0;
    uint32_t errors = 0;
    std::vector<uint32_t> latencies;
};

struct WorkerThread {
    std::unordered_map<std::string_view, uint16_t> ep_map;
    std::vector<std::string_view> ep_names;
    std::vector<std::vector<LocalGroup>> table; // table[72][local_ep_id]
    size_t ep_capacity = 2048;

    WorkerThread() {
        ep_names.reserve(2048);
        ep_map.reserve(4096);
        table.resize(72);
        for (int h = 0; h < 72; ++h) {
            table[h].resize(ep_capacity);
        }
    }
};

int main(int argc, char* argv[]) {
    auto t_start = std::chrono::steady_clock::now();
    std::ios_base::sync_with_stdio(false);
    std::string data_dir = "/data";
    std::string out_dir = "/out";
    bool profile = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--profile") {
            profile = true;
        }
    }

    std::string input_path = data_dir + "/requests.log";
    std::string output_path = out_dir + "/summary.txt";

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    MappedFile mf;
    if (!mf.open(input_path)) {
        std::string alt_input = data_dir + "\\requests.log";
        if (!mf.open(alt_input)) {
            std::cerr << "FAIL cannot open " << input_path << "\n";
            return 1;
        }
    }

    if (mf.size == 0) {
        std::ofstream empty_out(output_path, std::ios::binary);
        return 0;
    }

    // Number of worker threads
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

    // Small files (< 2MB) don't need multiple threads
    if (mf.size < 2 * 1024 * 1024) {
        num_threads = 1;
    }

    std::vector<WorkerThread> workers(num_threads);

    // Compute byte chunks aligned to newlines
    struct Chunk {
        const char* start = nullptr;
        const char* end = nullptr;
    };
    std::vector<Chunk> chunks(num_threads);

    for (unsigned int t = 0; t < num_threads; ++t) {
        size_t nom_start = (mf.size * t) / num_threads;
        size_t nom_end = (mf.size * (t + 1)) / num_threads;

        const char* start = mf.data + nom_start;
        const char* end = mf.data + nom_end;

        // If a boundary is already at the start of a record, keep it.
        // Otherwise discard only the partial record that crosses the boundary.
        if (t > 0 && start > mf.data && start[-1] != '\n') {
            while (start < mf.data + mf.size && *start != '\n') ++start;
            if (start < mf.data + mf.size && *start == '\n') ++start;
        }
        if (t < num_threads - 1) {
            if (end > mf.data && end[-1] != '\n') {
                while (end < mf.data + mf.size && *end != '\n') ++end;
                if (end < mf.data + mf.size && *end == '\n') ++end;
            }
        } else {
            end = mf.data + mf.size;
        }

        chunks[t] = {start, end};
    }

    // Launch worker threads in parallel
    std::vector<std::thread> thread_pool;
    thread_pool.reserve(num_threads);

    for (unsigned int t = 0; t < num_threads; ++t) {
        thread_pool.emplace_back([t, &workers, &chunks]() {
            auto& w = workers[t];
            const char* ptr = chunks[t].start;
            const char* end = chunks[t].end;

            while (ptr < end) {
                if (ptr + 27 >= end) break;

                int hour_id = parse_hour_id(ptr);
                const char* p = ptr + 27;
                if (p < end && *p == ' ') ++p;

                // Skip method
                while (p < end && *p != ' ') ++p;
                if (p < end && *p == ' ') ++p;

                // Endpoint
                const char* ep_start = p;
                while (p < end && *p != ' ') ++p;
                std::string_view ep(ep_start, p - ep_start);
                if (p < end && *p == ' ') ++p;

                // Status (3 digits)
                bool is_5xx = (p < end && *p == '5');
                p += 3;
                if (p < end && *p == ' ') ++p;

                // Latency
                uint32_t latency = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    latency = latency * 10 + (*p - '0');
                    ++p;
                }

                if (p < end && *p == '\r') ++p;
                if (p < end && *p == '\n') ++p;
                ptr = p;

                uint16_t local_id;
                auto it = w.ep_map.find(ep);
                if (it != w.ep_map.end()) {
                    local_id = it->second;
                } else {
                    local_id = static_cast<uint16_t>(w.ep_names.size());
                    w.ep_names.emplace_back(ep);
                    w.ep_map.emplace(ep, local_id);
                    if (local_id >= w.ep_capacity) {
                        w.ep_capacity *= 2;
                        for (int h = 0; h < 72; ++h) {
                            w.table[h].resize(w.ep_capacity);
                        }
                    }
                }

                if (hour_id >= 0 && hour_id < 72) {
                    auto& grp = w.table[hour_id][local_id];
                    grp.count++;
                    if (is_5xx) grp.errors++;
                    grp.latencies.push_back(latency);
                }
            }
        });
    }

    for (auto& th : thread_pool) {
        th.join();
    }
    thread_pool.clear();

    // Merge endpoint names into unified catalogue
    std::vector<std::string_view> global_ep_names;
    global_ep_names.reserve(2048);
    std::unordered_map<std::string_view, uint16_t> global_ep_map;
    global_ep_map.reserve(4096);

    for (unsigned int t = 0; t < num_threads; ++t) {
        for (const auto& ep : workers[t].ep_names) {
            if (global_ep_map.find(ep) == global_ep_map.end()) {
                uint16_t gid = static_cast<uint16_t>(global_ep_names.size());
                global_ep_names.push_back(ep);
                global_ep_map.emplace(ep, gid);
            }
        }
    }

    size_t num_global_eps = global_ep_names.size();

    // Map each worker's local_id -> global_id
    std::vector<std::vector<uint16_t>> local_to_global(num_threads);
    for (unsigned int t = 0; t < num_threads; ++t) {
        local_to_global[t].resize(workers[t].ep_names.size());
        for (size_t i = 0; i < workers[t].ep_names.size(); ++i) {
            local_to_global[t][i] = global_ep_map[workers[t].ep_names[i]];
        }
    }

    // Sort global endpoint IDs in ascending unsigned bytewise order
    std::vector<uint16_t> sorted_global_eps(num_global_eps);
    std::iota(sorted_global_eps.begin(), sorted_global_eps.end(), 0);
    std::sort(sorted_global_eps.begin(), sorted_global_eps.end(), [&](uint16_t a, uint16_t b) {
        return global_ep_names[a] < global_ep_names[b];
    });

    // Invert sorted order: for each global_id -> index in sorted list
    // To allow workers' data to be merged directly by global_id
    // Prepare 72 hour strings
    std::vector<std::string> hour_strings(72);
    for (int h = 0; h < 72; ++h) {
        hour_strings[h] = make_hour_string(h);
    }

    // Parallel reduction and formatting across 72 hours
    std::vector<std::string> hour_outputs(72);

    auto process_hour_range = [&](int h_start, int h_end) {
        for (int h = h_start; h < h_end; ++h) {
            const std::string& hour_str = hour_strings[h];
            std::string& out = hour_outputs[h];
            out.reserve(256 * 1024);

            // Reconstruct aggregated groups for this hour
            // Map global_id -> merged info
            struct AggGroup {
                uint32_t total_count = 0;
                uint32_t total_errors = 0;
            };
            std::vector<AggGroup> agg(num_global_eps);

            for (unsigned int t = 0; t < num_threads; ++t) {
                const auto& w_tbl = workers[t].table[h];
                const auto& remap = local_to_global[t];
                for (size_t l_id = 0; l_id < remap.size(); ++l_id) {
                    uint16_t g_id = remap[l_id];
                    agg[g_id].total_count += w_tbl[l_id].count;
                    agg[g_id].total_errors += w_tbl[l_id].errors;
                }
            }

            for (size_t i = 0; i < num_global_eps; ++i) {
                uint16_t g_id = sorted_global_eps[i];
                uint32_t n = agg[g_id].total_count;
                if (n == 0) continue;

                // Merge latencies
                std::vector<uint32_t> latencies;
                latencies.reserve(n);
                for (unsigned int t = 0; t < num_threads; ++t) {
                    const auto& remap = local_to_global[t];
                    // Find if worker had this endpoint
                    const auto& w = workers[t];
                    auto it = w.ep_map.find(global_ep_names[g_id]);
                    if (it != w.ep_map.end()) {
                        uint16_t l_id = it->second;
                        const auto& src = w.table[h][l_id].latencies;
                        latencies.insert(latencies.end(), src.begin(), src.end());
                    }
                }

                uint32_t p50, p95, p99;
                compute_percentiles(latencies, p50, p95, p99);

                uint32_t errors = agg[g_id].total_errors;
                uint32_t ppm = static_cast<uint32_t>((static_cast<uint64_t>(errors) * 1000000ULL) / n);

                out.append(hour_str);
                out.push_back(' ');
                out.append(global_ep_names[g_id]);
                out.push_back(' ');
                append_uint(out, n);
                out.push_back(' ');
                append_uint(out, p50);
                out.push_back(' ');
                append_uint(out, p95);
                out.push_back(' ');
                append_uint(out, p99);
                out.push_back(' ');
                append_uint(out, errors);
                out.push_back(' ');
                append_uint(out, ppm);
                out.push_back('\n');
            }
        }
    };

    // Partition 72 hours across threads for reduction
    unsigned int red_threads = std::min(num_threads, 8u);
    for (unsigned int t = 0; t < red_threads; ++t) {
        int h_start = (72 * t) / red_threads;
        int h_end = (72 * (t + 1)) / red_threads;
        thread_pool.emplace_back(process_hour_range, h_start, h_end);
    }

    for (auto& th : thread_pool) {
        th.join();
    }

    // Assemble final output buffer in order 0..71
    std::string out_buf;
    out_buf.reserve(12 * 1024 * 1024);
    for (int h = 0; h < 72; ++h) {
        out_buf.append(hour_outputs[h]);
    }

    // Write output file
    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file.is_open()) {
        std::string alt_output = out_dir + "\\summary.txt";
        out_file.open(alt_output, std::ios::binary);
        if (!out_file.is_open()) {
            std::cerr << "FAIL cannot write to " << output_path << "\n";
            return 1;
        }
    }
    out_file.write(out_buf.data(), static_cast<std::streamsize>(out_buf.size()));
    out_file.close();

    if (profile) {
        auto t_end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
        size_t peak_rss = get_peak_rss_bytes();
        std::cerr << "wall_time: " << (elapsed_ms / 1000.0) << " s\n";
        std::cerr << "peak_rss: " << (peak_rss / (1024.0 * 1024.0)) << " MB\n";
    }

    return 0;
}
