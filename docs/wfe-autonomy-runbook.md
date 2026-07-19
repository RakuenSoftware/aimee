# WFE Autonomy Runbook

Operate the `aimee/wi` daemon (item creation, branch claiming, turn iteration, delivery) for a single work-item without operator assistance.

## Scope

Used when no human is on the loop: triage is finished, the LLM-of-record is set, and the next checkpoint is a `aimee/forge` delivery. You (the delegate) drive a tight `edit → verify → commit` loop until the WI passes `aimee git verify`.

## Lifecycle

1. **Pick up the WI** — `aimee git status` confirms you are on `aimee/wi/wi_<id>.s0`; `aimee git log -5` shows prior turns on this branch.
2. **Decompose** — split the approved plan into ≤5 atomic units, ordered by dependency. Each unit = one concrete edit + its test/verify gate.
3. **Implement per unit** — write code, run the narrowest available test (`aimee git verify` when applicable, else the project's own suite filtered to the unit).
4. **Commit per unit** — `aimee git commit` with a Conventional Commit message; do not squash units together.
5. **Carry forward** — the next turn resumes on the latest SHA of the same branch.

## Verification Gates

- **Unit-level**: re-read your own diff (`aimee git diff HEAD~1`) before committing. If you cannot restate what the unit changes in one sentence, the unit is too coarse — split it.
- **Cross-unit**: `aimee git verify` after each commit when the runner is enabled; failures block the next commit.
- **End-to-end**: the suite the plan named (full tests, lint, type-check) — run before claiming the WI delivered. If any named gate is missing or the toolchain is unavailable, mark the WI `validation-pending` and do not assert correctness as fact.

## Failure Modes

- **`aimee git verify` red** — read the message end-to-end, then `aimee git diff` to see your work in context. Fix root cause, do not silence. Re-run the unit before advancing.
- **`require_session_worktree` guard** — you are outside the `.aimee/worktrees/` mount. `cd` into the worktree path printed by `aimee git status` and retry; do not bypass via raw shell `git`.
- **Branch drifted off `aimee/wi/wi_<id>.s0`** — stop. Re-claim via `aimee git branch claim` before any edit; never force-push a WI.
- **Tool budget ~14 turns, hard cap on scope** — if a unit would exceed the budget, scope it down. The plan can be amended by adding a follow-up WI, never by expanding this one.

## Hand-off

The work-item ends when (a) every planned unit is committed, (b) `aimee git verify` is green, and (c) the last message names the SHA and the next-WI owner. Anything else — including `validation-pending` gates — is reported verbatim, not papered over.
