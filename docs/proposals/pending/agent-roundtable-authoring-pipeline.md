# Proposal: Agent roundtable authoring pipeline (idea → reviewed proposal → implementation → reviewed PR)

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-11
- **Charter roles:** Orchestrate (pipeline state machine), Draft/Review
  (roundtable application), Gate-Promote (human pass/fail gates), Calibrate
  (done-bar + pass ceiling config), Persist (resumable ledger).
- **Scope:** a driving-agent orchestration spec + a thin persisted pipeline
  ledger. Net-new code is small: a `pipeline_run` ledger (DB1 or a `/v1/runs`
  sibling), config keys (`src/headers/config.h`, `src/config.c`,
  `src/config_fields.c`, `src/config_sections.c`, `src/config_save.c`), and the
  driving-agent runbook/skill. It **depends on** the roundtable engine (already on
  `testing`) and on `agent-directed-pr-review` P1a–c (the brief + structured items
  + the `ensemble_review` MCP tool). It reuses `gh`/`git` for PR/merge. No changes
  to the roundtable engine itself.

## Design at a glance

A closed-loop authoring pipeline that turns a one-line idea into a merged,
panel-reviewed implementation, with **two human gates** and **two roundtable
quality gates**:

```
[Human] idea
  → DRAFT roundtable generates a first proposal
  → REVIEW roundtable ⇄ author-revise  (until done-bar)        ← proposal quality gate
  → open proposal PR
  → [Human gate 1] pass / fail
        fail → back to proposal REVIEW loop (fail reason → brief)
        pass → merge proposal PR → implement
  → REVIEW roundtable ⇄ agent-fix  (until done-bar)            ← PR quality gate
  → [Human gate 2] pass / fail
        fail → back to implement + PR REVIEW loop (fail reason → brief)
        pass → merge implementation PR → done
```

The roundtable runs **as many passes as correctness takes** — depth is the point.
The done-bar is a *correctness* condition, not a pass budget; the configurable
pass ceiling is only an operator cost-backstop that **escalates to the human**
when hit without reaching the done-bar (it never auto-passes a not-done artifact).

## Relationship to existing proposals

- **The roundtable engine is done** (`docs/proposals/done/agent-roundtable-collaborative-drafting.md`,
  PRs #136/#142, on `testing`). `delegate_roundtable_run`
  (`src/headers/delegate_ensemble.h`) provides both `ROUNDTABLE_DRAFT` (produces an
  improved `artifact`) and `ROUNDTABLE_REVIEW` (produces deduped, severity-tagged
  `items[]` with a corroboration `count`, plus `answered_questions[]` and
  `coverage_gaps[]`), a deterministic `converged` predicate, `best_round`, and
  inherited cost/deadline bounds. **This proposal adds no engine code.**
- **`agent-directed-pr-review.md` (pending) is the callable surface and a hard
  dependency.** Its P1a (the **brief**: focus/fixes/invariants/questions, open
  mandate), P1b (return the structured items to the caller), and P1c (the
  `ensemble_review` MCP tool over the async run bridge, `CAP_DELEGATE`-gated) are
  what let the driving agent invoke directed review mid-task and read back
  actionable items. This pipeline is the *orchestration on top* of that surface.
- **`agent-roundtable-collaborative-drafting.md` (pending residual)** lists
  deeper convergence/economics tests; this proposal's validation (§10) exercises
  exactly those at pipeline scale.

## What exists vs. what is net-new

| Capability | Status |
|---|---|
| Multi-round panel, DRAFT + REVIEW modes, deterministic convergence, dedup, severity, cost/deadline bounds | **exists** (engine, `testing`) |
| Directed review (brief), structured items returned, `ensemble_review` MCP tool | **dependency** (agent-directed-pr-review P1a–c) |
| PR open / merge, diff capture (`git diff`) | **exists** (`gh`, `git`) |
| Outer REVIEW⇄revise loop, done-bar evaluation, pass ceiling + escalation | **net-new** (§3) |
| The two human gates + the two fail-return edges | **net-new** (§5) |
| Persisted, resumable pipeline ledger (hybrid state) | **net-new** (§4) |
| Config: done-bar, pass ceiling, outer cost cap | **net-new** (§6) |

## §1 The pipeline state machine

States (persisted in the §4 ledger):

`drafting → proposal_review → gate1_pending → implementing → pr_review → gate2_pending → done` (plus `failed`/`abandoned`).

Transitions:

1. **drafting** — DRAFT roundtable turns the human idea (+ any seed brief) into a
   first proposal artifact. One `ensemble_review`/roundtable call in
   `ROUNDTABLE_DRAFT` mode; the converged `artifact` becomes the working draft.
2. **proposal_review** — the §3 outer loop: REVIEW the draft, author-revise from
   the items, re-REVIEW, until the done-bar (§3). Then the agent opens the proposal
   PR (`gh pr create`, base `testing`, per repo flow) and moves to `gate1_pending`.
3. **gate1_pending** — human gate 1 (§5). **pass** → merge proposal PR, move to
   `implementing`. **fail** → the human's reason is appended to the brief and the
   state returns to `proposal_review`.
4. **implementing** — the agent implements the merged proposal (normal coding;
   not a roundtable activity), opens the implementation PR, captures the diff.
5. **pr_review** — the §3 outer loop in REVIEW mode over the diff: REVIEW,
   agent-fix, re-REVIEW, until the done-bar.
6. **gate2_pending** — human gate 2 (§5). **pass** → merge the implementation PR,
   move to `done`. **fail** → reason → brief, state returns to `implementing`.

Every state is durable (§4) so a human gate can be answered hours or days later,
across a server restart or a new agent session.

## §2 The two roundtable applications

Both phases use the **same** engine; they differ only in input and brief.

- **Proposal phase (B).** Input = proposal markdown. **Both** modes (per decision):
  DRAFT to generate the first artifact in `drafting`, then REVIEW to gate it in
  `proposal_review`. The brief carries the proposal's *goal*, the *invariants* it
  must satisfy, and the human's seed *questions*. Author-revise between REVIEW
  passes is done by the driving agent (it is the proposal's author).
- **PR phase (A).** Input = unified diff (the agent pastes `git diff
  <base>...HEAD`; aimee has no PR-fetch and needs none — `agent-directed-pr-review`
  §6). REVIEW mode only; the agent applies fixes between passes. The brief carries
  the *fixes just applied*, the *invariants* the change must not break, and the
  *questions* the author is unsure about.

The brief is **open-mandate** (agent-directed-pr-review §2): it weights attention
and seeds the questions, but every reviewer is still told to report any blocking
issue even outside the focus. Direction must never become a filter.

## §3 The outer loop, the done-bar, and the pass ceiling

Two distinct loop levels — keep them un-confused:

- **Inner rounds:** rounds *within one* `delegate_roundtable_run` call
  (`roundtable_max_rounds`, default 3). Engine-owned, unchanged.
- **Outer passes:** REVIEW → revise → re-REVIEW cycles the *pipeline* drives. This
  is what the human means by "passes" and what §6's ceiling bounds.

**Done-bar (configurable; correctness condition, not a budget).** A phase leaves
its review loop only when the latest REVIEW result satisfies the configured bar,
read from real engine fields (`roundtable_result_t`):

- `zero_blocking` *(default)* — `converged == true` and no `items[]` of
  `severity == "blocking"`. Suggestions/nits are surfaced in the gate digest but
  don't block.
- `zero_blocking_suggestions` — also requires no `suggestion`-severity items
  (nits allowed). Stricter, more passes.
- `zero_blocking_questions_answered` — `zero_blocking` plus every brief question
  present in `answered_questions[]` and `coverage_gaps[]` empty.

The driving agent computes the bar from the structured items
(agent-directed-pr-review P1b); it never re-judges convergence itself — it trusts
the engine's deterministic `converged`/`review_saturated`.

**Pass ceiling (configurable; cost backstop, not an early-exit).** `roundtable_pipeline_max_passes`
bounds outer passes. **Default 0 = unbounded** — review runs until the done-bar,
honoring correctness-over-pass-count. When an operator sets a positive cap and the
loop reaches it *without* meeting the done-bar, the agent **escalates to the
human** (surfaces the current artifact + the remaining blocking items + cost) and
**does not auto-pass**. A cap is a budget guard, never a way to ship a not-correct
artifact.

**Echo guard.** Between passes the brief carries the *author's fixes/changes*, not
the panel's verbatim prior findings, so a fresh panel re-derives rather than
anchors on its own prior output. (The engine already dedupes within a call; this
prevents cross-pass echo.)

**Non-termination backstop.** Besides the optional pass ceiling, the inherited
`ensemble_max_cost_usd` (per call) and a new cumulative
`roundtable_pipeline_max_cost_usd` (per phase, §6) bound spend; either tripping
escalates to the human rather than silently passing.

## §4 Hybrid state — the resumable pipeline ledger

The agent drives the loop, but pipeline state is **persisted** so it survives a
restart and a human gate can be answered later (the decision). A `pipeline_run`
record (DB1 table or a `/v1/runs` sibling — §12) holds:

- pipeline id, state (§1), phase, created/updated timestamps;
- the working artifact (proposal text or PR ref) and the current brief;
- per-pass history: each outer pass's roundtable `run_id`, `converged`,
  blocking/suggestion counts, `cost_usd`, `rounds_run`, `best_round`;
- the PR number(s) and merge SHAs;
- the human-gate verdicts and fail reasons.

This makes the human gate a durable checkpoint: `aimee pipeline status <id>` shows
where it is and the evidence; `aimee pipeline gate <id> pass|fail --reason "…"`
records the verdict and resumes the loop. The driving agent reconstructs its
position from the ledger on any new session.

## §5 The two human gates

At `gate1_pending` / `gate2_pending` the agent **pauses** and surfaces a compact
digest, then waits for the verdict:

- the PR link;
- the converged-review **digest**: blocking/suggestion/nit counts, the
  highest-corroboration items (`item.count`), answered questions, and any
  `coverage_gaps`;
- pipeline economics: total outer passes, cumulative `cost_usd`, rounds.

**pass** advances (merge → next phase). **fail** captures the human's reason; the
reason becomes a brief `focus`/`questions` entry for the next loop, and the state
returns to the prior review phase. The fail reason is durable in the ledger so the
re-review is genuinely directed by it.

## §6 Config

New keys (full plumbing: `config.h` field, `config_fields.c`,
`config_sections.c`, `config_save.c`, `aimee config get/set/list`, generated docs,
`test_config`/`test_config_surface`/`test_cmd_config`). Decide nesting
(`roundtable.pipeline.*` section vs top-level) at implementation:

- `roundtable_pipeline_done_bar` — enum `zero_blocking` (default) |
  `zero_blocking_suggestions` | `zero_blocking_questions_answered`.
- `roundtable_pipeline_max_passes` — int, **default 0 (unbounded)**; >0 = outer
  pass ceiling that escalates (never auto-passes) on hit.
- `roundtable_pipeline_max_cost_usd` — double, cumulative per-phase spend cap;
  0 = unbounded; tripping escalates.
- *(optional, deferred)* per-phase overrides (`…_proposal` / `…_pr` suffixes) if
  the proposal and PR phases want different bars.

Participants, aggregator, per-call cost, and inner-round bounds are **inherited**
from `ensemble_*` / `roundtable_*`; no duplication.

## §7 Making "converged" mean "correct"

Convergence is only as meaningful as the panel. To keep the quality gate honest
(not "agreeable panelists agreed"):

- **Participant diversity** — the panel reuses `ensemble_*` participants, which
  should be distinct models/providers; a single-model panel converges on its own
  blind spots. Validation (§10) asserts ≥2 distinct providers for a pipeline run
  or warns.
- **Open mandate** (§2) so direction never suppresses out-of-scope findings.
- **Corroboration surfacing** — the gate digest shows `item.count` so the human
  sees whether a finding was one panelist or all of them.
- **Adversarial framing** inherited from review mode's `review` charter role.

These make a clean done-bar correspond to *correctness*, which is the real target
— the pipeline never trades correctness for fewer passes.

## §8 Dependencies

- **Hard:** `agent-directed-pr-review` P1a (brief) + P1b (structured items
  returned) + P1c (`ensemble_review` MCP tool over the async bridge). Without P1c
  the agent cannot invoke directed review mid-loop; without P1b it cannot evaluate
  the done-bar.
- **Soft:** the `git diff` range helper (agent-directed-pr-review P2) for the PR
  phase input; otherwise the agent runs `git diff` itself.
- **Repo flow:** PRs base `testing`; main promotion is separate and out of scope.

## §9 Phasing

- **P0 — Ledger + states.** The `pipeline_run` record, the state enum/transitions,
  and `aimee pipeline status|gate` CLI. No loop yet; states settable manually.
  Mergeable alone.
- **P1 — Outer review loop + done-bar.** The REVIEW⇄revise loop over
  `ensemble_review`, the configurable done-bar evaluator, the pass ceiling +
  escalation, the echo guard. Drives the PR phase first (single mode, simplest).
- **P2 — Proposal phase (DRAFT + REVIEW).** Add the `drafting` DRAFT step and the
  proposal-review loop; wire `gh pr create`/merge for the proposal PR.
- **P3 — Human gates + fail-return edges** end to end, with the durable digest and
  reason-to-brief feedback. This closes the full loop.
- **P4 — Config surface** for all keys + generated docs + tests (lands with the
  phase that first reads each key, not deferred).

P0/P1 are useful standalone (a directed, looped PR reviewer with a durable
ledger) before the full idea→merge pipeline of P2/P3.

## §10 Validation

- **Loop correctness** — a fixture proposal/diff with N seeded blocking issues:
  the loop reaches the done-bar only after all N are resolved; the digest counts
  match the engine items; the ledger records each pass's `run_id`/cost.
- **Done-bar config** — each of the three bars stops the loop at the right point
  (suggestions block under bar 2; questions must be answered under bar 3).
- **Pass-ceiling escalation** — with a low cap and an unresolvable seeded issue,
  the loop escalates to the human at the cap and never auto-passes.
- **Resumability** — kill and restart between a review pass and a gate; the ledger
  restores state and the gate is answerable post-restart.
- **Fail-return** — a `fail --reason` re-enters the review loop with the reason
  present in the next brief (asserted in the reviewer prompt).
- **Panel diversity** — a single-provider panel warns; a ≥2-provider panel does
  not.
- **Cost accounting** — cumulative per-phase cost matches the sum of per-pass
  `cost_usd`; the per-phase cap trips correctly.

## §11 Non-goals (v1)

- Server-side git or GitHub API (no PR fetch, no inline-comment posting); the
  agent uses `gh`/`git` (agent-directed-pr-review §11).
- A fully autonomous, gate-less pipeline — the two human gates are mandatory by
  design; "configurable max passes" bounds cost, it does not remove the human.
- Engine changes to the roundtable. The pipeline is strictly on top.
- Multi-proposal/parallel-pipeline scheduling (one pipeline run at a time in v1).

## §12 Open questions

- **Ledger home:** a DB1 `pipeline_run` table vs a `/v1/runs` sibling vs reusing
  the existing op-run store with a new kind. The op-run store already persists run
  state for the async bridge; extending it may be lighter than a new table.
- **Who is the driving agent at the gate?** When paused at a human gate across
  sessions, does a fresh agent resume from the ledger automatically, or does the
  human re-invoke `aimee pipeline resume <id>`?
- **Per-phase config:** do the proposal and PR phases want independent done-bars /
  ceilings by default, or one shared set with optional overrides?
- **DRAFT seeding:** does the `drafting` step take only the idea, or also a
  pointer to sibling/related proposals so the first draft starts grounded?
