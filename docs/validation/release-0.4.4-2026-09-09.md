# 0.4.4 restores ordinary file work and the worktree off switch

The release combines the setup, private-index and audit repairs from PR #2975 with the
worktree repairs from PR #2979. Both are merged into `testing` at
`5abadd1fc1723777d1e8c0dfd8848b0429377aef`. The release script resolves **0.4.4** after
**v0.4.3**. Publication still requires the protected release approval.

LXC 103 now runs the published application image `ghcr.io/rakuensoftware/aimee:testing-5abadd1`,
with the matching published Linux thin client used for validation. This replaces the earlier
local `0.4.4-rc1` application image. PostgreSQL, embedding, accounts, identities and instance
volumes were retained. [Deployment evidence](release-0.4.4-2026-09-09/deployment.json)
records the immutable image digest and binary hashes. The image is healthy after a controlled
restart, with no automatic restarts.

## The reported denial failed before the upgrade and passed afterwards

The previous candidate returned `write blocked because this session is not running in a worktree`
for an authenticated hook in an ordinary non-Git folder, despite
`require_session_worktree=false`. The published candidate accepts the same request.

Ten live checks passed after upgrade and again after restart. They cover Write, Read and Bash
preflight, SessionStart and launch in both Git and non-Git folders. Launch retained the original
directory and created no session worktree. The first hook after restart reused its existing
session without an explicit SessionStart, exercising recovery of the server's process-local
hook identity. [Hook evidence](release-0.4.4-2026-09-09/hooks.json) records the denial before
upgrade and both successful runs.

The live instance's switch remained off throughout. Enabled isolation, unknown paths, Git
targets reached from non-Git folders, and authenticated client scope are covered by the native
and HTTP/mTLS regression suites. Both client and server require the upgrade. Other policy
checks remain independent of the worktree switch. The operator-disabled local hook wrapper
was retained.

## Existing project registrations and memories survived the upgrade and restart

All **74 project registrations** retained their names and roots. Both personal memories retained
their IDs, keys and content hashes. [Persistence evidence](release-0.4.4-2026-09-09/persistence.json)
contains hashes and counts, without memory contents or credentials.

The live index suite passed **90 checks across six projects** after upgrade and another 90
after restart. These include source spans, callers,
blast radius, hybrid retrieval, investigation, sensitive and missing paths, existing memories,
and 16 concurrent memory requests. [Index evidence](release-0.4.4-2026-09-09/live-index.json)
records the initial results; [restart evidence](release-0.4.4-2026-09-09/live-index-after-restart.json)
records the repeated checks. The [earlier report](release-0.4.4-2026-09-08.md) retains the
first-boot, real PAM GUI, interrupted clone-response and corpus-publication evidence for the
setup and indexing fixes in this same release.

## Promotion freezes the reviewed release surface

The worktree PR's final commit `ab27d0efdf` passed all **55 jobs** in its CI workflow, including
full sanitizers, scripts, PostgreSQL workflows, platform builds, and Docker deployment,
upgrade and rollback tests. [CI evidence](release-0.4.4-2026-09-09/ci-worktree.json) retains
each job's result and URL. The shipped-workflow E2E deadline now matches the native workflow
test's two-minute budget; stage coverage and required terminal states remain asserted.

Local validation also passed:

- **Release verification:** the worktree repair passed `make verify-ci`, including the complete
  build, lint, 637 scheduled native test binaries, Go modules, integration tests and generated
  documentation. Service-dependent skips are recorded in the earlier validation and PR #2979.
- **Live PostgreSQL tests:** workflow store, API and engine suites passed with `go test -race -count=1`
  against disposable PostgreSQL 17, including all five shipped workflows.
- **Published client:** all 28 HTTP/mTLS proxy regressions passed against the downloaded Linux
  client from the merged revision's publishing run.
- **Promotion checks:** all 60 script test files, source-lock and public-surface checks,
  module inventory, release policy and documentation checks passed.

The promotion refreshes `tests/baselines/refactor/index.json` and
`dependencies/aimee-application-sources.lock.json` for these merged changes. It adds no runtime
change, setting or database migration. Main promotion validates the versioned thin clients and
application images before the `main-merge-approval` gate; publication has its separate `release`
gate. The public 0.4.4 tag and artifacts are not created by this preparation.
