# Track A: Ingest

Team: **LightSpeed**  
Language: **C++20**  
Submission Branch: `speed-demon`  

---

## 1. Problem Overview

Track A requires processing 3.1 million HTTP request log records from `/data/requests.log` (approximately 238.8 MB) and writing an aggregated summary report to `/out/summary.txt`.

For each unique combination of `(hour, endpoint)`:
1. Count the total number of requests ($N$).
2. Calculate exact nearest-rank percentiles: **p50**, **p95**, and **p99** latency in microseconds.
3. Count the number of HTTP 5xx error responses (`500` through `599`).
4. Calculate the error rate in parts-per-million: $\lfloor (\text{errors} \times 1,000,000) / N \rfloor$.
5. Output records sorted primarily by chronological hour ascending (`2026-10-30T18:00:00Z` to `2026-11-02T17:00:00Z`) and secondarily by endpoint name ascending in unsigned bytewise order.

---

## 2. Architecture & Design

The production solution uses a high-performance multithreaded architecture designed to maximize execution speed on modern multicore hardware:

```
[ requests.log (238.8 MB) ]
            |
    mmap (MAP_SHARED)
            |
            v
[ Zero-Copy Byte Slicing ] ---> Partitioned into N newline-aligned chunks
            |
            +---> Worker Thread 0 (Thread-local ep_map & table[72][2048])
            +---> Worker Thread 1 (Thread-local ep_map & table[72][2048])
            +---> ...
            +---> Worker Thread 11 (Thread-local ep_map & table[72][2048])
            |
      (All workers join)
            |
            v
[ Main Thread: Unified Catalog Merge & Lexicographical Sort ]
            |
            v
[ Parallel Hourly Reduction (8 Threads over 72 Hours) ]
            |
            +---> In-place exact percentiles via std::nth_element
            +---> Direct thread-local output formatting
            |
      (All threads join)
            |
            v
[ Write /out/summary.txt (10.8 MB) ]
```

### Key Architectural Components

1. **Zero-Copy Memory-Mapped Input (`MappedFile`)**:
   - Maps `requests.log` directly into process address space using `mmap` (`MAP_SHARED` on Linux, `MapViewOfFile` on Windows).
   - Eliminates intermediate buffer copies and user-kernel buffer transitions.

2. **Newline-Aligned Chunk Partitioning**:
   - The file size is divided evenly among worker threads (`std::min(hardware_concurrency, 12u)`).
   - Chunk boundaries are adjusted forward to the nearest newline character (`\n`), guaranteeing that every line is processed by exactly one worker thread without dropped or duplicated records.

3. **Lock-Free Thread-Local Ingest**:
   - Each worker thread maintains its own open-addressing / `unordered_map` endpoint catalogue and a 2D table `table[72][2048]` of `LocalGroup` structures.
   - Zero locks, zero atomic instructions, and zero thread contention during the entire input scanning phase.

4. **In-Memory Catalog Unification**:
   - After parsing completes, the main thread collects all distinct endpoint names across workers into a global catalog and sorts them in ascending bytewise order.
   - Endpoint remapping vectors (`local_to_global[t]`) allow instant index translation.

5. **Parallel Hourly Reduction**:
   - The 72 chronological hours are partitioned across 8 worker threads.
   - For each active group, latencies are merged into a contiguous vector and evaluated for exact nearest-rank percentiles.

6. **Exact Nearest-Rank Percentiles**:
   - Formula: rank $r = \lceil p \times n / 100 \rceil$ (1-based), translated to 0-based index $r = (p \times n + 99) / 100 - 1$.
   - Small groups ($n \le 24$) are sorted directly with `std::sort`.
   - Larger groups use `std::nth_element` with multi-pivot partitioning (p95 first, then p50 within $[0, p95)$, then p99 within $(p95, n)$). No approximations or histogram binning are used.

7. **Fast Output Formatting**:
   - Each reduction thread formats its assigned hours directly into memory buffers with specialized integer formatting (`append_uint`).
   - The main thread concatenates formatted buffers and writes the complete `summary.txt` in a single streaming write.

---

## 3. Dependencies & Execution Contract

- **Runtime Dependencies**: Zero external or third-party dependencies ($D = 1$).
- **Toolchain**: C++20 standard library, compiled with `-O3 -std=c++20 -static-libstdc++ -static-libgcc`.
- **Base Images**: Digested-pinned allowlisted base images:
  - Build: `gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce`
  - Runtime: `debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad`
- **Offline Compliance**: Container builds and executes cleanly with `--network=none`.
- **Sandbox Compliance**: Runs as unprivileged user `65534:65534` with read-only root and input filesystem.

---

## 4. Verification & Validation Summary

| Test Suite | Scope | Result |
|---|---|:---:|
| **Official Full Dataset** | Seed-1 3,100,000 records | **PASS** (SHA-256: `495ca09682b9...`) |
| **Official Sample Dataset** | Seed-1 310,000 records | **PASS** (SHA-256: `ca6311bb...`) |
| **Official Tiny Dataset** | Seed-1 1,000 records | **PASS** (SHA-256: `c24fa50...`) |
| **Official Specification Suite** | 15 unit tests in `test_a_ingest.py` | **15 / 15 Passed** |
| **Benchlab Edge-Case Suite** | 6 boundary & edge-case test cases | **6 / 6 Passed** |
| **Sanitizers (ASan + UBSan)** | Address and Undefined Behavior sanitization | **0 errors, 0 leaks** |
| **Official Sandbox Gate (`check.py`)** | Full container build and execution check | **PASS** (Image: 120 MB) |
