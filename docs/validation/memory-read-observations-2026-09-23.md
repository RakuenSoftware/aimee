# Legacy read observations and history admission — 2026-09-23

Legacy shared reads retain their existing public JSON shape while carrying an
internal owner/revision observation from the same SQL statement as the selected
payload. Exact reads, scoped and visible searches, legacy query modes, history
and the negation candidate lane use these observations during public metadata
enrichment. Current reads also repeat current eligibility in that statement.
An unchanged payload cannot hide a revision change, suppression or expiry.

The observations remain internal to the Go owner; they are not caller-supplied
preconditions or new claims of final provider authorization. The original ranked
selection is refused on a detected change rather than silently relabeled with a
new revision. Subsequent provider release and transitive dependencies retain
their separate requirements.

Fact history now applies the established retained-history admission predicate
before ordering and limits, and honors explicit caller scope narrowing. Retained
superseded, archived and retired records remain available; deleted, revoked,
rejected, quarantined, unknown and suppressed-active records do not. Historical
enrichment repeats that admission and refuses a newly revoked selected record.

Public PostgreSQL regressions interleave changes after each legacy selection,
check the positive unchanged case and preserve historical rendering. The packaged
runtime-role fixture covers every excluded history state, explicit scope and
pre-limit filtering. The [full PostgreSQL/race suite](memory-read-observations-2026-09-23/full-race.txt)
passes in 254.529 seconds. The initial fixture-schema failures are retained
separately; the corrected fixtures include collection identity and row revisions.
The [export build](memory-read-observations-2026-09-23/export.txt) passes in 4.950
seconds, and ownership, module-boundary, descriptor, inventory and documentation
checks pass. Fresh deployment validation remains pending; no proposal completion
is claimed.
