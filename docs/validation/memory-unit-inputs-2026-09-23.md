# Generated unit input observations — 2026-09-23

Deterministic units now record the parent revision used during generation, a
SHA-256 commitment to the unit fields used by embedding text and retrieval
metadata, and an optional summary ID/revision. The existing lineage ledger stores
these observations atomically with unit replacement. It retains no additional
copy of the unit text. Stable unit IDs and separately authored cards remain intact.

The embedding-input query used by re-embedding and shared semantic/unit recall,
the direct unit embedder, and raw vector search withhold generator-owned units
whose parent, unit fields or used summary no longer match. A serving read cannot
certify old unit text against a newly read parent. The unit's summary dependency
also requires that summary's producer-observed parent revision.

These checks preserve advance indexing of future-valid records; the serving
queries still impose current valid-time eligibility separately. They do not add
a schema migration. Unowned custom units retain their existing eligibility rules.
Independent event, entity, temporal and chunk source revisions are not yet a
complete intermediate-input contract; this is not full arbitrary derivation
closure or MR-04 completion.

The [full packaged PostgreSQL/race suite](memory-unit-inputs-2026-09-23/full-race.txt)
passed in 253.133 seconds. The initial fixture field-name build failure is retained
separately. The final single-point lookup optimization passes the
[174.687-second runtime-role/vector replay](memory-unit-inputs-2026-09-23/final-runtime-race.txt)
and [export build](memory-unit-inputs-2026-09-23/export.txt). Ownership, module
boundary, descriptor/inventory, route, API and documentation checks pass. The
fresh matrix also checks that the real background unit producer records its
source observations; that deployment validation remains pending. Released CT100 remains
on 0.4.5 throughout candidate validation.

The combined unit/card candidate `76be02a42` passes **614/614** fresh T3 checks.
The [raw T3 receipt](memory-unit-inputs-2026-09-23/fresh/T3/topology.json) and
[nine verified image identities](memory-unit-inputs-2026-09-23/fresh/image-identities.json)
are retained, including all three actual provider byte limits of 32,768. Full
T2 validation remains pending. The intermediate unit-only image `0cda332a0` was
built but was superseded by this combined candidate before deployment testing.
