# Graph candidate revisions — 2026-09-23

Graph-added and optional PageRank-neighbor memory records now carry owner, exact record ID and revision from
the same PostgreSQL statement snapshot as their selected payload. The existing
visibility and current-validity predicates still apply before candidate limits.
Other graph queries retain their existing projection shape.

Fusion preserves the payload and version of the earlier candidate when a record
appears in both arms. It does not attach a newly read revision to an older payload.
New graph candidates can therefore participate in native retained-source checks
without making a false version claim about an earlier lexical candidate.

The PostgreSQL regression checks versioned graph-only bridge and neighbor records,
a corrected graph payload and its new revision, and preservation of an earlier
unversioned base candidate. The corresponding PageRank regression also checks a corrected neighbor payload
and revision. An initial run exposed a handcrafted graph fixture missing the
shipping revision/owner columns; the [failure log](memory-graph-versions-2026-09-23/initial-fixture-failure.txt)
is retained, and the fixture now includes those columns. The [final PostgreSQL race suite](memory-graph-versions-2026-09-23/full-race.txt)
passes in 120.838 seconds. The [exported owner build](memory-graph-versions-2026-09-23/export.txt),
ownership, C/bus boundaries and descriptor guards also pass. Fresh-image
validation of this follow-up is pending.

This binds selected records, not the graph path that led to them. Transitive edge
and intermediate-node release checks remain open, as do unversioned candidates
from other retrieval lanes. This change does not complete MR-01 or the program.
