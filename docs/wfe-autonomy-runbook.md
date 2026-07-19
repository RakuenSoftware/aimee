# WFE Autonomy Runbook

> The aimee Workflow Engine (WFE) runs autonomous work end-to-end: from a human-submitted proposal through planning, delegation, implementation, verification, and a merged pull request — server-owned and unattended, with every step recorded in a durable audit log. The runbook below is the operator's single reference for that lifecycle.

## Lifecycle stages

- **Proposal intake** — A human submits a written proposal via the `/v1/proposals` surface; a work item is created and durably persisted before any execution begins.
- **Plan & gate check** — The engine runs the proposal through the planner block and any required pre-flight gates (scope, budget, blast radius) before opening a worktree.
- **Worktree & delegate dispatch** — A per-work-item worktree is created on a `wi_<id>.sN` branch; the plan is split into units and each unit is dispatched to an isolated delegate.
- **Implement & verify** — Each delegate writes code, then `aimee git verify` exercises the unit's acceptance criteria; failures route back to a *different* delegate, never the same one twice.
- **Review & accept** — The primary agent reviews the verified units, accepts the work, and commits it on the work-item branch.
- **PR & merge** — A pull request is opened against the base branch, CI runs, and the engine merges on green (or routes back on failure).
- **Audit & cleanup** — Every transition, tool call, and verification result is appended to the per-work-item audit log; the worktree is retained until the work item is closed.

WFE_AUTONOMY_RUNBOOK_V1
