# Track A performance log

This log records the optimisation journey for LightSpeed's Track A submission. It separates team-reported local experiments from measurements produced by the official Speed Demon board.

The team reports that LightSpeed and points are now visible on the live website. That confirms the team is appearing on the board, but this file still needs the **actual Track A run ID and run details** to make its official-measurement section complete. Do not infer runtime, memory, gates, or a run ID from the displayed points alone.

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

## Official board measurements

Fill this row from the actual Track A result shown on the live board. Use the exact run ID and commit SHA; copy the measured fields from that result. If the board only shows the team and points, open the team's Track A run/result details first.

| Candidate / commit | Board run ID | T (s) | M (MB) | S (source lines) | D | K | Points | Gates |
|---|---|---:|---:|---:|---:|---:|---:|---|
| Final Track A candidate | Awaiting exact Track A run details | — | — | — | — | — | — | — |

Do not substitute local timings or invent run IDs. Add later optimisation steps only when a corresponding board run exists, and connect each claimed improvement to the run that measured it.

## Next measurement-led work

1. Open LightSpeed's Track A result on the live board and capture its run ID, commit SHA, measured metrics and gate status.
2. Replace the pending row above with those exact values.
3. Make further code changes only if there is enough time and a measured bottleneck to justify them; verify each candidate against the exact output hash and sandbox before retaining it.
