# Proposal: learning-to-rank the retrieval stack from real interactions

- **State:** DONE — shipped LTR substrate archived 2026-08-04; activation/IPW residual extracted.

> **Archived after partial delivery.** Retrieval events/outcomes, fused feature persistence, the
> fitter, pairwise weighting, offline serving path, and a measured promotion gate are shipping.
> Default-off label capture, head-only candidate capture, and true propensity logging remain in
> [`learning-to-rank-activation-and-ipw-residual.md`](../pending/learning-to-rank-activation-and-ipw-residual.md).

Written 2026-07-30 out of the retrieval measurement campaign
  ([retrieval-stack-report](../../validation/retrieval-stack-report-2026-07-30.md)).
- **Owner:** unassigned.
- **Depends on:** `kb_bandit` (decision points, reward recording, IPW replay) and
  the FTS lexical leg.

> ## ⚠️ Correction, 2026-07-30 — most of this is already built
>
> This proposal was drafted without auditing the tree, and it materially
> understates what exists. The serving path, the label channel, the fitter, and the
> promotion gate are all in-tree and shipped as
> [kb-hybrid-outcome-wiring](../done/kb-hybrid-outcome-wiring.md) (2026-07-26).
> Corrections to specific claims below:
>
> | Draft claim | Reality |
> | --- | --- |
> | "a retrieval-logging schema that does not yet exist" | `kb_ranker_emit_event` → `retrieval_event`; `kb_ranker_outcome_write` → `ranker_outcome` |
> | dense similarity "available today: **no** — rank is kept, score is discarded" | **Wrong.** `kb_result_t.dense_score`/`.lex_score` are carried through fusion (`src/kb/kb.c:1256`) and persisted by `kb_features_upsert_with_sketch` (`src/kb/kb.c:1859`). The score-discard problem was in the *benchmark harness's* stage-1 artifacts, not in production. |
> | "prior citation count for the doc — needs logging" | outcome verdicts exist per `(event, doc)`; what is missing is aggregation, not capture |
> | S3 "train and evaluate offline" | `src/kb/kb_ranker_fit.c` + the `scripts/rank-fit.py` sidecar, pointwise **and** pairwise objectives |
> | S5 "gated rollout" | `model_write_proposed` → benchmark gate → `kb_ranker_model_commit` |
> | "Not an online-learning system in the first slices" | already true: `kb_service_workers.c` runs the fit periodically, offline |
>
> **What is actually missing**, and it is much narrower than five slices:
>
> 1. `learning_implicit_retrieval_outcome` defaults **off**
>    (`src/modules/config/config_fields.c:297`), so no labels accumulate.
> 2. Capture is wired only into the agent search tool
>    (`src/modules/tools/agent_tools_dispatch.c:1957`) and is capped at **8 docs**
>    of a pool that can hold 100 — so only the head of the ranking is ever labelled.
> 3. IPW propensity logging is unimplemented; the fitter already consumes the
>    weight. (Item 1 of the done proposal's own remaining-work list.)
> 4. ~~The promotion gate was a 5-query fixture with a 1e-6 lift epsilon.~~
>    **Fixed 2026-07-30** — the gate now requires a minimum query count, a
>    shippable mean lift, and a paired per-query win/loss majority. See
>    [the gate section](#the-promotion-gate-fixed-2026-07-30).
>
> The feature set is live and is narrower than the ~20 features imagined below:
> `dense.cos`, `lex.cos`, `temp.recency`, `sketch.frequency_kind_scope`,
> `sketch.distinct_sources_hll` (`FEATURE_KEYS` in `kb_ranker_fit.c`).
>
> **Read the sections below as design rationale, not as a work plan.** The
> remaining work is a config change, a cap, and IPW — not a build.

## Problem

Final ranking is decided by **fixed, untuned rules**. Fusion of the dense and
lexical legs uses Reciprocal Rank Fusion with a textbook constant, and that
constant is measurably wrong for this corpus: sweeping it moved every metric at
once (`k=10` beat `k=60` on R@1 **and** R@10 — 0.3709/0.8984 against
0.3639/0.8642). A hand-picked constant is leaving quality on the table, and no
amount of hand-tuning generalises across corpora, tenants, or query mixes.

Meanwhile the system generates **thousands to hundreds of thousands of
interactions a day**, and none of that signal reaches ranking.

### What the measurement campaign established

Three findings from 10,000-query evaluations bound what is worth building:

1. **Reranking a strong dense ranking does not work.** Across 20 configurations
   and two embedders, the best result was **+0.0032 NDCG@10**; most were
   negative. A cross-encoder's ceiling sits below the ranking it is asked to
   improve, and the effect *worsens* as the embedder improves (GTE's gain halved
   from +0.0032 to +0.0016 when dense went from 0.5909 to 0.6075).
2. **The binding constraint is recall, not ordering.** Dense retrieval missed
   the labelled document entirely for **12.6%** of queries. Adding a lexical leg
   raised pool recall from 0.8735 to **0.9735**. Nothing that merely reorders a
   fixed candidate set can recover those.
3. **Fusion is where the leverage is.** RRF gave **+0.1168 R@10** over dense —
   roughly 35x the best reranker result — using a rule nobody tuned.

**The conclusion this proposal rests on:** the win is not a better model scoring
`(query, document)` text. It is learning how to **weight signals already
computed**. That is a learning-to-rank problem, and it is the one form of
"reranking" the evidence does not rule out.

## Decision

Train a feature-based ranker (gradient-boosted trees, LambdaMART-style) over the
fused candidate set, using **implicit relevance derived from real usage**, and
serve it as the final ordering step.

### Why this is not the reranker we just rejected

| | cross-encoder (rejected) | LTR (proposed) |
| --- | --- | --- |
| Input | `(query, doc)` text | ~20 precomputed features |
| Learns | semantic matching, again | how to weight existing signals |
| Trained on | generic public corpora | **this corpus, these users** |
| Query cost | 143 ms (measured) | microseconds |
| Hardware | GPU | CPU |
| Improves with use | no | **yes** |

### The label: LLM citations, not clicks

For a retrieval-augmented system the natural relevance signal is **which
retrieved chunks the answer actually cited**. If 20 chunks are retrieved and 3
are cited, those 3 are positives and the rest are observed under the same
impression.

This is a better signal than clicks in three ways: it is generated
automatically at full interaction volume; it carries far less position bias,
because the model reads the whole context rather than scanning top-down; and it
reflects usefulness for the actual downstream task rather than the
attractiveness of a snippet.

**It also fixes an evaluation problem we currently have.** The frozen-ab suite
uses *silver* labels with exactly one positive per query out of 26,473
documents, which severely depresses precision metrics (R@1 measured 0.3811 and
is not interpretable as "right 38% of the time"). Interaction-derived labels
would be multi-positive, drawn from the real query distribution, and reflect a
real notion of relevance.

## Non-goals

- **Not** a replacement for the embedder or the lexical leg. LTR ranks their
  union; it does not retrieve.
- **Not** a cross-encoder, and not a revival of one. Feature-based only.
- **Not** an online-learning system in the first slices. Offline training with
  periodic promotion, gated by the existing evidence pipeline.

## Feature set (all available or cheap)

| feature | source | available today |
| --- | --- | --- |
| dense similarity score | retrieval | **no — rank is kept, score is discarded** |
| lexical/BM25 score | FTS leg | yes |
| rank in each leg, RRF rank | fusion | computed |
| doc_kind, length, age | corpus | yes |
| project / workspace match | request scope | yes |
| prior citation count for the doc | usage history | **no — needs logging** |
| query length, term rarity | query | derivable |

**The blocking gap is logging, not learning.** Note the first row: the retrieval
path keeps candidate *ranks* but discards the underlying *scores*. This exact
gap blocked score-fusion analysis during the measurement campaign, and it would
make LTR untrainable after the fact. Persisting scores should land regardless of
whether this proposal proceeds — training data cannot be recovered
retroactively.

## Failure model

- **Feedback loop.** The ranker trains on impressions it produced, so documents
  it never surfaces can never be learned. Mitigation: a small randomised
  exploration fraction plus **inverse propensity weighting** — machinery
  `kb_bandit` already has (`tools/bandit_replay.py`, IPW replay recorded as
  benchmark evidence).
- **Position bias.** Weaker than with human clicks but non-zero; LLMs show
  primacy and recency effects over long contexts. Same IPW mitigation.
- **Attribution is not citation.** An uncited chunk may still have been
  necessary context. Treating uncited as a hard negative teaches wrong lessons;
  weight rather than binarise, and treat "no citations at all" as an unusable
  impression rather than 20 negatives.
- **Tenant skew.** A ranker trained mostly on one workspace's traffic may
  degrade others. Evaluate per-segment before promotion.
- **Silent regression.** The dominant failure mode observed throughout the
  measurement campaign was plausible-but-wrong output. Any promotion must be
  gated on measured metrics, never on "the model trained successfully".

## Bounded slices

**S1 — log what training needs (no model).** Persist per-impression: query,
candidate ids, per-leg scores *and* ranks, fusion rank, doc features, and the
citation outcome. Ship behind a flag; verify volume and cardinality. *This slice
has standalone value and unblocks everything else.*

**S2 — offline dataset + baseline replay.** Build a training set from logged
impressions. Reproduce current production ordering as a baseline, and confirm an
IPW replay of a *known-worse* ranking scores worse. Establishes the harness is
sensitive before any model is trusted.

**S3 — train and evaluate offline only.** LambdaMART/XGBoost ranker. Report
NDCG@10 and Recall@{1,5,10,20} against the fixed-rule fusion baseline, per
segment. No serving.

**S4 — shadow serving.** Score live traffic, log the ranking, serve the existing
order. Compare offline predictions against observed citations.

**S5 — gated rollout.** Promote through the existing bandit decision-point
machinery with exploration, per-tenant guardrails, and automatic rollback on
metric regression.

## Acceptance checks

**Mechanical**
- Logging writes one row per retrieved candidate per impression, with scores
  present and non-null for every leg.
- Replay of production's own ranking reproduces production metrics within noise.
- Ranker inference over 50 candidates stays under **1 ms** on CPU (the rejected
  cross-encoder was 143 ms; anything approaching that is a design failure).

**Integration**
- Offline NDCG@10 and R@10 beat tuned-RRF fusion on held-out impressions, split
  by time (train on earlier, test on later) so improvement is not memorisation.
- No segment regresses by more than a stated tolerance.
- Shadow-mode predictions correlate with observed citations before any rollout.

## Open questions

- Is citation the right label, or should "answer quality" (however scored) be
  the target, with citation as an intermediate?
- How much traffic is needed before the ranker beats tuned RRF? Tuning RRF is
  nearly free, so it is the honest baseline — not untuned RRF.
- Should ranking be per-tenant, or global with tenant features?
- Does exploration cost enough answer quality to matter to users?

## The promotion gate (fixed 2026-07-30)

The gate that authorises a fitted model into production was the weakest link in the
whole stack, and it was measured rather than assumed.

**What it was.** `kb_ranker_fit_run` scored candidate and incumbent weights on a
fixture and committed if `ndcg_cand > ndcg_incumbent + 1e-6`. The default fixture is
`benchmarks/rank/kb_hybrid/queries.json`: **5 queries, 2–4 candidates each**. The
gate's own unit tests used a **1-query** fixture.

**Why that could not work.** A sweep of `rrf_k` over that fixture moves nothing:

| rrf_k | 1 | 5 | 10 | 20 | 60 | 120 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| NDCG@5 | 0.7000 | 0.7000 | 0.7000 | 0.7000 | 0.7000 | 0.7000 |

Every candidate appears in both legs, so pool recall is 1.0 by construction and the
fusion constant cannot reorder anything. The fixture is blind to the largest effect
the measurement campaign found. And at 1e-6, "beats the incumbent" meant "is not
bit-identical to the incumbent" — on a 5-query fixture NDCG@5 is quantised, so one
query's reordering promoted a model.

**What it is now.** Three conditions, all required, all recorded in the
`benchmark_trace` artifact:

1. **`n_queries >= 30`** (`RANK_FIT_MIN_BENCH_QUERIES`) — otherwise
   `benchmark_underpowered`, and the model is held `proposed`. An underpowered gate
   fails closed: it cannot detect a regression, so it must not authorise one.
2. **mean lift > `1e-3`** (was `1e-6`) — below any effect worth shipping, but far
   above the floating-point noise the old value actually measured.
3. **paired win/loss majority** — candidate and incumbent score the *same* query
   before moving on, and `wins > losses` is required. A mean alone cannot separate
   "better on most queries" from "much better on one and worse on the rest"; the
   latter is what overfitting a small fixture looks like. Failing only this
   condition reports `no_paired_majority`, distinct from `no_lift`.

The trace now also records `n_queries`, `wins`, `losses`, `ties`, `min_queries`, and
`lift_epsilon`, so a reader can tell a decision backed by a real fixture from one
backed by five hand-authored queries. The previous payload could not express the
difference.

**Consequence, deliberately accepted.** The shipped default fixture now refuses
every promotion. That is the honest state: there is no real gate fixture in-tree, and
the previous behaviour was not "gating" but "promoting with a rationalisation".
`kb_ranker_enabled` defaults off, so nothing in production changes. Supplying a real
held-out fixture via `kb_ranker_fit_benchmark` is now a prerequisite for the ranker,
which is the correct ordering.

**Tests.** `test_ranker_fit.c` gained `gate_refuses_underpowered_benchmark` (a model
that wins its single query outright is still refused — refusal is on power, not
merit) and `gate_refuses_minority_gain` (+0.0998 mean NDCG from 12 wins against 20
losses is refused). The two pre-existing gate tests were moved off the 1-query
fixture onto a generated 32-query one, so they now exercise the real gate.

## Prior art in-tree

`kb_bandit` already learns a retrieval decision from outcomes
(`kb_bandit_recall_sufficiency_reward` tunes the memory retrieval limit) and
already records IPW replay results into the evidence stream. This proposal is
that same pattern applied to ordering rather than to a scalar, and should reuse
the decision-point, reward, and replay plumbing rather than inventing new.
