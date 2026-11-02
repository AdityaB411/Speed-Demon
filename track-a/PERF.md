# Track A performance log

Team: **LightSpeed**  
Track: **A — Ingest**  
Language: **C++20**  
Final implementation: `track-a/solution.cpp`

This log records the implementation decisions and the measurements available for this submission. It distinguishes local development experiments from measurements on the Speed Demon scoring machine. Local timings are not used as official scores.

## Final implementation and live-board result

The selected implementation uses read-only memory mapping, newline-aligned parallel parsing, worker-local aggregation, a global reduction, and exact nearest-rank percentile selection. It has no third-party runtime dependencies.

The expanded LightSpeed entry on the live leaderboard displays this Track A result:

| Metric | Board result |
|---|---:|
| Machine score / points | **54.478** |
| Efficiency coefficient K | **12.290423** |
| Wall time T | **0.146 s** |
| Peak memory M | **112 MB** |
| Source lines S | **499** |
| Dependency factor D | **1** |
| Commit shown by board | `59e7977` |

The board's short commit `59e7977` matches commit [`59e797734ef7a98a963428d8f7ba741ff4d3af58`](https://github.com/AdityaB411/Speed-Demon/commit/59e797734ef7a98a963428d8f7ba741ff4d3af58).

**Run ID and gate-detail note:** the expanded team panel supplied for this log shows the score, K, time, memory, source lines and short commit, but does not display the run ID or individual gate flags. The run ID is therefore recorded as **not exposed in the available result view**, not guessed. The board awarded a numeric score and did not label this result as failed. The organisers publish individual gate outcomes separately after code freeze.

The board result is the live practice-board measurement on the public seed-1 dataset. Final scoring uses the hidden holdout seed; this practice result is not represented as the final holdout score.

## Optimisation history

The following development measurements were previously reported from the team's local integration environment. They explain the engineering choices, but they were not measured on the scoring machine and have no live-board run IDs. They must not be compared directly with the official board time.

| Candidate | Change / experiment | Team-reported local result | Decision |
|---|---|---:|---|
| Single-thread C++ | Initial native C++ implementation | About 0.545 s; about 266.45 MB | Superseded |
| PAR-001 | Memory-mapped input, worker-local aggregation, up to 12 parsing threads, parallel hourly reduction | About 0.289 s warm native median; about 313.8 MB; local Docker checker wall time about 2.94 s | Selected |
| MEM-001 | Flat-arena memory-layout experiment | About 0.927 s native | Rejected; slower than PAR-001 |
| LOW-001 | Alternative low-level parser experiment | About 0.614 s native | Rejected; slower than PAR-001 |

The local Docker checker time includes container startup and is not the same measurement as the board's process wall time. The experiments above are retained as development history, not as official competition results.

## Correctness evidence reported during local development

The team previously reported the following local checks:

- Sample output SHA-256: `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3`.
- Full seed-1 output SHA-256: `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3`.
- Published-spec tests: 15/15 passed.
- Edge-case checks: 6/6 passed.
- Differential checks: 4/4 passed.
- Adversarial endpoint-cardinality check: 5,000 distinct endpoints reportedly passed.

These are previously reported local results, not a fresh test execution in this update. The official board's scored result is separate evidence from the live scoring machine.

## Scoring context

The organisers define the efficiency coefficient as:

`K = (T_ref/T)^0.50 × (M_ref/M)^0.20 × (S_ref/S)^0.15 × (D_ref/D)^0.15`

For Track A, the published reference is 32.05 s and 228 MB. The dependency factor is `D = 1 +` the number of direct non-standard-library runtime dependencies; the repository metadata declares zero third-party dependencies, so `D = 1`.

The machine score is derived from K. The remaining portion of the competition score is determined by human judging. The live board is a practice board; final scoring is run against the hidden holdout dataset after code freeze.

## Record integrity

- Official values above are transcribed from the team's expanded live-board result.
- The board's visible short commit is tied to the full Git commit linked above.
- No run ID or gate-by-gate status has been invented where the supplied result view does not show it.
- Local timings and test results are explicitly labelled as team-reported local evidence.
- No further optimisation candidate is claimed without a corresponding measurement.
