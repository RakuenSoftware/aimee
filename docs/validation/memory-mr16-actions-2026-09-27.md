# MR-16 governed actions — 2026-09-27

State: functional acceptance complete for the [documented adapters](../action-receipts.md)
on candidate `d49bf0d07`. The [evidence manifest](memory-mr16-evidence-2026-09-27/sha256.json)
pins controlled live checks, test results, harnesses and upgrade identities.

Native tool dispatch now commits exact host-issued intents through the durable
Go action owner. Admission rechecks current operator policy and guarded memory
evidence. Receipts distinguish acknowledgement, verified effect and uncertainty;
duplicates cannot redispatch. Cumulative reservations and trusted parent lineage
survive session projections and process restart. Public inspection, cancellation
and reconciliation cannot supply their own successful outcome assertion.

## Acceptance

The live HTTP/compiled-CLI run passes exact-file verification, changed payload and
destination rejection, forged read labels, duplicate suppression, actual server
restart, write-with-failed-readback, unknown-outcome retry refusal, exact-object
reconciliation and foreign-session refusal. A controlled native model turn reads
and writes its isolated worktree with durable memory-bound receipts. A procedure
correction committed after provider submission but before the tool response
prevents the stale write; the turn reports `stale_context` and the file is unchanged.

The PostgreSQL action-owner race suite verifies concurrent duplicate dispatch,
shared parent/child reservations, handler restart, ownership and corrupted-state
refusal. Execution-policy/tools race tests cover expiry, generation changes,
trusted classes, composition rules, cancellation, symlinks and exact verification.
The complete PostgreSQL memory race suite passes in 386.875 seconds; export build
passes in 5.892 seconds. Native action, execution-policy, CLI and complete KB
HTTP/TLS suites pass. All 77 lint checks ran; corrected registration, schema and
ownership checks pass, and generated-document verification passes after commit.

The first live attempt exposed a real async session-handoff defect: dispatch used
a generated session instead of the authenticated requested session. Commit
`d49bf0d07` fixes that handoff. The first memory fixture also used an untracked file
that correctly did not exist in the isolated worktree; the corrected fixture uses
the tracked file and the exact rewritten destination from its receipt. Failed
checks are retained alongside the passing evidence.

## Cost and limits

The reproducible admission reducer benchmark measures approximately 17.4 microseconds
with no prior actions, 0.485 milliseconds with 32 and 4.00 milliseconds with 256.
It includes journal decoding, validation, policy reduction and serialization, but
excludes database locks/commit and cross-owner transport. Twelve live governed
read API calls have median 414.3 milliseconds and maximum 483.3 milliseconds;
those are end-to-end times, not isolated admission overhead. Journal cost grows
with retained history; current bounds are 1,024 actions and 4 MiB.

These checks establish functional behavior, not model quality or cost savings.
Exact effect confirmation currently covers `write_file`; other supported tools
retain acknowledgement or unknown status. Publication sensitivity depends on
operator-declared path classes. There is no privilege-changing adapter or promise
to govern unrelated third-party CLI effects. A confirmed historical receipt does
not promise that a file remains unchanged forever. No optional policy is promoted.

## Upgrade and rollback

CT109 Server and KB upgraded from MR-15 through `33d3fed08` to `d49bf0d07` with
private store snapshots, preserving workspaces, enrollment and Vault state. Both
are healthy; optional health/selection/horizon flags remain unset. CT100 remains
healthy on `aimee-native-core:0.4.5-bridge.2` and released 0.4.5 dependencies.

The snapshot-restoring boot-failure controller remains available; this run did
not exercise rollback. After effects begin, preserve the current action journal
for reconciliation rather than restoring a pre-effect snapshot and hiding work.
Raw database dumps and model configuration remain private on CT109.
