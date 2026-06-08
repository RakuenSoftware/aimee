# Proposal: aimee optimization surface — assemble the measure→optimize→promote loop

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter role(s):** learning / self-improvement (no new store, no new DB
  tier — reuses the existing bandit decision-log tables in DB2, the memory
  benchmark RPC, and the learning/calibration config surface).
- **Scope:** `src/db2/bandit.c` + `src/db2/bandit.h` (decision-log accessors —
  extend, do not rewrite), `src/kb/kb_bandit.c` (arm selection — generalise off
  the single `kb_fusion_mode` decision point), `src/config_learning.c` (new
  decision-point registry + reward config), `src/server/server_memory_benchmark.c`
  (`memory.benchmark` RPC — reuse as the offline suite runner), `src/server/delegate_role.c`
  (first new decision point: delegate routing), a new thin-client command, unit
  tests, docs. No new long-lived service.

## Summary

aimee already has every primitive for a closed-loop optimizer — a live
Thompson-sampling contextual bandit with propensity logging, IPW weighting, an
exploration-budget gate, and counterfactual replay export
(`db2_bandit_decisions_export`); a benchmark RPC with `baseline.json` regression
gating; and self-calibrating guardrail thresholds. They are **scattered expert
tools, not a product loop**, and the online half is wired to exactly **one**
decision point (`kb_fusion_mode`). This proposal assembles them behind a single
surface and generalises the bandit to multiple decision points, so a candidate
config change can be **baselined → evaluated → promoted** with statistical gates
instead of by hand.

```
  register variants (arms) ──▶ measure ──────────────▶ promote (guarded)
   per decision point          ├─ offline: memory.benchmark suite         │
   (delegate routing,          ├─ online:  live bandit + exploration       │
    retrieval top-k, …)        └─ off-policy: replay logged decisions (IPW) │
                                          │                                 │
                                          ▼                                 │
                               regression gate (baseline.json) +            │
                               calibration credible interval ──────────────┘
```

## Motivation

The closed-loop optimizer is a well-known pattern: baseline a candidate change,
evaluate it on a fixed suite, promote the best, repeat. Demo-grade
implementations of it are not worth importing — their "optimizers" typically
enumerate a handful of hardcoded prompt strings scored by a brittle substring
match over a few examples, or build variant configs that are never actually
applied to the model. The useful import is the **product framing**, not the code:
aimee already has stronger primitives, they are just scattered and mostly
default-off.

What aimee already has (verified):

| Primitive | Where | State |
| --- | --- | --- |
| Contextual bandit (Thompson, propensity, IPW cap, exploration-budget gate, off-policy export) | `src/db2/bandit.c`, `src/kb/kb_bandit.c` | **real, but wired to one decision point** (`kb_fusion_mode`); `bandit_live_decision_enabled` defaults **off** |
| Offline eval + regression gate | `memory.benchmark` RPC (`server_memory_benchmark.c`), `baseline.json` gate (`cmd_memory_embed.c`), LLM-judge track (`benchmarks/judge-calibration`, `run-llm.sh`) | real |
| Self-calibrating thresholds | `config_learning.c` (conformal ε, credible δ, prior α/β, `tau_*`) | real |
| Trajectories / hard-negatives (reward inputs) | `trajectory_export.c`, `memory_hard_negative_log` | real |
| Typed artifact storage (variant provenance) | `db2_artifact_gen_id`, `learning_evidence.c` | real |

The gap is assembly, not invention. ~70% of the loop is built; it is default-off
and routed to a single decision.

## Design

### 1. Decision-point registry (generalise the bandit)

`db2_bandit_*` already keys arm stats by a free-form `decision_point` string, so
adding decisions is near-zero new infrastructure. Promote the implicit
`kb_fusion_mode` case in `kb_bandit.c` to a small **registry** of decision points,
each declaring: arm set, context features (for `context_hash`), and a reward
function name. Seed decisions:

- `delegate_routing` — which delegate/persona/cost-tier handles a sub-task
  (today **fully static** in `delegate_role.c`; highest upside).
- `retrieval_topk` — k for fusion recall.
- `briefing_style` — compact vs evidence-heavy session briefing.
- `guardrail_strictness` — strict vs balanced threshold arm.

Arms and prompt/config variants are stored as **typed artifacts** (reuse
`db2_artifact_gen_id`) with a version, not as ephemeral strings.

### 2. Reward signal (named per surface, not hand-waved)

The hardest part, and the one most loop sketches skip. Each decision point
declares a reward composed from already-logged signals: eval/benchmark outcome ×
latency × cost (all present in trajectories; hard-negatives are an explicit
penalty term). The reward closes the bandit decision via
`db2_bandit_decision_close`.

### 3. Three measurement modes (one surface, explicit regimes)

Offline batch and online learning are distinct regimes that are easily conflated;
aimee does both and should keep them separate:

- **Offline suite** — run a fixed suite via the existing `memory.benchmark` RPC;
  rank candidates. This is the "lab run".
- **Online** — enable live exploration behind the existing
  `bandit_live_decision_enabled` flag; the exploration-budget gate already caps
  exploration traffic.
- **Off-policy replay** — score a *candidate* policy on already-logged decisions
  via `db2_bandit_decisions_export` + IPW, with **no live traffic and no
  re-run**. This is a strong edge most implementations lack, and should be the
  default first pass before spending live traffic.

### 4. Promotion gate (statistical, not argmax)

"Rank and promote best" is naive. Promotion must clear the **existing**
`baseline.json` regression gate **and** a calibration credible-interval check
(`config_learning.c`) before a candidate becomes the default arm. Promotion
writes rollback metadata (previous arm + decision id) as a typed artifact.

### 5. Thin-client surface (avoid the `lab` name collision)

`aimee lab` / `kb_lab` **already exists** — it is the *ingest* lab (chunk-quality
audit, `src/kb/kb_lab.c`). Do **not** reuse `lab`. Proposed verb: `aimee optimize`
(or `aimee experiment`):

```
aimee optimize baseline   --point delegate_routing
aimee optimize variants   --point delegate_routing --register <artifact>
aimee optimize replay     --point delegate_routing          # off-policy, IPW
aimee optimize run        --point delegate_routing --suite memory   # offline
aimee optimize compare    --baseline <id> --candidate <id>
aimee optimize promote    --candidate <id> --guarded               # gated
```

### What this deliberately does **not** do

- No demo-optimizer code, branding, or unsupported sample-efficiency claims.
- No heavyweight experiment-tracking runtime dependency. aimee has
  `trajectory_export.c`; emit a standard, **export-only** tracing format.
- No new DB tier or long-lived service.

## MVP

Ship one decision point end-to-end: **`delegate_routing`** (static today → most
upside).

1. Register it in the decision-point registry with 3–5 routing arms.
2. Reward = memory-benchmark/eval outcome × latency × cost.
3. **Off-policy replay first** (rank arms on logged decisions, zero live cost).
4. Then enable live exploration behind `bandit_live_decision_enabled`.
5. Promote behind the `baseline.json` regression + credible-interval gate, with
   rollback metadata.

This is assembly of existing parts — far less new code than "build a lab".

## Phasing

- **P1** — decision-point registry + reward config; `delegate_routing` arms;
  `aimee optimize baseline|replay`. (Off-policy only; no live traffic.)
- **P2** — offline `aimee optimize run --suite` over `memory.benchmark`;
  `compare`.
- **P3** — online exploration for `delegate_routing`; `promote --guarded` with
  rollback metadata.
- **P4** — fan out to `retrieval_topk`, `briefing_style`, `guardrail_strictness`
  (the last two are the optimisation *seam* exposed by the companion proposal,
  [ingest-restoration-and-recall-contract.md](ingest-restoration-and-recall-contract.md)).

## Risks

- **Reward mis-specification** dominates outcomes — keep rewards inspectable and
  versioned; validate offline before any live traffic.
- **Exploration cost** in production — already bounded by the exploration-budget
  gate; keep `bandit_live_decision_enabled` opt-in per decision point.
- **Decision-point sprawl** — the registry must be a small, reviewed set, not an
  open registration surface.

## Relationship to the companion proposal

[ingest-restoration-and-recall-contract.md](ingest-restoration-and-recall-contract.md)
introduces two new tunable thresholds — *repair-vs-reject* and
*verbatim-vs-synthesize*. Those are registered here as decision points, so the
two proposals compose: the restoration proposal supplies *new things to
optimise*; this proposal supplies the *loop that tunes them safely*.
