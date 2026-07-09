# Proposal: sliced-lifecycle "build" workflow — proposal → plan → per-slice sub-PRs → acceptance → feature PR

- **State:** pending — design for roundtable review.
- **Author:** JBailes (via Claude Code)
- **Date:** 2026-07-09
- **Scope:** rework aimee's default full-lifecycle development workflow so it runs a
  real proposal-to-merge lifecycle end-to-end, and add the workflow-engine features
  that lifecycle needs but the engine does not yet support. Builds directly on the
  shipped workflow engine (`src/workflow/`, `docs/proposals/done/aimee-dev-lifecycle-workflow.md`).

## Goal

Given an **input** (a proposal *or* a bare user request), the engine should drive the
whole development lifecycle as autonomously as the workflow allows, pausing only where
the workflow demands a human:

1. The primary authors an **implementation/action plan** from the input.
2. The plan is submitted to the **roundtable together with the original proposal/request**,
   so the panel validates the plan actually satisfies what was asked. Standard roundtable
   behavior: non-convergence returns reasons to the author, who fixes and resubmits until
   the panel approves.
3. A **feature branch** is created.
4. The plan is **split into slices**; each slice runs its own **sub-PR** cycle:
   PR → roundtable review (resubmit-until-convergence) → **fully green CI** → merge into
   the feature branch.
5. When every slice has merged, the **whole feature branch is submitted to the roundtable
   with the original proposal** for an **acceptance review**: was the proposal completed,
   is the code quality sound, are any tests missing? Findings are fixed and resubmitted
   under standard roundtable behavior.
6. The end step opens a **PR from the feature branch to the default branch**, fixes on any
   roundtable feedback, then a **human-gated approval** of that PR — and it merges only
   once its **CI is fully green**.

The default workflow holds no special engine privilege; it is one composition of the
block catalog that users may clone and edit (`aimee workflow new`).

## Where the current engine already fits

`config/workflows/build.yaml` already encodes most of the spine:
`author.plan → gate.roundtable(loop) → pr → human-gate → implement → freeze →
gate.roundtable(loop) → pr → gate.ci → merge`. The block model, typed-artifact
validator, loop-back/`max_iters` machinery, `gate.ci` fail-closed CI gate, human gate,
and the autonomous-base merge rail all exist and are exercised by tests.

## The four gaps (why "add features to workflow that it supports")

1. **Per-slice PRs are not expressible.** `implement` decomposes the plan into units
   *internally* (`run_fanout_units`, `src/workflow/wfe_blocks.c:598`) and emits **one**
   aggregate branch → **one** freeze → **one** roundtable → **one** PR. Slices are invisible
   to the graph, so "each slice gets its own PR + roundtable + merge into the feature branch"
   cannot be composed today.

2. **The roundtable cannot review "plan + proposal" together.** `exec_roundtable`
   (`src/workflow/wfe_roundtable.c:80`) reviews only `row.content_hash` — the work item's
   single current content blob. It ignores its `in:` bindings entirely, so two inputs
   (plan *and* proposal, or feature-diff *and* proposal) cannot both reach the panel. The
   block catalog already *allows* multiple accepted input types; the executor just doesn't
   read them.

3. **No live panel + no feature-branch/base targeting.** `wfe_autonomy_register`
   (`src/server/wfe_live_delegate.c:429`) installs every provider **except**
   `wfe_set_panel_provider` — so the roundtable is fail-closed (`live_panel_run` returns
   `-1`, "§0: panel not composable yet") and never actually convenes. Separately, `pr.open`
   always targets `wfe_autonomous_base()`; there is no way to point sub-PRs at a durable
   **feature branch** while the final PR targets the default branch.

4. **No acceptance-review framing.** There is a diff-review roundtable, but not one that is
   handed the original proposal and asked "was this completed / is quality sound / what tests
   are missing?"

## Design

### Chosen shape (decisions locked with the requester)

- **Sub-workflow per slice.** The parent "build" workflow expresses the spine
  (plan → roundtable → feature branch → split → *foreach slice run a child workflow* →
  acceptance roundtable → final PR → human gate → green CI → merge). Each slice runs a
  **child workflow instance** whose body is the sub-PR cycle. Per-slice PRs and roundtables
  are therefore first-class, validator-checked, and inspectable in YAML — not hidden inside a
  provider.
- **Make the roundtable real.** Teach `gate.roundtable` to consume multiple bound inputs and
  hand their actual content to the panel, and wire a live panel provider off the ensemble
  engine so the roundtable genuinely convenes (fail-closed preserved when it can't).
- **Green CI is mandatory on every PR.** Every `pr.open` (each sub-PR and the final PR) routes
  through `gate.ci` before `merge`; `gate.ci` already fails closed on `WFE_CI_NONE`/pending.

### Target lifecycle (parent `build` workflow)

```
ingest (proposal | request)                     author.proposal (with_user) if request; else pass-through
  -> plan            author.plan                 proposal -> plan
  -> plan_gate       gate.roundtable {plan, proposal}   loop on_fail -> plan   (validates plan vs. ask)
  -> feature         branch.open                 create durable feature branch (WFE_ART_BRANCH)
  -> split           split                       plan -> packets (slices)
  -> slices          foreach.workflow            per packet: run child "slice" workflow onto feature branch
                                                 (parent parks until all children terminal)
  -> accept_gate     gate.roundtable {feature-diff, proposal}   loop on_fail -> split   (completion/quality/tests)
  -> final_pr        pr.open {base: default}     feature branch -> default branch PR
  -> final_ci        gate.ci                     must be fully green
  -> human           gate.human {policy: pr_review}   loop on_fail -> split
  -> merge           merge                       merge final PR into default branch
```

### Child `slice` workflow (one packet)

```
impl     implement {packet}         packet -> branch (on the feature branch)
freeze   freeze                     branch -> frozen_diff
rt_gate  gate.roundtable {diff}     loop on_fail -> impl   (standard resubmit-until-convergence)
pr       pr.open {base: feature}    frozen_diff -> sub-PR targeting the feature branch
ci       gate.ci                    must be fully green   (on_fail -> impl)
merge    merge                      merge sub-PR into the feature branch   (terminal: slice done)
```

### Engine features to add

- **E1 — Multi-input roundtable.** `exec_roundtable` resolves each `node->ins[]` binding to its
  producer's artifact content from the store and assembles a composite review packet
  (`{proposal, plan}` or `{proposal, frozen_diff}`) instead of one bare content hash. Add an
  optional `focus`/`review_lens` param so the acceptance gate is framed as
  "completion + code quality + missing tests." Fully unit-testable with the existing mock panel.
- **E2 — Live panel provider.** Register `wfe_set_panel_provider` in `wfe_autonomy_register` with a
  provider that convenes the ensemble roundtable (`delegate_ensemble` / `roundtable_pipeline`),
  passing the composite packet + required/eligible personas and returning per-persona verdicts.
  Fail-closed when the panel can't compose (unchanged posture).
- **E3 — Feature branch + base targeting.** A `branch.open` block that creates/returns a durable
  feature branch as `WFE_ART_BRANCH`, and a `base:` binding on `pr.open`/`merge` so a PR can
  target the feature branch (sub-PRs) or the default branch (final PR). Forge seam extended to
  push/open/merge against an explicit base; the protected-branch guard still refuses autonomous
  merges to `main`/`master`/`release*`, so the final PR to the default branch stays human-gated.
- **E4 — Sub-workflow-per-slice.** A `foreach.workflow` construct: for each packet emitted by
  `split`, instantiate a child work item running the `slice` workflow bound to that packet + the
  feature branch; the parent parks (`WFE_STEP_PENDING`) until all children reach terminal, then
  aggregates (all-merged → advance; any parked/failed → park for a human). Requires parent↔child
  linkage in DB1 and driver support to advance children. This is the load-bearing new capability.

### Non-goals / preserved invariants

- No change to the narrow `wfe_iface.h` execution seam contract (W1) — new behavior rides existing
  block/artifact enums where possible; any new block/artifact is appended, never reordered.
- Canonical-form/version hashing (`wfe_canonical.c`) and the typed validator remain the source of
  truth; every new block declares typed I/O and is validator-checked.
- Fail-closed everywhere: no gate silently degrades to single-lens approval; no PR merges on
  non-green CI; no autonomous merge reaches a protected branch.

## Risks

- **E4 is genuinely new control flow.** Parent-parks-until-children and child fan-in must not
  deadlock or double-advance. Mitigation: model child state in DB1, drive children through the
  existing autonomy driver, and gate parent advance on an explicit aggregate predicate — mock-tested
  before any live run.
- **Canonical hashing / validator churn.** Appending blocks/artifacts must not renumber existing
  enum members (would break stored versions). Mitigation: append-only + a version-stability test.
- **Live panel is integration-gated.** E2's real convening depends on the ensemble path; its logic
  is mock-tested now and only the live wiring waits on a reachable panel — same posture as the
  original W5.

See `sliced-lifecycle-build-workflow.plan.md` for the slice-by-slice implementation plan.
