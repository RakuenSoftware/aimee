# Rule expiry at recall and learning — 2026-09-23

Protected recall now checks a rule's explicit expiry before ordering, row limits
and byte allocation. An expired high-priority rule cannot crowd out current
rules or force a protected-context overflow. Null and empty endpoints retain
open-ended validity. The existing memory timestamp normalization treats expiry
as an exclusive upper boundary and rejects malformed nonempty timestamps.

The same predicate now gates rules read by feedback anti-pattern extraction and
style learning, before their selection limits. This prevents expired evidence
from creating new derived records through these two paths. It does not erase or
invalidate derivatives created earlier; that lineage work remains open.

PostgreSQL regression coverage seeds forty expired, high-priority, oversized
hard rules and checks that the current hard rule still fits. It also checks that
expired negative/style evidence creates neither new anti-patterns nor learned
preferences. Existing current-rule packing and overflow tests remain in place.
The [full PostgreSQL race suite](memory-rule-expiry-2026-09-23/full-race.txt)
passes in 138.331 seconds, and the [exported owner build](memory-rule-expiry-2026-09-23/export.txt)
passes. Ownership, C/bus boundaries, descriptors and documentation guards pass.
Fresh-image validation is pending. No proposal completion is claimed, and CT100
remains on released 0.4.5.


Pinned application/harness `cdafbd67c` has passed [614/614 T3 checks](memory-rule-expiry-2026-09-23/fresh/T3/topology.json),
including a committed private correction before a retryable HTTP 500 and refusal
before any second provider request. The runner exited successfully. The parallel
T2 run is still pending; this is not a complete matrix claim.
