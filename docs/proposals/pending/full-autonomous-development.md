# Full Autonomous Development

**Status:** APPROVED (human proposal gate passed 2026-06-19) — **implementing,
PARTIAL.** Stays in `pending/` (not `done/`): the safety floor is begun but not
complete. See **Closeout status** below.

## Closeout status (2026-06-28)

Reconciled against the tree + a security design-roundtable (the user delegates
decisions to the roundtable). **What's shipped, what remains, what's deferred, and
one ratified deviation — recorded here rather than rewriting the approved text.**

**Shipped (pre-existing, do-not-rebuild):** WP-1 delegate seam + live provider
(default-on); WP-4 intake (`POST /v1/dev/submit`); WP-3 scheduler (sweep + intake /
human-gate notify hooks); WP-5 partials (human-gate park, override cap, audit,
per-item USD cap).

**Shipped this closeout:**
- **F1a — WP-5 safety rails (PR #856).** A fail-closed autonomous **merge-target
  guard** (`wfe_autonomous_base()`, default `testing`; refuses main/master/release*
  — case-insensitive, anchored) wired into `exec_pr_open`/`exec_merge`, plus per-run
  **turn + wall-clock caps** that park `budget_exceeded`. The rails land FIRST so
  nothing downstream can run away or merge to a protected branch.
- **F3a — WP-1b mechanical verify gate (PR #860).** `exec_implement` now only
  advances a unit that PASSES the §1 `git_verify format=json` gate (shipped in
  autonomous-dev-execution-substrate). New `wfe_verify_provider_t` seam + live
  provider; the top-level verdict is parsed with cJSON and a missing provider /
  non-pass / unparseable verdict FAILS CLOSED; failures loop (engine bounds via
  `stage_attempt` → parks `max_attempts`). The roundtable's "essential" manager-loop
  backbone; the N-skeptic adversarial fan-out + patch-coordinator are Phase-C depth.
- **F1b — functional USD budget cap (PR #862).** A server-side wall-clock cost
  estimate (`wfe_autonomy_cost_estimate`, AIMEE_AUTONOMY_USD_PER_SEC) is threaded
  into every step's `cost_usd` (charged on loop/fail/advance alike), and intake sets
  a default per-run cap, so the engine's per-run USD cap now actually trips
  `budget_exceeded` (it was dead — steps reported 0.0). Completes the WP-5 budget
  story (turns + wall-clock + USD).

**Shipped this closeout (cont'd):**
- **F2 — per-work-item git worktree isolation (PR #865).** Each run's producing
  blocks act in a locked worktree `aimee/wi/<id>` (created lazily, persisted,
  reused-only-if-on-disk, flock-serialized, partial-state scrubbed, fallback to the
  shared repo on failure); terminal cleanup wired into the autonomy driver. A
  worktree is a path convenience, **not** a sandbox — the real seccomp/namespace
  sandbox stays a GA gate.

- **F5a — single-flight is SATISFIED BY THE SEQUENTIAL SCHEDULER** (resolved by
  design). `wfe_autonomy_run` runs only on the single `wfe_scheduler` thread
  (concurrency = 1; `notify` only signals a cond var), so two runs of the same work
  item can never overlap — a per-item claim primitive would be dead code today. The
  DB-CAS+TTL claim + per-target merge serialization become load-bearing only when the
  scheduler is made concurrent (Phase-C scale), and are tracked there.
- **F4 — live forge `wfe_forge_t` SHIPPED (PR #868, `src/server/wfe_live_forge.c`).**
  A server-side provider behind the `wfe_live_forge_enabled` config flag
  (**default-OFF**; `test_config` asserts the default), registered (via
  `wfe_live_forge_register` in `wfe_autonomy_register`) ONLY when the operator opts in;
  otherwise the engine keeps its fail-closed stub. Every op re-checks `forge_allowed()`
  (the flag AND the F1a merge-target rail) — including immediately before each mutating
  call (TOCTOU-safe) — so a flag flip or protected-base misconfig can never open/merge
  a real PR. open = `git push` (vaulted via `mcp_git_run`) + `gh pr create --base
  <autonomous_base>`; ci/mergeable/is_merged/merge map `gh` output to the engine enums,
  unknown → fail closed. `exec_pr_open` derives the work-item branch `aimee/wi/<id>`.
  Closes the criterion-5 **code** gap
  [autonomous-dev-execution-substrate.md](done/autonomous-dev-execution-substrate.md)
  deferred here. The CODE floor for full-autonomous-development is now complete.

**Remaining = enable-gates only (no further code floor):**
- **intake-auth hardening** on `POST /v1/dev/submit` (it is already `CAP_DELEGATE`;
  add per-principal rate/concurrency caps + submitter→run audit binding) — a hardening
  precondition for the live-forge enable.
- the **operator deployment gates** to flip `wfe_live_forge_enabled` on: branch
  protection on RakuenSoftware/aimee, fine-grained scoped + rotated forge creds, the
  break-glass TTL enable, and a real execution sandbox (see GA gates). A production
  live-forge roundtrip stays a deployment-tier `validation-pending` check, never a
  closeout test.

**Ratified deviation — default-OFF, not default-on.** §7 mandates the live forge
ship *default-on*. The security roundtable ruled that unsafe (it would let any
submitted proposal auto-open+merge real PRs before the rails are proven) and
directed **default-OFF behind `wfe_live_forge_enabled`** with a TTL/break-glass
promotion. This closeout adopts that: default-on is a deployment-state promotion,
not a code default. The approved §7 text is left intact; this bullet is the
ratifiable deviation record.

**Deferred to Phase C / explicit GA (deployment) gates — NOT closeout work:**
N-skeptic adversarial fan-out + patch-coordinator; CI-webhook resume (vs the 30s
sweep); richer failure taxonomy; multi-forge; a REAL execution sandbox
(seccomp/namespace) before live enable; **branch protection on
RakuenSoftware/aimee**; fine-grained scoped + rotated forge creds; the break-glass
TTL enable of `wfe_live_forge_enabled`; and a **production live-forge roundtrip**
(deployment-tier `validation-pending` against a sandbox/fork — never a closeout
test, never auto-merge to production as acceptance).

```yaml deferred
- {tier: deployment, reason: "live-forge enable (F4) requires branch protection + scoped/rotated creds + break-glass TTL on RakuenSoftware/aimee; a production roundtrip is a manual deployment gate, never auto-merged as acceptance", deferred_to: full-autonomous-development.md}
- {tier: hardware,   reason: "real execution sandbox (seccomp/namespace) for delegate code execution is a GA gate before live enable", deferred_to: full-autonomous-development.md}
```

---

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

## 7. Phasing & default-on (user, 2026-06-19)

Autonomous development is **core functionality and ships DEFAULT-ON** — not gated
behind an opt-in flag. "Default-on" means the capability is live: a submitted
proposal runs autonomously out of the box. Safety is enforced by the *gates*, not
by a master off-switch: human approval gates are never auto-satisfied unless the
submitter preauthorized them, per-run budget ceilings park rather than run away,
and autonomous merges only ever target `testing` (promotion to `main` stays a
human action). (Phase A's seam was inert only because no live provider was
registered yet; Phase B registers it on by default.)

- **Phase A (done):** WP-1 + WP-2 real delegate/forge seams.
- **Phase B:** the live delegate provider (manager loop) + WP-3 scheduler + WP-4
  intake — true unattended end-to-end, registered on by default.
- **Phase C:** richer policy (failure taxonomy, budget tuning, multi-run scaling).

Each phase is independently shippable and roundtable-gated.
