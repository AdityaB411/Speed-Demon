# Track A performance log

Team: **LightSpeed**  
Track: **A — Ingest**  
Language: **C++20**  
Current candidate: `memory-optimization` branch  
Implementation: `track-a/solution.cpp`

This log separates measurements from the official practice board from local engineering measurements. A local synthetic benchmark is not an official competition score.

## Previous official practice-board baseline

The earlier multithreaded / memory-mapped implementation produced the following visible Track A board results:

| Result | Run ID | Points | T | M | Gates |
|---|---|---:|---:|---:|---|
| Best baseline | `run-cb84769efa2d` | **54.478341** | 0.146 s | 112 MB | All 5 passed |
| Later baseline run | `run-4167da965fca` | 54.428351 | 0.146 s | 112 MB | All 5 passed |

The five visible gates were checker, build, correctness, timeout and sandbox. The best run's board-measured short commit was `59e7977`, matching `59e797734ef7a98a963428d8f7ba741ff4d3af58`.

**These are baseline results from the earlier implementation. They do not belong to the memory-optimized candidate.**

## Memory-optimized candidate — local synthetic benchmark

The new implementation uses a two-pass reader, compact group metadata and one flat latency array. It was tested locally against a synthetic dataset with 3,100,000 records and all 1,842 endpoint IDs. The synthetic dataset was generated for engineering load testing; it is not the official seed-1 dataset.

| Metric | Local candidate result |
|---|---:|
| Records | 3,100,000 |
| Input bytes | 216,246,144 |
| Endpoints | 1,842 |
| Output groups | 132,624 |
| Peak RSS (`/usr/bin/time -v`) | **15,520 KiB (about 15.2 MiB)** |
| Wall time | 0.70 s |
| Output SHA-256 | `d479a85ab537d3947534dfa377469f7692ca227eb6b32db819b314203abdb57b` |

The run used the key production Docker compile flags: `-O3 -std=c++20 -static-libstdc++ -static-libgcc`. The local wall time and memory figure are not official board measurements and must not be compared directly with the earlier board baseline.

## Correctness validation completed locally

| Test | Result |
|---|---|
| Handcrafted edge-case file | Exact match to independent Python oracle |
| Randomized 310,000-record file, 1,210 endpoints | Exact match to independent Python oracle |
| Synthetic 3,100,000-record file, 1,842 endpoints | Exact match to independent Python oracle |
| Empty input | Empty output file produced |
| AddressSanitizer + UndefinedBehaviorSanitizer edge run | Passed |
| Production compiler flags | Compiled without warnings |

The edge tests included out-of-order timestamps, day/month/hour boundaries, duplicate records, status 499 vs 500/599, zero and maximum uint32 latencies, and percentile group sizes 20, 100 and 101.

## Optimization rationale

The previous source allocated 72 × 2,048 `LocalGroup` objects in every parsing worker. With 12 workers and a 32-byte object, this reserved about 56.6 MB in group metadata alone, much of it representing empty latency vectors.

The new source:
- stores one 12-byte metadata record for each possible (hour, endpoint) group;
- counts records and errors in pass 1;
- prefix-sums group sizes to create exact slices;
- stores exactly one uint32 latency per request in a flat 12.4 MB array;
- uses pass 2 to fill those slices;
- computes percentiles in place;
- streams output instead of holding the entire output in memory.

The trade-off is reading the input twice. This removes per-worker table duplication, per-group vector slack, allocator overhead for many small vectors, and the full output string.

## Remaining validation gates

Before replacing the submission branch, the candidate still needs:

1. Run the repository checker with the official seed-1 full dataset and confirm the published SHA-256:
   `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3`.
2. Submit this branch to the official practice board and record the candidate's actual T, M, score, run ID and all gate outcomes.
3. Keep the previous `speed-demon` branch unchanged until the candidate has passed those checks.

The official board is a practice board. The final score is determined using the hidden holdout dataset after code freeze.
