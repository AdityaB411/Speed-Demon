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

## Memory-optimized candidate — local benchmarks and optimization audit

The implementation uses a two-pass reader, compact SoA group tables, an L2-cache-resident open-addressing endpoint catalogue with fused hash calculation, and one flat latency array. It was tested against both the official seed-1 full dataset (3,100,000 records, 238,836,234 bytes) and sample dataset (310,000 records, 23,870,290 bytes).

| Metric | Memory-optimized baseline | Final candidate (MEM-005) | Line-reduced candidate (OPT-001) |
|---|---:|---:|---:|
| Records | 3,100,000 | 3,100,000 | 3,100,000 |
| Input bytes | 238,836,234 | 238,836,234 | 238,836,234 |
| Endpoints | 1,842 | 1,842 | 1,842 |
| Output groups | 126,908 | 126,908 | 126,908 |
| Formatted Code Lines (`scc`) | 567 | 567 | **393** (-30.7%) |
| Raw Source Lines | 554 | 554 | **356** (-35.7%) |
| Peak RSS | **16.46 MiB (16,856 KiB)** | **16.28 MiB (16,672 KiB)** | **16.28 MiB (16,672 KiB)** |
| Internal execution time | ~631 ms | ~490 ms | **~469 ms** |
| Pass 1 time | 269.8 ms | 202.0 ms | 199.5 ms |
| Pass 2 time | 297.0 ms | 231.6 ms | 220.3 ms |
| Output time | 64.2 ms | 57.4 ms | 49.9 ms |
| Full `check.py` runtime | 6.54 s | 3.66 s | **3.93 s** |
| Official seed-1 SHA-256 | `495ca096...` | `495ca096...` | `495ca096...` |

The runs used the key production Docker compile flags: `-O3 -std=c++20 -static-libstdc++ -static-libgcc`.

## Code optimization and source-line reduction audit (OPT-001)

The objective was to reduce canonical formatted source lines (`clang-format --style=LLVM` + `scc`) from ~500 to $\le 400$ lines while maintaining exact byte-for-byte correctness and competitive runtime.

### Key Refactorings:
1. **Parser Unification via Compile-Time Dispatch (`parse_line<ParseLatency>`)**:
   Unified `parse_count_record` and `parse_latency_record` into a single templated scanner with `if constexpr (!ParseLatency)`. Both passes share identical timestamp parsing, method skipping, and fused FNV-1a endpoint hashing without duplicating parsing code.
2. **Endpoint Catalogue Probing Consolidation**:
   Factored common open-addressing linear probing logic into `lookup_slot()`, eliminating duplicate probing loops across `find_or_add_endpoint` and `find_endpoint`.
3. **Streamlined `OutputWriter`**:
   Introduced `put_s()` and `put_u()` helpers with per-line 256-byte boundary checks, replacing separate character/string/uint bounds checking and reducing output formatting overhead.
4. **Hour Timestamp Generation**:
   Replaced verbose helper with a compact structured binding lookup array `kDays`, generating all 72 ISO-8601 timestamps without heap allocations.
5. **Removal of Unused Headers**:
   Eliminated `<fstream>`, `<unordered_map>`, `<iostream>`, `<new>`, `<numeric>`, and `<stdexcept>`. Standard error reporting uses `<cstdio>` (`std::fprintf`) with zero stream overhead.
6. **Percentile Selection Optimization**:
   Simplified small-group sorting ($n \le 3$) and eliminated redundant branches in 3-partition `std::nth_element`.

## Correctness validation completed locally

| Test | Result |
|---|---|
| Official seed-1 full dataset (3,100,000 records) | Exact match to published SHA-256: `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` |
| Official seed-1 sample dataset (310,000 records) | Exact match to published SHA-256: `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3` |
| Official spec suite `test_a_ingest.py` (15 unit tests) | 15 / 15 Passed (`OK`) |
| Edge-case suite (16 comprehensive tests: rollovers, 499 vs 500/599, n=1..101, boundaries, /..125b) | Byte-for-byte exact match to independent Python oracle |
| Randomized multi-seed differential tests (seeds 1, 2, 3, 42) | Byte-for-byte exact match to independent Python oracle |
| AddressSanitizer + UndefinedBehaviorSanitizer on full 3.1M dataset | Passed (0 errors, 0 warnings, 0 leaks) |
| Production compiler flags (`-Wall -Wextra -Werror -O3`) | Compiled cleanly without warnings |
| Canonical Line Count (`clang-format --style=LLVM` + `scc`) | **393 lines** (Target: $\le 400$) |

## Remaining validation gates

Before replacing the submission branch, the candidate still needs:

1. Confirm all untracked test and profiling artifacts remain local and uncommitted.
2. Submit this branch to the official practice board and record the candidate's actual T, M, score, run ID and all gate outcomes.
3. Keep the previous `speed-demon` branch unchanged until the candidate has passed those checks.
