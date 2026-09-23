# Legacy query and coreference eligibility — 2026-09-23

The five legacy query-records modes now apply the shared current-eligibility
predicate before ordering and limits: like, top L2, session priority, facts and
patterns, and evaluation. An active lifecycle alone no longer admits future,
expired or suppressed records through these paths.

Coreference prior-context lookup applies the same predicate before its bounded
window. A newer unavailable antecedent cannot displace a valid older one, bias
heuristic binding, or reach the optional model resolver as context. The target must also be active and unsuppressed before either resolver runs;
future-valid target preparation remains available to indexing.

Regression coverage gives ineligible query candidates competitive priorities and
checks all five modes with a three-row limit. The coreference fixture puts eight
unavailable newer rows ahead of an eligible antecedent and checks both heuristic
resolution and the actual optional-model input. The [full packaged PostgreSQL/race suite](memory-query-coref-eligibility-2026-09-23/full-race.txt)
passes in 206.436 seconds. The final target guard passes the
[230.266-second runtime-role/coreference replay](memory-query-coref-eligibility-2026-09-23/final-runtime-race.txt).
The evaluation-corpus command now honors explicit scope narrowing, with its
[public PostgreSQL regression](memory-query-coref-eligibility-2026-09-23/scope-race.txt)
passing in 3.177 seconds; calls without a supplied scope retain their existing
contract. The [export build](memory-query-coref-eligibility-2026-09-23/export.txt)
and ownership/boundary/descriptor checks pass.

Matching application/harness `b1692776e` passes **1,699/1,699** fresh checks:
[1,085 T2 checks](memory-query-coref-eligibility-2026-09-23/fresh/T2/topology.json)
and [614 T3 checks](memory-query-coref-eligibility-2026-09-23/fresh/T3/topology.json),
with both runners exiting zero. All [nine actual image identities](memory-query-coref-eligibility-2026-09-23/fresh/image-identities.json)
and three 32,768-byte provider caps are verified. The five added authenticated
HTTP legacy-query eligibility checks pass. These repairs do not certify all MR-01
acceptance gates or complete derivation lineage. CT100 remains on released 0.4.5.
