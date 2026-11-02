# LightSpeed — Speed Demon 2026 · Track A: Ingest

**Team:** LightSpeed · **Team slug:** `lightspeed-4a5b5a`  
**Competition:** Speed Demon 2026 · **Submission branch:** `speed-demon`  
**Track:** A — Ingest · **Language:** C++20 · **License:** MIT

This public repository contains the LightSpeed entry for Track A only. Tracks B, C, and D are intentionally not entered.

The program processes the Track A HTTP request log at `/data/requests.log` (3.1 million records; approximately 238.8 MB). It groups records by UTC hour and exact endpoint, calculates request counts, exact nearest-rank p50/p95/p99 latencies and 5xx error rates, and writes the required byte-exact output to `/out/summary.txt`.

## Current official board result

<sub>Live practice-board result for the public seed-1 dataset; this is not the hidden-holdout final score.</sub>

| Metric | Official result |
|---|---:|
| Speed points | **54.764396** |
| Efficiency coefficient (K) | **12.453400** |
| Execution time (T) | **0.142494 s** |
| Peak memory (M) | **111,947,776 bytes** |
| Measured source size (S) | **499 lines** |
| Dependency factor input (D) | **1** (zero external dependencies) |
| Docker image | **119,869,264 bytes** (about 119.87 MB) |
| Official gates | **5/5 passed** |

- **Run:** [`run-a03726c4d127`](https://speeddemon.dev/board/)
- **Scored commit:** [`19053765eec20d904bf82a81d1cef89813686ea1`](https://github.com/AdityaB411/Speed-Demon/commit/19053765eec20d904bf82a81d1cef89813686ea1)
- **Board status:** scored; check, build, correctness, timeout and sandbox gates all passed.

The live board measures the public seed-1 dataset. The competition's final scoring uses a hidden holdout seed, so the result above is evidence of a successful public-data run, not a claim about the final holdout outcome.

## Architecture

The production implementation is a C++20, multi-threaded pipeline:

1. **Memory-mapped input:** maps the read-only log into the process address space on Linux using `mmap`; no intermediate whole-file input buffer is allocated.
2. **Newline-aligned partitioning:** splits the byte range across up to 12 workers and advances boundaries to line endings so each input record belongs to one chunk.
3. **Thread-local ingestion:** each worker parses timestamps, methods, endpoints, status codes and latencies into its own endpoint catalogue and hourly aggregates. The ingest phase avoids shared mutable aggregation state and per-record locks.
4. **Catalogue reconciliation:** merges worker-local endpoint names, produces a deterministic bytewise endpoint ordering and builds local-to-global ID maps.
5. **Parallel hourly reduction:** distributes the 72-hour output window across up to eight reduction workers. Each group computes exact nearest-rank percentiles; small groups use sorting and larger groups use `std::nth_element`.
6. **Deterministic output:** formats hourly results into per-hour buffers, concatenates them in chronological order and writes the single required output file.

See [Track A implementation notes](track-a/README.md), [the optimization log](track-a/PERF.md), and [the implementation](track-a/solution.cpp).

## Build and run

Build the image offline from the Track A directory:

```bash
docker build --network=none -t lightspeed-track-a ./track-a
```

Run in the competition-style sandbox (replace the host paths with your dataset and output directories):

```bash
docker run --rm \
  --network=none \
  --read-only \
  --tmpfs /tmp:rw,size=1g,exec \
  --memory=8g --memory-swap=8g \
  --pids-limit=512 \
  --cap-drop=ALL \
  --security-opt=no-new-privileges \
  --user 65534:65534 \
  -v /path/to/data:/data:ro \
  -v /path/to/out:/out:rw \
  lightspeed-track-a
```

The container reads `/data/requests.log`, writes only `/out/summary.txt`, and has no runtime network access.

### Official local checker

From the repository root, with Docker available:

```bash
python3 check.py a --data /path/to/seed-1/full --expected 495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3
```

The checker builds with `--network=none`, runs with the competition sandbox restrictions and verifies the SHA-256 of the one permitted output file. The checker’s printed elapsed time includes container startup and is not the official board timing.

## Submission files

| Path | Purpose |
|---|---|
| `track-a/solution.cpp` | C++20 production implementation |
| `track-a/Dockerfile` | Offline, digest-pinned multi-stage build |
| `track-a/.speed-demon.toml` | Machine-readable track/team/language/dependency metadata |
| `track-a/README.md` | Problem interpretation, architecture, runtime contract and validation |
| `track-a/PERF.md` | Baselines, experiments, measured results, rejected candidates and final selection |
| `check.py` | Local competition-style build/run/output checker |
| `LICENSE` | MIT licence |

**External runtime dependencies:** none. The implementation uses the C++ standard library and operating-system mapping APIs only. The metadata declares `dependency_count = 0`.

## Reproducibility and validation

The current implementation's public seed-1 output hashes are recorded in [Track A verification](track-a/README.md). The official board run above independently passed all five gates on the scored commit. Additional test and optimization evidence, including unsuccessful candidates, is preserved in [PERF.md](track-a/PERF.md).

The repository intentionally excludes the large datasets, generated outputs, local binaries, and `benchlab/` experiment workspace from the submission tree. The source and measured optimization history remain available; no precomputed answer or input-specific output cache is used.
