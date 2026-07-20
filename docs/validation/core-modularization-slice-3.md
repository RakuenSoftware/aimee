# Core modularization slice 3: chronological Git proposal ordering

## Decision

This slice makes the Git contract's chronological-ordering prerequisite executable. It adds no Git
implementation, descriptor, build/profile registration, readiness claim, or source move.

The immutable discovery anchor is Slice 2 merge commit
`a3c4d413b6ce5f674994a6e6c4589ae2383819a4`. The checker reads the canonical contract, handoff,
approval-evidence presence, and contract checker from that commit. It executes the pinned checker
from a private temporary directory, validates the live contract and handoff, and requires the live
historical cutoff, invariants, trigger surface, exact paths, and paired claim roots to equal the
pinned values. Missing, pending, invalid, symlinked, or drifted metadata fails closed. The file
under review therefore cannot redefine the checker or the changes that trigger it.

## Historical ordering

The checker walks the complete commit DAG reachable from `HEAD` but not from the Slice 1 cutoff,
oldest first in topological order. It compares every commit with that commit's first parent. This
preserves signal-and-revert pairs on side branches and proposed pull-request branches. A signal is
valid only when the Slice 2 anchor occurs on the signal's first-parent ancestry before the signal's
parent; merely merging the anchor as a later, non-first parent does not satisfy ordering.

On a GitHub pull request, the synthetic merge commit's first parent is the target feature branch and
its second parent is the proposed branch. The full-DAG walk evaluates both histories as well as the
synthetic merge. The gate validates this merge-ref shape; squash and rebase results are subsequently
checked as ordinary commits when pushed to the feature branch.

Historical presence of `src/modules/git/` at the cutoff is the declared baseline, not a signal. Any
later add, modification, deletion, type change, rename, or copy whose source or destination lies
under that tree is a migration signal. Exact descriptor, generated Make/CMake, generated profile,
and readiness paths from the pinned contract are signals too. Git output is NUL-delimited with
explicit rename and copy detection. Even low-similarity moves surface as add+delete pairs and
trigger. This source-tree sweep remains the conservative migration boundary until the deferred
descriptor and generated-profile checks activate.

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

Only `.yaml`, `.yml`, and `.json` files can emit status-claim signals. The checker obtains filenames
from NUL-delimited Git records and compares UTF-8 blobs directly, so quoted filenames and
repository-controlled diff attributes cannot hide a claim. A rename or copy into a declared root is
compared from an empty root-local baseline; a rename out is a removal and does not newly assert
readiness. Modified lines are considered through their added side. Deleted claims do not newly
assert readiness. A non-UTF-8 structured blob cannot claim readiness, although a binary file at an
exact path or under `src/modules/git/` remains a path signal. Markdown and other prose, substrings,
case variants, false values, commented markers, and lines outside declared roots are inert.

The formats remain distinct. YAML accepts only the unquoted key and literal lowercase `true`, with
an optional `#` comment. JSON accepts only the quoted key and literal lowercase `true`, with an
optional comma. YAML 1.1 alternatives such as `yes`, `True`, and `!!bool true`, block scalars, `%`
comments, and cross-format forms are inert.

## Event binding

CI checks out `github.sha` with full history, SHA-pinned actions, `persist-credentials: false`, and
read-only contents permission. The checked-out `HEAD` must equal `GITHUB_SHA`. The only allowed
contexts are `push` on the feature branch, `pull_request` on a merge ref whose
`GITHUB_BASE_REF` is the feature branch and whose `GITHUB_HEAD_REF` is non-empty, and
`workflow_dispatch` on the feature branch. A partial GitHub environment and every other event/ref
pair fail closed. A separate negative workflow assertion proves that a non-repository
`--config-root` is rejected.

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
