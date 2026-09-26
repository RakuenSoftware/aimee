# MR-07 durable session integration

MR-07 remains in progress. This implementation replaces the injected-store-only
checkpoint with authenticated PostgreSQL session ownership and real host calls.
It does not enable adaptive enforcement or claim a paired quality result.

Migration 38 adds bounded exploration state to the existing session row.
The private host operation locks the authenticated session directory and state,
then commits task revisions and shared session usage together. Legacy session
saves preserve the new column. Native dispatch and authenticated hooks use the
same Go policy/accounting implementation. Hard directives precede admission;
admission is atomically marked possibly dispatched, so lost replies and client
crashes cannot create refundable executions. Issuer failure falls back to baseline
when no operator accounting ceiling requires a durable admission.

Memory-owned offers include plan/source commitments, coverage provenance and
producer identity. Native code forwards this metadata and binds the authenticated
principal, session and durable job. Unknown worktree/index generations remain
explicitly unavailable and cannot support calibrated enforcement.

The native `context_contract_expand` tool accepts a reason and references to a
host-recorded indexed miss/failure. The actual `find_symbol` empty format is
recognized exactly; cross-project observations and invented gaps are rejected.
Turn completion derives starvation from persisted constraints and indexed gaps;
model text and trivial writes are not progress attestations.

The [evidence](memory-mr07-session-evidence-2026-09-26/) records passing Go race
tests, native policy/ingress tests, and an isolated-schema real PostgreSQL test:
24 concurrent hook/native checks across two tasks admit exactly the three allowed
scans, survive handler reopening, and do not double-charge retries. Foreign
principals and non-host calls are rejected. The full memory race suite passed;
after the native-offer addition, native/source-release/receipt regression tests
also passed. A repeated broad live family suite hit an existing test-table cleanup
dependency; it is not represented as passing. The new live owner test is isolated
and passed independently.

Remaining acceptance: fresh CT109 candidate deployment; complete external-client
recovery and generation freshness; measured calibration and the predeclared
MR-18 paired quality/efficiency gates. Unit and storage fixtures are not evidence
of task-quality noninferiority. MR-08 and MR-09 have not started.
