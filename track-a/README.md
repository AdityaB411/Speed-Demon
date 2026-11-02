# Track A: Ingest

## Approach

The solution aggregates the 3.1 million request records in `/data/requests.log` and writes the byte-stable result to `/out/summary.txt`.

### Memory-oriented design

The implementation deliberately uses two sequential passes instead of retaining separate latency vectors in every worker and hour/endpoint group.

**Pass 1 — group counts and errors**

- Read the input in fixed 64 KiB blocks. Each block is scanned for complete LF-terminated records; a small carry area preserves a record split across two reads.
- Parse the fixed-width UTC timestamp and exact endpoint.
- Assign each endpoint a compact 16-bit ID. The published catalogue contains exactly 1,842 endpoints.
- Update one fixed table indexed by `hour * 1842 + endpoint_id`. Each group occupies 12 bytes: request count, 5xx count and a cursor.
- A status beginning with `5` counts as an error; 499 does not.

**Prefix allocation**

- Prefix-sum group counts in chronological-hour / endpoint-ID order.
- The resulting cursor for each group identifies the start of its slice in one flat latency array.
- Allocate exactly one `uint32_t` per input record: 12.4 MB for 3.1 million records. There is no per-group vector capacity slack or allocator metadata for millions of individual latency-vector allocations.

**Pass 2 — latency placement**

- Read the input again using the same bounded block reader.
- Look up the already assigned endpoint ID and append the latency at that group's cursor.
- Each group therefore ends up with one contiguous latency slice, without storing a group ID alongside every latency.

### Percentiles and output

- Groups with at most 24 values are sorted directly.
- Larger groups use exact nearest-rank selection with `std::nth_element` for p95, p50 and p99. No approximation or histogram bucketing is used.
- The rank calculation uses integer arithmetic: `(p * n + 99) / 100 - 1` for zero-based indexing.
- Output is streamed one line at a time; the complete 10 MB result is not assembled in RAM.
- Hours are emitted chronologically and endpoint names are sorted bytewise. All endpoints are ASCII under the specification, so ordinary string-view ordering matches the required bytewise order.
- Error rate is `floor(errors * 1,000,000 / count)`, computed using integer arithmetic.

### Memory trade-off

The previous implementation allocated 72 × 2,048 `LocalGroup` objects per worker. With 12 workers and a 32-byte `LocalGroup`, that represented about 56.6 MB of largely empty group/vector objects before accounting for the actual latency data.

The new implementation removes:
- all duplicated worker-local hour/endpoint tables;
- all per-group latency vectors and their capacity slack;
- the full in-memory output string;
- full-file memory mapping.

It adds one sequential reread of the input. This is an intentional time-for-memory trade-off. The goal is to keep peak resident memory close to the unavoidable 12.4 MB latency payload plus compact group metadata and runtime overhead.

## Correctness assumptions from the specification

The fast parser relies on the published input contract: ASCII, well-formed space-delimited records, fixed-width 27-byte UTC timestamps, one LF at the end of every line, status values from 100 through 599, uint32 latencies, and the fixed 72-hour time span. It does not rely on global timestamp ordering: records are grouped by their parsed hour, so interleaved shards and out-of-order hour transitions are handled. Duplicate lines are processed independently and therefore count as separate requests.

The group table is sized for the specified catalogue of exactly 1,842 distinct endpoint templates. This is a task constraint, not a heuristic based on seed 1.

## Dependencies and build

No third-party runtime dependencies. The implementation uses C++20 standard-library facilities and POSIX file I/O. The Dockerfile builds with the allowlisted, digest-pinned GCC 15 image and runs on the digest-pinned Debian 13 slim image.

## Local validation status

The memory-optimized source was compiled with the same key flags as the Dockerfile (`-O3 -std=c++20 -static-libstdc++ -static-libgcc`) and also built with AddressSanitizer and UndefinedBehaviorSanitizer for edge-case testing.

Recorded local checks:
- Handcrafted edge-case input, including hour/month boundaries, duplicate lines, 5xx vs 499, zero and maximum uint32 latencies, and percentile group sizes 20, 100 and 101: byte-identical to an independent Python oracle.
- Randomized 310,000-record dataset with 1,210 endpoints: byte-identical to an independent Python oracle.
- 3,100,000-record synthetic full-scale dataset with all 1,842 endpoints: byte-identical to an independent Python oracle.
- Empty input: empty `summary.txt`.
- AddressSanitizer / UndefinedBehaviorSanitizer edge run: passed.

These are local results. The 3.1-million-record dataset was synthetic, not the official seed-1 generator output. The official seed-1 SHA-256 and hidden holdout have not been revalidated in this branch, and the official board has not yet measured this candidate.
