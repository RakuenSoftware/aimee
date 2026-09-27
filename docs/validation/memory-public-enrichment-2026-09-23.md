# Public record enrichment consistency — 2026-09-23

Public record rendering previously combined a selected payload with metadata
from a later READ COMMITTED statement. The shared Go enrichment path now refuses
changed scope, tier, kind, key, content or confidence. When the selected record
carries a version, its exact owner and revision must still match the metadata
statement. A failure returns no partially enriched batch.

The same helper serves public record views, answers, hybrid explanations and
wiki exports. Historical selection remains governed by the original read policy;
this change does not replace it with a current-only predicate. Unversioned reads
still lack an observation for unchanged-payload revision transitions, and this
check does not close the later provider-dispatch race or certify MR-01.

Regression coverage interleaves mutations between selection and enrichment,
checks owner and revision changes independently of payload changes, and confirms
that unchanged records render normally. The [initial full packaged race run](memory-public-enrichment-2026-09-23/initial-full-race.txt)
completed in 207.723 seconds: the packaged runtime replay passed, while two
restricted-role temporary fixtures lacked SELECT on the collection owner. After
correcting their grants, both [public regressions](memory-public-enrichment-2026-09-23/final-public-race.txt)
pass in 3.615 seconds. The [module export build](memory-public-enrichment-2026-09-23/export.txt)
passes in 4.326 seconds; ownership, boundary and documentation checks pass.
Fresh-image validation of this repair remains pending. CT100's three released
0.4.5 containers remain healthy and unchanged.

The [read-observation follow-up](memory-read-observations-2026-09-23.md) extends
this fence to legacy in-process selections and repeats current/historical
admission during enrichment. Its full packaged race and export checks pass.
