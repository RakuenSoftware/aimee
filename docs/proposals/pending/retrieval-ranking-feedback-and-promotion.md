# Retrieval ranking, grouped feedback and measured promotion

- **State:** pending.
- **Consolidation — 2026-10-08:** remaining work from `learning-to-rank-activation-and-ipw-residual.md`, `kb-hybrid-outcome-wiring-residual.md`, `per-query-feature-persistence-residual.md` and [the completed slice of memory-query-classification-and-fitted-route-weights.md](../done/memory-query-classification-and-fitted-route-weights.md).

## Existing implementation

The whole-token sweep, baseline pairwise objective, IPW weight consumption, per-document overlap, capture/plumbing, offline fitting and fail-closed promotion gate exist. See [learning-to-rank](../done/learning-to-rank-from-interactions.md), [KB outcome wiring](../done/kb-hybrid-outcome-wiring.md) and [query grouping](../done/per-query-grouping-key-for-ranking.md). These do not establish decision-time propensity, durable grouped feature rows or measured live uplift.

## Remaining implementation

1. Scored query classification with calibrated confidence; fitted route weights and deterministic diversity/sufficiency across candidate channels. Retain the whole-token sweep: reject false matches such as `ship` in `relationship`, `fix` in `fixture` and `add` in `address`, while preserving admitted inflections (`s`, `es`, `ed`, `d`, `ing`) and punctuation; apply final authority, budget and coverage guards independently of learned scores.
2. Capture the complete bounded candidate pool or an explicitly sampled pool, rather than only the first eight. At selection time record actual policy probability, sampling decision, ranks/features and outcome linkage; derive capped inverse-propensity weights from observed probabilities, never retrospective guesses.
3. Persist a durable query/retrieval grouping key on feature rows. Backfill only provable groupings; quarantine legacy ungrouped rows. Fitting, diagnostics and retention operate on grouped sets. Newly admitted rows must not produce `missing_grouping_key`. Test concurrent identical queries, retries and cross-tenant separation.
4. Add explicit evaluation-feedback ingestion with authenticated provenance and idempotency; publish missing/invalid propensity and feedback-lag diagnostics. Bot repetition/agreement is not independent evidence or a successful human outcome.
5. Decide activation of `learning_implicit_retrieval_outcome` (currently off), with a volume cap and rollback. Require real time-split held-out data, segment gates and adequate power before promotion. Validate uplift and bias on representative traffic, including conversational memory; do not fit/evaluate on the same query groups.

## Acceptance

Impressions reproduce the actual serving policy and contain grouping, candidate ranks/scores, outcomes, sampling decisions and observed propensity. A known-worse policy scores worse under capped IPW. Held-out gates refuse underpowered evidence, expose segment regressions and support rollback. Compare answer correctness, useful context, unsupported claims, latency and token cost, not just retrieval counts. MR policy qualification remains tracked in [promotion residuals](memory-reliability-promotion-and-adapter-residuals.md).

## Archived source records

- [learning-to-rank-from-interactions.md](../done/learning-to-rank-from-interactions.md).
