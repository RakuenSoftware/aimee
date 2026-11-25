# 1.1.2 release preparation

The next patch in the declared 1.1 series is **v1.1.2**, following released v1.1.1
(published 2026-11-25). `scripts/next-release-version.sh` resolves it from the tags
(`version=1.1.2 series=1.1 latest=v1.1.1`); no patch version is hard-coded in application
sources.

## Promotion candidate

`release/v1.1.2` starts from testing commit `758aa1b91` (merge of PR #3102) and incorporates
main, which brings no content change. The application change is PR #3102: Server session
checkouts, job replies that fit the HTTP relay, integer token sums on PostgreSQL, case-insensitive
catalog matching in the model probe, and refusing `--acp` on a Server. PR #3102 passed all 57
checks on its final head `2bfdbe40b`.

Promotion refreshes only the bundled source digests in
`dependencies/aimee-application-sources.lock.json` for the workspace, aimee and providers
modules, each traced to PR #3102. `scripts/refactor_baselines.py check` passes without a
refreeze: no reviewed public surface changed. Compose files, the C core, external module pins,
and the synthesis and embedder image locks are unchanged from v1.1.1.

[Release notes](../releases/v1.1.2/RELEASE_NOTES.md) describe the user-visible changes.

## Validation and remaining gates

`scripts/check_application_source_lock.py check` and `scripts/refactor_baselines.py check` pass
on the prepared tree. The promotion PR runs its own CI and the reusable release builds before the
protected `main-merge-approval` gate; the `release` environment separately approves publication.
This preparation creates no tag, public release or production rollout.

The failures fixed here were found on the live CT100 Server running published v1.1.1. After 1.1.2
is published it is deployed there, and its delegation from a session worktree, `aimee jobs show`
on a long failed job, per-role metrics, the MiniMax probe and the `--acp` refusal are checked live.
