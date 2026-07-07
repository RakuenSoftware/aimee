# Proposal: Proposal-supersession hygiene — same-commit move + a documented rule

- **State:** proposed (pending — not started)

## Thesis

`docs/proposals/pending/` is only useful as signal if a finished or superseded
proposal *leaves* it. `check-proposal-reconcile.py` already catches one drift
class — a pending/accepted proposal that has actually shipped (classified
"terminal-done"). But the specific drift the roadmap flagged is different and
still unguarded: a PR that **supersedes** a proposal (deletes/replaces its
subject) without moving the old file, so a dead proposal lingers in `pending/`
(the roadmap cites the pluggable-db proposal doing exactly this). And there is **no
`CONTRIBUTING.md`** stating the rule, so contributors have no norm to follow —
only a lint that fires after the fact on one of two failure modes.

## Goal

The supersession-move becomes both a written rule and an enforced one: the PR that
supersedes a proposal moves the old one (to `rejected/` / `deferred/` / `done/`)
**in the same commit**, and a lint catches the case where it didn't.

## §0 What already exists

- `scripts/check-proposal-reconcile.py` (`proposal-reconcile-check`, in `lint`)
  flags a `pending/`|`accepted/` proposal classified terminal-done.
- `scripts/check-proposal-links.py` (`proposal-links-check`) validates links.
- **No `CONTRIBUTING.md`** at repo root or `docs/`.
- No check for "superseded-but-not-moved" (a live `pending/` file whose subject a
  later PR removed/replaced).

## §1 Document the rule

Add `CONTRIBUTING.md` with a short "Proposals" section: proposals live under
`docs/proposals/{pending,accepted,done,rejected,deferred}/`; the PR that lands,
rejects, defers, or **supersedes** a proposal moves the file to its terminal state
**in the same commit**; supersession notes the successor in the moved file's
header. Reference the PR template so it is seen at author time, not review time.

## §2 Extend the reconcile check for supersession

Teach `check-proposal-reconcile.py` a second failure mode: a `pending/`/`accepted/`
proposal whose declared subject/anchor no longer resolves (the code, doc, or
sibling proposal it proposes has been removed or replaced) is "superseded, not
moved" → fail with the remediation ("move it to rejected/ or deferred/"). Keep the
existing `--plant-test` self-check so the guard itself stays honest.

## §3 One-time sweep

Reconcile today's `pending/` against reality (this proposal set is part of that
pass) and move any already-superseded stragglers. A clean `pending/` is the
precondition for the rule to mean anything.

## Non-goals

Not a workflow engine and not auto-moving files in CI (moves stay author-driven,
reviewed) — just a written norm plus a lint that makes violating it visible.
