# Full Autonomous Development

**Status:** APPROVED (human proposal gate passed 2026-06-19) — implementing.
**Builds on:** server-owned turn lifecycle (Phase 1, PR #514) — a turn/workflow
runs server-side and survives any client disconnect.

**Scope decision (user, 2026-06-19):** aimee must **NEVER self-initiate work**.
Autonomous execution is strictly *proposal-driven*: a human hands aimee a
proposal and aimee executes that proposal end-to-end. There is no agent-initiated
task generation, no background "find work to do" loop. WP-4 intake is the only
way an autonomous run begins, and it always originates from a human-submitted
proposal.

**Architecture invariant (user, 2026-06-19):** ALL autonomous action flows
*through the workflow engine* (`wfe`). The engine's block catalog, gates
(roundtable/human/CI), autonomy driver, persistence and audit are the single
substrate for autonomy — there is no side-channel that takes an autonomous
action outside the engine. The scheduler (WP-3) only ever resumes
`wfe_autonomy_run()`; intake (WP-4) only ever creates a `wfe` work item. This
guarantees every autonomous step is gate-governed, budget-bounded, persisted and
auditable by construction.

## 1. Goal (the user's words)

> "I should be able to hand you a proposal, you implement it in full, going to
> the roundtable with any questions or other things you need, and you implement
> it in full. Just because someone closes a tab in the browser doesn't mean
> stop; development should continue."

Concretely: a user submits a **proposal** (a development task/spec). Aimee then
drives the *entire* development lifecycle to completion **server-side and
unattended** — designing, planning, writing code via delegates, reviewing
itself via the roundtable, opening PRs, reacting to CI — pausing only at the
human gates the user reserved, and resuming the instant a gate is satisfied. No
browser tab needs to stay open.

## 2. What already exists (do NOT rebuild)

This proposal is mostly **wiring + hardening of existing parts**, not new
architecture:

- **Workflow engine (`src/workflow/wfe_*`)** — a block-composed state machine
  with persisted work-item state (db1 `lifecycle_*` tables), `gate.roundtable`,
  `gate.human`, `gate.ci`, verdicts, and approvals.
- **The default dev lifecycle (`config/workflows/build.yaml`)** — already
  composes: `author.proposal → gate.roundtable → pr.open → gate.human → plan →
  gate.roundtable → implement → freeze → gate.roundtable → pr.open → gate.human
  (pass→merge / fail→implement)`. User-editable (`aimee workflow new`).
- **Autonomy driver (`wfe_autonomy.c`)** — `wfe_autonomy_run()` auto-advances
  machine gates, auto-satisfies only *preauthorized/optional* human gates, parks
  (`pending_human`) at every other human gate and at a degraded roundtable, and
  **never forges a human approval** (gate-override is a signed human-only action,
  capped at `WFE_MAX_OVERRIDES`).
- **Server-owned turn lifecycle (PR #514)** — turns publish to the presence ring
  and run to completion regardless of the connection; `turn_registry` owns
  cancel + child reaping; reconnect replays from a cursor.
- **Delegates + roundtable** — `/v1/delegate/{run,roundtable}`, vault-backed
  multi-model panels, on-demand exec.

## 3. The gap (what makes it NOT yet autonomous)

Three concrete gaps, all surfaced by reading the code:

1. **The delegate-driven blocks are integration-gated stubs.**
   `wfe_blocks.c`: `author.proposal`, `author.plan`, `implement`, `document`
   *construct* the delegate commands but do not actually dispatch a delegate to
   write the artifact/code and commit it to the work item's branch
   ("In production this dispatches `aimee delegate run`…"). `pr.open`/`merge`
   are likewise gated. **Without these, the workflow advances but produces no
   work.**

2. **Nothing drives the workflow forward unattended.** `wfe_autonomy_run()` runs
   a work item "as far as gate policies allow," then returns. Nothing re-invokes
   it when an async precondition clears — a delegate turn finishes, CI reports,
   or a human satisfies a gate. Today that re-invocation is a human action in the
   webchat. **A server-owned scheduler must resume parked work items on those
   events** (this is the piece the Phase-1 foundation makes safe: the driving
   loop and its delegate turns are server-owned, not connection-scoped).

3. **No proposal intake path.** There is no "hand aimee a proposal → spawn an
   autonomous work item running `build.yaml`" entrypoint. A user must hand-drive
   the webchat workflows tab.

## 4. Proposed design

### WP-1 — Real delegate-driven block executors
Wire `author.proposal`, `author.plan`, `implement`, `document` to dispatch real
delegates (inline `delegate_run_inline` / `/v1/delegate/run`) that produce the
artifact or code **into the work item's own git worktree/branch**, then commit.
Reuse the existing worktree isolation + write-guard machinery and the
source-authority TLS context (per [[delegate-concurrency-env-race]]). `implement`
is a bounded delegate fan-out over the plan's units; each unit lands on the
branch; failures are surfaced as a verdict, not a crash.

### WP-1b — Primary MANAGES; delegates DO; primary verifies (user, 2026-06-19)
The autonomous **primary agent is a manager, not a worker.** It never authors
artifacts or code itself and never does the hands-on verification labor — it
decomposes, dispatches, adjudicates, and loops. All production work and all
verification labor run in delegates (and automated gates). The `implement` stage
is a manager-driven loop:

1. **Split.** The primary decomposes the plan into independent, file/module-scoped
   work units sized for one delegate turn (minimal interdependence).
2. **Dispatch.** Each unit → an `engineer` delegate that implements it in the
   work-item worktree and commits (reuse `delegate_patch_coordinator` for parallel
   patch integration; per-delegate source-authority TLS isolates concurrent units,
   per [[delegate-concurrency-env-race]]).
3. **Verify as hard as possible — also via delegates/gates, not the primary's own
   hands.** In ascending cost: (a) mechanical — build compiles, targeted
   tests/lint pass (`gate.ci` / custom command blocks); (b) review — a
   `gate.roundtable` / `reviewer` delegate panel checks the unit against its spec;
   (c) adversarial — N skeptic verifier delegates prompted to *refute* risky
   changes; majority-refute → reject. The primary only *adjudicates* the verdicts.
4. **Re-delegate on reject (user, 2026-06-19).** When verification deems a unit
   unacceptable, the primary may **send it back to a *different* delegate** for
   rework (fresh perspective), not necessarily the original author — bounded by a
   per-unit retry cap (Q2); on exhaustion, park per the failure taxonomy (Q4).

Only verified units advance. This is a first-class engine concern built from the
existing block catalog (`gate.*`, delegate fan-out) — never an out-of-band action,
consistent with the all-autonomy-through-wfe invariant. The primary's role maps
to the engine's autonomy driver + a coordinator delegate; the workers are
ordinary delegates.

### WP-2 — Forge blocks
Wire `pr.open` and `merge` to the existing git/forge layer (`mcp_git_*`,
`git_forge_vault`, per-host creds) so the autonomous run opens a real PR and (on
a passed human gate) merges it. CI is consumed via `gate.ci`.

### WP-3 — Server-owned autonomy scheduler
A persistent server component that owns the set of active autonomous work items
and resumes `wfe_autonomy_run()` on the events that clear a park:
- a delegate/turn for the work item completes (hook off the turn registry);
- `gate.ci` transitions (CI webhook / poll);
- a human satisfies a `gate.human` (already an API mutation — fire the resume);
- a periodic backstop sweep (crash recovery), reusing the work-item persistence.
Bounded concurrency + per-work-item single-flight (mirror `turn_registry`'s lock
discipline). Cost/turn caps per work item; respects [[delegates-never-token-limited]]
for the *model* ceiling but caps *total* spend per run.

### WP-4 — Proposal intake
`POST /v1/dev/submit {proposal_md, workflow?: "build", limits?}` → creates a
work item bound to `build.yaml` (or a named workflow), seeds the proposal
artifact, and registers it with the scheduler. Webchat gets a "Submit for
autonomous development" affordance; the run is observable live (presence ring)
and after disconnect (cursor replay). The two human gates (proposal-approval,
final PR pass/fail) surface as actionable notifications.

### WP-5 — Safety rails (non-negotiable)
- Human gates are **never** auto-satisfied unless the user marked them
  preauthorized for that run; gate-override stays human-only and capped.
- Every autonomous commit/PR is attributed to the run and fully audited.
- Hard per-run budget ceiling (turns, tokens, wall-clock); on breach → park
  `pending_human`, never silently continue.
- Honor [[no-coauthor-trailers]] in all generated commits/PRs.
- Default branch protection respected: autonomous merges only to `testing`,
  never directly to `main` (promotion stays a human action).

## 5. Open questions for the roundtable

(Per the directive to consult the roundtable for opinions.)

- **Q1.** Scheduler home: a new server subsystem vs. extending the existing
  compute-pool/coord-dispatcher? Trade-offs in lock discipline + crash recovery.
- **Q2.** `implement` granularity: one delegate for the whole change vs. a
  fan-out over plan units with a patch-coordinator merge — which is more robust
  for unattended runs, and how to bound retries?
- **Q3.** CI feedback loop: webhook vs. poll for `gate.ci`; how an autonomous run
  reacts to a red CI (auto-loop back to `implement` with the failure as input —
  how many times before parking?).
- **Q4.** Failure taxonomy: which failures park for a human vs. auto-retry vs.
  terminal-reject, to avoid both runaway loops and premature abandonment.
- **Q5.** Multi-run isolation: N concurrent autonomous work items each need their
  own worktree/branch/session — confirm the worktree-GC + source-authority TLS
  story holds under this load.

## 6. Human gates for THIS work

1. **This proposal** — approve scope before implementation (current gate).
2. Implementation plan — roundtable-reviewed, then a brief human ack.
3. Final PR(s) — human pass/fail before merge to `testing`.

## 7. Phasing

- **Phase A:** WP-1 + WP-2 (real blocks) behind a default-off flag; a human still
  drives advancement in the webchat. Proves the blocks produce real work.
- **Phase B:** WP-3 + WP-4 (scheduler + intake) — true unattended end-to-end.
- **Phase C:** richer policy (failure taxonomy, budget tuning, multi-run scaling).

Each phase is independently shippable and roundtable-gated.
