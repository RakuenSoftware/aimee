# 1.1.3 release preparation

The next patch in the declared 1.1 series is **v1.1.3**, following released v1.1.2
(published 2026-11-25). `scripts/next-release-version.sh` resolves it from the tags
(`version=1.1.3 series=1.1 latest=v1.1.2`); no patch version is hard-coded in application
sources.

## Promotion candidate

`release/v1.1.3` starts from testing commit `1f3b4d682` (merge of PR #3105) and incorporates
main, which brings no content change. The application change is PR #3105: history checkpoint
budgets, admitting the host's source-custody refusal in history, no ambient project on personal
`memory_serve`, the caller context on delegate memory assembly, and memory failure logging. PR
#3105 passed all 57 checks on its final head `336e73f0c`, including the real-provider LSP S1
release validation; the frozen S1 call table is unchanged.

Promotion refreshes only the bundled source digest of the aimee module in
`dependencies/aimee-application-sources.lock.json` (the store's IR history save budget), traced
to PR #3105. `scripts/refactor_baselines.py check` passes without a refreeze. Compose files, the
C core, external module pins, and the synthesis and embedder image locks are unchanged from
v1.1.2.

[Release notes](../releases/v1.1.3/RELEASE_NOTES.md) describe the user-visible changes.

## Validation and remaining gates

`scripts/check_application_source_lock.py check` and `scripts/refactor_baselines.py check` pass
on the prepared tree. The promotion PR runs its own CI and the reusable release builds before the
protected `main-merge-approval` gate; the `release` environment separately approves publication.

The fixes were reproduced and verified on the live CT100 Server with a development build of the
PR branch: with module and bridge diagnostics enabled, a delegate's `memory_serve` was refused
for the injected project (job 78, which then continued and completed), and after the fix the same
delegate call returned memory in 64 ms and the job completed (job 79). The 30-second history
checkpoint budget is covered by a guard test; a 75-turn run was not repeated. After 1.1.3 is
published it is deployed to CT100, replacing the development build and its diagnostic settings.
