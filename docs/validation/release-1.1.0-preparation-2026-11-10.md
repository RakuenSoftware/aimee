# 1.1.0 release preparation

This preparation moves the declared series from `1.0` to `1.1` in `src/headers/aimee_version.h`.
With no `v1.1.*` tag, `scripts/next-release-version.sh` resolves the next version as **v1.1.0**,
following released v1.0.2. The series moves because the memory owner built into 1.0.x is
replaced; no patch version is hard-coded in application sources.

## Promotion candidate

This preparation is delivered to `testing`; the main promotion must originate from `testing`, as
required by the unchanged `allow-only-testing-source` policy. It starts from testing commit
`07b3d28ee038012fdd98880336fe82f92f995c54`, which already includes the memory-integration fixes of
[PR #3092](https://github.com/RakuenSoftware/aimee/pull/3092).

On top of that commit the preparation adds:

- one runtime fix found by exploratory testing: a streamed `/v1/chat/completions` response ended
  with two `data: [DONE]` events;
- `scripts/upgrade/migrate-memory.py` and its tests, the upgrade path for 1.0.x memory records;
- documentation brought in line with the memory extraction, including the architecture page and
  diagrams, the upgrade guide and the module documents the main-only contract check reads;
- the refreshed bundled application source snapshot and public-surface baseline, which gate
  `main` only and are refrozen once per promotion.

[Release notes](../releases/v1.1.0/RELEASE_NOTES.md) describe user-visible changes and upgrade
limits.

## Checks that only run for `main`

The Module inventory workflow and the application source-lock step do not run on `testing`. Run
locally against this candidate, they showed four failures a promotion PR would have hit, all
resolved here:

- `scripts/check_module_docs.py`: `docs/modules/` held a non-module document (moved to
  `docs/VAULT_CONDITIONAL_CUSTODY.md`); `gateway.md`, `ir.md`, `vault.md` and `memory.md` did not
  meet the required section contract. `gateway.md` also named a predicate that no longer exists.
- `scripts/refactor_baselines.py`: the public-surface baseline had drifted across the merged
  changes since 1.0.2 and is refrozen from this tree.
- `scripts/check_application_source_lock.py`: the bundled source snapshot was stale and is
  refrozen from this tree.

The remaining single-command steps of that workflow pass on this tree.

## Validation on a disposable host

All of the following ran on a fresh Debian 13 virtual machine with Docker 26.1.5.

The published `:testing` application image for commit `07b3d28`
(`ghcr.io/rakuensoftware/aimee@sha256:e716b855e5212ba7c19ab6dac954207a84e6b74c4eb100e9a198b71067b3249d`,
reporting `vtesting-07b3d28`) passed every leg CI's e2e-docker job runs, with the frozen
reference memory owner selected: T1 (207 checks), T2 (45), T3, the real-Docker private verifier,
Vault egress (26), plain PostgreSQL storage, the plain KB upgrade (15), T2 on LUKS (5), LUKS fresh
(11) and legacy upgrade (13) storage, and the LUKS KB upgrade (15).

An in-place upgrade was run from published 1.0.2 images holding stored memories, for a Server
and for a KB, onto the published `:testing` images:

- with no memory implementation installed, both started, kept their 1.0.x memory rows, and
  reported `missing_module: memory`; chat still completed;
- `scripts/upgrade/migrate-memory.py export` wrote the active records read-only and refused to
  overwrite its snapshot;
- `import` stored every record through the public store command, a second run stored none and
  reported all as already imported, and list, search, scoped visibility and chat recall then
  returned the 1.0.2 records. The Server import ran against an installed implementation; the KB
  import ran against the frozen reference owner, which serves the public KB store command.

## Remaining gates and limits

The main PR must run its own CI and all reusable release builds before the protected
`main-merge-approval` gate. The `release` environment separately approves publication after merge;
no tag, public release or production rollout is created by this preparation.

Public CI and the checks above do not certify an installed memory implementation. Its own
process, datastore, provider-send and recovery qualification, including shared-memory writes on
a KB, accompanies its deployment. Records held by a Cognee or Hillock backend selected in 1.0.2
are outside the upgrade tool.
