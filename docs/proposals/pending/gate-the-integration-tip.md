# Proposal: gate the integration branch tip, not only the PRs that build it

- **State:** pending — single slice.

## Problem

`.github/workflows/ci.yml` triggers on:

```yaml
on:
  pull_request:
    branches: [main, testing, feature/core-modularization]
    types: [opened, synchronize, reopened, edited]
  workflow_dispatch:
```

There is no `push` trigger. CI therefore runs against a PR's *merge preview*,
never against the branch tip that results from merging it.

Measured on the current release tip, `testing` @ `e161dd34`:

```
$ gh api repos/{owner}/{repo}/commits/e161dd34/check-runs
total_count = 2
  build (aimee-server, Dockerfile.server)  success
  build (aimee-kb, Dockerfile)             success
```

Two image builds. None of the 17 gate jobs — `unit-tests`, `build-integrity`,
`e2e-docker` T1/T2/T3, `lint`, the Windows/macOS legs — has ever run against
this commit. The same is true of every previous tip.

## Why this is not merely theoretical

Each of the 14 commits in this release (#1994–#2010) was gated in isolation,
against the tip as it stood when that PR was opened. Nothing re-checks the
combination. The failure mode this misses is the semantic conflict: two PRs that
each pass alone and break in combination — one renames a helper while another
adds a caller, one changes a default while another adds a test asserting the old
one. Textual conflicts are caught by git; semantic ones are caught only by
running the gate on the merged result, which never happens.

GitHub's merge-queue and "require branches to be up to date before merging"
settings exist precisely to close this gap. Neither is in use here: PRs merge
against whatever base they were opened on.

Note that this is the same class of hole as the slice sub-PR gap described in
[ci-on-slice-subprs.md](../done/ci-on-slice-subprs.md) — work merging through a gate
that did not actually run. They are independent instances and can be fixed
independently, but a fix for one does not address the other.

## A push trigger alone does not give a complete gate

Two jobs are conditioned on the event type:

```yaml
no-coauthor-trailers:
  if: github.event_name == 'pull_request'
bench-check:
  if: github.event_name == 'pull_request'
```

Confirmed empirically: the manually dispatched run `30223112295` on `e161dd34`
reported **success**, but with those two jobs `skipped` — 15 of 17 jobs actually
ran. A `push`-triggered run would skip them for the same reason.

The two are not equivalent in importance:

- `no-coauthor-trailers` scans a PR's new commits (`base..head`). On a tip gate
  there is no such range, so skipping it is correct, and the check has already
  done its job on the way in.
- **`bench-check` is a genuine gap.** It is skipped on a tip gate, so
  performance regressions arising from the *combination* of merged PRs are not
  checked at integration level — which is exactly the class of problem this
  proposal exists to catch.

Any option below that relies on the `push` event must therefore also decide what
to do about `bench-check`: either relax its `if:` to run on the tip (and give it
a meaningful baseline to compare against, which on a push is not the PR base),
or accept explicitly that integration-level performance is ungated. This
proposal does not resolve that; it is called out so the gap is not mistaken for
coverage. Note that a green tip run means "15 of 17 jobs passed", and any
release certification should say so rather than claiming a full gate.

## Options

1. **Add a `push` trigger** for `main`, `testing`, `feature/core-modularization`.
   The tip is gated on every merge. Cost: one extra full run per merge; a
   failure is discovered post-merge, so it reports rather than prevents.
2. **Require branches up to date before merging** (branch protection). Forces a
   rebase/merge-from-base before a PR can land, so the gated merge preview is
   the tip. Cost: re-runs CI on every PR each time the base moves; on a busy
   base this serialises merges and is the setting most likely to cause friction.
3. **GitHub merge queue.** Gates the prospective merged result and only lands it
   if green. Strongest guarantee, prevents rather than reports. Cost: most
   configuration; changes how everything merges.
4. **Scheduled gate on the tip** (e.g. nightly `schedule:` on `testing`). Cheap
   to add, catches combination breakage within a day. Cost: not tied to a merge,
   so attribution to a specific PR is weaker and detection is delayed.
5. **Leave as-is**, and rely on the release-time full run being dispatched by
   hand — which is what was done for this release, and only because the gap was
   noticed.

## Established vs. assumed

Established:

- The trigger list contains no `push` (read from `ci.yml`).
- `e161dd34` has 2 check runs, both image builds (API, quoted above).
- The full gate on the tip *can* be dispatched manually: `workflow_dispatch` is
  configured, and doing so for `e161dd34` produced run `30223112295`.

Assumed, NOT established:

- **That a semantic conflict has actually shipped.** No historical incident is
  cited here. The argument is that the gap exists and is unguarded, not that it
  has already caused a specific failure. If the decision hinges on demonstrated
  harm rather than exposure, that evidence has not been gathered.
- **Merge frequency and its interaction with option 2's serialisation cost.**
  Not measured.

## Recommendation

Option 1, and it should be uncontroversial: the repo is public, so the extra run
costs no billed minutes (see the cost analysis in
[ci-on-slice-subprs.md](../done/ci-on-slice-subprs.md)), and it is a two-line change to
an existing trigger block.

Option 1 reports rather than prevents — a broken tip is found minutes after the
merge, not before it. That is a genuine weakness, and options 2 and 3 are
strictly stronger. But they change how every PR merges, which is a workflow
decision with human cost, whereas option 1 is additive and reversible. The
recommendation is to take option 1 now to get visibility, and treat option 3 as
a separate decision informed by how often option 1 actually goes red. If it
never goes red over a meaningful number of merges, the stronger options are not
worth their friction.

## Acceptance

- A merge to `testing` produces a CI run against the resulting tip commit.
- That run's check runs are visible on the tip commit
  (`gh api repos/{owner}/{repo}/commits/<tip>/check-runs` returns the gate jobs,
  not just the image builds).
- A release can be certified by pointing at a green run on the exact commit
  being released, with no manual `workflow_dispatch` step.
- The certification states which jobs ran and which were skipped. A tip run that
  skips `bench-check` is not described as a full gate.
- `bench-check` on the tip is either enabled with a defined baseline, or its
  absence is recorded as a known, accepted gap — not left to be discovered.
- PR-triggered runs are unchanged.
