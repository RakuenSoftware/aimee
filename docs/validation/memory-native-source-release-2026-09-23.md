# Native retained sources and private revalidation — 2026-09-23

Native recall projections now carry exact versions of retained private and shared
memory records and a separate selection commitment. A row omitted by the byte
allocation contributes no source reference. Version changes alter the selection
commitment even when the rendered text is identical. Unknown owner placement or
invalid present versions refuse projection; absent legacy versions remain absent.
Hard rules, reminders, directives and other unversioned channels are not certified
by these record references.

After accepting projection bytes, the native host forwards opaque metadata to the
Go owner for a request-bound source handle. At each provider attempt, Go partitions
that handle into private and shared checks with one fresh challenge. Private IDs
stay on the Server; shared references go to the KB with the captured scope. Go
requires matching challenge and subset commitments from every participating owner.
Missing, stale, swapped or replayed answers refuse admission. The durable receipt
binds the complete retained versioned subset and the resulting source check.

Private SQL compares owner, ID, exact revision, lifecycle and expiry in one
statement snapshot. Pending commitment references require pending lifecycle;
ordinary native records require active lifecycle. Shared native references use
the existing scoped current-validity predicates, with a separate pending contract.
These checks do not lock a record through a remote provider call or close the
post-check mutation window.

The PostgreSQL regression exercises private corrections, foreign owners, pending
versus active records, expiry, revocation and erasure. Pure Go tests exercise
retained selection, exact large IDs, mixed-owner partitions and both required
answers. The native fixture runs the real Go handle/plan policy, carries private
checks locally, refuses a stale answer and verifies no shared-owner call occurred.
Its database-owner response is a transport fixture; the PostgreSQL tests check
that owner's actual SQL independently. Agent refusal propagation also passes.

The [final PostgreSQL race suite](memory-native-source-release-2026-09-23/full-race.txt)
passes in 118.743 seconds. [Targeted routing races](memory-native-source-release-2026-09-23/routing-race.txt),
[native transport](memory-native-source-release-2026-09-23/ingress.txt),
[agent refusal](memory-native-source-release-2026-09-23/agent.txt) and the
[exported build](memory-native-source-release-2026-09-23/export.txt) pass.
Ownership, C/bus boundaries, module inventory/descriptors and documentation guards
also pass. Fresh-image validation is pending. The preceding
receipt-cache matrix is recorded separately and cannot certify this implementation.
No proposal is marked complete; CT100 remains on released 0.4.5.
