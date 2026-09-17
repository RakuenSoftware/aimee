# MR-08: Retrieval health telemetry with defined metrics

- **State:** Proposed
- **Priority:** P1; instrument and establish baseline during the foundation wave
- **Owner:** Memory diagnostics and observability
- **Depends on:** [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) for actual final selection; [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) for family metrics
- **Delivery:** Three implementation slices

## Problem and intended result

Individual-query metrics can hide population problems: a small set of records dominates context, low-trust derivatives spread across tasks, or suppressed content reappears through a particular retrieval arm. Health telemetry must describe actual serving, preserve access boundaries and distinguish a useful frequently served constraint from pathological repetition.

Extend existing memory metrics with bounded selection-derived events and scope-aware aggregates. Add `aimee memory health --window 24h --json` and a readable equivalent.

## Event contract

Join final selection to request/task/turn IDs, a keyed query fingerprint, query class, scope/purpose, policy versions, arm states and trace/contract IDs. For each delivered record retain only authorized metadata needed for analysis: record/version, kind/state, family reference, provenance/trust class, applicable time, final rank, actual arm contribution and omission/displacement reason.

Use a keyed, namespace-specific fingerprint for low-entropy queries rather than publishing a plain hash. Raw query and memory text are not required. Record IDs/family links remain access-controlled. High-cardinality identities belong in bounded event storage, not metric labels. Deduplicate by request/attempt and record/version.

Sample at the invocation level and retain sampling probability, policy and window. Do not independently sample individual records and then pretend the reconstructed list is complete. Keep required audit/invariant-violation counters separate from optional sampled health events; expose telemetry loss and incomplete windows.

## Metric definitions

Define the population by scope, purpose, query class, time window and serving stage. Default “served” means dispatched model input; assembled-but-unsent is a separate population. A network-uncertain dispatch is explicitly classified. Count each record once per invocation for concentration, even if represented twice in the payload; [MR-03](memory-reliability-03-final-payload-context-budgets.md) separately flags that duplicate rendering defect.

Let `c_i` be delivered occurrences of record i, `C = sum(c_i)` and `p_i = c_i/C`. Top-k share is the sum of the k largest `p_i`, not “the fraction ranked at position k”. HHI is `sum(p_i²)`; Simpson diversity is `1 − HHI`; entropy is `−sum(p_i log2 p_i)`. Normalized entropy requires more than one distinct record. Empty populations return null with counts, not a perfect score.

| Metric | Numerator / denominator or definition |
|---|---|
| Repeat-serving rate | Delivered record occurrences also present in the immediately prior eligible turn of the same task / comparable occurrences |
| Consecutive appearance length | Distribution of contiguous task-turn runs per record, with gaps and task boundaries explicit |
| Low-trust/inferred fan-out | Distinct authorized tasks receiving a record/family in the window; report counts and population size |
| Lifecycle re-entry | Current-mode deliveries violating lifecycle policy at release time / current-mode deliveries |
| Family diversity | Distinct established families per invocation, plus unknown-origin count |
| Sole-support displacement | Selection events dropping the sole admissible independent support for a required claim |
| Arm contamination | Labeled invalid/inappropriate candidates or deliveries attributable to an arm / evaluated candidates or deliveries for that arm |
| Fusion recovery | Labeled requirements missed by the declared baseline arm but satisfied by fused final selection / evaluated baseline misses |

Contamination and recovery require labels or a verifier; production disagreement is not automatically contamination. Historical deliveries are excluded from current-mode stale/superseded leakage metrics. Record-level and version-level concentration are separate views.

## Aggregation and alerts

Begin with query-class and memory-kind baselines. Do not alert merely because an active constraint appears often. Report exposure, independent usefulness and displacement together. Unknown labels, small denominators, sample weights and dropped events appear in both text and JSON outputs. Weighted sample estimates are identified as estimates with uncertainty; exact integrity counters remain exact within their declared coverage.

Health-report access uses the same scoped identity as recall. Cross-scope administrative aggregation requires a dedicated permission and suppresses identifying drill-down by default. A scope label alone is not authentication.

## Implementation slices

1. Extend `server-go/modules/memory/metrics.go` and existing trace/event output; add bounded ingestion, deduplication, retention and loss accounting.
2. Implement tested metric functions and CLI/JSON projections over a fixed event window. Persist aggregate version and population filters.
3. Add baseline-relative alerts and drill-down to authorized selection traces. Keep ranking unchanged during this phase.

## Acceptance gates and rollout

Hand-calculated populations pin top-k/HHI/entropy, empty/single-record behavior, repeats and sample handling. Historical recalls do not trigger stale-current alerts. Retries do not inflate exposure. Unauthorized users cannot discover record/family IDs through health queries. An event overflow produces visible loss rather than a reassuring zero. Measure overhead and retention growth before widening collection; rollback disables optional collection without dropping required receipts.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
