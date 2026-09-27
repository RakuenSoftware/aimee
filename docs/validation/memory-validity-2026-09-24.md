# MR-01 validity diagnostic — 2026-09-24

`aimee memory validity <id> --mode current` and `POST /v1/memory/validity`
now route to the Go memory owner. Private placement is the default; use
`--store=kb` with an explicit project/workspace for shared records. Historical
KB inspection uses `--mode historical --valid-at <absolute timestamp>`.
Personal historical mode and belief-time reconstruction return
`unsupported_mode`, matching the existing read-policy capability boundary.

The diagnostic evaluates the actual current or historical read predicate and
captures its record owner/revision in the same SQL statement. It reports
lifecycle, temporal applicability, eligibility and exclusion reasons without
returning record text. Evidence and authority remain explicitly unknown.
The result is an observation, not a grant to dispatch a later provider request.

An authenticated user context is required to inspect excluded-record metadata.
The Go owner inherits a host-verified scope when arguments omit it and refuses
arguments that widen that scope. Explicit scope does not grant `include_all`.
Missing and hidden records produce identical decisions without checked versions
or record metadata. The C host only selects placement and forwards envelopes;
there is no C eligibility fallback.

The [full memory race suite](memory-validity-2026-09-24/full-race.txt) passes in
292.719 seconds. The subsequent verified-scope guard, diagnostic parity and
concurrent pool tests pass in the [targeted race run](memory-validity-2026-09-24/scope-race.txt)
(1.306 seconds). The [final export build](memory-validity-2026-09-24/export.txt)
passes in 5.407 seconds. [Native CLI, Server forwarding and dispatch tests](memory-validity-2026-09-24/native-tests.txt)
pass, including exact large IDs, opaque decision transport and unavailable-owner
refusal. Module descriptors include the new owner and test files. Ownership,
boundary, route, API and documentation checks are required before publication.

Fresh HTTP and CLI checks now compare diagnostic revisions with ordinary reads
in both placements. Execution against a fresh application image remains pending.
The seven MR-01 acceptance gates remain tracked in the
[closeout checklist](../proposals/pending/memory-reliability-01-closeout.md).
This change does not close MR-01 or advance work to MR-02.

Review before deployment also caught a compatibility error: a verified
`service` identity names the managed data plane, not a memory scope. The follow-up
preserves its existing cross-project access while still requiring authenticated
user purpose and applying the requested memory scope. Project-bound credentials
remain unable to widen their scope. The
[service-scope race regression](memory-validity-2026-09-24/service-scope-race.txt)
and [export](memory-validity-2026-09-24/service-scope-export.txt) pass. The initial
`1bc49fc19` image is superseded for deployment validation by this correction.

The first fresh `49274b8c5` T3 run failed at the new personal validity HTTP
check: the private host's established success contract requires `store: user`,
which the diagnostic omitted. The [failed raw gate](memory-validity-2026-09-24/first-fresh/T3/local-memory.json)
is retained. The Go owner now labels both placements explicitly. The native
regression covers the real private forwarding helper and rejects the original
missing-label envelope. [Targeted race](memory-validity-2026-09-24/envelope-race.txt),
[export](memory-validity-2026-09-24/envelope-export.txt) and
[native transport](memory-validity-2026-09-24/envelope-native.txt) checks pass.
Fresh deployment must be rerun on this correction; the failed image is not a
passing release candidate.

## Fresh envelope-fix deployment receipt

Candidate `386cfaf4c` completed T2 and T3 with actual runner exit zero: **1714/1714**
checks (T2 1097, T3 617). The [raw fresh receipts](memory-validity-2026-09-24/fresh/T2/topology.json)
and [KB-free receipt](memory-validity-2026-09-24/fresh/T3/topology.json) include
private and shared HTTP/CLI validity observations matching serving revisions.
The three HTTP hidden-intermediate graph checks also pass: exclusion, restoration
and renewed exclusion after scope loss. These are process-level evidence for
MR-01/A3, without closing the other acceptance clauses.

All [nine actual container image identities](memory-validity-2026-09-24/fresh/image-identities.json)
and three provider request-byte caps were captured while the stacks were alive.
The application image was
`sha256:7ce715d82655f45a91e645caf2be3c31d9b7cd368178f0e572943b24846463af`;
PostgreSQL remained on schema 34. Both disposable stacks were cleaned up.
This receipt precedes the separately validated verified-credential serving fix
in `7eb4cf3b1`; it does not validate that later change.
