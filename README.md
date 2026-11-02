# LightSpeed — Speed Demon 2026, Track A

This repository contains Team LightSpeed's official competition entry: **Track A — Ingest**, implemented in C++20. Tracks B, C, and D are intentionally out of scope.

The solution aggregates 3.1 million HTTP request records from `/data/requests.log` (238.8 MB), groups them by UTC hour and exact endpoint, computes request counts, nearest-rank p50/p95/p99 latencies, and 5xx error rates, and writes the byte-exact result to `/out/summary.txt`.

---

## Architecture & Implementation

The production submission implements a high-performance, multithreaded architecture:

1. **Zero-Copy Memory-Mapped Ingest**: Uses `mmap` (`MAP_SHARED`) to access `/data/requests.log` with zero user-kernel memory copies.
2. **Newline-Aligned Chunk Partitioning**: Partitions input bytes across worker threads (`std::min(hardware_concurrency, 12u)`), advancing partition boundaries to the next newline (`\n`) for disjoint, full-coverage parallel processing.
3. **Lock-Free Thread-Local Aggregation**: Each worker thread maintains an independent endpoint catalog and local 2D table `table[72][2048]` of request counts, 5xx error counts, and latency arrays. Zero cross-thread synchronization during parsing.
4. **Unified Catalog Remapping**: Merges distinct endpoint names across threads, sorts endpoints in unsigned bytewise order, and builds $O(1)$ remapping arrays.
5. **Parallel Hourly Reduction**: Partitions the 72 chronological hours across 8 threads to compute exact nearest-rank percentiles in place via `std::nth_element` (or direct sort for $n \le 24$).
6. **Concurrent In-Memory Output Formatting**: Generates formatted output per hour in parallel into thread-local buffers, concatenated and streamed to `/out/summary.txt` in a single write.

---

## Build and Run

The Dockerfile compiles completely offline using allowlisted, digest-pinned GCC 15 and Debian 13 slim images:

```bash
docker build --network=none -t lightspeed-track-a ./track-a
```

To run under the official submission sandbox:

```bash
docker run --rm \
  --network=none \
  --read-only \
  --tmpfs /tmp:rw,size=1g,exec \
  --memory=8g \
  --pids-limit=512 \
  --user 65534:65534 \
  -v /path/to/data:/data:ro \
  -v /path/to/out:/out:rw \
  lightspeed-track-a
```

To run the official sandbox checker:

```bash
python3 check.py a --data /path/to/data --expected 495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3
```

---

## Submission Details

- **Team**: `LightSpeed`
- **Team Slug**: `lightspeed-4a5b5a`
- **Track**: `A — Ingest` (Only)
- **Language**: `C++20`
- **Runtime Dependencies**: Zero external dependencies ($D = 1$)
- **License**: MIT
- **Submission Branch**: `speed-demon`
- **Verified Official Board Result**: **54.478341 points** (Run ID `run-cb84769efa2d`, $T = 0.146\text{ s}$, $M = 112\text{ MB}$, all 5 gates passed).
