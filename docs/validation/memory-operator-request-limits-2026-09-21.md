# Deployment-owned final provider context ceilings

Application and matrix harness: `72bf98b032`.
Application image:
`sha256:6a9ea9df64959e7d221b3a597c170e8867af0f6d30c6d47f43d5df48395cd66e`.

The final provider gate now accepts a deployment-owned byte ceiling in
`AIMEE_PROVIDER_CONTEXT_LIMITS`. The host forwards the bounded opaque JSON value
to Go economizer admission. Go validates both policy layers and uses the smaller
byte cap in one admission call. Absence of a request header inherits the
operator's limit; a larger caller limit cannot raise it. An explicit caller zero
remains zero. Empty/unset deployment environment means no operator cap.

Invalid operator policy fails closed with `request_budget_policy_invalid` and
HTTP 503, while malformed caller limits retain HTTP 400. Unsupported operator
token caps/reserves count as invalid deployment policy because there is still no
provider-bound counter. No failed admission dispatches a provider request.
Streaming refusals remain explicit failures.

The common fence covers Server Chat, Responses and Messages and native agent
primary/fallback calls. The native host transports metadata and maps the decision; Go owns interpretation
and intersection. The C bus is unchanged. Metadata version 2 binds both limit
layers, route, final body length and digest to the decision. Caller-only version 1
remains supported; callers with neither policy layer avoid the admission RPC.

## Local evidence

- Go race tests pass for economizer and the bundled launcher.
- Actual C-host/shipped-Go-process conformance passes for caller-only and inherited
  policy metadata, including exact fit, overflow, absent caller caps and attempts
  to raise the operator allocation. Decision commitments bind the complete frame.
- The standalone Go export builds and passes the same real C-host conformance
  test, including metadata version 2. The repository lock is unchanged.
- Native client and serialization-fence regressions pass. The fence exercises
  operator enforcement without HTTP context, with optional reduction on/off and
  all three provider routes. Malformed operator policy, stale commitments,
  unavailable admission and malformed frame lengths fail closed.
- All 77 local lint checks pass after regenerating the environment reference.
  Ownership checks preserve the pure-Go memory boundary and C bus.

Local Go-handler microbenchmarks, three runs on Linux/amd64 (Intel i7-14700K):

| Limit layers | Time range | Allocated bytes | Allocations |
|---|---:|---:|---:|
| Caller only | 2.344–2.442 µs | 2,072 | 34 |
| Operator only | 2.343–2.384 µs | 2,072 | 34 |
| Operator and tighter caller | 4.208–4.290 µs | 4,096 | 67 |

These exclude body hashing, bus transport and provider latency. They do not prove
full-request P95 or the MR-18 performance gate.

All **58 remote CI checks pass** on implementation `72bf98b032`, including the
complete script suite, sanitizers, Windows/macOS builds and all deployment
placements including encrypted T2. [CI receipt](memory-shared-reliability-2026-09-21/ci-72bf98b032.json).
A later documentation/evidence commit does not change the tested production code.

## Fresh deployment evidence

Fresh isolated stacks in owned CT 9498 on `.253` pass **1,246/1,246** checks:
**763 T2** and **483 T3**, including 273 provider-boundary checks per placement.
Every application uses the same verified image and deployment ceiling:
`{"schema_version":1,"max_request_bytes":32768}`. All nine application,
PostgreSQL and embedder image identities and all three application policies were
verified from the actual container metadata.

- [T2 checks](memory-shared-reliability-2026-09-21/fresh-t2-72bf98b032.json)
- [T3 checks](memory-shared-reliability-2026-09-21/fresh-t3-72bf98b032.json)
- [Final provider accounting](memory-shared-reliability-2026-09-21/provider-accounting-72bf98b032.json)
- [Images and deployment limits](memory-shared-reliability-2026-09-21/image-identities-72bf98b032.json)
- [Go microbenchmark samples](memory-shared-reliability-2026-09-21/admission-microbenchmarks-72bf98b032.json)

All nine task-owned containers are stopped; volumes and evidence are retained.
The stacks are `aimee-e2e-kb-67fd7510dc`, `aimee-e2e-server-a3155e3b21` and
`aimee-e2e-server-97a61e48c1`. Raw logs remain under
`/opt/aimee-memory-proposals-evidence/` in CT 9498. Sanitized receipts contain
check results, byte/digest provenance and verified image/policy identities;
no credentials or provider prompt bodies are committed.

The provider fixture verifies that oversized buffered and streaming calls cannot
bypass the deployment ceiling with no header, an inherited limit object, or a
larger caller cap. Every refusal sends zero provider requests and emits no false
stream completion. Existing exact-fit and tighter caller-cap tests still run.
Only the external completion endpoint is a fixture; host serialization, Go owners,
storage, bus transport and final admission are real.

Task-composition limits, exact provider token counting, protected repacking,
source-version release checks and durable dispatch/acknowledgement receipts
remain open. No MR-01–MR-18 proposal is certified complete by these checks.
