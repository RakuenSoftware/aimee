# Native recall projection validation — 2026-09-21

The native agent asks the existing Go recall operation to render within its
remaining context byte allocation. The shared memory owner and module-side bus
transport remain Go; the C bus is unchanged. This continues MR-03 and MR-06
implementation and does not certify either proposal.

## Behavior

Go reserves the complete hard-rule array before optional memory, then retains
whole rows in the established six-section order. Mandatory overflow returns
`protected_context_overflow`. A successful empty projection is an intentional
selection and cannot invoke a second prospective-only fallback.

The existing personal/composed recall request carries `native_context_bytes`.
The prospective-only path uses the same Go renderer within its existing match
request. Neither path adds a module round trip. Returned recall rows match the
rendered selection, so host activation processing excludes omitted rows.

The C host verifies projection version, rendered byte count, allocation and text
SHA-256, then appends the opaque text. It validates every retained reminder ID
before any append or trigger action. Only owner-selected IDs are forwarded, as
canonical decimal strings. Public prospective output preserves int64 tokens;
mark-triggered and complete use the existing Go exact-ID decoder. The previous
float map round trip could round IDs, and the numeric-only decoder rejected
lossless string transport.

## Checks

- Go tests cover complete sets of 20 hard rules, Unicode, exact allocation fits,
  one-byte underflow, literal zero, whole-row selection, max-int64 identity,
  malformed envelopes and invalid allocations.
- PostgreSQL regressions exercise real private/composed runtime operations,
  protected overflow and public prospective matching. A max-int64 reminder
  survives selection, string-based marking and completion.
- The native production context builder rejects bad digests, byte counts,
  allocations, versions and reminder identities before provider dispatch.
  Existing initial/refresh refusal, inherited refusal and recovery cases remain.
- The KB transport fixture verifies forwarding the native allocation through
  composition. The ownership ledger records the reviewed external adapter changes.

The complete Go memory suite passes with required PostgreSQL evaluation/replay.
The native application build and both context-refusal/KB-client fixtures pass.
All 767 benchmark tests pass (two existing skips). All 77 lint checks pass after
adding the new source to the module descriptor and rerunning the initially
failing descriptor gate. The independently exported Go memory process builds.

These native tests use controlled module replies and provider transport; they do
not substitute for fresh-image native-agent acceptance. The earlier fresh
1,311-check HTTP run on `9deb1efc14` predates this change. No new whole-request latency claim is
made for this projection.

## Still open

Legacy external rule rendering and caller-prompt truncation remain
separate native host paths. Provider-exact token counts/reserves, complete source
version binding and durable prepared/admitted/dispatched/acknowledged receipts
remain open. Reminder marking still occurs at host assembly, not at a durable
provider-dispatch acknowledgment. Text/source digests are projection evidence,
not durable release receipts.


## Follow-up: generic context assembly

The native session-start fallback also forwards its remaining byte allocation to
the existing Go `assemble_context` command. Go retains complete rows in retrieval
order, counting headers and the terminal newline. A literal zero produces empty
context. Omitted rows are marked unselected in explain output; budget diagnostics
report exact bytes separately from the legacy token estimate. Absent allocations
preserve the existing rendering.

The C host verifies the same versioned opaque projection and preserves owner
refusals. It no longer truncates generic memory context. This adds no RPC.
Go tests exercise zero, exact-fit and one-byte-short allocations, Unicode,
max-int64 record IDs, invalid public/internal budgets, and the restricted-role
PostgreSQL public command. This follow-up is later than the fresh `1f198a6b07`
image; it requires its own fresh-image evidence.

The follow-up passes the full Go memory suite with required PostgreSQL
replay/evaluation, native application build and context-refusal fixture, all 77
lint checks, and all 17 S1 integration-contract tests.

The non-explain path now avoids computing diagnostic scores and metadata for
unused candidates. A local 12-row fixture with 8,192 bytes available, repeated
Unicode content and three benchmark runs measures:

| Path | Time per operation | Allocated bytes | Allocations |
|---|---:|---:|---:|
| Assembly with diagnostic metadata | 246.6–251.6 µs | 193,049–193,096 | 93 |
| Projection only | 1.60–1.71 µs | 14,694–14,695 | 8 |

Command: `go -C server-go test ./modules/memory -run '^$' -bench '^BenchmarkContextAssemblyProjection$' -benchmem -count=3`.
The benchmark compares the two assembly paths on the same input and budget;
PostgreSQL public regressions verify identical retained text with/without explain.
It excludes retrieval, transport and provider time and does not establish a
whole-request P95 improvement or MR-18 acceptance.


## Asynchronous native worker prerequisite

Inspection of `/v1/runs` found that registry-based model selection no longer
loaded the full `agent_config_t`, but the worker still passed that uninitialized
object to `agent_run_with_tools`. The worker now explicitly loads the routing
configuration and refuses execution when loading fails. A stale comment from the
old model-selection contract is removed.

The native application builds; existing run-store and native context-refusal
fixtures pass. These fixtures do not execute the full asynchronous HTTP worker.
Live `/v1/runs` success, owner-outage refusal and recovery remain required before
claiming asynchronous native acceptance. The fresh `1f198a6b07` image predates this
worker prerequisite.


## Fresh recall image regression evidence

Application and harness `1f198a6b07bf70cfffeab914c279abdb8058cb6b` pass
**1,311/1,311** checks on fresh owned `.253` projects in CT 9498:

- Enrolled T2: **803/803** ([receipt](memory-shared-reliability-2026-09-21/fresh-t2-1f198a6b07.json)).
- Standalone T3: **508/508** ([receipt](memory-shared-reliability-2026-09-21/fresh-t3-1f198a6b07.json)).
- Each placement passes 298 provider-boundary checks, including paused real Go
  owner refusals with zero provider calls and supervised recovery. T3 additionally
  covers concurrent requests, exact IDs and semantic recall/outage/retirement.
- Application image: `sha256:a0711c9d2e95d87c60424675285fbe91e7faaf537b60ea0d9c8601a42308d19f`.
  [Image identities](memory-shared-reliability-2026-09-21/image-identities-1f198a6b07.json)
  verify the nine containers and 32 KiB deployment-owned request cap.
- [Provider accounting](memory-shared-reliability-2026-09-21/provider-accounting-1f198a6b07.json)
  retains byte counts and digests without prompt bodies or credentials.
- All nine containers are stopped. Images, volumes and remote receipts remain.

The fresh matrix covers HTTP, module transport, storage and recovery regressions.
It does not invoke the native agent initial/refresh or asynchronous worker paths;
those distinctions remain as described above. The later generic assembly
`df4380b4fc` and worker initialization `ad4604dfaa` fixes postdate this image and
have local validation only. No MR-01–MR-18 proposal is fully accepted by this run.
