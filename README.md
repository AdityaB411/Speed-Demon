# LightSpeed — Speed Demon 2026, Track A

This repository contains **one competition entry: Track A — Ingest**, implemented in C++20.

The solution reads the 3.1 million HTTP request records from `/data/requests.log`, groups them by UTC hour and exact endpoint, computes request counts, nearest-rank p50/p95/p99 latencies and 5xx error rates, and writes the byte-exact result to `/out/summary.txt`.

## Submission layout

```text
.
├── LICENSE
├── README.md
├── check.py
└── track-a/
    ├── .speed-demon.toml
    ├── Dockerfile
    ├── PERF.md
    ├── README.md
    └── solution.cpp
```

Only Track A is entered. The unused Track B, C and D starter folders are intentionally not included.

## Implementation

The C++ implementation uses read-only memory mapping, newline-aligned parallel parsing, worker-local endpoint aggregation, a global reduction, and exact nearest-rank selection using `std::nth_element` for larger groups. It uses no third-party runtime dependencies. The Track A README documents the input assumptions and algorithm in more detail.

## Build and run

The Dockerfile uses digest-pinned, allowlisted GCC 15 and Debian 13 slim images and compiles offline.

From the repository root, build the track image:

```bash
docker build --network=none -t lightspeed-track-a ./track-a
```

For the board-equivalent sandbox, use the supplied checker. Put the official seed-1 full dataset's `requests.log` in `data/a/`:

```bash
python3 check.py a --data data/a --expected 495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3
```

The expected SHA-256 above is the published full-dataset Track A output hash. The sample output hash is `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3`.

## Submission status

- Team: `LightSpeed`
- Entered track: `A`
- Language: `C++`
- Third-party runtime dependencies: none
- Licence: MIT
- Board measurements: add the official run IDs and measurements to `track-a/PERF.md` after the Board App detects this repository and completes a run.

Local validation results and official board measurements are distinguished in the performance log. A local runtime is not represented as an official score.
