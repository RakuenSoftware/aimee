# WFE Autonomy Runbook

The Workflow Engine (WFE) drives a proposal from human intake to a merged pull request without a human attending each step: a primary agent decomposes the work, delegates bounded units to sub-agents in isolated worktrees, verifies each unit, and only then advances the run. The end-to-end lifecycle is *proposal intake → plan → roundtable gate → implement per-slice → verify → PR open*, with the operator staying in the loop at approval gates rather than at every commit.

## Lifecycle stages

- **Proposal intake** — A human submits a written proposal (the run is always framed by a proposal; the engine never starts work on its own). The intake endpoint seeds a work item on a chosen workflow, typically the default `build` workflow.
- **Plan** — The primary agent drafts a plan from the proposal: it decomposes the change into units, decides the verification shape per unit, and records the plan as the artifact the implementation steps will accept as typed input.
- **Roundtable gate** — A roundtable review evaluates the plan before any code is written. The plan only advances on a passing verdict; a failing plan is sent back for revision rather than implemented as-is.
- **Implement per-slice** — Each unit is dispatched to a delegate in an isolated worktree with a tight scope. Units are dispatched one at a time (or in the order the workflow specifies); each delegate writes only the files its scope authorizes.
- **Verify** — Every unit is checked (`aimee git verify`) against its acceptance criteria before the run advances. Units that fail verification are re-dispatched; the run does not move to the next stage on a red unit.
- **PR open** — Once all units pass, the primary opens a pull request against the target branch (autonomous merges target `testing`). The PR is the deliverable; the work item closes when the PR is opened and the merge gate (CI + reviewable diff) resolves.

WFE_AUTONOMY_RUNBOOK_V1

commit-e2e-1784479438
