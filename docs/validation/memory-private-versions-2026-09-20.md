# Private memory revision validation

Implementation `9582cd98122f284280b49103f4d71fc031e04cb8` adds private-store
migration 27 and Go admission/inspection for retained personal revisions.
This implementation is included in the single continuing PR #2990.

## Local integration and contract checks

The full memory and Aimee-family race suites pass with both required PostgreSQL
fixtures enabled (112.384 seconds and 1.392 seconds respectively). The shipping
private schema and migrations 26–27 run in an isolated schema under a fresh
non-owner role. Tests cover same-key and legacy updates, unchanged writes and
read counters, expected-version conflicts, competing database writers, owner
changes, lifecycle restrictions and hard-erasure cascade. A late history-write
failure rolls back content, revision and invalidation together. Runtime history
writes and temporary-table substitution are refused.

Native HTTP and MCP contract tests pass. Schema synchronization, generated
documentation, source registration and memory ownership checks pass. Memory
behavior and module-side bus transport remain Go; the C bus remains C.

## Fresh deployment

The initial fresh T2 run on owned `.253` CT 9498 used application image
`sha256:70078c369f76445b5871f667aab86eccb810bc8e521d6cf25ee9fbbfbbed4ed3`.
It passed 84 of 85 private checks before stopping: PostgreSQL restart
reconciliation restored blanket runtime grants, undoing migration restrictions
on private history and invalidation progress. Raw evidence is retained at
`/opt/aimee-memory-proposals-evidence/t2-9582cd9812`.

The correction grants baseline permissions only during legacy ownership adoption
and preserves modern migration-owned ACLs on restart. Private migration 28 repairs
permissions already weakened by older reconciliation. The targeted private race
test passes (1.044 seconds); real PostgreSQL 18 upgrade tests pass for both legacy
`aimee_store` and `aimee_shared`, including restricted table, sequence and function
permissions after restart. Fresh validation must use the corrected PostgreSQL
image as well as the application image; the older `aimee-postgres:pr2983` image
contains the faulty reconciliation.

CI also exposed stale command-discovery and MCP golden expectations. The probe
now includes the two correction-review commands; the golden includes the new
memory version fields. Both native registry and real C-host/Go-process probes
pass. The frozen semantic-context comparison records only exact memory changes
in the shared MCP files and continues to reject unrelated semantic-context drift.
The corrected implementation and harness
`f5c3a6f2c28c66032493d050b6d908c7ecc0166c` passed the complete fresh T2 gate:
**346/346 verdicts**, comprising 87 private, 208 shared, 27 correction-review,
six identity and 18 topology checks. The
[sanitized receipt](memory-shared-reliability-2026-09-20/fresh-t2-f5c3a6f2c2.json)
contains only verdict names and booleans.

Both application containers were verified against
`sha256:4515b57c8b3d53be4c9fe096faf791cc081f6493231a23ac7c28cdbbc64381eb`;
both PostgreSQL containers used
`sha256:b6209cde68c9a7a65c562b8a4ca45682f138b4a2de5b5dbcfe7ca04ec48e962f`.
The embedder remains pinned to
`sha256:b03199bee881bf632f7194f472de7bc370e66d16b7215bb6aa506fb2b1510209`.
Raw evidence is retained at
`/opt/aimee-memory-proposals-evidence/t2-f5c3a6f2c2` in owned CT 9498;
the disposable containers are stopped, with volumes and evidence retained.

The actual HTTP/MCP tests verify exact private versions, labelled history,
stale-correction conflicts, current/history persistence after restart, forbidden
runtime history/progress writes and helper execution, atomic rollback on injected
history failure, and payload erasure. Existing shared isolation, correction review,
retry, rollback, outage/recovery and immutable identity checks also pass.

The earlier CI failures in command discovery, MCP registry golden expectations
and Linux/macOS semantic-context source validation pass on the corrected commit.
The entire CI workflow was still running when this evidence was recorded; this
fresh deployment receipt does not claim completion of that workflow.

## Enrolled restart readiness

CI on `f5c3a6f2c2` failed on the first MCP proposal retry after KB restart,
although the independent fresh deployment passed. The review gate waited for
KB container health but omitted the enrolled Server's reconnect readiness used
by the other placement restart tests. Harness `1d37efd79d` adds that end-to-end
read check, requires the exact same target version, and retains the strict
proposal/replay assertions. Content-free failure categories aid diagnosis if
the retry still fails; mutations are not retried until a passing result appears.

A second independently created fresh T2 environment passed **348/348 checks**:
87 private, 208 shared, 29 correction-review, six identity and 18 topology.
Its [receipt](memory-shared-reliability-2026-09-20/fresh-t2-1d37efd79d.json)
uses the same corrected application/PostgreSQL/embedder images above. Raw evidence
is retained under `/opt/aimee-memory-proposals-evidence/t2-1d37efd79d` in owned
CT 9498. The disposable containers are stopped; volumes and evidence remain.

## Private caller-context transport

The private host-only command adapter previously dropped the authenticated
request context before reaching Go. It now carries the verified principal,
transport identity and user-authority flag separately from command arguments,
using the existing command wire format. Internal commands remain absent from
public discovery and public dispatch; plugin calls cannot use this route.
HTTP store/supersede/retire and MCP mutations use the same private adapter.
Authenticated MCP activity retains model authority, and body-supplied actor,
authority and operation fields cannot replace host-selected context or routing.

Native module-command, HTTP memory adapter and MCP adapter tests pass, including
context isolation and exact large integer tokens. This prepares private mutation
admission; it does not itself persist private authorship or supply review parity.
The C event bus implementation is unchanged.

Implementation and harness `68eaab6d44696f7f0190df7b3bf09a2e7d709fe2` now pass
another complete fresh T2 run: **348/348 checks** with the same per-group counts
above. The [new receipt](memory-shared-reliability-2026-09-20/fresh-t2-68eaab6d44.json)
is distinct from the pre-transport run. Both application containers were verified
against `sha256:4d8f87e7c6030f9d6c41f5e1dffb753ab23b8006d7b5f803c4982603b4cfc6e5`;
both PostgreSQL containers retain the corrected `b6209cde68c9…` image identified
above. HTTP/MCP private mutation envelopes and exact versions, shared scope
isolation, review/retry, rollback, restart and outage recovery all pass.

The first provisioning attempt stopped before application tests because Docker
had exhausted its default IPv4 pools; a direct network-create probe confirmed
that cause. Removing only completed, stopped owned fixtures and their networks
resolved it without changing source or images. Failed provisioning evidence is
retained at `/opt/aimee-memory-proposals-evidence/t2-68eaab6d44`; passing evidence
is at `/opt/aimee-memory-proposals-evidence/t2-68eaab6d44-r2`, inside owned CT 9498.
Passing fixture containers are stopped with volumes and evidence retained.
CI on the same implementation had 48 successful jobs and no failures when this
receipt was recorded; full workflow completion is not claimed.

## Remaining scope

Exact-version inspection is labelled historical and does not feed ordinary
recall. It does not implement valid-time or belief-time reconstruction. Private
trusted-author/reviewer admission, proposal parity and idempotency keys remain
open, along with the wider MR-01–18 acceptance inventory. No new whole-request
P95 or throughput improvement is claimed by this validation.
