# Proposal: Agent roundtable authoring pipeline (idea → reviewed proposal → implementation → reviewed PR)

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-11 (revised post-PR-#183 review)
- **Charter roles:** Orchestrate (pipeline state machine), Draft/Review
  (roundtable application), Gate-Promote (human pass/fail gates), Calibrate
  (done-bar + pass ceiling config), Persist (resumable ledger).
- **Scope:** a driving-agent orchestration spec + a durable persisted pipeline
  ledger. Net-new code is small: a namespaced `roundtable_pipeline_runs` ledger
  or an explicit migration/extension of the existing DB1 `pipelines` schema
  (not the transient `/v1/runs` live store), config keys
  (`src/headers/config.h`, `src/config.c`, `src/config_fields.c`,
  `src/config_sections.c`, `src/config_save.c`), and the
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

## PR #183 review — gaps and how they are resolved

The initial proposal is directionally aligned with the roundtable work, but a
review against the current tree found several implementation gaps that need to be
part of the proposal rather than left to interpretation:

1. **`pipeline_run` cannot be a blank-slate name.** Aimee already has a durable
   DB1 `pipelines` table and `autopilot` pipeline handler
   (`db1_pipeline_t`, `handle_autopilot`) with `start/status/list/cancel/resume`
   style actions. It tracks a narrow plan/job pipeline, not proposal artifacts,
   PR refs, pass history, gate verdicts, or GitHub state. The proposal must either
   extend that schema deliberately or create a namespaced
   `roundtable_pipeline_runs` table; it must not introduce an ambiguous second
   "pipeline" surface. **Resolved in §4, §5, §9, §10, and §12.**
2. **`/v1/runs` is not a durable checkpoint store.** `openai_runs_store` is an
   in-process live store, bounded to 256 records, oldest-reused, and not durable
   across restarts. It is suitable for child roundtable/op-run IDs, not for a
   human gate that can sit for hours or days. **Resolved in §4, §8, §9, and §12.**
3. **The CLI/API surface is underspecified.** The text proposes
   `aimee pipeline status|gate`, but the existing callable pipeline surface is
   the `autopilot` MCP handler and does not have a gate action. The proposal must
   define whether this is a new first-class CLI/MCP/HTTP route or an extension of
   `autopilot`, with route tests and no name collision. **Resolved in §5, §9,
   and §10.**
4. **GitHub/worktree state must be resumable too.** Reusing `gh`/`git` is fine,
   but the ledger needs enough branch, commit, remote, PR, mergeability, and auth
   assumptions to detect drift when a gate resumes in a later session. **Resolved
   in §4, §5, and §10.**
5. **Large artifacts cannot be blindly inlined.** Proposal markdown and PR diffs
   can exceed MCP/op-run payload and snapshot limits. The ledger should store
   artifact refs plus content hashes and pass diffs/proposals by path, blob, or
   chunked capture where needed, not unbounded text blobs. **Resolved in §2, §4,
   §10, and §11.**
6. **Panel diversity validation must resolve agent configs.** The configured
   `ensemble.reference_models` are agent/model names; provider diversity is not
   the same as string uniqueness. The validator must resolve each participant's
   provider/model at runtime before warning or passing. **Resolved in §7 and
   §10.**
7. **DRAFT and REVIEW do not use the same callable tool today.** The current
   `ensemble_review` MCP bridge forces review mode; the DRAFT phase should call
   the existing roundtable route/CLI with `mode=draft` (or add an explicit
   draft-capable MCP surface), while REVIEW loops use `ensemble_review`.
   **Resolved in §1, §2, and §8.**

## Relationship to existing proposals

- **The roundtable engine is done** (`docs/proposals/done/agent-roundtable-collaborative-drafting.md`,
  PRs #136/#142, on `testing`). `delegate_roundtable_run`
  (`src/headers/delegate_ensemble.h`) provides both `ROUNDTABLE_DRAFT` (produces an
  improved `artifact`) and `ROUNDTABLE_REVIEW` (produces deduped, severity-tagged
  `items[]` with a corroboration `count`, plus `answered_questions[]` and
  `coverage_gaps[]`), a deterministic `converged` predicate, `best_round`, and
  inherited cost/deadline bounds. **This proposal adds no engine code.**
- **`agent-directed-pr-review.md` (pending) is the callable surface and remains a
  hard dependency for the final contract.** The current tree already exposes an
  `ensemble_review` MCP entry that queues `delegate.roundtable`, but this
  pipeline still depends on that proposal's P1 contract being complete: P1a (the
  **brief**: focus/fixes/invariants/questions, open mandate), P1b (return the
  structured items to the caller in a stable shape), and P1c (the MCP/async run
  bridge, `CAP_DELEGATE`-gated). This pipeline is the *orchestration on top* of
  that surface.
- **`agent-roundtable-collaborative-drafting.md` (pending residual)** lists
  deeper convergence/economics tests; this proposal's validation (§10) exercises
  exactly those at pipeline scale.

## What exists vs. what is net-new

| Capability | Status |
|---|---|
| Multi-round panel, DRAFT + REVIEW modes, deterministic convergence, dedup, severity, cost/deadline bounds | **exists** (engine, `testing`) |
| Directed review (brief), structured items returned, `ensemble_review` MCP tool | **partial in tree; dependency on final agent-directed-pr-review P1a–c contract** |
| PR open / merge, diff capture (`git diff`) | **exists** (`gh`, `git`) |
| Outer REVIEW⇄revise loop, done-bar evaluation, pass ceiling + escalation | **net-new** (§3) |
| The two human gates + the two fail-return edges | **net-new** (§5) |
| Persisted, resumable roundtable pipeline ledger (hybrid state) | **net-new or explicit DB1 pipeline extension** (§4) |
| Config: done-bar, pass ceiling, outer cost cap | **net-new** (§6) |

## §1 The pipeline state machine

States (persisted in the §4 ledger):

`drafting → proposal_review → gate1_pending → implementing → pr_review → gate2_pending → done` (plus `failed`/`abandoned`).

Transitions:

1. **drafting** — DRAFT roundtable turns the human idea (+ any seed brief) into a
   first proposal artifact. One roundtable call in `ROUNDTABLE_DRAFT` mode
   (through `/v1/delegate/roundtable` / `aimee delegate roundtable --mode draft`,
   or an explicit draft-capable MCP surface) produces the working `artifact`.
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
- **PR phase (A).** Input = unified diff (normally `git diff <base>...HEAD`;
  aimee has no PR-fetch and needs none — `agent-directed-pr-review` §6). REVIEW
  mode only; the agent applies fixes between passes. For large diffs, the driving
  agent must pass an artifact file/path or chunked diff slices rather than an
  unbounded inline string, and the ledger stores the diff ref plus content hash.
  The brief carries the *fixes just applied*, the *invariants* the change must
  not break, and the *questions* the author is unsure about.

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
the engine's deterministic saturation logic as exposed through `converged`.

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
restart and a human gate can be answered later (the decision). This must be a DB1
checkpoint surface, not `/v1/runs`: the current `openai_runs_store` is a bounded,
in-process live store and is not durable across restart. `/v1/runs` IDs are child
execution handles that may be stored in pass history, never the source of truth
for the pipeline.

Aimee already has a durable DB1 `pipelines` table for autopilot-style plan/job
state (`task`, `status`, `current_phase`, `plan_id`, `job_id`, attempts, and
classification). That schema is too narrow for this workflow, so implementation
must choose one explicit path:

- create a namespaced `roundtable_pipeline_runs` table plus child
  `roundtable_pipeline_passes` / `roundtable_pipeline_gates` tables; or
- migrate/extend the existing `pipelines` table in a backwards-compatible way,
  with the old autopilot actions continuing to work.

Either way, the durable record holds:

- pipeline id, state (§1), phase, created/updated timestamps, and schema version;
- artifact refs: proposal path/blob/ref, implementation branch, diff ref or
  chunk manifest, and content hashes, not unbounded inline text as the primary
  representation;
- the current brief plus compact gate digest;
- per-pass history: each outer pass's child roundtable `run_id`, status,
  `converged`, blocking/suggestion counts, `cost_usd`, `rounds_run`, `best_round`,
  result hash, and any payload/chunk refs;
- repository/worktree state: repo root, remote, base branch, head branch,
  head/base commit SHAs, dirty-state snapshot, PR number(s), PR URLs, merge SHAs,
  and last checked mergeability;
- the human-gate verdicts, fail reasons, actor/timestamp, and resume action taken.

This makes the human gate a durable checkpoint: `status` shows where it is and
the evidence; `gate pass|fail --reason "…"` records the verdict and resumes the
loop. The driving agent reconstructs its position from the ledger on any new
session and validates branch/PR drift before continuing.

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

The command/API surface must be explicit in implementation. The proposal may add
`aimee pipeline status|gate` as a new first-class CLI/MCP/HTTP surface, or extend
the existing `autopilot` pipeline handler, but it must not leave two unrelated
"pipeline" namespaces with different IDs. The gate action is net-new; today's
autopilot actions cover `start`, `advance`, `status`, `list`, `cancel`, `resume`,
`link-plan`, and `link-job`, not pass/fail gates.

Before a **pass** can merge or advance, the resumed agent revalidates the stored
worktree/PR state: the PR still exists, its head SHA matches the ledger or the
digest is marked stale, the base branch is still the intended target (`testing`),
the worktree is clean enough for the operation, `gh`/GitHub auth is available,
and mergeability has not changed underneath the gate. A failed validation returns
to the relevant review phase or asks the human for a fresh verdict with the stale
evidence called out.

## §6 Config

New keys (full plumbing: `config.h` field, `config_fields.c`,
`config_sections.c`, `config_save.c`, `aimee config get/set/list`, generated docs,
`test_config`/`test_config_surface`/`test_cmd_config`). Decide nesting
(`roundtable.pipeline.*` section vs top-level) at implementation. If nested under
`roundtable`, the parser/saver must explicitly add nested-object support; today's
roundtable config surface is scalar keys like `roundtable.max_rounds`,
`roundtable.converge_threshold`, `roundtable.deadline_ms`, and `roundtable.turns`.

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

- **Participant diversity** — the panel reuses `ensemble_*` participants. The
  configured `ensemble.reference_models` values are agent/model names, so the
  validator must resolve each participant through the agent config and compare
  provider/model identity, not just string uniqueness. A single-provider panel can
  converge on its own blind spots. Validation (§10) asserts ≥2 distinct providers
  for a pipeline run or warns.
- **Open mandate** (§2) so direction never suppresses out-of-scope findings.
- **Corroboration surfacing** — the gate digest shows `item.count` so the human
  sees whether a finding was one panelist or all of them.
- **Adversarial framing** inherited from review mode's `review` charter role.

These make a clean done-bar correspond to *correctness*, which is the real target
— the pipeline never trades correctness for fewer passes.

## §8 Dependencies

- **Hard:** `agent-directed-pr-review` P1a (brief) + P1b (structured items
  returned) + P1c (`ensemble_review` MCP tool over the async bridge) as a stable
  callable contract. The current tree has an `ensemble_review` MCP route that
  queues `delegate.roundtable`, but the pipeline still needs the final structured
  result shape, status/polling contract, cancellation behavior, and payload
  limits from that proposal. Without P1b it cannot evaluate the done-bar.
- **Hard for proposal drafting:** a callable `ROUNDTABLE_DRAFT` path. The existing
  `/v1/delegate/roundtable` / `aimee delegate roundtable --mode draft` surface is
  enough if the driving agent can call it; otherwise add a narrow draft MCP
  sibling rather than overloading the review-only `ensemble_review` tool.
- **Hard:** durable DB1 checkpointing for the roundtable pipeline. `/v1/runs`
  remains a child-run polling surface only because its store is bounded and
  non-durable.
- **Soft:** the `git diff` range helper (agent-directed-pr-review P2) for the PR
  phase input; otherwise the agent runs `git diff` itself.
- **Repo flow:** PRs base `testing`; main promotion is separate and out of scope.

## §9 Phasing

- **P0 — Ledger + states + namespace decision.** Choose the DB shape
  (`roundtable_pipeline_runs` tables vs explicit migration of DB1 `pipelines`),
  add the state enum/transitions, artifact refs/hashes, child run references, and
  the CLI/MCP/HTTP namespace decision (`pipeline` vs `autopilot` extension). No
  loop yet; states/gates settable manually. Mergeable alone, with restart tests.
- **P1 — Outer review loop + done-bar.** The REVIEW⇄revise loop over
  `ensemble_review`, the configurable done-bar evaluator, the pass ceiling +
  escalation, the echo guard. Drives the PR phase first (single mode, simplest).
- **P2 — Proposal phase (DRAFT + REVIEW).** Add the `drafting` DRAFT step and the
  proposal-review loop; wire `gh pr create`/merge for the proposal PR.
- **P3 — Human gates + fail-return edges** end to end, with the durable digest,
  PR/worktree drift validation, mergeability/auth checks, and reason-to-brief
  feedback. This closes the full loop.
- **P4 — Config surface** for all keys + generated docs + tests (lands with the
  phase that first reads each key, not deferred).

P0/P1 are useful standalone (a directed, looped PR reviewer with a durable
ledger) before the full idea→merge pipeline of P2/P3.

## §10 Validation

- **Loop correctness** — a fixture proposal/diff with N seeded blocking issues:
  the loop reaches the done-bar only after all N are resolved; the digest counts
  match the engine items; the ledger records each pass's `run_id`/cost.
- **DRAFT/REVIEW callable split** — the proposal phase invokes a draft-capable
  roundtable path that returns an `artifact`; the review phases invoke the
  review-capable path that returns structured items for the done-bar.
- **Ledger compatibility** — if reusing DB1 `pipelines`, old autopilot
  `start/status/list/cancel/resume/link-*` behavior remains intact; if using a new
  table, pipeline IDs and command names are unambiguous.
- **Run-store separation** — kill/restart after child `/v1/runs` records are gone;
  the pipeline still resumes from DB1 and marks missing child run details as
  historical evidence, not lost state.
- **Done-bar config** — each of the three bars stops the loop at the right point
  (suggestions block under bar 2; questions must be answered under bar 3).
- **Pass-ceiling escalation** — with a low cap and an unresolvable seeded issue,
  the loop escalates to the human at the cap and never auto-passes.
- **Resumability** — kill and restart between a review pass and a gate; the ledger
  restores state and the gate is answerable post-restart.
- **Git/PR drift** — mutate the PR head SHA, target branch, or mergeability while
  paused at a gate; the pass action refuses stale evidence and requires a fresh
  review or human confirmation.
- **Large payload handling** — a diff/proposal larger than a single MCP/op-run
  payload is reviewed through artifact refs or chunks, and the ledger stores
  hashes so stale chunks are detected.
- **Fail-return** — a `fail --reason` re-enters the review loop with the reason
  present in the next brief (asserted in the reviewer prompt).
- **Panel diversity** — resolve `ensemble.reference_models` through agent config;
  a single resolved provider warns; a ≥2-provider panel does not.
- **Cost accounting** — cumulative per-phase cost matches the sum of per-pass
  `cost_usd`; the per-phase cap trips correctly.

## §11 Non-goals (v1)

- Server-side git or GitHub API (no PR fetch, no inline-comment posting); the
  agent uses `gh`/`git` (agent-directed-pr-review §11).
- Storing whole proposals or large diffs as unbounded DB blobs. DB1 stores refs,
  manifests, hashes, and compact digests; working files/blobs carry the large
  content.
- A fully autonomous, gate-less pipeline — the two human gates are mandatory by
  design; "configurable max passes" bounds cost, it does not remove the human.
- Engine changes to the roundtable. The pipeline is strictly on top.
- Multi-proposal/parallel-pipeline scheduling (one pipeline run at a time in v1).

## §12 Open questions

- **Ledger home:** a namespaced DB1 `roundtable_pipeline_runs` schema vs an
  explicit backwards-compatible extension of the existing DB1 `pipelines` table.
  `/v1/runs` / `openai_runs_store` is ruled out for checkpoints because it is a
  bounded live store and is not durable across restarts.
- **CLI/API namespace:** add `aimee pipeline ...` as a new surface, or extend the
  existing `autopilot` pipeline actions with gate/status operations? The answer
  must keep IDs and help text unambiguous.
- **Who is the driving agent at the gate?** When paused at a human gate across
  sessions, does a fresh agent resume from the ledger automatically, or does the
  human re-invoke `aimee pipeline resume <id>`?
- **Per-phase config:** do the proposal and PR phases want independent done-bars /
  ceilings by default, or one shared set with optional overrides?
- **DRAFT seeding:** does the `drafting` step take only the idea, or also a
  pointer to sibling/related proposals so the first draft starts grounded?
