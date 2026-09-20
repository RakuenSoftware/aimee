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
A complete new fresh T2 result is still pending.

## Remaining scope

Exact-version inspection is labelled historical and does not feed ordinary
recall. It does not implement valid-time or belief-time reconstruction. Private
trusted-author/reviewer admission, proposal parity and idempotency keys remain
open, along with the wider MR-01–18 acceptance inventory. No new whole-request
P95 or throughput improvement is claimed by this validation.
