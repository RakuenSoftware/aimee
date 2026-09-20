# Exact byte limits for typed memory projections

Typed context accepts the shared version-one `context_limits` contract through
both public assembly and host runtime entry points. The Go owner validates it
before retrieval and again at the data boundary. The existing C host adapter
forwards the Go ingress plan's limits unchanged; selection and accounting remain
Go, and the C bus is unchanged.

The cap includes UTF-8 JSON escaping, field names, punctuation and both trust
wrappers. Packing removes complete rows in the existing reverse channel order.
When no evidence fits and even the wrappers exceed the cap, the projection is
empty. Zero is a literal zero-byte cap. Retained IDs describe only emitted rows;
exact byte counts and a SHA-256 digest identify the projection. Provider token
counts remain unavailable, and token limits/reserves are explicitly refused.

Repacking previously serialized every remaining row after every removal. It now
serializes each candidate once, caches prefix lengths, and writes the retained
projection once. Sorted channel keys and encoding/json item encoding preserve
the established wire format. A 512-candidate regression verifies one encoding
per candidate; exact-fit, Unicode, metadata overhead, empty projection, trust
separation and independent canonical JSON comparisons cover correctness.

## Local validation and performance

The required PostgreSQL-backed race suites pass: memory 53.914 seconds and
families 1.416 seconds. The native KB adapter, ingress pre-injection and IR module
plan tests pass using the rebuilt Go fixture. Memory ownership and C boundary
checks pass, as do 14 semantic-context contract tests.

The same `BenchmarkTypedProjectionRepacking` ran against pushed baseline
`d28e213717` and the candidate on the same Intel i7-14700K, with Go's benchmark
parallelism 8, 500 ms samples and three repetitions. It packs 128 small-summary
rows with metadata into a zero estimated-token budget, exercising repeated
removal. Results are medians:

| Measure | Baseline | Cached packing |
|---|---:|---:|
| Time per operation | 3,197,069 ns | 93,227 ns |
| Allocated bytes per operation | 3,912,523 | 166,127 |
| Allocations per operation | 43,121 | 1,470 |

This stress microbenchmark is approximately 34.3 times faster and allocates
95.8% fewer bytes. It does not establish whole-request P95, retrieval quality,
provider token cost or performance at other candidate distributions.

## Remaining scope

This cap applies to the typed memory projection. It does not account for the
entire final provider request, provide exact provider tokenization, protect
mandatory task evidence from removal, or bind source versions at dispatch.
The outer ingress packer can still omit a complete typed block; remaining-budget
feedback and final release receipts remain required for MR-03/MR-06 acceptance.
Fresh deployment results will be recorded against the exact candidate revision.
