# Contributing to aimee

Thank you for your interest in contributing. This document covers the workflow
conventions every contributor and reviewer needs to know.

## Proposals

Long-lived design work — features, refactors, governance changes, integration
plans — lives under [`docs/proposals/`](docs/proposals/). The directory the
file lives in **is** its lifecycle state. The folder is the source of truth,
not prose inside the file and not any external index.

### Directory layout

```
docs/proposals/
├── pending/     # proposed or being designed — subject is missing or not yet shipped
├── accepted/    # approved for implementation but not yet shipped
├── done/        # shipped (merged into the tree)
├── rejected/    # declined, with the reason documented in the file
└── deferred/    # parked — not now, may return later
```

A proposal belongs in `pending/` or `accepted/` only while its subject is
missing or not yet shipped. Once the subject lands, is rejected, is deferred,
or is superseded, the file moves out of `pending/`/`accepted/` into its
terminal directory.

### Lifecycle rule

The PR which lands, rejects, defers, or supersedes a proposal moves the file
to its terminal directory **in the same commit** as the change that triggers
the transition. The move is the enforcement mechanism: it is not enforced by
a script, a CI check, or a lint gate in this PR, and the move is not optional.
Concretely:

- A PR that ships the subject of a proposal moves the proposal file from
  `pending/` or `accepted/` into `done/` in that same PR.
- A PR that declines a proposal moves the file into `rejected/` and records
  the reason in the file.
- A PR that parks a proposal moves the file into `deferred/` and notes the
  expected re-trigger.
- A PR that replaces a proposal with a successor moves the original file
  into `done/` (if the work landed) or `rejected/` (if it did not) and adds a
  `## Successor` header on the superseded file pointing at the replacement.

### Terminal-state meaning

- **`done/`** — the subject shipped in a merged PR. The file is a historical
  record, not live work.
- **`rejected/`** — the proposal was reviewed and declined. The reason is
  recorded in the file. A rejected proposal is not a failure of its author;
  it is a documented decision.
- **`deferred/`** — the proposal is parked. It is neither in flight nor
  rejected. It may be revived by a future PR that moves it back into
  `pending/`.

### Supersession

A superseded proposal carries a `## Successor` header as informational
metadata, with the path or PR reference of the replacement proposal. The
header is informational only; the move itself (from `pending/`/`accepted/`
into `done/` or `rejected/`) is what records that the proposal is closed.
The `## Successor` header lets a future reader find the replacement without
having to reconstruct the decision from the index.

### PR template checklist

Every PR that touches a proposal file must complete the "Proposal moves"
item in the pull-request template. That item cross-references this section
and is the authoring-time nudge that keeps the move and the change in the
same commit. See `.github/PULL_REQUEST_TEMPLATE.md`.