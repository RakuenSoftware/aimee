# Current-state evidence coverage — 2026-09-23

MR-05 now has an opt-in, bounded current-state requirement contract on the typed
context owner. Requirements identify a task revision and subject/relation
obligations; they cannot supply expected record IDs. Strict decoding rejects
unknown fields, empty/all-optional/duplicate obligations and more than 16 roles.
Only `current_state` is supported; other modes explicitly remain unknown.

The evaluator uses final retained current assertions with matching owner record
versions and observed direct memory dependencies. Confidence, observation text,
unversioned summaries and revoked/historical assertions cannot fill a role.
Conflicting objects or recorded contradictions prevent satisfaction. It reports
missing, budget_dropped, conflicted, unavailable and satisfied separately, and
aggregates only required obligations into complete/partial/insufficient/unknown.
A complete result means these explicit roles were covered at this packing
boundary. It does not establish completeness of the candidate universe or answer
correctness.

The outer ingress packing step reconstructs coverage from retained evidence.
Prior conflict/unavailability is a restrictive input, never proof of satisfaction;
dropping a conflicting assertion cannot cure the conflict. Repacking to zero
bytes changes previously satisfied roles to budget_dropped and commits the new
selection digest. Caller-provided complete verdicts cannot fill absent roles.
The report labels its release state not_revalidated. Existing provider-fence
source revalidation remains required independently.

Validation includes the full memory PostgreSQL suite (61.571 seconds), current
state unit cases, exact owner IDs above 2^53, typed transport roundtrips, zero-byte
outer packing, and the real native ingress adapter with the Go owner fixture.
The restricted-runtime-role PostgreSQL replay calls the real typed endpoint,
observes a current assertion, drops it under the byte cap, then adds a hidden
parent and verifies it cannot establish coverage. Default calls without explicit
requirements retain their previous unknown sufficiency behavior.

This is not complete MR-05 implementation. Timeline/temporal-anchor planning,
independent origins, bounded recovery/tool admission, stale cache repair and
automatic task-contract propagation remain open. No proposal is certified here.

Raw [Go results](memory-evidence-coverage-2026-09-23/go-tests.txt) and
[native results](memory-evidence-coverage-2026-09-23/native-tests.txt) accompany this record.

The scoped PostgreSQL/race suite additionally passed in 234.212 seconds.
Independent export builds and executes the new coverage regressions. The raw
[race result](memory-evidence-coverage-2026-09-23/race-tests.txt) and
[export result](memory-evidence-coverage-2026-09-23/export-tests.txt) are retained.

The same batch adds MR-10's pure shadow evaluator. It accepts bounded policy
artifacts and owner-snapshot anchors, evaluates system/record/domain/kind
precedence, and records policy digest/revision and exact deadlines. Missing,
malformed, mismatched or future anchors are unknown under the declared
conservative current-serving rule. Nontransient kinds receive no new age filter;
historical/diagnostic purposes never bypass base eligibility. Tests cover these
boundaries and deterministic nonrenewal. This evaluator is not connected to live
serving: canonical anchor admission, reporting and rollout still need implementation.

Fresh application `686b99ea05df4387326e30ac171e96bce01780e8` and harness
`2cae9dec4` pass 1,552 recorded boolean verdicts: 108 T1, 925 T2 and 519 T3.
[Raw receipts](memory-evidence-coverage-2026-09-23/fresh/summary.json) include
[image identities](memory-evidence-coverage-2026-09-23/fresh/image-identities.json)
and the [environment](memory-evidence-coverage-2026-09-23/fresh/environment.json).
The operator byte ceiling was unset in these stacks, so 73 optional ceiling
checks per Server topology did not execute. This is narrower than the earlier
1,688-check deployment receipt; the next harness makes that setting mandatory.
The fresh containers/networks/volumes were removed by the harness. CT109 remains
available for the next candidate's isolated tests; CT100 is unchanged.

The full 77-check lint run passed 76 checks and failed only formatting under
local clang-format 22. Re-running that check with pinned clang-format 19 passes.
Both the [original run](memory-evidence-coverage-2026-09-23/lint-clang22.txt) and
[pinned-format result](memory-evidence-coverage-2026-09-23/pinned-format.txt) are retained.
