# 1.0.2 release preparation

The next patch in the declared 1.0 series is **v1.0.2**, following released v1.0.1.
The shared `scripts/next-release-version.sh` resolves this version for both promotion
validation and publication; no patch version is hard-coded in application sources.

## Promotion candidate

[PR #3027](https://github.com/RakuenSoftware/aimee/pull/3027) delivers the release
preparation to `testing`. The subsequent main promotion must originate from
`testing`, as required by the unchanged `allow-only-testing-source` policy.

The preparation branch `release/v1.0.2` starts from testing commit
`c2184d11a29c2eb9b040163c7c60a68a7f979b4d` and incorporates main commit
`875cff004cc5155de76b1b7d83b3c4fde8ac80ef`. Main's only separate commit is the
v1.0.1 promotion merge. The ancestry reconciliation required no source conflict.
The application changes are the merged Hillock/first-start selection work in PR #3024
and audit-storage/startup/runner recovery work in PR #3025.

Promotion refreshes only the bundled source digests for memory, workspace, audit and
egress, plus reviewed public-surface hashes for configuration, server startup,
audit/configuration/HTTP headers and packaging. The C core and real external module
pins remain unchanged. The baseline refresh admits the existing reviewed implementation;
it introduces no runtime changes or weakened checks.

[Release notes](../releases/v1.0.2/RELEASE_NOTES.md) describe user-visible changes,
restart behavior and upgrade limits.

## Validation and remaining gates

Testing's exact candidate passed [push CI](https://github.com/RakuenSoftware/aimee/actions/runs/38566839154).
Local promotion checks cover application-source locking, public-surface locking, their
regression tests, module process contracts, exact external pins through the application
source-lock gate, and release approval topology (15 regression tests passed).
The historical `check_c_repository_lock.py` still rejects bundled memory changes
against independently published mirrors; it is not the application promotion gate
and was not used to rewrite independent repository provenance. The main PR must run its own CI and all reusable release builds
before the protected `main-merge-approval` gate. The `release` environment separately
approves publication after merge; no tag, public release or production rollout is
created by this preparation.

The scheduled [performance benchmark](https://github.com/RakuenSoftware/aimee/actions/runs/38872524341)
is red on this testing commit because five memory benchmarks are unavailable in its
native harness. It exits without certifying a baseline. Passing application CI does
not establish a new performance or live-model-quality claim.

Production audit recovery and upgrade validation remain distinct from deterministic
regressions. Preserve the original ledger, backups and Vault identity, verify live
and historical audit status separately, and verify the published version's readiness,
restart persistence and actual detached Git operations before production rollout.
