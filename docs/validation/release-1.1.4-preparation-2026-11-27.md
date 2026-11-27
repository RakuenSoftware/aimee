# 1.1.4 release preparation

The next patch in the declared 1.1 series is **v1.1.4**, following released v1.1.3
(published 2026-11-26). `scripts/next-release-version.sh` resolves it from the tags
(`version=1.1.4 series=1.1 latest=v1.1.3`); no patch version is hard-coded in application
sources.

## Promotion candidate

`release/v1.1.4` starts from testing commit `374e45077` (merge of PR #3108) and incorporates
main, which brings no content change. The application change is PR #3108: host error results of
source tools admitted in delegate history, read-only delegate worktrees removed, hook identity
renewed in Git repositories after a Server restart, expected module-call outcomes no longer logged
as failures, repeated configuration-caused memory refusals logged once, and a retried Cppcheck
dependency install in CI. PR #3108 passed all 57 checks on its final head `4cb5110f8`.

Promotion refreshes only the bundled source digests of the workspace and audit modules in
`dependencies/aimee-application-sources.lock.json` (the new `worktree_discard` and the expected
outcome for module calls), traced to PR #3108. `scripts/refactor_baselines.py check` passes
without a further refreeze; PR #3108 recorded its public header changes. Compose files, external
module pins, and the synthesis and embedder image locks are unchanged from v1.1.3.

[Release notes](../releases/v1.1.4/RELEASE_NOTES.md) describe the user-visible changes.

## Validation and remaining gates

`scripts/check_application_source_lock.py check` and `scripts/refactor_baselines.py check` pass
on the prepared tree. The promotion PR runs its own CI and the reusable release builds before the
protected `main-merge-approval` gate; the `release` environment separately approves publication.

The failures were found on the live CT100 Server running published 1.1.3: a delegate that
called `memory_serve` without `view` died at "request build refused at retained tool sources"
(job 81), and the Server log held read-only worktree warnings and retained worktrees from earlier
delegate runs. Each fix has a unit test that fails without it. After 1.1.4 is published it is
deployed to CT100 and a delegate is run that makes a rejected `memory_serve` call and continues.
