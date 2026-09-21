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
separation and independent canonical JSON comparisons cover correctness. An
additional 512-case comparison exercises all 128 channel combinations at four
byte caps against encoding/json, including separately framed reviewed procedures.

## Local validation and performance

The required PostgreSQL-backed race suites pass: memory 53.914 seconds and
families 1.416 seconds. The native KB adapter, ingress pre-injection and IR module
plan tests pass using the rebuilt Go fixture. Memory ownership and C boundary
checks pass, as do all 17 semantic-context contract/evidence tests. The additional
512-case canonical encoder comparison passes under the race detector.

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

[Raw baseline samples](memory-shared-reliability-2026-09-20/typed-packing-before-2026-09-20.txt)
and [candidate samples](memory-shared-reliability-2026-09-20/typed-packing-after-2026-09-20.txt)
retain all three repetitions.

This stress microbenchmark is approximately 34.3 times faster and allocates
95.8% fewer bytes. It does not establish whole-request P95, retrieval quality,
provider token cost or performance at other candidate distributions.

## Remaining scope

This cap applies to the typed memory projection. It does not account for the
entire final provider request, provide exact provider tokenization, protect
mandatory task evidence from removal, or bind source versions at dispatch.
The outer ingress packer can still omit a complete typed block; remaining-budget
feedback and final release receipts remain required for MR-03/MR-06 acceptance.

## Fresh deployment results

Application and harness `c9d4a9a9ec` passed **818/818 checks** on the disposable
CT 9498 on `.253`: **528/528** in enrolled T2 and **290/290** in standalone T3.
The authenticated KB typed-context gate contributes 18 checks for exact bytes,
zero caps, exact fits, deterministic retained evidence and explicit refusal of
unsupported token limits. Existing private/shared revision, correction/review,
restart, rollback, outage/recovery, semantic, identity and provider checks pass.

[Named T2 verdicts](memory-shared-reliability-2026-09-20/fresh-t2-c9d4a9a9ec.json),
[named T3 verdicts](memory-shared-reliability-2026-09-20/fresh-t3-c9d4a9a9ec.json),
[provider accounting](memory-shared-reliability-2026-09-20/provider-accounting-c9d4a9a9ec.json)
and [all nine image identities](memory-shared-reliability-2026-09-20/image-identities-c9d4a9a9ec.json)
retain no request bodies or credentials. Application image:
`sha256:44e2e36d39a041625510b8b0b1e8706675aa61f65aca2e38fa7c4b2b6f52d944`.
The tested PostgreSQL and embedder images match the prior final-assembly run.
Raw receipts remain under `/opt/aimee-memory-proposals-evidence/t2-c9d4a9a9ec`
and `t3-c9d4a9a9ec`. Task containers were stopped after completion; volumes and
receipts were retained. The additional canonical-encoder test and this evidence
were committed after the tested application revision; application code is unchanged.
