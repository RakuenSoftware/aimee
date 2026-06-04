# Code-graph-fusion ablation results

Per-arm results from `aimee memory benchmark code-graph-fusion --arm <name>` run
against a live populated instance (aimee-server `572e80c0` → aimee-kb / DB2),
2026-06-02. Each `arm-<name>.json` is one run of the 108-query production corpus
through `kb_client_memory_find_facts_ex`, with the arm's `graph_code_fusion_state`
applied in aimee-kb. Latency is wall-clock around the kb RPC (includes the hop).

## Latency (single representative run per arm)

| arm | fusion_state | p50 ms | p95 ms | p99 ms |
|---|---|---|---|---|
| baseline | off | 153.0 | 186.7 | 198.2 |
| structural_graph_only | shadow | 151.0 | 190.0 | 217.0 |
| structural_graph_plus_code_vectors | shadow | 153.6 | 187.8 | 196.1 |
| fusion_without_learned_utility | on | 234.8 | 287.0 | 321.9 |
| learned_utility_replay_only | on | 237.0 | 275.2 | 290.7 |
| utility_cap_default | on | 243.5 | 352.4 | 395.0 |
| utility_cap_rebalanced | on | 244.0 | 412.5 | 800.2 |
| code_structural_factor_disabled | on | 233.6 | 284.5 | 319.4 |
| full_fusion | on | 236.6 | 273.3 | 285.1 |

Stabilised baseline vs full_fusion (median of 5 runs each):
baseline p50 156.9 / p95 221.3; full_fusion p50 236.7 / p95 298.5 →
**+50.9% p50, +34.9% p95**.

## AC status from this pass

- **AC#1 (fusion-off latency baseline):** measured; see
  `docs/proposals/benchmarks/effectiveness-weighted-code-vector-graph-baseline.json`.
  (`utility_score_distribution` in that file is still pending; it needs a direct
  DB2 query, no RPC surface yet.)
- **AC#4 (p95 overhead ≤ 25%):** **FAILS**; full_fusion is ~35% over baseline
  p95.
- **AC#5 (per-arm matrix):** latency captured here for all 9 arms.

## Caveats: do NOT read recall from these files

- **`recall_*`, `mrr`, `ndcg_*` are 0** because the corpus is unlabelled
  (`expected_ids: []` for all 108 queries). These artifacts are **latency-only**
  until the corpus is labelled (see `labelled` field).
- **Sub-gate arms are not yet distinct:** only `graph_code_fusion_state`
  (off/shadow/on) is forwarded to aimee-kb; `utility_scoring` /
  `code_projection` are not separately plumbed through the RPC, so `on` arms
  that differ only on those score/measure near-identically (latency differences
  among them are run-to-run noise).
- Shadow arms return the fusion-off order, so their latency tracks baseline.

## Promotion decision (AC#6)

Fused ranking **stays gated off by default**: p95 latency exceeds the 25%
budget and there is no measured recall lift to justify it (corpus unlabelled).
Revisit after labelling shows whether quality justifies revising the threshold.
