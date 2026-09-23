# Generated episode input observations — 2026-09-23

The deterministic episode generator now records the parent revision used for
its text, its own resulting episode revision, and the ID/revision of a summary
when that summary supplied the text. These observations live in the existing
lineage ledger under `memory-episode-input-v1` and are replaced atomically with
the generated episode. They are never filled in by a serving read.

Current list/get, typed context, briefing and release checks require matching
observations for generator-owned episodes. A used summary must also pass its
own observed-parent check. Parent edits, independently edited episodes, missing
producer observations and stale intermediate summaries cannot acquire freshness
from a new read. Filtering precedes selection limits. Existing authored episodes
retain their direct parent eligibility policy; this change does not invent a
source history for authored text or certify full transitive lineage.

No schema change is required. The [full packaged PostgreSQL race suite](memory-episode-inputs-2026-09-23/full-race.txt)
passed in 250.535 seconds, and the [export build](memory-episode-inputs-2026-09-23/export.txt)
passed. The final profile-path check and intermediate-summary regressions pass in the
[224.057-second runtime-role/domain race replay](memory-episode-inputs-2026-09-23/final-runtime-race.txt).
The [final export](memory-episode-inputs-2026-09-23/final-export.txt), ownership,
boundary, descriptor, route, API and documentation checks pass. Fresh deployment
evidence is pending.
The released 0.4.5 deployment on CT100 remains unchanged. Full MR-04 independent
family accounting, arbitrary derivation closure and restore-resistant erasure
remain open.

Matching application/harness `74ed997e7` passes **614/614** fresh T3 checks;
[raw results](memory-episode-inputs-2026-09-23/fresh/T3/topology.json) and
[nine verified image identities](memory-episode-inputs-2026-09-23/fresh/image-identities.json)
are retained. All three provider request byte limits are 32,768. T2 has passed
all four new generated-episode HTTP checks; its complete run remains pending.
