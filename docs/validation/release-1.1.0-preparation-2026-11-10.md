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

- the new memory implementation, shipped in the image: the vendored binary bundle under
  `vendor/aims-memory`, its verification (`scripts/check_memory_bundle.py`, part of lint), the
  image build that links and installs it, the one-shot `aimee-memory-init` Compose service and its
  provisioning command (`deploy/container/memory-provision.py`), and the startup helper changes
  that read the embedder identity and provision memory before sealing credentials;
- one runtime fix found by exploratory testing: a streamed `/v1/chat/completions` response ended
  with two `data: [DONE]` events;
- `scripts/upgrade/migrate-memory.py` and its tests, the upgrade path for 1.0.x memory records;
- the e2e embedder pin moved to the current published `:testing` embedder, which reports the
  embedding identity memory pins;
- documentation brought in line with the new memory packaging, including the architecture page,
  the deployment and upgrade guides and the module documents the main-only contract check reads;
- the refreshed bundled application source snapshot and public-surface baseline, which gate
  `main` only and are refrozen once per promotion.

The bundle was built from aims-db commit `afdf0187d8890e387480b5fadea50e8b2125e5e6` against Aimee
commit `fc3d9425d232dcff8ec5ff869cc9cb6f975ca920`; `vendor/aims-memory/bundle.json` records both,
the archive hash and the 80 public files it was compiled against.

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

All of the following ran on a fresh Debian 13 virtual machine with Docker 26.1.5 and Compose
2.26.1, against an application image built from this tree with `Dockerfile.server` (reporting
`v1.1.0-baked`), the published `:testing` PostgreSQL image and the published `:testing` embedder
(`ghcr.io/rakuensoftware/aimee-embedder-a25m@sha256:ceb4ad977fdd7062b425f0cd6e67c358302889be3b9793eb8825923d9718767a`).
The image was built with no private source present: the resources linked against the vendored
bundle alone.

### Shipped memory

- **Fresh install.** A Server and a KB started from this release's Compose files through the
  startup helper. Provisioning created each installation and each became ready (Server about 65
  s, KB about 50 s). On the Server, memory store, list and single- and multi-keyword search
  returned the stored record.
- **Image update.** A Server holding a stored memory was stopped and started on a different image.
  Provisioning reported the changed executable, prepared the installation again and the earlier
  memory was still found; a new one could be stored. An unchanged restart reported that nothing
  changed. Adding an account through `AIMEE_MEMORY_ACCOUNTS` re-prepared the installation and
  both memories were still found. A provisioning run with a changed setting while the application
  was running was refused with a message saying to stop it first. 16 of 16 checks passed.
- **Upgrade from 1.0.2, Server.** A published 1.0.2 Server holding three memories was upgraded in
  place. Provisioning created the installation on the first start of the new image; the instance
  was ready with an empty memory. `migrate-memory.py` exported three records, a dry run reported
  three to store, the import stored three, a second import stored none and reported three as
  already imported. List and search returned all three, a chat request carried the recalled facts
  to the provider, and the three 1.0.2 rows were still in `user_memories`.
- **Upgrade from 1.0.2, KB.** A published 1.0.2 KB holding three shared memories was upgraded in
  place and became ready with an empty memory. The import was **refused**: the shipped
  implementation answered each `memory.store` on the KB's own API with `unsupported_mode`, so
  none of the three records was stored. The 1.0.2 rows were retained in `memories`.

### Exploratory pass

Against a baked Server provisioned for two accounts (`aimee` and a browser login) and a baked KB:

- an API pass of 32 checks as the runtime user (memory store, get, list, search, delete, receipts,
  chat and stream shapes, oversized and malformed requests) had no failure;
- a stored memory was found again after a full container restart;
- the memory module process was killed; the supervisor started a new one, readiness stayed `200`
  and the container did not restart;
- a browser pass (Chromium, desktop and mobile viewports, a real PAM login) covered wrong-password
  rejection, login, session identity, 15 application routes, a chat send and logout: 36 of 39
  checks passed. The other three are the same two observations: the chat page logs `404` for
  `GET /api/plugins` (the handler's answer when no plugin loader is available) and for the
  workflow lookup of a channel that has none, and `/graph` received `502` from
  `GET /api/graph/hubs` for a project with no code graph on this instance. Both pages rendered,
  and the chat message was sent and shown;
- the Server log held one warning, for the unconfigured knowledge service.

### Unchanged host behaviour

With provisioning switched off and the frozen reference memory owner selected, as CI's e2e-docker
job runs it, the image passed every leg: T1 (207 checks), T2 (45), T3 (7), the real-Docker private
verifier, Vault egress (26), plain PostgreSQL storage, the plain KB upgrade (15), T2 on LUKS (5),
LUKS fresh (11) and legacy upgrade (13) storage, and the LUKS KB upgrade (15).

The same T1 and T2 topologies run against the shipped memory instead of the reference owner
stopped at their first KB check, `KB shared memory module stores a record`, for the reason above.

## Remaining gates and limits

The main PR must run its own CI and all reusable release builds before the protected
`main-merge-approval` gate. The `release` environment separately approves publication after merge;
no tag, public release or production rollout is created by this preparation.

Known limits of the candidate, each observed above or stated by the packaging:

- **A KB refuses shared memory writes.** Six request shapes were sent to a freshly provisioned
  KB's `memory.store` action (global with and without a full record, the provisioned shared
  project with model and user authority, another project, a workspace); each returned
  `unsupported_mode`. List and search returned empty results and `memory_ready` was true. The
  cause inside the implementation was not established. The KB legs of T1 and T2 therefore cannot
  qualify the shipped memory, and 1.0.x shared memories cannot be imported.
- **Without an installation the instance fails closed.** With provisioning off and no reference
  owner, `/v1/ready` returned `503` with `modules` as the failed boundary, `/v1/health` still
  answered, and a memory store returned `503 user memory module unavailable`. Switching
  provisioning on at the next start created the installation and the instance became ready.
- **The memory bundle is `linux/amd64` only**, and the release image workflow also builds
  `linux/arm64`. That image carries no memory.
- **Personal memory is served for a fixed list of accounts** (`AIMEE_MEMORY_ACCOUNTS`). An account
  created after provisioning has no personal memory until it is added to the list and the stack
  is restarted.
- **The bundle is tied to 80 public files.** A change to any of them needs a bundle rebuilt from
  the private repository before it can merge; lint and the image build refuse a stale bundle.
- Public CI does not build or test the implementation's source. Records held by a Cognee or
  Hillock backend selected in 1.0.2 are outside the upgrade tool.
