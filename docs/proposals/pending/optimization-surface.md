# Proposal: aimee optimization surface — assemble the measure→optimize→promote loop

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter role(s):** learning / self-improvement (no new store, no new DB
  tier — reuses the existing bandit decision-log tables in DB2, the memory
  benchmark RPC, and the learning/calibration config surface).
- **Scope:** `src/db2/bandit.c` + `src/db2/bandit.h` (decision-log accessors —
  extend, do not rewrite), `src/kb/kb_bandit.c` (arm selection + the **missing
  reward-closure path**), `src/kb/kb_service_memory.c` (the one live sample site),
  `src/kb/kb_intel_payload.c` (export — currently reports a decision point that is
  never sampled), `src/config_learning.c` (new decision-point registry + reward
  config), `src/server/server_memory_benchmark.c` (`memory.benchmark` RPC — first
  generalise it beyond its current `code-graph-fusion` handler), delegate
  selection/invocation code (first *new* decision point: delegate routing), a new
  thin-client command, unit tests, docs. No new long-lived service.

## Summary

aimee already has most primitives for a closed-loop optimizer — a
Thompson-sampling contextual bandit with propensity logging, IPW weighting, an
exploration-budget gate, and counterfactual replay export
(`db2_bandit_decisions_export`); a benchmark RPC with `baseline.json` regression
gating; and self-calibrating guardrail thresholds. They are **scattered expert
tools, not a product loop** — and, critically, the loop is **not actually closed
in production**: exactly one decision point is live-sampled, its reward is never
observed, and the introspection surface reports a *different*, unsampled decision
point (see *Current implementation constraints*). This proposal closes the reward
loop, assembles the pieces behind a single surface, and generalises the
caller-facing surfaces to multiple decision points, so a candidate config change
can be **baselined → evaluated → promoted** with statistical gates instead of by
hand.

```
  register variants (arms) ──▶ measure ──────────────▶ promote (guarded)
   per decision point          ├─ offline: benchmark suite adapter         │
   (retrieval limit/top-k,     ├─ online:  live bandit + exploration        │
    delegate routing,          └─ off-policy: replay logged decisions (IPW) │
    fusion mode, …)                       │   ⚠ needs reward closure first   │
                                          ▼                                  │
                               regression gate (baseline.json) +            │
                               calibration credible interval ───────────────┘
```

## Motivation

The closed-loop optimizer is a well-known pattern: baseline a candidate change,
evaluate it on a fixed suite, promote the best, repeat. Demo-grade
implementations of it are not worth importing — their "optimizers" typically
enumerate a handful of hardcoded prompt strings scored by a brittle substring
match over a few examples, or build variant configs that are never actually
applied to the model. The useful import is the **product framing**, not the code:
aimee already has stronger primitives, they are just scattered, mostly default-off,
and not yet wired into a loop that observes its own rewards.

What aimee already has (verified):

| Primitive | Where | State |
| --- | --- | --- |
| Contextual bandit (Thompson, propensity, IPW cap, exploration-budget gate, off-policy export) | `src/db2/bandit.c`, `src/kb/kb_bandit.c` | DB/helpers are generic by `decision_point`; **one** point is live-sampled (`kb_memory_retrieval_limit`), gated by `bandit_live_decision_enabled` (default **off**); **reward closure is unwired** |
| Offline eval + regression gate | `memory.benchmark` RPC (`server_memory_benchmark.c`), `baseline.json` gate (`cmd_memory_embed.c`), LLM-judge track (`benchmarks/judge-calibration`, `run-llm.sh`) | real, but split: CLI supports multiple benchmark modes; RPC currently accepts only `code-graph-fusion` |
| Self-calibrating thresholds | `config_learning.c` (conformal ε, credible δ, prior α/β, `tau_*`) | real |
| Trajectories / hard-negatives (reward inputs) | `trajectory_export.c`, `memory_hard_negative_log` | real |
| Typed artifact storage (variant provenance) | `db2_artifact_gen_id`, `learning_evidence.c` | real |

The gap is assembly **and one missing keystone (reward attribution)**, not
invention. Much of the loop is built; it is default-off, partially hard-coded at
the caller-facing surfaces, missing the benchmark/suite adapter that would make
promotion repeatable, and — most importantly — never observes the outcome of the
decisions it logs.

## Current implementation constraints

This proposal should not hide the work behind "already generic". The bandit
subsystem is more skeletal in production than its API suggests:

- **The reward loop is open.** `kb_bandit_reward()` / `db2_bandit_decision_close()`
  exist but are **never called in production** (only in tests). The one live
  sampler logs a decision and never records an outcome, so posteriors never
  update from live traffic, and `db2_bandit_decisions_export` returns rows with a
  null/unclosed `reward`. **Off-policy replay — the proposal's "default first
  pass" — is inert until decisions are closed.** This is P1's first deliverable,
  not a detail.
- **The one live decision point is `kb_memory_retrieval_limit`, not
  `kb_fusion_mode`.** The only live `kb_bandit_sample()` call is in
  `kb_service_memory.c` — a 2-arm `{10, 20}` retrieval-limit *shadow* bandit. This
  is effectively the `retrieval_topk` listed below as "future work"; it already
  exists and is the correct proving ground.
- **`kb_fusion_mode` is a static config knob, not a bandit.** It defaults to
  `"rrf"` (`config_learning.c`) and is read directly as the fusion mode in
  `kb.c`. It is **never** passed to `kb_bandit_sample`. Yet
  `kb_intel_bandit_export_response()` advertises arm stats for `kb_fusion_mode`
  (fixed `rrf` / `static_alpha` / `dynamic_alpha` arms) — a **phantom export**:
  it reports a decision point that has no live decisions, while the point that
  *is* sampled (`kb_memory_retrieval_limit`) is not exported. Step 0 is to make
  the export reflect reality (either sample `kb_fusion_mode` for real, or export
  the live point).
- **`memory.benchmark` is not yet a general suite runner.** The server RPC
  rejects anything except `code-graph-fusion`; the broader benchmark modes live
  in the CLI-side `memory benchmark` code.
- **`delegate_routing` is broader than `delegate_role.c`.** That file handles
  role canonicalisation and role-policy defaults; the actual decision point must
  sit where an invocation chooses an agent/persona/cost tier.
- **Coordinate with `feat/p4-graph-audit`.** Local work adds a graph-derived
  `code.audit` surface. If this proposal lands after it, treat code-audit/report
  enrichment as another measurable evaluation surface or explicitly defer it.

### Standalone fix-it: the phantom bandit export

The third constraint above is a real current-code wart worth fixing on its own,
**independent of whether the larger optimization surface is built**:

> `kb_intel_bandit_export_response()` (`src/kb/kb_intel_payload.c`) advertises arm
> stats for `kb_fusion_mode` with fixed `rrf` / `static_alpha` / `dynamic_alpha`
> arms — but `kb_fusion_mode` is never passed to `kb_bandit_sample`, so it has
> **no live decisions**. Meanwhile the decision point that *is* sampled
> (`kb_memory_retrieval_limit`, in `src/kb/kb_service_memory.c`) is **not**
> exported. The introspection endpoint — and `aimee kb`'s "Bandit export" line
> (`cmd_kb.c`) — therefore reports a decision point that does not exist at runtime
> and hides the one that does.

**Minimal fix (no new infrastructure):** make the export reflect reality — emit
the decision point(s) actually present in the bandit decision log (the distinct
`decision_point` values, via `db2_bandit_*`), instead of a hard-coded
`kb_fusion_mode` literal. This is a small, self-contained correctness change that
can ship as its own PR, and it doubles as **Step 0** for the registry work in §1:
once the export is data-driven, adding decision points is purely additive. It
should carry a unit test asserting the export lists only points that have logged
decisions.

## Design

### 0. Close the reward loop (prerequisite)

Before any registry or CLI work, wire outcome attribution: a sampled decision
(`decision_id` from `kb_bandit_sample`) must be closed with an observed reward via
`kb_bandit_reward` → `db2_bandit_decision_close`, at the point where the turn's
outcome is known. Until this exists, every downstream mode (replay, online,
promotion) has nothing to learn from. Start with `kb_memory_retrieval_limit`,
whose outcome (a recall hit/miss / downstream answer quality) is already logged in
trajectories and hard-negatives.

### 1. Decision-point registry (generalise the bandit)

`db2_bandit_*` already keys arm stats by a free-form `decision_point` string, so
the storage layer needs little new infrastructure. Promote the hard-coded cases
in the caller surfaces to a small **registry** of decision points, each
declaring: arm set, context features (for `context_hash`), reward function name,
benchmark adapter, and promotion gate. Seed decisions:

- `kb_memory_retrieval_limit` (retrieval top-k) — the **only** point already
  live-sampled (2-arm shadow bandit); lowest-risk proving ground once reward
  closure exists.
- `kb_fusion_mode` — currently a static config knob with a reporting-only
  "bandit" export; convert it to a real sampled decision (and retire the phantom
  export) as the first conversion of a static knob into a tuned arm.
- `delegate_routing` — which delegate/persona/cost-tier handles a sub-task (today
  static policy distributed across role policy, agent config, and launch paths;
  high upside once the loop is proven).
- `briefing_style` — compact vs evidence-heavy session briefing.
- `guardrail_strictness` — strict vs balanced threshold arm.

Arms and prompt/config variants are stored as **typed artifacts** (reuse
`db2_artifact_gen_id`) with a version, not as ephemeral strings.

### 2. Reward signal (named per surface, not hand-waved)

The hardest part, and the one most loop sketches skip. Each decision point
declares a reward composed from inspectable signals: eval/benchmark outcome,
latency, cost when available, and explicit penalties such as hard-negatives. Do
not assume every term is available for every surface; the registry declares which
terms are required, optional, or unavailable. The reward closes the bandit
decision via `db2_bandit_decision_close` (see §0).

### 3. Three measurement modes (one surface, explicit regimes)

Offline batch and online learning are distinct regimes that are easily conflated;
aimee does both and should keep them separate:

- **Offline suite** — run a fixed suite via a benchmark adapter. First generalise
  the current `memory.benchmark` RPC beyond `code-graph-fusion`, or route the
  optimizer through the existing CLI benchmark harness until the RPC catches up.
- **Online** — enable live exploration behind the existing
  `bandit_live_decision_enabled` flag; the exploration-budget gate already caps
  exploration traffic.
- **Off-policy replay** — score a *candidate* policy on already-logged decisions
  via `db2_bandit_decisions_export` + IPW, with **no live traffic and no
  re-run**. This is a strong edge most implementations lack, and should be the
  default first pass before spending live traffic — but it is **only meaningful
  once decisions carry rewards** (see §0).

### 4. Promotion gate (statistical, not argmax)

"Rank and promote best" is naive. Promotion must clear the **existing**
`baseline.json` regression gate **and** a calibration credible-interval check
(`config_learning.c`) before a candidate becomes the default arm. Promotion writes
rollback metadata (previous arm + decision id) as a typed artifact.

### 5. Thin-client surface (avoid the `lab` name collision)

`aimee lab` / `kb_lab` **already exists** — it is the *ingest* lab (chunk-quality
audit, `src/kb/kb_lab.c`). Do **not** reuse `lab`. Proposed verb: `aimee optimize`
(or `aimee experiment`):

```
aimee optimize points
aimee optimize baseline   --point kb_memory_retrieval_limit
aimee optimize variants   --point kb_memory_retrieval_limit --register <artifact>
aimee optimize replay     --point kb_memory_retrieval_limit   # off-policy, IPW
aimee optimize run        --point kb_memory_retrieval_limit --suite code-graph-fusion
aimee optimize compare    --baseline <id> --candidate <id>
aimee optimize promote    --candidate <id> --guarded          # gated
```

### What this deliberately does **not** do

- No demo-optimizer code, branding, or unsupported sample-efficiency claims.
- No heavyweight experiment-tracking runtime dependency. aimee has
  `trajectory_export.c`; emit a standard, **export-only** tracing format.
- No new DB tier or long-lived service.

## MVP

Ship one decision point end-to-end: **`kb_memory_retrieval_limit`**. It is the
only point already live-sampled, so it is the best proving ground for the reward
loop, registry, export, replay, benchmark, compare, and guarded-promote
mechanics. Then add **`delegate_routing`** as the first genuinely new decision
point.

1. **Close the reward loop** for `kb_memory_retrieval_limit` (§0) — attribute the
   turn outcome back to the logged decision. Without this, steps 4–6 learn
   nothing.
2. Register `kb_memory_retrieval_limit` with its existing `{10, 20}` arms; fix the
   `kb_intel` export so introspection reports the point that is actually sampled.
3. Generalise bandit export/introspection to accept `--point` and discover arms
   from `policy_arm` artifacts or the registry.
4. Generalise benchmark execution enough to run `code-graph-fusion` through the
   optimizer path.
5. **Off-policy replay** (rank arms on now-rewarded logged decisions, zero live
   cost).
6. Then enable live exploration behind `bandit_live_decision_enabled`, and promote
   behind the `baseline.json` regression + credible-interval gate, with rollback
   metadata.

This is assembly of existing parts, but the assembly includes real API work at the
**reward-closure**, export, benchmark, and promotion boundaries.

## Phasing

- **P1** — close the reward loop for `kb_memory_retrieval_limit`; decision-point
  registry + reward config; fix the phantom export; `aimee optimize
  points|baseline|replay`. (Off-policy only; no new live traffic.)
- **P2** — offline `aimee optimize run --suite code-graph-fusion`; `compare`.
- **P3** — online exploration for `kb_memory_retrieval_limit`; `promote --guarded`
  with rollback metadata.
- **P4** — convert `kb_fusion_mode` from a static knob to a sampled decision; add
  `delegate_routing`; then fan out to `briefing_style`, `guardrail_strictness`
  (the last two are the optimisation surface exposed by the companion proposal,
  [ingest-restoration-and-recall-contract.md](ingest-restoration-and-recall-contract.md)).

## Risks

- **Open reward loop is the top risk.** Until decisions are closed with observed
  outcomes, replay/online/promotion are all inert — fix it first (§0).
- **Reward mis-specification** dominates outcomes — keep rewards inspectable and
  versioned; validate offline before any live traffic.
- **Exploration cost** in production — already bounded by the exploration-budget
  gate; keep `bandit_live_decision_enabled` opt-in per decision point.
- **Decision-point sprawl** — the registry must be a small, reviewed set, not an
  open registration surface.

## Relationship to the companion proposal

[ingest-restoration-and-recall-contract.md](ingest-restoration-and-recall-contract.md)
introduces two new tunable thresholds — *repair-vs-reject* and
*verbatim-vs-synthesize*. Those are registered here as decision points, so the two
proposals compose: the restoration proposal supplies *new things to optimise*;
this proposal supplies the *loop that tunes them safely*.
