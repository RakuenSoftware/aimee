# Core modularization slice 3: chronological Git proposal ordering

## Decision

This slice makes the Git contract's chronological-ordering prerequisite executable. It adds no Git
implementation, descriptor, build/profile registration, readiness claim, or source move.

The immutable discovery anchor is Slice 2 merge commit
`a3c4d413b6ce5f674994a6e6c4589ae2383819a4`. The checker reads the canonical contract blob from
that commit, validates the live child through `check_git_core_contract.py`, and requires its
historical cutoff, invariants, and trigger surface to remain equal. A missing, pending, invalid, or
drifted live contract fails unconditionally. This avoids trusting the file under review to define
which changes should trigger its own validation.

## Historical ordering

The checker walks the checked-out revision's first-parent chain between the Slice 1 cutoff and
`HEAD`, oldest first. Each commit is compared with its first parent. On a pull request, GitHub's
synthetic merge commit therefore exposes the proposed branch as a net delta from the feature-branch
parent. On the feature branch, a signal and its later revert remain separate first-parent commits,
so the revert does not erase the earlier signal.

Historical presence of `src/modules/git/` at the cutoff is not a signal. Any later add,
modification, deletion, type change, rename, or copy whose source or destination lies under that
tree is a migration signal. Exact descriptor, generated Make/CMake, generated profile, and
readiness paths from the pinned contract are signals too. Git output is NUL-delimited with explicit
rename and copy detection. Even low-similarity moves surface as add+delete pairs and trigger. This
source-tree sweep remains the conservative migration boundary until the deferred descriptor and
generated-profile checks activate.

Every signal commit must have the Slice 2 merge commit
`a3c4d413b6ce5f674994a6e6c4589ae2383819a4` as an ancestor of its first parent. This requires
strict precedence: contract-before-signal passes; signal-before-contract and
same-commit contract-plus-signal fail. A later revert does not erase the ordering violation.

## Status claims

The marker vocabulary comes only from the pinned contract's `status_claim_roots[*].claim` values.
Currently it contains `git-runtime-ready`. Under a declared root, added whole lines match only:

- YAML: indentation, exact case-sensitive claim, optional horizontal space, `:`, optional space,
  `true`, and an optional trailing comment, for example `git-runtime-ready: true # ready`.
- JSON: indentation, the quoted exact claim, optional horizontal space, `:`, optional space,
  `true`, and an optional comma, for example `"git-runtime-ready": true,`.

Only `.yaml`, `.yml`, and `.json` files can emit status-claim signals. Modified lines are considered
through their added side. Deleted claims do not newly assert readiness. Binary line diffs do not
claim readiness, although a binary file at an exact path or under `src/modules/git/` remains a path
signal. Markdown and other prose, substrings, case variants, false values, commented markers, and
lines outside declared roots are inert.

## Event binding

CI checks out full history with SHA-pinned actions, `persist-credentials: false`, and read-only
contents permission. The checked-out `HEAD` must equal `GITHUB_SHA`. The only allowed contexts are
`push` on the feature branch, `pull_request` on its merge ref, and `workflow_dispatch` on the
feature branch; every other event/ref pair fails closed.

## Included

- `scripts/check_proposal_ordering.py`, with no parallel shell implementation
- immutable Slice 2 discovery and live-child equality
- `scripts/tests/test_check_proposal_ordering.py`
- the `proposal-ordering` feature-branch CI job

## Deferred

- descriptor and generated-profile set equality
- descriptor schema and Make/CMake generation
- Git source migration and runtime acceptance fixtures
- module registration and readiness

Those checks activate in their owning slices.
