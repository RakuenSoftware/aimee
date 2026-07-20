# Contributing

Thanks for contributing to aimee. This guide covers the mechanics of the
proposal workflow, which is how non-trivial design changes move from idea to
shipped code. Read it before opening a pull request that touches a proposal
under `docs/proposals/`.

## Proposals

Design changes that span more than a single commit are tracked as Markdown
files under `docs/proposals/`. The directory layout is the source of truth for
where a proposal lives in its lifecycle:

| Directory                  | Meaning                                                                |
| -------------------------- | ---------------------------------------------------------------------- |
| `docs/proposals/pending/`  | Authored, under review, not yet decided.                               |
| `docs/proposals/accepted/` | Approved but not yet shipped; implementation is tracked separately.    |
| `docs/proposals/done/`     | Shipped — the implementation has landed.                               |
| `docs/proposals/rejected/` | Reviewed and turned down.                                              |
| `docs/proposals/deferred/` | Set aside without rejection; may be picked up later.                   |

### Move the file in the same commit that decides it

The PR which **lands, rejects, defers, or supersedes** a proposal must move
the proposal file from its current directory to the appropriate target
directory (`accepted/`, `done/`, `rejected/`, or `deferred/`) **in the same
commit**. Do not leave the proposal file in `pending/` once a decision has
been made, and do not defer the move to a follow-up commit.

This keeps the directory layout authoritative at every commit on the main
branch: any reader of `HEAD` can tell the state of every proposal from its
path alone, with no need to cross-reference the PR that last touched it.

### Note the successor when superseding

When a proposal is **superseded** by a newer one (typically moving from
`pending/` into `deferred/` because the design has been folded into a
successor proposal), the moved file's header must note the successor so a
reader landing on the old file can follow the thread forward. A minimal
header note is sufficient, for example:

```markdown
- **State:** superseded
- **Superseded by:** `docs/proposals/pending/<successor-file>.md` (PR #<n>)
```

The successor note must be added in the same commit that performs the move.

### PR template cross-reference

The same rule is restated in the pull request template at
[`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md) so
that authors encounter it while filling out the PR description. The written
norm in this file is the canonical statement; the PR template is a reminder
at author time, and any CI lint that enforces the rule is a backstop — not
a substitute for following it.
