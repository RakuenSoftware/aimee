# Proposal: learning-to-rank the retrieval stack from real interactions

- **State:** 🟡 **PENDING — problem and design sketched, not started.** Written
  2026-07-30 out of the retrieval measurement campaign
  ([retrieval-stack-report](../../validation/retrieval-stack-report-2026-07-30.md)).
- **Owner:** unassigned.
- **Depends on:** `kb_bandit` (decision points, reward recording, IPW replay),
  the FTS lexical leg, and a retrieval-logging schema that does not yet exist.

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

## Prior art in-tree

`kb_bandit` already learns a retrieval decision from outcomes
(`kb_bandit_recall_sufficiency_reward` tunes the memory retrieval limit) and
already records IPW replay results into the evidence stream. This proposal is
that same pattern applied to ordering rather than to a scalar, and should reuse
the decision-point, reward, and replay plumbing rather than inventing new.
