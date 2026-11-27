# 1.1.5 release preparation

The next patch in the declared 1.1 series is **v1.1.5**, following released v1.1.4
(published 2026-11-27). `scripts/next-release-version.sh` resolves it from the tags
(`version=1.1.5 series=1.1 latest=v1.1.4`); no patch version is hard-coded in application
sources.

## Promotion candidate

`release/v1.1.5` starts from testing commit `d84349c2b` (merge of PR #3113) and incorporates
main, which brings no content change. The application changes are:

- PR #3111: delegate worktree key directories removed once empty.
- PR #3112: enrolled device tokens accepted with the gateway's session binding; the write-tier
  resolver no longer reads the binding as an identity token.
- PR #3113: code scans of one project coordinate instead of superseding each other (store
  migration 59 records scan progress), refusals are structured and logged, the first index of a
  repository runs as a detached background job for discovery and MCP initialize, stored
  embeddings are reused for unchanged files, and a failed batch stops the upload.

PR #3113 passed all 57 checks on its final head `cf8b24255`, including the Postgres store tests
for in-progress refusal, takeover and embedding reuse, the sanitizer suites and the thin-client
integration tests.

Promotion refreshes the bundled source digests of the workspace and aimee modules in
`dependencies/aimee-application-sources.lock.json`, traced to PRs #3111 and #3113.
`scripts/refactor_baselines.py check` passes without a further refreeze. Compose files, external
module pins, and the synthesis and embedder image locks are unchanged from v1.1.4.

[Release notes](../releases/v1.1.5/RELEASE_NOTES.md) describe the user-visible changes.

## Validation and remaining gates

`scripts/check_application_source_lock.py check` and `scripts/refactor_baselines.py check` pass
on the prepared tree. The promotion PR runs its own CI and the reusable release builds before the
protected `main-merge-approval` gate; the `release` environment separately approves publication.

The failures were observed on the live CT100 Server running published 1.1.4: eight scans of the
canonical aimee project superseded within 40 minutes, none sealed; the embedder averaged about
1.7 s per file; 324,173 retained file versions held 30,250 distinct contents; and gateway
requests from an enrolled client were refused with 401 "write tier denied (no_team_configured)".
After 1.1.5 is published it is deployed to CT100, a session in an unindexed checkout is checked
for the background-index answer, and the first index is followed until it publishes.
