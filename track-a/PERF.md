# Track A performance log

This log distinguishes team-reported local experiments from measurements produced by the Speed Demon live board. **No official board run ID is available yet**: the board feed reported that the Speed Demon Board App was not installed on this repository. Do not treat local timings as official scoring measurements.

## Scoring reference

The published Track A reference anchor is **32.05 s** and **228 MB** on the scoring machine. The board, not a local machine, determines the official run time, memory, source-size and dependency measurements.

## Local experiment history

The following values are taken from the team's previously supplied local benchmark/integration reports. They are retained as engineering history only; the hardware and measurement method are not the official scoring measurement.

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

## Official board measurements

| Candidate / commit | Board run ID | T (s) | M (MB) | S (source lines) | D | K | Points | Gates |
|---|---|---:|---:|---:|---:|---:|---:|---|
| Final Track A candidate | Pending Board App installation and successful board run | — | — | — | — | — | — | — |

After the live board detects the repository, replace the pending row with the actual run ID, commit SHA and measured values. Add subsequent optimisation steps only when the corresponding board run exists. Never substitute local timings or invented run IDs for board evidence.

## Next measurement-led work

1. Install the Speed Demon Board App on this repository and confirm the board watcher detects the `speed-demon` branch.
2. Obtain a successful Track A board run and record its actual run ID and metrics above.
3. Consider further changes only when profiling identifies a bottleneck; verify every candidate against the exact output hash and the official sandbox before retaining it.
