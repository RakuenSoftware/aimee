# Proposal: Agent roundtable authoring pipeline (idea → reviewed proposal → implementation → reviewed PR)

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-11 (revised post-PR-#183 fifth review)
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
  `testing`) and the in-tree `ensemble_review` review surface, with the remaining
  review contract narrowed to stable terminal result shape/payload semantics. It
  routes PR/merge/diff work through Aimee's workspace-aware git/PR tools rather
  than assuming a raw local `gh`/`git` shell. No changes to the roundtable engine
  itself.

## Design at a glance

A closed-loop authoring pipeline that turns a one-line idea into a merged,
panel-reviewed implementation, with **two human gates** and **two roundtable
quality gates**:

```
[Human] idea
  → DRAFT roundtable generates a first proposal skeleton (8KB-capped; expanded in review)
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

## PR #183 second review — feasibility and accuracy against the engine

A second pass that read the engine buffers and the in-tree MCP surface (not just
the proposal headers) found four more items — one of which contradicts a core
premise.

8. **The `agent-directed-pr-review` dependency is overstated; the review surface
   is already on `testing`.** `ensemble_review` is fully registered
   (`src/mcp_tools.c:610`) and dispatched (`handle_mcp_ensemble_review`,
   `server_mcp.c:1502`): its schema already accepts a `diff` (minLength 20) **and**
   a `brief` (string or `{focus,fixes,invariants,questions}`), queues
   `delegate.roundtable`, returns a run id, and its result already exposes the
   structured `items`/`items_round` vs `artifact_round` distinction. So P1a (brief)
   + P1b (items) + P1c (tool) are effectively landed for **review** mode. The real
   remaining dependency is narrow: confirm the `/v1/runs` result contract is stable
   enough to evaluate the done-bar, and add a **draft**-capable callable path
   (#9/§8). **Resolved in §8 and the What-exists table** (downgraded from "hard
   dependency on an unmerged proposal" to "review surface in tree; verify result
   contract").

9. **DRAFT mode cannot generate a full-length proposal.** The roundtable
   `artifact` is `xstrdup0(res.response)` (`delegate_ensemble.c:592`), and every
   participant/aggregator `response` is a fixed `char[8192]`
   (`delegate_ensemble.h:14`) produced under a ~4096-token aggregator cap
   (`:390`). 8 KB is ~150–250 lines of markdown; real proposals in this tree run
   300–1400 lines. So "DRAFT generates a first proposal" is **not feasible as one
   call** — the artifact truncates. This is distinct from gap #5 (storage): #5 is
   "don't inline large artifacts into the ledger," this is "the engine cannot
   *emit* one." **Resolved in §1, §2, and §11** by having DRAFT produce a
   **skeleton/outline** that the author-revise loop expands section by section,
   never expecting a complete proposal from a single DRAFT artifact.

10. **The done-bar result must be captured to DB1 at pass completion — the
    `/v1/runs` record is eviction-prone.** The done-bar is read by polling
    `/v1/runs/{id}`, backed by the same `openai_runs_store`
    (`OPENAI_RUNS_STORE_MAX 256`, oldest-reused, in-process, non-durable —
    `openai_runs_store.c:15`). A long pass (up to `roundtable_deadline_ms`, default
    10 min), 256 sibling runs, or a restart can evict the record **before** the
    agent reads it. Gap #2 moved the *ledger* to DB1 but left the *result-capture
    race* open. **Resolved in §4** by requiring the structured pass result (items,
    counts, `converged`, cost) to be persisted into the DB1 ledger the moment the
    run reaches a terminal state — the `/v1/runs` record is a transient read, never
    the retained result.

11. **The "pass → merge" edges need explicit merge authority and CI assumptions.**
    Both gates end in the agent merging a PR to `testing`, but this repo's flow
    bases PRs on `testing`, may require `gh pr merge --admin` (CI has been
    billing-suspended, so checks can hang), and enforces branch policy. The pass
    edge cannot assume an unattended `gh pr merge` succeeds. **Resolved in §5 and
    §8** by stating the merge is an explicit, possibly admin-gated step the agent
    performs only after the human pass, with CI-state and branch-policy
    preconditions surfaced rather than assumed.

## PR #183 third review — orchestration ownership and gate correctness

The updated proposal is much closer to the implementation, but a third pass found
several remaining gaps at the boundary between a durable pipeline and Aimee's
current live-tool/workspace surfaces:

12. **Eager result capture cannot be a best-effort driving-agent poll.** §4 says
    the driving agent copies `/v1/runs` results into DB1 "the moment" the child run
    reaches terminal. If the agent is asleep, disconnected, waiting at a gate, or
    delayed behind other work, the same eviction/restart race from #10 remains.
    The capture must be owned by the pipeline orchestrator itself: either a
    server-side pipeline worker submits the child run and writes the terminal
    result to DB1 in the same completion path, or the pipeline does not advance
    and explicitly marks the pass `lost_result`. **Resolved in §4 and §9.**
13. **The done-bar must treat degraded or incomplete runs as not done.** The
    current bars check `converged` and item severities, but the real result also
    carries `truncated`, `degraded`, `cost_capped`, `deadline_hit`, `cancelled`,
    `items_round`, and `artifact_round`. A pass with truncated items, a degraded
    best-effort result, a cost/deadline stop, cancellation, or ambiguous
    items/artifact provenance cannot satisfy a correctness gate. **Resolved in §3
    and §10.**
14. **The questions-done bar needs a cap policy.** `roundtable_result_t` can track
    only `ROUNDTABLE_MAX_QUESTIONS` (16) answered questions / coverage gaps, and
    long briefs are 4 KB-normalized. `zero_blocking_questions_answered` must reject
    or split briefs with >16 questions; otherwise "every brief question answered"
    silently means "every retained question." **Resolved in §3 and §10.**
15. **GitHub operations must use Aimee's workspace authority, not raw local shell
    assumptions.** Aimee has a shared/detached/mirror workspace resource plane,
    `mcp_git_run`, `git_pr`, and a per-workspace forge-token broker. A pipeline
    resumed from another surface may not have the same local cwd or `gh` auth as
    the original session. All git/gh operations need to route through the existing
    workspace provider/git tools and record the workspace id/provider plus forge
    token availability. **Resolved in §4, §5, §8, and §10.**
16. **The pipeline needs branch/worktree isolation rules.** "One pipeline at a
    time" does not protect the user's current checkout. The proposal must require
    a dedicated proposal branch and implementation branch, and either a dedicated
    worktree or an explicit clean-worktree preflight before any write. Dirty
    user changes are a hard stop unless the user explicitly opts into using that
    checkout. **Resolved in §1, §4, §5, and §10.**
17. **The dependency section regressed into duplicate/stale wording.** §8 repeats
    the DRAFT dependency and the relationship section still describes
    `agent-directed-pr-review` as a pending hard dependency even after #8 says
    review mode is in tree. The proposal should make the contract precise:
    review-mode surface exists; remaining gaps are draft-callability, stable
    terminal result capture, and pipeline orchestration. **Resolved in the
    relationship table and §8.**

## PR #183 fourth review — reconciling capture ownership with the Hybrid decision

The third review correctly demanded orchestrator-owned result capture (#12), but
its wording ("in the same completion path") implies a *server-side* orchestrator,
which collides with the chosen **Hybrid** architecture (the external agent drives
the loop; only state is persisted). Two items resolve that, and a real seam exists
to make it concrete.

18. **"Orchestrator-owned capture" is unresolved against Hybrid — name the seam.**
    Under Hybrid the driving agent owns the *loop*, but it cannot be "in the same
    completion path" as a child run; if capture depends on the agent polling, #10's
    eviction race is still live whenever the agent is asleep or at a gate. The
    resolution is **not** to promote the agent to a server-side orchestrator (that
    is the architecture the user did *not* pick). It is to use the worker that
    *already runs the roundtable*: `op_run_worker_run`
    (`server_http_routes.inc:339`) executes the async `delegate.roundtable` and
    calls `openai_runs_store_finalize` at terminal. Thread a `__pipeline_pass_id`
    into the request body exactly as `__run_id` is injected today (`:419`), and
    extend that finalize path to **also write the structured terminal result into
    the DB1 pipeline-ledger row**. The agent stays the loop/gate orchestrator; the
    server worker — which already runs and finalizes the run — closes the capture
    race durably. This is the genuine Hybrid: agent owns the loop, server owns
    durable capture of each child run it already executes. **Resolved in §4 and §9.**

19. **`lost_result` should re-run the pass, not escalate to a human.** A capture
    miss (eviction, restart mid-pass) is an infrastructure hiccup, not a
    correctness decision that needs a human. The pipeline should **auto re-run the
    review pass** — it is idempotent over the same artifact; a re-run produces a
    fresh, valid result reviewing the same input — bounded by the pass ceiling and
    cost cap, under a **stable `pipeline_pass_id`** so the lost attempt's cost is
    booked as overhead without double-counting the pass. Escalate to the human only
    if capture keeps failing across re-runs (a real fault), never on the first
    miss. **Resolved in §3 and §4.**

## PR #183 fifth review — pass identity, worker capture, and replay safety

The fourth review picked the right Hybrid seam (`op_run_worker_run`), but the
proposal still needs to define how pipeline pass identity reaches that seam
safely and how retry accounting stays correct.

20. **`__pipeline_pass_id` is not forwarded by today's `ensemble_review` tool.**
    `handle_mcp_ensemble_review` currently constructs a fresh body containing only
    `prompt`, `mode:"review"`, `brief`, `rounds`, and `turns` before calling
    `server_http_submit_op_run`; unknown caller args are not copied. So the text's
    "thread `__pipeline_pass_id` into the request body" cannot be implemented
    from the pipeline unless the callable surface grows a pipeline-owned
    `pipeline_pass_id` parameter (or `server_http_submit_op_run` gains metadata
    parameters) and forwards it deliberately. **Resolved in §4, §8, and §10.**
21. **Pass IDs must be server-owned and state-validated.** A user-facing MCP arg
    named `__pipeline_pass_id` would let any caller with `CAP_DELEGATE` try to
    write arbitrary pass rows. The pipeline surface should accept a normal
    `pipeline_pass_id` only from the pipeline orchestrator/gate command, validate
    that the pass belongs to an active roundtable pipeline in the expected state,
    and then inject the private `__pipeline_pass_id` internally. Raw
    double-underscore fields remain server-private. **Resolved in §4, §8, and
    §10.**
22. **Capture only works on the async op-run path.** The worker capture seam is
    present for `/v1/delegate/roundtable` and `server_http_submit_op_run`, but a
    direct synchronous `server_dispatch("delegate.roundtable")` call bypasses
    `op_run_worker_run`. The pipeline must require async op-run submission for
    both DRAFT and REVIEW passes, or add an equivalent capture hook to any direct
    path it uses. **Resolved in §2, §4, §8, and §10.**
23. **A stable pass id still needs per-attempt rows.** Re-running a lost pass under
    the same `pipeline_pass_id` is correct for loop accounting, but the ledger must
    preserve each attempt (`attempt_no`, `run_id`, status, terminal flags, result
    hash, captured/lost marker, cost if known). Otherwise a retry overwrites the
    evidence needed to explain overhead and repeated capture failures. **Resolved
    in §4 and §10.**
24. **Replay is idempotent only if the artifact input is hash-pinned.** A lost
    result should be re-run over the *same* proposal/diff. Before retrying, the
    pipeline must compare the current artifact/diff hash to the pass input hash;
    if it changed, the old pass is stale and a new pass id is required. **Resolved
    in §4 and §10.**
25. **The worker must persist failed/cancelled terminal states too.** Done-bar
    evaluation needs completed results, but durable diagnostics need failed,
    cancelled, malformed, and capture-failed attempts as well. The capture hook
    must write terminal status for every op-run outcome, not just successful
    roundtable JSON. **Resolved in §4 and §10.**

## Relationship to existing proposals

- **The roundtable engine is done** (`docs/proposals/done/agent-roundtable-collaborative-drafting.md`,
  PRs #136/#142, on `testing`). `delegate_roundtable_run`
  (`src/headers/delegate_ensemble.h`) provides both `ROUNDTABLE_DRAFT` (produces an
  improved `artifact`) and `ROUNDTABLE_REVIEW` (produces deduped, severity-tagged
  `items[]` with a corroboration `count`, plus `answered_questions[]` and
  `coverage_gaps[]`), a deterministic `converged` predicate, `best_round`, and
  inherited cost/deadline bounds. **This proposal adds no engine code.**
- **`agent-directed-pr-review.md` (pending) is no longer a broad hard dependency
  for review mode.** The current tree already exposes `ensemble_review` with a
  directed brief and structured roundtable result over the async bridge. This
  pipeline depends only on the stable interface contract that proposal documents:
  terminal result shape, polling/cancellation semantics, payload bounds, and the
  item/artifact provenance fields. Draft callability and durable pipeline
  orchestration remain this proposal's responsibility.
- **`agent-roundtable-collaborative-drafting.md` (pending residual)** lists
  deeper convergence/economics tests; this proposal's validation (§10) exercises
  exactly those at pipeline scale.

## What exists vs. what is net-new

| Capability | Status |
|---|---|
| Multi-round panel, DRAFT + REVIEW modes, deterministic convergence, dedup, severity, cost/deadline bounds | **exists** (engine, `testing`) |
| Directed review (brief), structured items returned, `ensemble_review` MCP tool (review mode) | **in tree on `testing`** (`mcp_tools.c:610`, `handle_mcp_ensemble_review`); verify result contract (§8) |
| A **draft**-capable callable path | **gap** — `ensemble_review` forces review mode; DRAFT needs `/v1/delegate/roundtable --mode draft` or a draft MCP sibling (§8) |
| PR open / merge, diff capture | **exists via workspace-aware git/PR tools** (`git_pr`, `mcp_git_run`, `gh`/`git` underneath) |
| Outer REVIEW⇄revise loop, done-bar evaluation, pass ceiling + escalation | **net-new** (§3) |
| The two human gates + the two fail-return edges | **net-new** (§5) |
| Persisted, resumable roundtable pipeline ledger (hybrid state) | **net-new or explicit DB1 pipeline extension** (§4) |
| Config: done-bar, pass ceiling, outer cost cap | **net-new** (§6) |

## §1 The pipeline state machine

States (persisted in the §4 ledger):

`drafting → proposal_review → gate1_pending → implementing → pr_review → gate2_pending → done` (plus `failed`/`abandoned`).

Transitions:

1. **drafting** — DRAFT roundtable turns the human idea (+ any seed brief) into a
   first proposal **skeleton**, not a finished proposal. One roundtable call in
   `ROUNDTABLE_DRAFT` mode (through `/v1/delegate/roundtable` / `aimee delegate
   roundtable --mode draft`, or an explicit draft-capable MCP surface) produces the
   working `artifact` — but that artifact is `char[8192]`-capped (#9), so DRAFT
   yields a section outline + goal/scope, which the `proposal_review` author-revise
   loop expands section by section. A full proposal is never expected from one
   DRAFT call. The pipeline creates or selects a dedicated proposal branch/worktree
   before writing the proposal file.
2. **proposal_review** — the §3 outer loop: REVIEW the draft, author-revise from
   the items, re-REVIEW, until the done-bar (§3). Then the agent opens the proposal
   PR through the workspace-aware PR surface (base `testing`, per repo flow) and
   moves to `gate1_pending`.
3. **gate1_pending** — human gate 1 (§5). **pass** → merge proposal PR, move to
   `implementing`. **fail** → the human's reason is appended to the brief and the
   state returns to `proposal_review`.
4. **implementing** — the agent implements the merged proposal on a dedicated
   implementation branch/worktree (normal coding; not a roundtable activity),
   opens the implementation PR, captures the diff.
5. **pr_review** — the §3 outer loop in REVIEW mode over the diff: REVIEW,
   agent-fix, re-REVIEW, until the done-bar.
6. **gate2_pending** — human gate 2 (§5). **pass** → merge the implementation PR,
   move to `done`. **fail** → reason → brief, state returns to `implementing`.

Every state is durable (§4) so a human gate can be answered hours or days later,
across a server restart or a new agent session.

## §2 The two roundtable applications

Both phases use the **same** engine; they differ only in input and brief.

- **Proposal phase (B).** Input = proposal markdown. **Both** modes (per decision):
  DRAFT to generate the first **skeleton** in `drafting` (8 KB-capped, #9), then
  REVIEW to gate it in `proposal_review`, with the driving agent expanding the
  skeleton into the full proposal across the author-revise passes. The full
  proposal lives in the working file/PR, not in the DRAFT artifact; REVIEW reviews
  the file (or chunked sections for large proposals, like the diff path below),
  not the truncated artifact. Pipeline-owned DRAFT and REVIEW calls must use the
  async op-run path so server-worker result capture runs. The brief carries the
  proposal's *goal*, the *invariants* it must satisfy, and the human's seed
  *questions*.
- **PR phase (A).** Input = unified diff (normally `git diff <base>...HEAD`;
  aimee has no PR-fetch and needs none — `agent-directed-pr-review` §6). REVIEW
  mode only; the agent applies fixes between passes. For large diffs, the driving
  agent must pass an artifact file/path or chunked diff slices rather than an
  unbounded inline string, and the ledger stores the diff ref plus content hash.
  Pipeline-owned PR reviews use the async op-run path with validated pass
  metadata, not a direct synchronous roundtable dispatch. The brief carries the
  *fixes just applied*, the *invariants* the change must not break, and the
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

Every bar first requires a valid completed result: `truncated == false`,
`degraded == false`, `cost_capped == false`, `deadline_hit == false`,
`cancelled == false`, and result provenance that the gate digest can explain. For
review-mode done-bars, `items_round` must be the round being evaluated; if the
surfaced artifact comes from a different `artifact_round`/`best_round`, the digest
must say so and the phase cannot silently pass on mismatched evidence.

- `zero_blocking` *(default)* — `converged == true` and no `items[]` of
  `severity == "blocking"`. Suggestions/nits are surfaced in the gate digest but
  don't block.
- `zero_blocking_suggestions` — also requires no `suggestion`-severity items
  (nits allowed). Stricter, more passes.
- `zero_blocking_questions_answered` — `zero_blocking` plus every accepted brief
  question present in `answered_questions[]` and `coverage_gaps[]` empty. The
  brief must contain at most `ROUNDTABLE_MAX_QUESTIONS` (16) questions or be split
  into multiple review passes; overflow is not treated as answered.

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
- per-attempt history under each pass: `attempt_no`, child `run_id`, submitted and
  terminal timestamps, captured/lost status, terminal flags, result hash, and cost
  if the result was available;
- repository/worktree state: repo root, remote, base branch, head branch,
  workspace id/provider (`shared`, `detached`, or `mirror`), dedicated worktree
  path if any, head/base commit SHAs, dirty-state snapshot, PR number(s), PR URLs,
  merge SHAs, forge-token/`gh` authority status, and last checked mergeability;
- the human-gate verdicts, fail reasons, actor/timestamp, and resume action taken.

**Result capture is server-worker-owned, not agent-poll (#10/#12/#18).** The
done-bar is read from the roundtable run's structured result, but that lives in
`/v1/runs` / `openai_runs_store`, which is bounded (`OPENAI_RUNS_STORE_MAX 256`,
oldest-reused) and non-durable. Capture must not depend on the *driving agent*
being awake to poll — under Hybrid the agent owns the loop, not the child-run
completion path. Instead, the worker that **already runs the roundtable** owns
capture: `op_run_worker_run` (`server_http_routes.inc:339`) executes the async
`delegate.roundtable` and calls `openai_runs_store_finalize` at terminal. The
pipeline creates a DB1 pass row first, then the pipeline-owned callable surface
forwards a normal `pipeline_pass_id` that is validated against the active
pipeline/pass state and injected internally as private `__pipeline_pass_id` (raw
double-underscore fields are not user API). `server_http_submit_op_run` /
`rh_dispatch_op_async` must preserve that private metadata alongside the existing
`__run_id` injection (`:419`). Extend the finalize path, preferably through a
narrow op-run-finalize hook rather than hard-coding roundtable ledger writes into
generic HTTP routing, to **also persist the structured pass result** (items +
severities + `count`, `converged`, validity flags, `items_round`,
`artifact_round`, counts, `cost_usd`, `rounds_run`) into the DB1 ledger row before
the run record can be evicted. Failed, cancelled, malformed, and capture-failed
terminal states are recorded too. The `/v1/runs` id is retained only as a
historical pointer; the ledger row is the source of truth for the done-bar and the
gate digest.

If a result is still not captured (a genuine fault), the attempt is marked
`lost_result` and the pipeline **auto re-runs the review pass** under the same
stable `pipeline_pass_id` but a new `attempt_no`/`run_id`. That retry is idempotent
only if the stored artifact/diff hash still matches; if the input changed, the old
pass is stale and the pipeline starts a new pass instead. Retries are bounded by
the pass ceiling and cost cap, with lost attempts booked as overhead when cost is
known. A human is involved only if capture keeps failing across re-runs (#19) —
never on a single transient miss.

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
worktree/PR state through Aimee's workspace-aware git surface (`git_pr` /
`mcp_git_run`, not an unqualified local shell): the PR still exists, its head SHA
matches the ledger or the digest is marked stale, the base branch is still the
intended target (`testing`), the dedicated worktree is clean enough for the
operation, `gh`/GitHub authority is available through local auth or the
per-workspace forge-token broker, and mergeability has not changed underneath the
gate. A failed validation returns to the relevant review phase or asks the human
for a fresh verdict with the stale evidence called out.

**Merge is an explicit, policy-aware step, not an assumed `gh pr merge` (#11).**
This repo bases PRs on `testing`, enforces branch policy, and has run with CI
billing suspended (so required checks can hang or fail fast). The pass→merge edge
therefore: (a) runs only after the human pass; (b) surfaces CI/mergeability state
in the gate digest rather than assuming green; (c) may require `gh pr merge
--admin` and states so; and (d) records the merge SHA in the ledger. If the merge
cannot complete under policy (auth, protected branch, hung checks), the pipeline
parks at the gate with the reason surfaced — it never reports a phase advanced
when the merge did not land.

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

- **Mostly satisfied (review mode):** `ensemble_review` is already on `testing`
  with the brief (string or `{focus,fixes,invariants,questions}`), the structured
  `items`/`items_round`/`artifact_round` result, and the run-id/poll lifecycle
  (`mcp_tools.c:610`, `handle_mcp_ensemble_review`). The remaining dependency on
  `agent-directed-pr-review` is **narrow**: confirm the `/v1/runs` result shape is
  stable and complete enough to evaluate the done-bar (the items array with
  severities and `count`), and treat any tightening of that contract as a shared
  interface, not a blocker. The pipeline is *not* gated on that proposal merging
  wholesale.
- **Hard for proposal drafting (open gap):** a callable `ROUNDTABLE_DRAFT` path.
  `ensemble_review` forces review mode, so DRAFT must go through
  `/v1/delegate/roundtable` / `aimee delegate roundtable --mode draft`, or a narrow
  draft MCP sibling — never by overloading the review-only tool. And per #9 the
  DRAFT artifact is ~8 KB-capped, so this path yields a skeleton, not a full
  proposal.
- **Hard:** durable DB1 checkpointing for the roundtable pipeline. `/v1/runs`
  remains a child-run polling surface only because its store is bounded and
  non-durable.
- **Hard:** a pipeline-owned async submission contract. The existing
  `ensemble_review` MCP surface does not forward arbitrary metadata, so the
  pipeline needs an explicit `pipeline_pass_id`/attempt submission path that
  validates state and injects private `__pipeline_pass_id` before
  `op_run_worker_run` finalizes the child run.
- **Hard:** workspace-aware git/forge authority for PR create/merge and diff
  capture. Pipeline git/gh operations must route through Aimee's git tools or
  workspace provider so shared, detached, and mirror workspaces behave consistently.
- **Soft:** the `git diff` range helper (agent-directed-pr-review P2) for the PR
  phase input; otherwise the pipeline captures the diff through the
  workspace-aware git surface.
- **Repo flow:** PRs base `testing`; main promotion is separate and out of scope.

## §9 Phasing

- **P0 — Ledger + states + namespace decision.** Choose the DB shape
  (`roundtable_pipeline_runs` tables vs explicit migration of DB1 `pipelines`),
  add the state enum/transitions, artifact refs/hashes, pass/attempt rows, child
  run references, and the CLI/MCP/HTTP namespace decision (`pipeline` vs
  `autopilot` extension). Wire the server-worker capture seam: validate
  `pipeline_pass_id`, inject private `__pipeline_pass_id`, preserve it through
  async enqueue, and extend `op_run_worker_run`'s finalize path or hook to persist
  every terminal attempt to the ledger (#18/#20-#25). No loop yet; states/gates
  settable manually. Mergeable alone, with restart tests.
- **P1 — Outer review loop + done-bar.** The REVIEW⇄revise loop over
  `ensemble_review`, the configurable done-bar evaluator, the pass ceiling +
  escalation, the echo guard. Drives the PR phase first (single mode, simplest).
- **P2 — Proposal phase (DRAFT + REVIEW).** Add the `drafting` DRAFT step and the
  proposal-review loop; wire proposal PR create/merge through the workspace-aware
  git/PR surface.
- **P3 — Human gates + fail-return edges** end to end, with the durable digest,
  PR/worktree drift validation through the workspace-aware git surface,
  mergeability/auth checks, and reason-to-brief feedback. This closes the full
  loop.
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
- **Server-worker result capture (#10/#12/#18)** — drive ≥256 sibling runs (or
  force eviction) between a child run completing and any agent poll; the pass
  result is already in DB1 because `op_run_worker_run`'s finalize path wrote it
  (keyed by `__pipeline_pass_id`), **with the driving agent killed** for the
  window. The done-bar/digest are unaffected by the evicted `/v1/runs` record.
- **Pass-id forwarding (#20/#21)** — submit a pipeline-owned `ensemble_review` /
  draft roundtable pass and assert the private `__pipeline_pass_id` reaches
  `op_run_worker_run`; an arbitrary user-supplied double-underscore id is rejected
  or ignored, and a stale/non-owned pass id cannot mutate the ledger.
- **Async-only capture (#22)** — prove pipeline-owned DRAFT and REVIEW calls go
  through async op-run capture; any direct synchronous roundtable dispatch is
  either forbidden for pipeline passes or has an equivalent capture hook.
- **Lost-result re-run (#19)** — simulate a genuine capture fault: the pass is
  marked `lost_result` and auto re-runs under the **same `pipeline_pass_id`** (no
  double-counted pass; lost attempt booked as overhead), escalating to the human
  only after repeated capture failures, never on the first miss.
- **Attempt accounting (#23/#25)** — successful, failed, cancelled, malformed,
  capture-failed, and lost attempts all get attempt rows and preserve run IDs,
  terminal status, flags, result hashes, and known cost without overwriting prior
  attempts.
- **Replay hash pinning (#24)** — mutate the proposal/diff between a lost attempt
  and retry; the pipeline refuses to reuse the old pass id and starts a new pass
  because the artifact hash changed.
- **DRAFT skeleton + expansion (#9)** — a DRAFT call returns a skeleton within the
  8 KB cap without truncating mid-structure; the author-revise loop grows it to a
  full proposal in the working file, and REVIEW reviews the file/chunks, not the
  capped artifact.
- **Done-bar config** — each of the three bars stops the loop at the right point
  (suggestions block under bar 2; questions must be answered under bar 3).
- **Done-bar validity flags** — `truncated`, `degraded`, `cost_capped`,
  `deadline_hit`, `cancelled`, lost result, or ambiguous `items_round` /
  `artifact_round` provenance prevents a done-bar pass and escalates.
- **Question cap** — a brief with >16 questions is rejected or split before using
  `zero_blocking_questions_answered`; overflow cannot be silently treated as
  answered.
- **Pass-ceiling escalation** — with a low cap and an unresolvable seeded issue,
  the loop escalates to the human at the cap and never auto-passes.
- **Resumability** — kill and restart between a review pass and a gate; the ledger
  restores state and the gate is answerable post-restart.
- **Git/PR drift** — mutate the PR head SHA, target branch, or mergeability while
  paused at a gate; the pass action refuses stale evidence and requires a fresh
  review or human confirmation.
- **Workspace authority** — run proposal/implementation PR operations in shared
  and detached workspaces; git/gh calls route through `mcp_git_run`/`git_pr` and
  forge-token state is surfaced when required.
- **Branch/worktree isolation** — dirty user checkout blocks the pipeline unless
  the run has a dedicated branch/worktree or an explicit operator override.
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

- New server-side GitHub review/comment APIs (no PR fetch, no inline-comment
  posting). The pipeline uses Aimee's existing workspace-aware git/PR tool surface,
  which may call `gh`/`git` underneath with the right workspace/forge authority.
- Storing whole proposals or large diffs as unbounded DB blobs. DB1 stores refs,
  manifests, hashes, and compact digests; working files/blobs carry the large
  content.
- Expecting DRAFT mode to emit a complete proposal (#9). The 8 KB artifact cap
  means DRAFT produces a skeleton; the author-revise loop expands it in the working
  file. Generating long-form text inside the roundtable artifact is out of scope.
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
