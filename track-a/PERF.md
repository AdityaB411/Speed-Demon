# Track A — Performance and optimization log

**Team:** LightSpeed  
**Track:** A — Ingest  
**Language:** C++20  
**Repository:** `AdityaB411/Speed-Demon`  
**Submission branch:** `speed-demon`  
**Final production candidate:** PAR-001 historical champion restoration  
**Scored release commit:** `19053765eec20d904bf82a81d1cef89813686ea1`

This log distinguishes official board measurements from local experiments. Local timings were useful for candidate comparison but are not interchangeable with the official scoring machine's measurement.

---

## 1. Scoring model and reference anchors

The published Track A reference anchors on the official scoring machine (Intel Core i5-13500) are:

- (T_{ref}=32.047276\text{ s})
- (M_{ref}=227,983,360\text{ bytes})
- (S_{ref}=56\text{ lines})
- (D_{ref}=1)

The published efficiency coefficient is:

[
K=\left(\frac{T_{ref}}{T}\right)^{0.50}
\times\min\left(\frac{M_{ref}}{M},50\right)^{0.20}
\times\min\left(\frac{S_{ref}}{S},1\right)^{0.15}
\times\left(\frac{D_{ref}}{D}\right)^{0.15}
]

The machine speed points are:

[
P=50\log_{10}(K)
]

Runtime has the largest exponent (0.50), followed by memory (0.20), source size (0.15) and dependency factor (0.15). The overall judging rubric also includes code quality, optimization technique and the optimization log; raw machine points are not the entire human evaluation.

---

## 2. Official board measurements

The following values are copied from the official live board's scored runs. The latest row is the result for the current release commit.

| Candidate | Commit | Board run | T (s) | M (bytes) | S | D | K | Points | Gates / status |
|---|---|---|---:|---:|---:|---:|---:|---:|---|
| **Final release (PAR-001)** | `1905376` | `run-a03726c4d127` | **0.142494** | **111,947,776** | **499** | 1 | **12.453400** | **54.764396** | **5/5 passed; current best** |
| Historical champion (PAR-001) | `59e7977` | `run-cb84769efa2d` | 0.146371 | 111,808,512 | 499 | 1 | 12.290423 | 54.478341 | 5/5 passed; previous best |
| Historical confirmation | `59e7977` | `run-4167da965fca` | 0.146220 | 111,808,512 | 499 | 1 | 12.262180 | 54.428351 | 5/5 passed |
| COMBO-002 | `82fae49` | `run-08ca08c492bb` | 0.608770 | 25,624,576 | 406 | 1 | 8.345791 | 46.073377 | 5/5 passed; rejected regression |

### Final release board evidence

- **Team:** LightSpeed (`lightspeed-4a5b5a`)
- **Run:** [`run-a03726c4d127`](https://speeddemon.dev/board/)
- **Scored commit:** [`19053765eec20d904bf82a81d1cef89813686ea1`](https://github.com/AdityaB411/Speed-Demon/commit/19053765eec20d904bf82a81d1cef89813686ea1)
- **Stage:** `scored`; `ok = true`
- **Gate breakdown:** check = pass; build = pass; correctness = pass; timeout = pass; sandbox = pass.
- **Build time:** 8.327 s.
- **Image size:** 119,869,264 bytes (approximately 119.87 MB).
- **Timeout limit reported by the board:** 96.142 s.

The board's current best and latest run both reference the final release commit. The measured execution time is the board's process timing, not the time printed by the local `check.py` wrapper (which includes container startup).

---

## 3. Optimization journey

### Baseline and first working implementation

The initial reference implementation was functionally correct but intentionally simple and slow. It performed the required parsing, grouping and statistics work in Python. The first C++ implementation moved the hot path into native code and established exact output parity before aggressive optimization.

The initial measured local full-dataset reference time was approximately 28.905 s. The early single-threaded C++ implementation reduced this to around 1 s on the development machine. These are historical local measurements and must not be compared directly with the official board's machine timing.

### PAR-001 — parallel, memory-mapped production path

Selected characteristics:

- Read-only memory mapping for input access, avoiding a second full-size application input buffer.
- Newline-aligned partitioning and parallel record parsing, capped at 12 ingest workers.
- Independent per-worker endpoint catalogs and aggregation state to avoid locks/atomics in the ingest hot path.
- A deterministic global endpoint catalog and local-to-global ID remapping after ingestion.
- Parallel hourly reduction across up to eight workers.
- Exact percentile selection: direct sorting for small groups and `std::nth_element` for larger groups.
- Per-hour output buffers assembled in chronological order.

This is the architecture restored for the final release. The final board run improved on the previous best public-board result while retaining the same source-size and dependency measurements.

### COMBO-002 — memory-first integrated experiment (rejected)

COMBO-002 combined a two-pass reader, flat latency storage, fused CRC32C hashing and a 256 KiB buffer.

Official board evidence:

- Runtime increased from the historical champion's 0.146371 s to 0.608770 s.
- Peak memory decreased from 111,808,512 bytes to 25,624,576 bytes.
- Source size decreased from 499 to 406 lines.
- Score fell from 54.478341 to 46.073377 points.

**Decision:** reject. The memory reduction was not enough to offset the execution-time regression under the published scoring weights. The final release restores PAR-001.

---

## 4. Isolated challenger experiments

The following development-machine experiments were run in isolated worktrees. They are kept as research results, not represented as official board measurements.

| Candidate | Main idea | Local wall time (median / mean) | Local peak RSS | Local cgroup peak | Lines reported | Decision |
|---|---|---:|---:|---:|---:|---|
| **HYPER-001 / PAR-001 family** | mmap, parallel parsing, parallel reduction | 0.9625 s / 0.9614 s | 306.1 MB | 307.4 MB | 425 code / 499 board | Retained as production architecture |
| **PAR-002** | Two-pass parallel scatter, flat arena, prefix sums | 0.9097 s / 0.9744 s | 266.7 MB | 269.1 MB | 655 | Rejected |
| **HYBRID-001** | Parallel buffered `pread`, flat arena, no full-file mmap | 1.5325 s / 1.5447 s | 46.8 MB | 46.5 MB | 541 | Rejected for final release |
| **HYBRID-002** | HYBRID-001 plus CRC32C, O(1) catalog indexing and LTO | 1.3975 s / 1.4205 s | 57.9 MB | 57.5 MB | 575 | Rejected for final release |

### PAR-002 rejection

Although PAR-002's median local time was promising, its mean (0.9744 s) was slower than the HYPER-001 mean (0.9614 s), and its reported variability was higher (standard deviation 0.1328 s versus 0.0484 s). It also increased the reported board line count from 499 to 655. Its projected score was therefore below the known official champion. It was not promoted.

### Measurement interpretation

The local experiments used different memory views and measurement methods. Process peak RSS and cgroup `memory.peak` are separate quantities and are reported separately. These local numbers should not be treated as directly comparable to the board's `M_bytes`.

An earlier instrumented phase report produced stage-duration figures whose sum exceeded the clean production wall time. Those stage values were not accepted as additive production-time fractions and are not used to justify the final architecture. Clean board timings and independently captured candidate measurements are the evidence used here.

---

## 5. Correctness and release verification

The final production source was checked against the public seed-1 expected outputs:

| Dataset | Records | Expected SHA-256 | Result |
|---|---:|---|:---:|
| Tiny | 1,000 | `c24fa507c963eaeac9224fd8435019b4f4e8c002ece7e32f316681a3957eb92f` | **PASS** |
| Sample | 310,000 | `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3` | **PASS** |
| Full | 3,100,000 | `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` | **PASS** |

Additional recorded checks:

- Official Track A specification tests: **15/15 passed**.
- Synthetic edge-case suite: **6/6 passed**.
- Differential harness: **4/4 passed**.
- String-view lifetime/growth stress: **5,000/5,000 passed**.
- AddressSanitizer and UndefinedBehaviorSanitizer: no reported errors.
- Official board run for final commit: **all five gates passed**.

The tests above are development/seed-1 evidence; the official final holdout is not publicly available for local reproduction.

---

## 6. Final candidate decision

**Selected:** PAR-001 historical champion restoration, committed as `19053765eec20d904bf82a81d1cef89813686ea1`.

The decision is based on:

1. Exact byte-level parity with all three public seed-1 expected output hashes.
2. All five official board gates passing on the release commit.
3. The best verified official board result for this team at the time of this log: **54.764396 points**, (K=12.453400).
4. The memory-first COMBO-002 regression and other isolated challenger results being documented rather than hidden.
5. No third-party runtime dependencies and an offline, digest-pinned Docker build.

### Evidence boundary

The values in the official board table are public seed-1 practice-board measurements. Final scoring uses the organiser's hidden holdout seed. No claim is made that the local tests prove the unseen holdout result or that a particular final leaderboard position is guaranteed.

---

## 7. Reproduction commands

From the repository root, after obtaining the official public seed-1 data and installing Docker:

```bash
# Build using the offline build contract
docker build --network=none -t lightspeed-track-a ./track-a

# Verify the full public seed-1 output
python3 check.py a --data /path/to/data/a-full \
  --expected 495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3

# Verify the sample public seed-1 output
python3 check.py a --data /path/to/data/a \
  --expected ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3
```

The checker reports end-to-end elapsed time including container startup. Use the official board run for official (T), (M), (S), (D), (K) and speed points.
