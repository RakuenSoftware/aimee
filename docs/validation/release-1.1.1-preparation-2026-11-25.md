# 1.1.1 release preparation

The next patch in the declared 1.1 series is **v1.1.1**, following released v1.1.0
(published 2026-11-16). `scripts/next-release-version.sh` resolves it from the tags
(`version=1.1.1 series=1.1 latest=v1.1.0`); no patch version is hard-coded in application
sources.

## Promotion candidate

`release/v1.1.1` starts from testing commit
`f29cdf4c8` (merge of PR #3099) and incorporates main commit `825fe79c3`, the v1.1.0
promotion merge. That merge brings no content change. The application changes are PR #3098
(live 1.1.0 failures: delegate runtime, webchat attach, background delegate sources,
personal memory commands, KB-less behaviour, audit startup) and PR #3099 (indexed receipt
lookups and the replay receipt test).

Promotion refreshes only:

- bundled source digests in `dependencies/aimee-application-sources.lock.json` for the
  delegates, workspace, vault, audit, aimee and providers modules;
- reviewed public-surface hashes in `tests/baselines/refactor/index.json` for
  `docs/gen/configuration.md`, `src/headers/cli_client.h`, `src/headers/server_internal.h`
  and `src/modules/audit/include/aimee/audit/audit_worm.h`. Each change traces to a commit in
  PR #3098 or #3099.

The C core, external module pins, and the synthesis and embedder image locks are unchanged
from v1.1.0. The refresh admits already-reviewed, merged implementation; it introduces no
runtime change and weakens no check.

[Release notes](../releases/v1.1.1/RELEASE_NOTES.md) describe user-visible changes, the new
privileged `aimee-delegate-runtime` Compose service and the one-time audit index build.

## Validation and remaining gates

`scripts/check_application_source_lock.py check` and `scripts/refactor_baselines.py check`
pass on the prepared tree. PRs #3098 and #3099 each passed full PR CI before merging. Push CI
for testing commit `f29cdf4c8` was still running when this record was written; the
promotion PR runs its own CI and the reusable release builds before the protected
`main-merge-approval` gate. The `release` environment separately approves publication after
merge. This preparation creates no tag, public release or production rollout.

The scheduled performance benchmark has failed on every daily run since 2026-11-08, before
v1.1.0 was published, and does not certify a baseline. Passing application CI makes no performance or live-model-quality
claim.

Live checks on the 1.1.0-based CT100 deployment that carries these fixes: a webchat turn
attached, selected a model and completed end to end. That run used a local Gemma 4 E4B
model, because the Codex credential needed re-authentication and the qwen host was offline. A
delegated job also completed through the private delegate runtime on the same model.
Production rollout still requires verifying the published version's readiness, restart
persistence and the one-time audit index build on the live ledger.
