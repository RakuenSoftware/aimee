# Proposal: Learning-to-rank weight fitting — close the loop on the KB-hybrid ranker

- **State:** PENDING — design only. Completes the one unshipped half of the
  statistical ranking substrate: the ranker *infers* with a learned linear model
  today, but nothing ever *fits* it from data.
- **Author:** JBailes
- **Date:** 2026-07-07
- **Charter roles:** Rank-Fuse (learned lexical/dense/recency/sketch blend
  replacing hand-set weights), Calibrate (fit weights from observed retrieval
  outcomes, not declared priors), Evaluate-Optimize (offline fit + benchmark gate
  before any default flip), Gate-Promote (the fitted model lands as a
  promotion-gated `ranker_model` artifact, never a live-edited constant).

## Thesis

The KB-hybrid ranker is built as a *learned* linear model and shipped with every
part of the loop **except the learning**:

- **Inference is live.** `kb_ranker_rerank_with_sketch` (`src/kb/kb_ranker.c:53`)
  scores each candidate as
  `w_dense·dense + w_lex·lex + w_recency·recency + w_sketch_frequency·f + w_sketch_distinct·d`
  and reorders. The model is a `ranker_model` artifact
  (`kind='ranker_model'`, `target_surface='kb_hybrid'`, `feature_set_version='v1'`,
  `model_kind='linear'`) loaded by `kb_ranker_model_load` and persisted by
  `kb_ranker_model_write` (`src/kb/kb_ranker.c:107,158`), promoted through the
  ordinary `proposed → committed` artifact state machine.
- **Features are computed and persisted.** `kb_features_upsert_with_sketch`
  (`src/kb_features.h`) writes a `feature_rows` row per candidate with
  `dense.cos`, `lex.cos`, `temp.recency`, `sketch.frequency_kind_scope`,
  `sketch.distinct_sources_hll` (feature set `v1`), plus `mdl.*` on synthesis
  candidates.
- **Labels are captured.** The `retrieval_event` / `retrieval_outcome` tables
  (shipped by *Outcome-Driven Demotion and Poison Resilience*) already attribute
  downstream outcomes back to the rows a query surfaced, keyed by
  `retrieval_event_id`.

What is missing is the single arrow that connects them: **no code reads
`feature_rows ⋈ retrieval_outcome` and fits `w_*`.** The weights are the hand-set
literals `{w_dense=0.6, w_lex=0.4, w_recency=0, w_sketch_frequency=0,
w_sketch_distinct=0}` (`src/kb/kb_ranker.c:32`); `kb_ranker_model_load` even
*re-hardcodes* those same defaults before reading the artifact
(`src/kb/kb_ranker.c:188`). `kb_ranker_model_write` accepts a `weights_json` blob
from a caller that does not exist. The two sketch weights ship pinned at `0.0`, so
the frequency/distinct-source features are computed, stored — and never used.

This is the *Calibrate* half of the statistical decision substrate, exactly
analogous to *Bayesian Calibration of Promotion Thresholds* (which replaced static
`threshold.*` config with fitted `calibration_profile` artifacts). Here we replace
static `ranker_model` weights with **fitted** `ranker_model` artifacts.

## Goal

A promotion-gated fitting loop that turns accumulated `feature_rows` +
`retrieval_outcome` into a `ranker_model` artifact — measurably better than the
`{0.6, 0.4}` default on the benchmark suite, or it does not promote.

1. A **fitter sidecar** that reads the joined feature/outcome view and emits a
   `weights_json` for `kb_ranker_model_write`.
2. **Promotion discipline:** the fitted model enters `proposed`, is benchmark-gated,
   and only a passing model is `committed` — the same gate every other artifact
   crosses. Default weights remain the fallback when no committed model beats them.
3. **Honest rollout:** ship fitting behind a flag, prove lift on the benchmark
   suite before the loaded model is allowed to displace the default. If it does not
   beat `{0.6, 0.4}`, it stays bench-only — the precedent set by *Dynamic Alpha
   Fusion* (shipped, `rrf` never flipped to default).

## §0 What already exists (so we don't rebuild it)

| Piece | Where | Status |
| --- | --- | --- |
| Linear inference + reorder | `kb_ranker_rerank_with_sketch` | shipped |
| Model artifact write/load | `kb_ranker_model_write` / `_load` | shipped |
| `ranker_model` artifact schema + promotion states | `db2_artifact_write`, `proposed`/`committed` | shipped |
| Per-candidate feature rows | `kb_features_upsert_with_sketch`, `feature_rows` | shipped |
| Feature-set versioning | `KB_FEATURE_SET_VERSION="v1"` | shipped |
| Outcome labels + attribution | `retrieval_event` / `retrieval_outcome`, `retrieval_event_id` | shipped |
| Benchmark harness + suite | `benchmarks/suite/`, `benchmarks/catalog.toml` | shipped |
| Python-sidecar pattern | `scripts/embed-minilm.py`, `scripts/mcts-planner.py`, `scripts/guardrails-semantic.py` | shipped |

The fitter is the only new moving part; everything it consumes and everything that
consumes *it* is already in place.

## §1 The training view

Define a read-only join the fitter consumes, one row per (query, candidate):

- **features:** the `v1` vector from `feature_rows` for the candidate
  (`dense.cos`, `lex.cos`, `temp.recency`, `sketch.frequency_kind_scope`,
  `sketch.distinct_sources_hll`).
- **label:** derived from `retrieval_outcome` for the row's `retrieval_event_id`
  (used-in-answer / cited / positively-attributed = 1, surfaced-but-unused = 0;
  a pairwise variant orders within a query by outcome strength).
- **grouping key:** `retrieval_event_id` (the query), so pairwise/listwise
  objectives respect query boundaries.

The join is materialized in SQL (no model code touches raw tables); the sidecar
receives it as a JSON batch on stdin, mirroring the existing sidecars' stdio
contract.

## §2 The fitter sidecar

`scripts/rank-fit.py` (name TBD), same stdio-JSON shape as the other sidecars:

- **input:** `{feature_set_version, objective, rows:[{features, label, group}]}`.
- **fit:** start with **pointwise logistic / ridge** on the five `v1` features
  (matches `model_kind='linear'`; interpretable; no new inference code needed).
  Pairwise (LambdaMART-style) is a later objective behind the same
  `objective` field — the artifact schema already carries `"objective":"pointwise"`.
- **output:** `{weights:{"dense.cos":…, "lex.cos":…, "temp.recency":…,
  "sketch.frequency_kind_scope":…, "sketch.distinct_sources_hll":…},
  fit_metrics:{…}}` — feature-keyed exactly as `kb_ranker_model_load` parses.
- **discipline:** refuse to emit when the feature-set version in the batch does not
  match `KB_FEATURE_SET_VERSION`, or when fewer than a floor number of
  labelled groups exist (avoid overfitting a thin log). Refusal → keep the
  default; never silently ship a garbage model.

The sidecar is CPU-only and dependency-light (numpy/scikit or a hand-rolled
closed-form ridge), consistent with the CPU-first curator profile.

## §3 The fit → promote command

A `aimee kb ranker fit` CLI (and matching `/v1/intelligence/ranker/fit` for the
service) that:

1. materializes the §1 view,
2. runs the §2 sidecar,
3. writes the result via `kb_ranker_model_write` (lands `proposed`),
4. runs the benchmark suite's recall track against the proposed model,
5. **commits only on lift** over the current committed model (or the `{0.6,0.4}`
   default when none is committed), recording the delta as a `benchmark_trace`
   artifact — the same evidence trail *Contextual Bandits* uses for replay.

Refit is keyed on `feature_set_version` and re-triggered on a feature-set bump,
mirroring the prompt/model-version-keyed refit in *Bayesian Calibration*.

## Phasing (each independently shippable; behavioural steps default-off)

1. **View + read path.** Materialize the §1 training view and a
   `aimee kb ranker export-view` dump. No model change. Pure observability into
   whether enough labelled data exists to fit at all.
2. **Fitter sidecar + `fit` command, proposed-only.** Produce `ranker_model`
   artifacts in `proposed`; never auto-commit. Operator inspects.
3. **Benchmark-gated commit.** Wire the recall track as the promotion gate;
   commit on lift, record the `benchmark_trace`. Default weights still win when no
   proposed model beats them.
4. **Scheduled refit + feature-set-bump invalidation.** Fold into the existing
   scheduled-maintenance cycle; refit on `v1 → v2` feature bumps.

## Non-goals

- No new features. This fits weights over the **existing** `v1` feature set; adding
  features is a feature-set version bump, out of scope here.
- No new inference engine. `score_candidate` stays a linear dot product;
  non-linear models would be a separate `model_kind` and are not proposed.
- No default flip by fiat. If fitting does not beat `{0.6, 0.4}` on the suite, the
  ranker keeps the default and this ships bench-only — that is an acceptable
  outcome, not a failure to paper over.

## Risks / honest limits

- **Thin-log overfit.** Early on there may be too few labelled groups to fit
  responsibly; §2's floor + refusal is the guardrail, but it means the loop may sit
  idle (correctly) for a while. This is a validation-pending property until real
  outcome volume exists.
- **Outcome-label bias.** `retrieval_outcome` labels are themselves downstream of
  the *current* ranking (position bias). Pointwise fitting inherits that bias; the
  pairwise/IPW-corrected objective is the mitigation and is deferred to a later
  phase, not claimed now.
- **Feature staleness.** `feature_rows` are written at retrieval time; a refit over
  a mixed-version corpus must filter on `feature_set_version` (enforced in §2).

## Tests

- Fixture-backed fitter: a synthetic `feature_rows ⋈ retrieval_outcome` batch with
  a known-separable signal → asserts fitted weights recover the planted ordering
  and that `kb_ranker_model_load` round-trips the emitted `weights_json`.
- Refusal paths: version mismatch and below-floor group count both return the
  default, not a model.
- Promotion gate: a proposed model that loses on the benchmark fixture is **not**
  committed; one that wins is, and a `benchmark_trace` is recorded.

## Open questions for the roundtable

- Pointwise-first vs. pairwise-first — is position bias severe enough at our scale
  to justify the harder objective in phase 2 rather than phase 4?
- Should the two sketch weights (pinned `0.0` today) be unlocked only once a fitted
  model exists, or is a hand-set nonzero prior worth trying independently as a
  cheaper bench experiment?
- Reuse `/v1/intelligence/*` namespace and the bandit `replay-record` evidence
  path, or a dedicated ranker surface?

## Provenance note

The shipped headers `src/kb/kb_ranker.c:3` and `src/kb_features.h` both cite
`docs/proposals/accepted/statistical-decision-systems-for-ranking-calibration-and-experiments.md`,
which is **absent from the tree** (`docs/proposals/accepted/` is empty). This
proposal is the fitting half of that missing design, re-derived from the shipped
code rather than the lost document. Reconciling the dangling `accepted/` references
is tracked in the `docs/PROPOSALS.md` refresh, not here.
