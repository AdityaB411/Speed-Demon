# Track A performance log

Team: **LightSpeed**  
Track: **A — Ingest**  
Language: **C++20**  
Repository: `AdityaB411/Speed-Demon`  
Submission Branch: `speed-demon`  
Final Candidate: **Historical Champion Restoration (HYPER-001 / Commit `59e797734ef7a98a963428d8f7ba741ff4d3af58`)**

---

## 1. Official scoring reference

The published Track A reference anchors on the official scoring machine (Intel Core i5-13500) are:
- $T_{ref} = 32.047276\text{ s}$
- $M_{ref} = 227983360\text{ bytes}$ (228 MB)
- $S_{ref} = 56\text{ lines}$
- $D_{ref} = 1$

The official scoring formula is:
$$K = \left(\frac{T_{ref}}{T}\right)^{0.50} \times \min\left(\frac{M_{ref}}{M}, 50\right)^{0.20} \times \min\left(\frac{S_{ref}}{S}, 1\right)^{0.15} \times \left(\frac{D_{ref}}{D}\right)^{0.15}$$
$$\text{Speed Points} = 50 \times \log_{10}(K)$$

Because runtime $T$ carries an exponent of **0.50** (compared to **0.20** for memory and **0.15** for source code lines), execution speed is the primary driver of the score:
- A 50% reduction in $T$ yields $+7.526$ points.
- A 50% reduction in $M$ yields $+3.010$ points.
- A 50% reduction in $S$ yields $+2.258$ points.

---

## 2. Official board measurement history

The table below records actual, verified measurements from the official Speed Demon competition board:

| Candidate | Commit SHA | Board Run ID | T (s) | M (MB) | M (Bytes) | S (Lines) | D | K | Points | Official Gates | Status |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| **Historical Champion (PAR-001)** | `59e7977` | `run-cb84769efa2d` | **0.146371** | **112.0** | 111,808,512 | 499 | 1 | **12.290423** | **54.478341** | All 5 Passed | **Team Best Score** |
| Historical Baseline 2 | `59e7977` | `run-4167da965fca` | 0.146220 | 112.0 | 111,808,512 | 499 | 1 | 12.262180 | 54.428351 | All 5 Passed | Confirmatory run |
| **COMBO-002 Submission** | `82fae49` | `run-08ca08c492bb` | 0.608770 | 25.6 | 25,624,576 | 406 | 1 | 8.345791 | **46.073377** | All 5 Passed | Regression (-8.40 pts) |

### Root-Cause Analysis of the COMBO-002 Regression
The COMBO-002 candidate combined a two-pass reader, flat latency array, fused CRC32C hashing, and a 256 KiB buffer. While it successfully reduced peak memory from 111.8 MB to 25.6 MB (+8.3 points gain on $M$), its single-threaded two-pass sequential I/O slowed runtime from 0.146 s to 0.608 s (-17.5 points penalty on $T$). Net impact: $-8.40$ points.

---

## 3. Phase 4 Experimental Challenger Evaluation

During Phase 4, three independent architectures were developed and benchmarked in isolated worktrees under official sandbox parameters (`--memory=8g --cpuset-cpus=0-7 --user 65534:65534`):

| Candidate | Architecture / Core Changes | Local Wall Time (Median / Mean) | Local Peak RSS | Local Cgroup Peak | Code Lines ($S$) | Projected Board Score | Decision |
|---|---|---:|---:|---:|---:|---:|---|
| **HYPER-001** | Historical champion restoration (`59e7977`): zero-copy `mmap`, 12-thread parallel parse, parallel 8-thread reduction | **0.9625 s / 0.9614 s** | 306.1 MB | 307.4 MB | 425 code / 499 board | **54.478341 pts** (Known) | **SELECTED FOR SUBMISSION** |
| **PAR-002** | 2-pass parallel scatter with `mmap`: flat arena, prefix sums, lock-free thread writes | 0.9097 s / 0.9744 s | 266.7 MB | 269.1 MB | 655 code | ~53.60 pts | Rejected: Mean time slower than champion; +31% code line penalty |
| **HYBRID-001** | Parallel buffered reader (`pread` 256 KiB), no full-file `mmap`, flat arena | 1.5325 s / 1.5447 s | 46.8 MB | 46.5 MB | 541 code | ~32.32 pts (local) | Baseline research candidate |
| **HYBRID-002** | HYBRID-001 + hardware CRC32C, $O(1)$ catalog indexing, compiler LTO | 1.3975 s / 1.4205 s | 57.9 MB | 57.5 MB | 575 code | ~32.58 pts (local) | Promising memory-first hybrid; slower than champion |

### Why PAR-002 Was Rejected
Under the competition evaluation protocol:
1. **No Speed Advantage**: PAR-002's mean local wall time (0.9744 s) was slower than HYPER-001 (0.9614 s), with higher variance (StdDev 0.1328 s vs 0.0484 s).
2. **Code Line Penalty**: Formatted code lines expanded from 499 to 655 lines, reducing the $(S_{ref}/S)^{0.15}$ factor from 0.7202 to 0.6917.
3. **Score Deficit**: Even assuming identical execution speed on the official board, PAR-002 projects to $\sim 53.60$ points, strictly lower than the historical champion's verified 54.478341 points.

---

## 4. Final Submission Selection & Verification

To maximize Team LightSpeed's score and guarantee a robust, verified submission before the code freeze deadline, **the Historical Champion (`59e7977`) is restored as the production implementation on `speed-demon`**.

### Verification Summary
- **Official Seed-1 Full Dataset (3,100,000 records)**: Output SHA-256 `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` (**PASS**)
- **Official Seed-1 Sample Dataset (310,000 records)**: Output SHA-256 `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3` (**PASS**)
- **Official Tiny Dataset (1,000 records)**: Output SHA-256 `c24fa507c963eaeac9224fd8435019b4f4e8c002ece7e32f316681a3957eb92f` (**PASS**)
- **Official Spec Unit Tests (`test_a_ingest.py`)**: 15 / 15 Passed (`OK`, 9.52 s)
- **Benchlab Edge-Case Suite (`run_edge_cases.py`)**: 6 / 6 Passed
- **AddressSanitizer (ASan) & UBSan**: Clean (0 errors, 0 leaks, 0 warnings)
- **Official Submission Sandbox Check (`check.py`)**: All 5 gates passed (`PASS`, 120 MB image)
