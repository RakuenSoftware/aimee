# MR-07 implementation checkpoint — not complete

MR01–06 remain complete (6 of 18). MR-07 is in progress; MR-08 has not started.

The execution-policy legacy seam now returns observe-only discovery metadata
when it has no authenticated task contract. It evaluates baseline forbidden
commands, path restrictions and approval rules first. Registered shell aliases
receive the same forbidden-command check. Go and native discovery classifiers
exclude compound shell operations and effectful find/preprocessor operations,
including quoted option spellings. The legacy positive attention-guard operator
cap remains unchanged; no adaptive enforcement is activated.

The private Go state machine adds typed ceilings, contract bindings and revision
history, atomic in-process reservations, retry identity checks, conservative
refunds, distinct-file accounting, and completed-turn starvation relaxation.
Bounded fallback requires a host-recorded failed/empty indexed outcome, matching
gap and revision, canonical path scope and one-minute expiry. Thirty-two
concurrent fallback attempts admit one; sixty-four ordinary attempts share a
two-scan allowance. Invalid adaptive state does not remove operator accounting.

Validation: execution-policy race tests, Go vet, native attention-guard tests,
and all 77 lint checks passed. The [evidence directory](memory-mr07-checkpoint-evidence-2026-09-25/)
records these results. The snapshot roundtrip test uses an injected commit
callback: it does **not** prove live durable storage or authenticated issuance.
No MR-07 candidate was built or deployed on CT109. No paired quality or latency
gate was run or claimed. These checks do not close MR-07 acceptance.

Outstanding: wire the existing task/session state owner and cross-task session
allowance, consume final memory-plan coverage and versions, unify both live
guard decisions, record real tool outcomes and expose expansion, then run the
predeclared paired workload and fresh CT109 acceptance. Complete MR-07 before
implementing MR-08 telemetry. Detailed outstanding work is also recorded in
[status.json](memory-mr07-checkpoint-evidence-2026-09-25/status.json).

CT100 continues to run all three released 0.4.5 images, healthy. The paired local
thinclient reports authorized HTTP 200 against 192.168.1.100:8743. Production
was not redeployed and the user-disabled PreToolUse hook was not re-enabled.
