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
1,311-check HTTP run predates this change. No new whole-request latency claim is
made for this projection.

## Still open

Legacy external rule rendering and caller-prompt/full-context truncation remain
separate native host paths. Provider-exact token counts/reserves, complete source
version binding and durable prepared/admitted/dispatched/acknowledged receipts
remain open. Reminder marking still occurs at host assembly, not at a durable
provider-dispatch acknowledgment. Text/source digests are projection evidence,
not durable release receipts.
