# Future-valid index admission — 2026-09-23

Whole-record background embedding previously called the current-read API after
locking its input. A future-valid input could therefore repeatedly fail before
its boundary and exhaust the retry limit without another canonical write. The
Go worker now reads its authorized active, unsuppressed input under the same
parent lock, independently of current-time recall. Unit embedding uses the same
admission rule and suppressed inputs do not reach the embedding service.

Pre-indexing grants no read authority. The raw memory/unit vector operation now
checks current canonical parent eligibility and canonical/index scope agreement
before ordering and limits. It withholds future, expired, suppressed, retired,
moved and orphan inputs rather than exposing their IDs or allowing them to crowd
out an authorized result. Higher-level recall keeps its existing version and
serving-model checks. Personal placement and the model route are unchanged.

The [full PostgreSQL race suite](memory-future-index-admission-2026-09-23/go-race.txt)
passes in 124.182 seconds. Added [vector boundary replays](memory-future-index-admission-2026-09-23/vector-race.txt)
pass at both shipping vector widths, including limit-one fallback and unit-parent
eligibility. The shared worker replay proves that a future record already has a
vector while current reads and direct vector queries withhold it; suppression
blocks both whole-record and unit model calls. The
[exported owner](memory-future-index-admission-2026-09-23/export.txt) also passes.

Fresh deployment validation remains pending. A new fixture waits through an
actual 45-second validity boundary after the real background worker settles,
then checks that current access changes without another canonical revision or
embedding write. That fixture does not certify historical semantic recall.
Full generation coverage, re-embedding admission parity, durable activation of
copied relationships, rollback and release-time receipts remain open. These
functional suite timings are not matched performance measurements.
