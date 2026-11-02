# Track A: Ingest

## Approach

The solution aggregates the 3.1 million request records in `/data/requests.log` and writes the byte-stable result to `/out/summary.txt`.

### Input and parsing

- The input is mapped read-only with `mmap` on Linux (with a guarded Windows implementation for local development). Endpoint names are represented as `std::string_view` slices into that stable mapping, avoiding a separate allocation and copy for every endpoint occurrence.
- For large inputs, the file is divided into at most 12 approximate byte ranges. Each range is aligned to complete LF-terminated records before a worker parses it. Small inputs below 2 MiB use one worker.
- The parser reads the fixed-width 27-byte UTC timestamp directly. The specification guarantees the same 72-hour date span for every seed, so the timestamp is mapped to an hour index from 0 through 71 without a general-purpose date parser.
- Method is ignored as specified. A status beginning with `5` represents a 500–599 response; latency is parsed as an unsigned 32-bit integer.

The workers aggregate independently. Each worker assigns compact local endpoint IDs and stores counts, 5xx counts and latency values by hour and endpoint. This avoids shared mutable aggregation state while parsing.

### Reduction and output

After parsing, the solution builds a global endpoint catalogue and maps each worker's local IDs to global IDs. It merges counts and error counts, combines the latency values for each non-empty (hour, endpoint) group, and selects the nearest-rank p50, p95 and p99 values.

For groups with more than 24 values, `std::nth_element` selects the three required ranks without fully sorting the group. Smaller groups are sorted directly. The rank calculation uses integer arithmetic: `(p * n + 99) / 100 - 1` for zero-based indexing.

The output is assembled in chronological hour order, with endpoints ordered lexicographically. It writes the eight required fields, integer parts-per-million error rates, LF line endings and a trailing newline for every record.

## Correctness assumptions from the specification

The fast parser relies on the published input contract: ASCII, well-formed space-delimited records, fixed-width UTC timestamps, one LF at the end of every line, status values from 100 through 599, uint32 latencies, and the fixed 72-hour time span. It does not rely on global timestamp ordering: records are grouped by their parsed hour, so interleaved shards and out-of-order hour transitions are handled. Duplicate lines are processed independently and therefore count as separate requests.

The implementation uses 16-bit endpoint IDs. This is sufficient for the specification's catalogue of 1,842 distinct endpoints, but it is an intentional bound tied to that published contract.

## Dependencies

No third-party runtime dependencies. The implementation uses C++20 standard-library facilities and operating-system file-mapping/resource APIs. The Dockerfile builds with the allowlisted, digest-pinned GCC 15 image and runs on the digest-pinned Debian 13 slim image.

## With another 24 hours

The next worthwhile work would be to profile allocation and peak-memory costs on the scoring machine, then test any alternative aggregation layout against the exact output hash across multiple generator seeds. A change would only be kept if it improved measured performance without changing output or exceeding the sandbox limits. The current approach favours a verified parallel parser and exact percentile selection over speculative SIMD or approximate statistics.
