# Track A — Ingest

**Team:** LightSpeed  
**Language:** C++20  
**Submission branch:** `speed-demon`  
**Production source:** [`solution.cpp`](solution.cpp)  
**Performance record:** [`PERF.md`](PERF.md)

## 1. Problem and output contract

Track A processes HTTP request records from `/data/requests.log` (3.1 million records; approximately 238.8 MB in the public full seed-1 dataset) and writes exactly one file: `/out/summary.txt`.

For each unique `(UTC hour, endpoint)` group, the program emits:

1. Total request count, (N).
2. Exact nearest-rank p50, p95 and p99 latency in microseconds.
3. Number of HTTP 5xx responses (status codes 500–599).
4. Error rate in parts per million: (lfloor errors \times 1,000,000 / N \rfloor).

Rows are ordered by UTC hour ascending and then endpoint in bytewise lexicographical order. The output must match the reference byte-for-byte.

The represented 72-hour window is 2026-10-30 18:00 UTC through 2026-11-02 17:00 UTC.

## 2. Production architecture

```text
/data/requests.log
        |
  read-only mmap
        |
 newline-aligned byte ranges
        |
  up to 12 ingest workers
  (worker-local endpoint maps,
   counts, 5xx counts, latencies)
        |
   join and catalogue merge
        |
 deterministic global endpoint order
        |
  up to 8 hourly reduction workers
  (exact nearest-rank percentiles)
        |
 chronological output assembly
        |
 /out/summary.txt
```

### Input and partitioning

- On Linux, `MappedFile` opens the input read-only and maps it with `mmap(MAP_SHARED)`. The mapping avoids copying the entire file into a separate application-owned input buffer.
- Large files are divided into byte ranges for up to 12 workers; small files use one worker.
- Interior range boundaries are advanced to the next newline so a record is not split between workers.
- Each worker owns its endpoint catalogue and aggregation table during ingestion. There are no shared aggregation updates or per-record locks in the hot parse loop.
- Endpoint views refer to the still-live input mapping; the mapping remains open until all reduction and output work is complete.

### Aggregation and exact statistics

- Worker-local endpoint catalogues are reconciled into a global catalogue. Output ordering is explicitly sorted and therefore does not depend on hash-table iteration order.
- The program merges per-worker counts and error counts for each active `(hour, endpoint)` group.
- Nearest-rank percentile index (zero-based) is:
  [
  index(p,n)=\left\lceil\frac{pn}{100}\right\rceil-1
  =\frac{pn+99}{100}-1
  ]
  where integer division is used.
- Groups of at most 24 samples are sorted directly. Larger groups use `std::nth_element` to select the exact p95, p50 and p99 order statistics without fully sorting every latency vector.
- No approximate quantiles, cached answers or precomputed output files are used.

### Output

- The 72 hours are processed in parallel, but each hour writes to its own buffer.
- Buffers are concatenated in hour order, preserving deterministic output ordering.
- Integer output uses a dedicated decimal append routine.
- The program writes only `summary.txt` beneath `/out`.

## 3. Build, runtime and dependencies

The Dockerfile uses digest-pinned GCC 15 and Debian 13 slim images and compiles the program during an offline build:

```dockerfile
RUN g++ -O3 -std=c++20 -static-libstdc++ -static-libgcc solution.cpp -o /solution
```

Build from the repository root:

```bash
docker build --network=none -t lightspeed-track-a ./track-a
```

The runtime has no third-party dependencies. It uses C++20 standard-library facilities and Linux system calls for file mapping. Metadata declares `dependency_count = 0` (dependency factor (D=1)).

The official runner supplies a read-only input mount at `/data`, writable output at `/out`, no network, an unprivileged UID, a read-only root filesystem, a 1 GB `/tmp`, and the published CPU/memory/process limits. The image is built offline and is below the 2 GB image cap.

## 4. Validation evidence

### Public seed-1 output parity

| Dataset | Records | Expected output SHA-256 | Result |
|---|---:|---|:---:|
| Tiny | 1,000 | `c24fa507c963eaeac9224fd8435019b4f4e8c002ece7e32f316681a3957eb92f` | **PASS** |
| Sample | 310,000 | `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3` | **PASS** |
| Full | 3,100,000 | `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` | **PASS** |

### Test suites recorded for the final production candidate

| Suite | Result |
|---|---:|
| Official Track A specification tests | **15/15 passed** |
| Synthetic edge-case suite | **6/6 passed** |
| Differential harness | **4/4 passed** |
| String-view lifetime/growth stress | **5,000/5,000 passed** |
| AddressSanitizer + UndefinedBehaviorSanitizer | **No reported errors** |

### Official board result

The live practice board scored the current production commit on the public seed-1 full dataset:

| Field | Value |
|---|---:|
| Run ID | `run-a03726c4d127` |
| Scored commit | `19053765eec20d904bf82a81d1cef89813686ea1` |
| Result | **Scored / passed** |
| Speed points | **54.764396** |
| Efficiency coefficient (K) | **12.453400** |
| Execution time (T) | **0.142494 s** |
| Peak memory (M) | **111,947,776 bytes** |
| Source size (S) | **499 lines** |
| Dependency count (D) | **1** |
| Image size | **119,869,264 bytes** |
| Build time | **8.327 s** |

All five official gates passed: **check, build, correctness, timeout and sandbox**.

**Important:** the practice-board run uses the public seed-1 dataset. Final competition scoring uses a hidden holdout seed; the published practice result does not guarantee a particular holdout score.

## 5. Known trade-offs and next improvements

- The memory-mapped implementation is selected for measured throughput. Its resident file pages contribute to process/cgroup memory, so it is not the lowest-memory candidate.
- The memory-first two-pass candidate reduced the measured memory footprint but regressed runtime substantially; it was not promoted.
- The current exact percentile approach deliberately retains all group latencies until reduction. A future memory-oriented design could investigate compact contiguous arenas, but only with full byte-parity and official-environment measurements.
- Further tuning should use the official holdout-safe workload contract, preserve deterministic ordering, and compare candidates one at a time. No unmeasured optimization is claimed in this submission.

See [`PERF.md`](PERF.md) for the experiment record and candidate decisions.
