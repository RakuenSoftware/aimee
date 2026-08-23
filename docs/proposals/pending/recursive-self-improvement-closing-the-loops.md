# Proposal: Recursive self-improvement — closing the loops aimee opens but never finishes

- **State:** 🟡 **PENDING — S0 and S1 implemented and wired; S2–S6 not started.** Net-new design over
  existing substrate. It does **not** re-propose the learning-signals router, the
  learning-to-rank fitter, `kb_bandit`, the retrieval-outcome ledger, or the memory
  maintenance passes — it adds the loops those systems leave open.
- **Author:** JBailes
- **Date:** 2026-08-23
- **Charter roles:** Calibrate (measured reward instead of proxy reward),
  Evaluate-Optimize (an eval suite that grows from live failure), Gate-Promote
  (gates that are themselves measured), Review (recursion-safety accounting),
  Persist (durable candidate and fate ledgers).
- **Owns:** the *admission contract extension* for self-generated evaluation and
  negative knowledge, endogeneity accounting, and post-commit regret.
- **Depends on:** `learning_router_record_signal` and the learning admission contract
  (`src/modules/learning/`, `include/aimee/learning/learning.h`); `db1_eval_*`
  (`src/modules/db1/eval.c`); `agent_eval_run_with_options` and
  `agent_eval_load_tasks` (`src/modules/benchmarks/agent_eval.c`,
  `src/posix/agent_runtime_support.c`); `kb_bandit_sample` / `kb_bandit_reward`
  (`src/kb_bandit.h`, `src/kb/kb_bandit.c`); `db2_anti_pattern_*`,
  `db2_agent_outcome_*`, `db2_curiosity_*` (`src/modules/db2/c/`); the graph
  self-audit (`GET /v1/code/graph/audit`).

## Problem and boundary

aimee already learns recursively along **one** axis: which *evidence* to trust.
Retrieval outcomes feed trust scores; ranker features feed a fitted model behind a
benchmark gate; registered decision points sample bandit arms and take rewards;
implicit detectors raise signals that become capped, human-gated proposals; memory is
deduped, summarised, reinforced, and demoted.

Five loops are open, and each open loop is the reason another one cannot be trusted.

**1. The yardstick is frozen.** Every promotion gate — `kb_ranker_model_commit`, the
parity bands cited across acceptance criteria — measures against a fixed suite
(`tests/eval/*`, `benchmarks/locomo`, `benchmarks/longmemeval`, the mini fixtures).
Nothing turns a live failure into a permanent check. A learning system optimising
against a static yardstick converges on the yardstick, not the goal. This is the
load-bearing gap: it caps the value of everything else here and everything already
shipped.

**2. Credit assignment is proxy-shallow.** `kb_bandit_recall_sufficiency_reward()`
documents itself as an "inspectable proxy". `eval_feedback_loop()`
(`src/modules/benchmarks/agent_eval.c:296`) reinforces a rule on **word overlap** with
a failed task *and* on word overlap with a passed task — the same rule is bumped for
opposite outcomes, by up to +10 and +5 respectively, with no causal claim behind
either. No reward in the system is measured against a counterfactual.

**3. Nothing learns the policy layer.** We fit which *documents* to trust; we never fit
which *instructions* work. Prompt fragments (`src/prompts.c`, `src/role_templates.c`,
`src/tool_prompts/`), toolset composition, and persona blocks are hand-written
constants outside every loop.

**4. Findings are produced and never consumed; negative knowledge is lexical.** The
graph self-audit emits unverified inferred edges, orphans, and low-cohesion
communities. The curiosity backlog (`src/modules/db2/c/curiosity.h`) records
`missing_fact`, `contradiction`, `stale_fact`, `weak_coverage`,
`unverified_assumption`. Both require a human to act, so both accumulate. The
`anti_patterns` catalog is real but **lexical**: it matches word-bounded phrases in
`file_path + " " + command`, mined from `tool_error_pattern`. There is no record that
an *approach* to a *goal* was tried and failed, so approach-level repetition is
unpriced.

**5. The learning system does not learn about itself, and its recursion is
unmeasured.** `learning_metrics_commit_ratio()` measures **acceptance**, not
**correctness**: a proposal committed and later contradicted, superseded, or reverted
scores identically to one that held. Per-sink caps and confidence thresholds are
hand-tuned constants governing an adaptive system. And as the self-generated share of
input signal grows, nothing measures drift toward an echo chamber — the same failure
mode `repetition-collapse-guardrail` addresses one layer down.

### Boundary

This proposal owns **how self-generated learning is admitted, measured, and bounded**.
It does not own the retrieval stack, the ranker, the memory tiers, the benchmark
harness, or the module/bus topology; it consumes all of them through their existing
contracts.

## Decision

Six loops, ordered so the measurement and safety work lands **before** the
optimisation work that depends on it.

| Slice | Loop | Closes |
| --- | --- | --- |
| **S0** | Endogeneity accounting + collapse gate | §5 recursion safety |
| **S1** | Failure → quarantined regression task → admitted suite | §1 the yardstick |
| **S2** | Counterfactual trajectory replay → causal reward | §2 credit assignment |
| **S3** | Approach-level negative knowledge, recalled at plan time | §4 anti-patterns |
| **S4** | Operator-invoked resolution of audit + curiosity findings | §4 unconsumed findings |
| **S5** | Post-commit regret + gates fitted from regret | §5 meta-learning |
| **S6** | Policy fragments as bandit arms | §3 the policy layer |

S0 and S1 are prerequisites for the rest. S2's rewards are only meaningful against a
suite that grows (S1); no slice may widen the self-generated share of learning signal
without S0's accounting; S5 and S6 are unsafe before both.

### Feature-liveness compliance

[`feature-liveness-and-background-curator-removal.md`](../done/feature-liveness-and-background-curator-removal.md)
deleted the background skill curator as a self-contained feature island and now
forbids, by lint, "a renamed workflow/job that periodically groups skills, scores
similarity/use, and proposes or writes skill changes without the learning admission
contract." That proposal names this one's territory explicitly: *"Any future
replacement is separately proposed under the memory/learning proposal's admission
contract."*

This proposal is bound by that rule and takes it as a design input, not an obstacle:

- **No slice here introduces a self-scheduled background worker.** Every pass is
  invoked by an operator command, a `/v1` route, or an existing supervised stage.
  S4 in particular is deliberately **operator-invoked**, not a cadence job — that is
  the whole difference between it and the deleted curator.
- **Every slice names a production consumer outside its own cluster**, with
  entrypoint-to-effect evidence, before it may be retained:
  - S0 → the promotion gates (ranker commit, S1 admission) and the operator metrics
    surface consume the ratio;
  - S1 → `agent_eval_load_tasks` and every CI job that runs a suite consume admitted
    tasks as ordinary files;
  - S2 → `kb_bandit_reward` and the `benchmark_trace` artifact stream;
  - S3 → plan-time context assembly;
  - S4 → the graph confidence write path and the curiosity item state;
  - S5 → the learning router's per-detector confidence and per-sink caps;
  - S6 → the fragment injection sites.
- **No new `maintenance_state` keys** beyond descriptor-declared ones with a named
  owner and journey.
- Tests exercise the consumer journey, not the island.

### Non-goals

- **No online weight updates in a serving path.** Every loop is offline, batched, and
  gated, exactly as the ranker fitter already is.
- **No autonomous commit of anything that reaches a brief.** S1 admits *checks*, not
  behaviour. S3/S4 write findings; the existing accept/reject and cap machinery still
  governs commits. S6 is arm selection under an existing gate, not free-form prompt
  rewriting.
- **No cross-installation learning.** Federated lesson distillation, and fine-tuning a
  local model from production trajectories, are real and adjacent and deliberately
  excluded — they need their own design pass, and
  [`dedicated-extraction-model-curator-tier-a.md`](dedicated-extraction-model-curator-tier-a.md)
  already approaches the second from the other side. Successor proposal.
- **No second benchmark harness.** S1 grows the existing suite format
  (`suite_dir/*.json` parsed by `agent_eval_load_tasks`).
- **No DB1/DB2 boundary crossing.** Per-machine eval state stays in DB1; shared
  knowledge (anti-patterns, curiosity, artifacts, bandit) stays in DB2.
- **No changes to `eval_feedback_loop()`'s existing behaviour in S0/S1.** Its
  word-overlap heuristic is named here as motivation for S2; replacing it is S2's
  work, under S2's acceptance checks.

## Threat and failure model

The central risk is **the system teaching itself something false and then grading
itself against it.**

| Failure | Control |
| --- | --- |
| **Echo chamber** — self-generated signal dominates and the loop reinforces its own priors | S0: each committed proposal's evidence chain is classified exogenous/endogenous; gated promotions fail **closed** when the exogenous share falls below `learning.endogeneity.min_exogenous_ratio` |
| **Poisoned yardstick** — a flaky or wrong failure becomes a permanent task and the system optimises to satisfy it | S1: quarantine + **reproduce-twice from distinct sessions** + recorded provenance + operator visibility + a permanent per-signature rejection |
| **Suite bloat** — thousands of near-duplicate regressions make every gate slow and meaningless | S1: dedup on a normalised failure signature, per-window admission cap, retirement of long-passing tasks |
| **Reward hacking via replay** — a perturbation that games the harness rather than the task | S2: replay reuses the live suite's success checks; a perturbation that cannot change the check's inputs records `attributable=0` and yields no reward |
| **Runaway compute** — resolution passes burn the machine | S4: operator-invoked only, bounded per-invocation job budget, no scheduler hook |
| **Prompt injection through learned artifacts** | Any corpus-derived or model-authored string that becomes a stored task, pattern, or description passes the existing S0 sanitizer from `graph-feedback-self-audit-and-learning.md` before persistence |
| **Silent behaviour change on upgrade** | Every new config key defaults off or to the conservative bound; an installation that enables nothing sees no behaviour change |

## S0 — Endogeneity accounting and the collapse gate

**Contract.** For a committed learning proposal, walk its `evidence_refs` chain to the
roots and classify each root:

- **exogenous** — a human correction, a test or verify exit code, an official grader, a
  git outcome, an operator accept;
- **endogenous** — model-authored evidence, a prior learning artifact, a
  self-generated eval result, a lesson derived from another lesson.

`learning_metrics_endogeneity(window_days, out)` reports the exogenous share over the
window, overall and per sink. `learning_gate_check()` returns closed when the overall
share is below the configured floor. Every gated promotion consults it: ranker commit,
S1 admission, S5 threshold changes.

**Why first.** It is cheap, it is the only slice that *constrains* the others, and it
turns "is aimee learning, or hallucinating at itself?" into a number an operator can
see.

## S1 — The eval suite grows from live failure

**Contract.** A confirmed failure becomes a *candidate*; a candidate that reproduces
becomes an *admitted* task in a real suite directory.

**Sources of a confirmed failure**, all already captured:
`db1_eval_failed_tasks_recent()`; `db2_agent_outcome_recent_failures()`; a
`learning_signals` row with `polarity='negative'` carrying `correction_text`; a
dogfood autolabel negative; a failed mechanical verify in the autonomous-dev loop.

**Pipeline.**

1. **Synthesise.** Derive `(name, prompt, role, success_check, max_turns)` from the
   failure. The success check is the *inverse of the observed failure*, expressed in
   the existing check vocabulary: correction text becomes a `contains` check, a failed
   verify becomes its command's exit status, a wrong retrieval becomes the id that
   should have been cited.
2. **Fingerprint and dedup.** A normalised signature over (role, failure mode,
   success-check value). A matching candidate or admitted task bumps `occurrences`
   rather than inserting a row.
3. **Quarantine.** State `candidate`. Nothing runs it as a gate; it is operator-visible.
4. **Admit on reproduction.** When a signature is observed
   `learning.eval_synthesis.min_occurrences` times (default 2) from **distinct
   sessions**, and S0's gate is open, materialise `<suite>/<name>.json` in the
   regression suite directory and record the admission with its provenance.
5. **Retire.** An admitted task passing continuously for
   `learning.eval_synthesis.retire_windows` windows becomes `archived` and leaves the
   hot suite — the suite tracks live risk, not history.

**Boundary.** Candidates and admissions are per-machine, so the ledger is DB1
(`eval_candidates`), beside `eval_results`, owned by the `db1` module. The synthesis
and admission **policy** lives in the `learning` module, because this is precisely the
learning admission contract the curator-removal proposal defers replacements to.
Materialised tasks are ordinary files in the existing schema: any harness, CI job, or
human that runs a suite today runs these with no code change.

**Operator surface.** `aimee eval candidates [list|show|admit|reject|retire]`.
Admission is automatic under the gate; `admit` forces one, `reject` suppresses a
signature permanently — the poisoned-yardstick escape hatch.

## S2 — Counterfactual trajectory replay

**Contract.** `aimee trajectory replay <trajectory> --perturb <decision_point>=<arm>`
re-runs a recorded trajectory with exactly one decision changed, scores both arms with
the same success check, and emits the delta as (a) a `kb_bandit_reward()` against the
recorded `decision_id` and (b) a `benchmark_trace` artifact for the promotion
pipeline.

This turns documented proxies into measured causal rewards and supplies the input S5
needs. It composes `trajectory_export.c`, `trace_analysis.c`, and the existing
ablation harness; the perturbation vocabulary starts from the decision points already
registered via `kb_bandit_arm_register`. Non-attributable outcomes are dropped, not
guessed.

S2 also replaces `eval_feedback_loop()`'s word-overlap reinforcement with attributed
reward, under its own acceptance checks.

## S3 — Approach-level negative knowledge

**Contract.** Extend negative knowledge from lexical to semantic. Add a row class keyed
on `(goal_signature, approach_signature, failure_mode)` with an embedding on the goal,
written from the same confirmed failures S1 consumes, and recalled **at plan time**
rather than at command-execution time: "this shape was tried against a goal like this
one; it failed because Y."

Recall is advisory context, never a block. The blocking escalation path
(`db2_anti_pattern_list_hot`) is unchanged and stays lexical.

## S4 — Findings get resolved

**Contract.** An **operator-invoked** pass — `aimee graph resolve` / `aimee memory
curiosity resolve --auto`, with a per-invocation job budget and no scheduler hook —
drains two backlogs that today only accumulate:

- **Graph audit** — take unverified inferred edges, confirm or refute them with the
  extractor/LSP, write the verdict back through the existing §1↔§3 finding-verdict
  path.
- **Curiosity** — for `unverified_assumption` and `weak_coverage`, attempt resolution
  from the indexed corpus, then resolve or annotate the item.

Both write through existing verdict paths; neither invents a new store, and neither
schedules itself. If a cadence is later wanted, it is proposed separately with a named
journey and a liveness disposition, not smuggled in here.

## S5 — Post-commit regret, and gates fitted from it

**Contract.** Record every committed proposal's **fate**: standing, superseded,
contradicted by a later signal, or reverted. Regret is the share that did not stand
within a horizon.

Then use it: per-detector confidence becomes a function of that detector's observed
regret, and per-sink weekly caps and confidence thresholds become bandit arms whose
reward is *retained* commits — committed **and** still standing — not commits. The
gate stops being a hand-tuned constant and becomes measured, which is only safe
because S0 bounds how self-referential the input is and S2 makes the reward causal.

## S6 — The policy layer enters the loop

**Contract.** Register injectable policy fragments — prompt blocks, role-template
sections, tool descriptions, toolset composition — as versioned arms at named decision
points through the existing `kb_bandit_arm_register`. Selection is bandit sampling;
reward is S2's measured delta; promoting a new default fragment goes through the same
benchmark gate as a ranker model, against a suite that now includes S1's admitted
regressions.

Last, deliberately: it is the highest-variance surface in the system, and it is only
defensible once the reward is causal (S2), the yardstick grows (S1), the gate is
measured (S5), and the recursion is bounded (S0).

## Compatibility and migration

- **DB1:** one new table, `eval_candidates`, created by `src/modules/db1/schema.sql`
  under `CREATE TABLE IF NOT EXISTS`. Additive; no existing table changes; no
  destructive migration.
- **DB2:** additive columns on `anti_patterns` (S3) and an additive proposal-fate
  table (S5), both nullable/defaulted and mirrored in the Postgres and SQLite schemas.
- **Modules:** new sources, headers, and tests are declared in the owning module
  descriptors (`src/modules/db1/module.yaml`, `src/modules/learning/module.yaml`);
  no new module and no dependency-edge change. New config keys come from the field
  descriptors, not hand-edited tables.
- **Config:** every key defaults off or conservative. Upgrade is behaviour-preserving.
- **Suites:** admitted tasks are ordinary `*.json` files in the existing schema; an
  installation that never enables S1 sees an empty regression directory.
- **Supersession:** none. Builds on `done/graph-feedback-self-audit-and-learning.md`,
  `done/learning-to-rank-weight-fitting.md`,
  `pending/learning-to-rank-activation-and-ipw-residual.md`,
  `pending/kb-hybrid-outcome-wiring-residual.md`, and the bandit/counterfactual-replay
  record, without altering their contracts. It is the admission-contract successor
  named by `done/feature-liveness-and-background-curator-removal.md`. It shares
  motivation with `pending/standing-benchmark-cadence.md` — that proposal puts the
  *fixed* suites on a cadence, this one makes the suite *grow*; they compose, and
  neither blocks the other.

## Acceptance checks

**Mechanical (unit; no live DB or model required):**

- **S0** — evidence chains with known roots classify deterministically; a wholly
  endogenous chain drives the ratio below the floor and `learning_gate_check()`
  returns closed; an empty window does not report a false-confident ratio.
- **S1** — synthesis from each failure source produces a task that
  `agent_eval_load_tasks` parses and whose success check round-trips; identical
  failures collapse to one signature and bump `occurrences`; admission refuses below
  `min_occurrences`, refuses repeat occurrences from the same session, and refuses
  while S0's gate is closed; a rejected signature is never re-admitted; retirement
  moves a long-passing task out of the hot suite.
- **S2** — a perturbation that cannot change the success check records
  `attributable=0` and emits no reward.
- **S3** — goal/approach recall returns the failed approach for a semantically near
  goal and never fires the blocking path.
- **S5** — a commit later contradicted scores as regret; the same commit standing does
  not.

**Integration:**

- **S1 end-to-end** — induce a failure, observe a candidate, reproduce it from a second
  session, observe the materialised file, run the suite and see it execute as an
  ordinary task, then retract it and see it leave the suite.
- **S4** — a run with a seeded unverified edge and a seeded curiosity item resolves both
  within one invocation and stays inside its job budget.
- **Liveness** — `scripts/check_background_skill_curator_absence.py` still passes
  (no slice reintroduces a self-scheduled grouping/scoring worker), and each new
  cluster carries a disposition and a named non-self production consumer before
  it is retained.
- **Every slice** — `make -j4 && make unit-tests && make lint && make docs-gen-check`
  clean, and ASAN over the new DB paths.

```yaml acceptance
- {id: 1, tier: mechanical, check: "make -C src test-learning"}
- {id: 2, tier: mechanical, check: "make -C src unit-tests"}
- {id: 3, tier: mechanical, check: "make -C src lint && python3 scripts/check_background_skill_curator_absence.py"}
- {id: 4, tier: integration, check: "make -C src test-learning test-memory test-workflows"}
```

## Slice order and status

| Slice | Depends on | Status |
| --- | --- | --- |
| S0 endogeneity + gate | — | substrate landed |
| S1 eval synthesis + admission | S0 | substrate landed; call sites + CLI outstanding |
| S2 counterfactual replay | S1 | not started |
| S3 approach anti-patterns | S1 | not started |
| S4 operator-invoked resolution | — | not started |
| S5 regret + fitted gates | S2 | not started |
| S6 policy arms | S1, S2, S5 | not started |

## Delivery record — S0 and S1 substrate (2026-08-23)

**What landed.**

| Piece | Where |
| --- | --- |
| Provenance grouping SQL | `db2_learning_committed_source_counts` (`src/modules/db2/c/learning.c`, `db2_learning.h`) |
| Classifier, metrics, gate | `src/modules/learning/learning_endogeneity.c`; public API in `include/aimee/learning/learning.h` |
| Synthesis policy (pure) | `src/modules/learning/learning_eval_synthesis.c`; public API in `include/aimee/learning/eval_synthesis.h` |
| Candidate ledger | `eval_candidates` in `src/modules/db1/schema.sql`; `db1_eval_candidate_*` in `src/modules/db1/eval.c` |
| Observation + admission | `src/eval_synthesis.c`, `src/headers/eval_synthesis.h` |
| Tests | `src/tests/test_learning_eval_synthesis.c` (pure policy), `src/tests/test_eval_candidates.c` (ledger + end-to-end admission) |

**Operator surface.** `aimee eval candidates [--state S] [--limit N]` reads the
backlog and the gate; `aimee eval candidates-update <scan|admit|reject|retire>`
drives it. Both are ordinary `/v1` routes (`GET`/`POST /v1/eval/candidates`).
Nothing schedules itself.

**Wiring — where the failures come from.** `scan` sweeps two ledgers, and the
boundary between them and everything else is a finding worth stating: a
synthesisable regression needs a **replayable prompt**. Failed `agent_jobs`
carry one and become tasks whose bar is "this must now succeed"; negative
signals carrying a correction carry both a prompt and what should have been
said, so they become `contains` checks. `agent_outcomes` (role/reason only) and
`eval_results` (which names an *existing* suite task, and so feeds retirement)
carry neither, and are deliberately not synthesis sources — manufacturing a
prompt for them would be fabrication. Observation is idempotent per session, so
re-running a sweep cannot manufacture its own reproduction.

Verified: `make -j8 all`, `make unit-tests` (all pass), `make lint` (63/63),
`make docs-gen-check`, `make integration-tests` (115/115) — clean on the
development machine and on a separate host, plus a live end-to-end run against a
real server. See
[the validation report](../../validation/recursive-self-improvement-s0-s1-2026-08-23.md).

**Three design decisions taken during implementation, recorded here rather than
left implicit:**

1. **The gate opens on an empty window, it does not fail closed.** Failing
   closed on "no observations" is indistinguishable from failing closed on "all
   endogenous", and it would make a fresh installation unable to admit anything
   forever. So the floor is enforced only once `committed_total` reaches
   `LEARNING_ENDOGENEITY_MIN_SAMPLE`; below that the gate is open and the ratio
   is reported as not yet meaningful. `LEARNING_GATE_UNAVAILABLE` is kept
   distinct from `CLOSED` so a caller can tell "the loop is eating itself" from
   "the query failed"; a build with DB2 compiled out reports OPEN, since no
   learning is being persisted there to guard.

2. **`signal_type` overrides `source` in classification.** The capture API
   (`kb_service_backend_agent.c`) defaults an unset `source` to `"explicit"`.
   Without the override, a caller could launder a self-derived signal into the
   exogenous count by simply omitting a field — which would defeat the entire
   measurement. The self-derived detector types are therefore endogenous
   whatever the source claims, and unknown provenance classifies endogenous.

3. **Synthesis refuses untrusted text rather than escaping it.**
   `sanitize_for_prompt` (`src/kb/prompt_sanitizer.c`) is the render boundary,
   but it is built into the KB binary, and pulling it into the server build to
   escape text at *storage* time would be a module-ownership change this slice
   has no mandate for. Instead `learning_eval_text_admissible()` is a
   conservative character allowlist — it rejects control characters, non-ASCII,
   and the markup characters that carry injection structure — and synthesis
   fails closed on any field it rejects. It is deliberately not a second
   sanitizer (it tracks no marker list, so it cannot drift from `kMarkers[]`),
   and it is stricter than the render boundary, not a replacement for it.
   Relaxing it to route through the real sanitizer is a follow-up that must
   first resolve where that sanitizer lives.

**Deliberately not done in this slice, and why:**

- **Config keys.** `min_exogenous_ratio`, `min_sample`, `min_occurrences`, and
  `retire_windows` are compile-time constants with the documented defaults, and
  every policy function takes explicit bounds (`learning_gate_check_with`) so
  wiring the field descriptors is mechanical. Adding config fields touches the
  generated accessor set and its drift check, which is its own change.
- **The dogfood-autolabel and mechanical-verify sources.** Both are named in S1
  as failure sources and neither is wired. The reason is the same finding above:
  the dogfood ledger is JSONL keyed on retrieval moments and the verify gate
  reports a command's exit status — neither carries a replayable prompt today,
  so wiring them means first deciding what prompt a synthesised task would
  replay. That is a design question, not plumbing, and it belongs with S2's work
  on trajectories rather than being guessed at here.
- **Config keys.** `min_exogenous_ratio`, `min_sample`, `min_occurrences`, and
  `retire_windows` are compile-time defaults, overridable per invocation through
  the CLI (`--min-occurrences`, `--retire-windows`, `--window-days`). Every
  policy function already takes explicit bounds (`learning_gate_check_with`), so
  promoting them to config fields is mechanical — but it touches the generated
  accessor set and its drift check, which is its own change.
