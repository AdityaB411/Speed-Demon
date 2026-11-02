# LightSpeed — Speed Demon 2026, Track A

This repository contains one competition entry: **Track A — Ingest**, implemented in C++20. Track B, C and D are intentionally out of scope.

The solution reads the 3.1 million HTTP request records from `/data/requests.log`, groups them by UTC hour and exact endpoint, computes request counts, nearest-rank p50/p95/p99 latencies and 5xx error rates, and writes the byte-exact result to `/out/summary.txt`.

## Implementation

The memory-optimized implementation uses two sequential input passes and one flat latency arena:

1. **Count pass:** parse each record's hour, endpoint and status. A fixed table of 72 × 1,842 compact group records stores each group's count and 5xx count.
2. **Prefix pass:** assign each group a start cursor in the flat latency arena.
3. **Latency pass:** allocate exactly one `uint32_t` per input record and place each latency directly into its group's contiguous slice.
4. **Reduction and output:** calculate exact nearest-rank percentiles in place with `std::nth_element` (or a small-group sort), then stream output one line at a time in chronological-hour and bytewise-endpoint order.

Input is read in bounded 64 KiB blocks. The solution does not memory-map the full input, does not construct per-worker copies of the hour/endpoint table, and does not retain the complete output string in memory. There are no third-party runtime dependencies.

The trade-off is a second sequential read of the input file. This deliberately exchanges extra input I/O for substantially lower peak memory.

## Build and run

The Dockerfile uses digest-pinned, allowlisted GCC 15 and Debian 13 slim images and compiles offline.

From the repository root:

```bash
docker build --network=none -t lightspeed-track-a ./track-a
```

For the board-equivalent sandbox, put the official seed-1 full dataset's `requests.log` in `data/a/` and run:

```bash
python3 check.py a --data data/a --expected 495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3
```

The expected full-dataset SHA-256 above is the published Track A output hash. The sample output hash is `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3`.

## Submission status

- Team: `LightSpeed`
- Entered track: `A`
- Language: `C++20`
- Third-party runtime dependencies: none
- Licence: MIT

The previous practice-board result (54.478 points, 146 ms, 112 MB) belongs to the earlier multithreaded implementation and its measured commit. The memory-optimized branch has local full-scale synthetic-test results, but it has **not yet been measured by the official board**. Do not treat the local timing or memory figures as official competition results.
