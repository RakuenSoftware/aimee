# MR-09: Fair hybrid candidates, bounded priors and exposure-aware selection

- **State:** Proposed
- **Priority:** P1 for candidate parity; P2 for fitted routing and exposure control
- **Owner:** Go memory retrieval and context selection
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md); evaluate with [MR-08](memory-reliability-08-retrieval-health-telemetry.md)/[MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md)
- **Delivery:** Four independently gated slices

## Problem and intended result

Different memory endpoints provide different retrieval arms. In a shared array, lexical hits can consume all candidate slots before semantic-only or graph-only evidence competes. Non-query boosts and repeated-serving feedback can then dominate selection without revealing why.

Provide bounded independent arm pools, explicit fusion, limited prior influence and optional final-selection diversity. Preserve personal/shared ownership and hard eligibility.

## Integration points

Implement collection/fusion in `server-go/modules/memory/{data.go,visibility_search.go,personal_vectors.go,fusion.go}`. Migrate the memory semantics of `kbs_semantic_assertion_hybrid` from the typed context backend to that Go owner and switch its C callers to the shared contract. Publish capabilities per endpoint and placement: lexical, dense, graph, code, temporal mode, index readiness and fallback. An unavailable arm must not silently satisfy an endpoint's semantic-retrieval promise.

## Candidate collection and fusion

Reserve bounded per-arm candidate quotas under an overall work/latency budget. Retrieve eligible candidates independently, union by stable record/version, revalidate and fuse before applying the final top-k. Retain native score semantics and rank contributions. RRF is a valid baseline; its score is not a probability and raw scores from different arms must not be added without normalization.

Graph traversal validates every hop and has node/depth/deadline limits. Query routing may skip an arm with an explicit reason, but early stopping requires evidence that the current plan can satisfy its requirements. It cannot be based solely on “enough hits”. Missing embeddings do not route private memory into shared infrastructure.

## Bounded prior rule

Define a versioned base relevance scale for a query class. Scope authorization and validity are hard gates, not priors. Optional recency, usage, local relevance and trust-ranking adjustments must each have a cap and a joint cap.

One concrete policy is `final_score = base_score + clamp(sum(delta_j), −B, B)`, with every `delta_j` also bounded. Under that contract, priors alone cannot reverse a base-score gap greater than `2B`. The scale and B must be fitted/validated for the actual fusion scores; do not copy an arbitrary 0–1 bound onto RRF. Record base rank, every adjustment and final rank. If rank-only fusion is retained, define and test an equivalent maximum rank displacement instead.

No exposure count directly increases authority or truth confidence. Distinguish explicit successful use from automatic serving; only [MR-15](memory-reliability-15-procedure-outcomes-and-task-cost.md) outcome evidence may justify an outcome-based ranking feature.

## Final-selection diversity

After hard gates and fusion, optionally choose among near-equivalent candidates to reduce duplicate text, same-family concentration and needless repeated exposure. Define a query-class relevance tolerance before tuning. Preserve active constraints, authoritative corrections, the strongest required hit and sole independent support for an unsatisfied requirement.

Type floors are desired reservations, not permission to exceed caller/token limits. If floors conflict, protect mandatory content first, satisfy task requirements next, then apply discretionary diversity. Record unsatisfied floors and displaced evidence. An exposure penalty never hides the only available useful record. Coverage is recomputed after final packing.

## Implementation slices

1. Restore declared hybrid capability parity and fair arm admission with distractor fixtures. No learned weight changes.
2. Add versioned fusion/prior policy artifacts and actual contribution traces; constrain each prior and aggregate influence.
3. Test deterministic duplicate/source-family/type selection at equal final budgets. Keep exposure adaptation disabled.
4. Fit query routing and exposure policy on held-out data; enable only if paired outcome and latency/cost gates pass.

## Acceptance gates

- A full lexical pool cannot exclude a semantic-only candidate from fusion; duplicate-heavy and graph-only cases also compete.
- Unsupported/unavailable arms report explicit state and bounded fallback.
- Each prior and their combination obey the declared maximum overturn; stable ties are deterministic.
- Thirty copies cannot displace required independent evidence; frequent authoritative corrections and constraints survive exposure controls.
- Floors cannot exceed hard item, byte or token limits. Missing requirements remain visible.
- A disabled experiment returns the baseline selection policy while retaining eligibility, truthful traces and fair admission.

## Rollout and rollback

Canary each slice separately. Freeze evaluation before tuning and record model/index/policy versions. Keep a last-known-good policy artifact for atomic rollback. Do not use health concentration alone as the optimization objective.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
