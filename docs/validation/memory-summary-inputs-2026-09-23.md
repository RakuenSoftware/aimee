# Generated summary input observations — 2026-09-23

A fresh read of a parent revision must not certify an older summary. Generated
headline and signals summaries now record their actual parent revision in the
existing dependency table, with producer `go-derived-text-v1` and policy
`summary-input-v1`. The Go producer holds the parent lock from reading source
text through replacement, and records the final revision after its own derived
metadata and scope-tag writes. Both background indexing and explicit reindex use
this pipeline. No database schema change is required.

Summary serving requires that exact observed parent revision and the known
single-parent contract. Unknown or additional inputs are withheld before result
limits. The condition covers summary lookup, public record enrichment, ingress
previews, answer assembly, episode/unit generation from summaries, and final
source revalidation. Independently eligible canonical text remains available.
Public reads never create or repair producer observations.

Existing unobserved summaries, including curated rows, remain stored but are not
served as current generated summaries. Reindex rebuilds deterministic headline
and signals rows; it does not manufacture provenance for curated text. The legacy
summary dependency trigger also resets observations on direct no-op writes. Such
rows fail closed until the producer observes them again, even when the summary
revision itself did not change.

Regression coverage includes missing observations, independent summary edits,
parent edits, exact large identifiers, no-op dependency resets, source-release
checks, and regeneration from changed canonical text under the packaged non-owner
runtime role. Authenticated HTTP coverage repeats the stale-read cases. The [full packaged PostgreSQL/race suite](memory-summary-inputs-2026-09-23/full-race.txt)
passes, as does the [export build](memory-summary-inputs-2026-09-23/export.txt).
Initial fixture failures are retained separately. Ownership, module boundaries,
descriptors, CLI routes, API conformance and documentation checks pass. Fresh
image validation of this repair remains pending.

This closes one MR-04 freshness gap. Full derivation family identity, transitive
lineage, query dependencies and restore-resistant erasure remain open. It does not
certify MR-01 or MR-04 complete. Released CT100 remains on version 0.4.5.

Application/harness `83ff06d9c` passes **614/614** fresh T3 checks; the
[raw receipt](memory-summary-inputs-2026-09-23/fresh/T3/topology.json) and
[nine image identities](memory-summary-inputs-2026-09-23/fresh/image-identities.json)
are retained. All three actual provider request byte limits are 32,768. T2 has
passed all 16 summary preview/release cases, but its full run is still pending.
