# Track A performance log

This log records the optimisation journey for LightSpeed's Track A submission. It separates team-reported local experiments from measurements produced by the official Speed Demon board.

## Latest visible official board result

The team expanded its entry on the live board. The screenshot shows the Track A row with the following values. The short commit shown by the board, `59e7977`, matches the beginning of the full commit SHA recorded below.

| Candidate / commit | Board run ID | T (s) | M (MB) | S (source lines) | D | K | Points | Gates |
|---|---|---:|---:|---:|---|---:|---:|---|
| Track A / `59e797734ef7a98a963428d8f7ba741ff4d3af58` | Not visible in screenshot | 0.146 | 112 | 499 | Not shown | 12.290423 | 54.478 | Not shown |

**Evidence note:** values above are transcribed from the team's leaderboard screenshot. The screenshot does not expose a run ID or gate-by-gate result, so those fields are deliberately marked as not visible rather than guessed. Open **ALL RUNS** in the expanded Track A panel to find the run details and record the exact run ID and gate status if available.

## Scoring reference

The published Track A reference anchor is **32.05 s** and **228 MB** on the scoring machine. The board determines official runtime, memory, source-size and dependency measurements. The live-board result is a practice measurement; final scoring uses the recorded commit and the hidden holdout dataset.

## Local experiment history

The following values are from the team's previously supplied local benchmark/integration reports. They are engineering history only; the hardware and measurement method are not the official scoring measurement.

| Candidate | Change / result | Local measurement | Decision | Official board run ID |
|---|---|---:|---|---|
| Reference | Organiser's Python reference | Official anchor: 32.05 s, 228 MB | Baseline | Not applicable |
| Single-thread C++ | Initial native C++ implementation | About 0.545 s; about 266.45 MB | Superseded | Not available |
| PAR-001 | Memory-mapped input, worker-local aggregation, up to 12 parsing threads, parallel hourly reduction | About 0.289 s warm native median; about 313.8 MB; local Docker checker wall time about 2.94 s | Selected implementation | Not available |
| MEM-001 | Flat-arena memory-layout experiment | About 0.927 s native | Rejected; slower than PAR-001 | Not available |
| LOW-001 | Alternative low-level parser experiment | About 0.614 s native | Rejected; slower than PAR-001 | Not available |

The selected implementation is the C++ source currently in this folder. The local Docker checker timing includes container overhead and must not be compared directly with the board's process timing.

## Previously reported local correctness checks

The team previously reported these results from its local integration environment:

- Sample output SHA-256: `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3`.
- Full output SHA-256: `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3`.
- Published-spec test suite: 15/15 passed.
- Edge-case checks: 6/6 passed.
- Differential checks: 4/4 passed.
- Adversarial endpoint-cardinality check: 5,000 distinct endpoints reportedly passed.

These are previously reported local results, not a fresh run in this audit and not live-board run IDs.

## Next step

1. Click **ALL RUNS** in the expanded Track A panel.
2. If the run details show a run ID or gate results, append those exact values to the latest-result row above.
3. Avoid further code changes unless a measured issue requires them; keep the submitted branch and board-measured commit aligned.
