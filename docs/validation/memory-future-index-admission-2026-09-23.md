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

Fresh T1 candidate `d5b3980f2` passes **134/134 checks**, including an actual
45-second validity boundary after the real background worker settles. Current
access changes without another canonical revision or embedding write. The
[raw verdicts](memory-future-index-admission-2026-09-23/fresh/T1/topology.json)
and [three image identities](memory-future-index-admission-2026-09-23/fresh/image-identities.json)
are retained; the actual application provider cap is 32,768 bytes. Application
image: `sha256:03e39b52ade17017c893feefccea152e5404eba659ad4a133a80d920ba97234a`.
The database is schema-34 `aimee-pr2990-postgres:d0e3c5752`, with the released
0.4.5 embedder. The harness exits zero and removes its containers. T2 was not
rerun for this intermediate candidate; combined T1/T2 validation of the copied
relationship activation follow-up remains pending. This test does not certify
historical semantic recall.
Full generation coverage, rollback and release-time receipts remain open. These
functional suite timings are not matched performance measurements.

A follow-up applies the same active/unsuppressed admission to generation inputs,
coverage and pending-metadata checks. Restricted-role replay suppresses a source
after backfill and proves both direct re-embedding and cutover exclude its whole
record and units, without another model call. Future-valid generation inputs
remain admitted. The generic raw-vector operation preserves the established
contract for other families (including semantic assertions); it does not interpret
their IDs as memory parents. This compatibility rule has its own vector regression.
The [final race suite](memory-future-index-admission-2026-09-23/reembed-race.txt)
passes in 132.763 seconds; [export](memory-future-index-admission-2026-09-23/reembed-export.txt)
also passes. The first follow-up run exposed a multi-statement prepared-query
mistake in the new fixture, corrected before the passing run; its
[diagnostic](memory-future-index-admission-2026-09-23/reembed-fixture-failure.txt)
is retained. The fresh T1 receipt above uses this follow-up, not the earlier
`f28d6b8ce` commit alone.

Linked relation generation now also prepares authorized, unsuppressed future-valid
inputs ahead of time, while continuing to exclude expired inputs. Every copied
input's existing current-time/revision fence still applies at serving. Consequently
a clock-only boundary can expose the prepared relationship without an unrelated
mutation or timer-driven rewrite. The expanded real-clock fixture checks both
its early omission and later appearance with the same relation identity.
Restricted-role replay proves the copy is prepared but withheld; the
[final PostgreSQL race suite](memory-future-index-admission-2026-09-23/linked-future-race.txt)
passes in 138.321 seconds, and
[export](memory-future-index-admission-2026-09-23/linked-future-export.txt) passes.
An additional bulk-cascade regression confirms the existing link journal handles
erasing both endpoints in one statement. No schema change is needed. Combined
fresh T1/T2 validation of this follow-up remains pending.
