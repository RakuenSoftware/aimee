# Webchat git projects + in-browser VSCode

- **State:** IMPLEMENTED + CLOSED OUT (2026-06-29). Both phases shipped across a
  series of merged PRs; this proposal is filed to `done/`. See **Status at
  close-out** at the foot of this file for the shipped surface, the
  acceptance-criteria audit, and the follow-ups carried as future work
  (done ≠ GA).
- **Scope:** deterministic / webchat (Go) + server workspace plane + a new
  per-user code-server sidecar. Not an intelligence-surface proposal (no
  Architecture Charter role).
- **Author:** JBailes, 2026-06-17.
- **Builds on:** the existing workspace/project model (`aimee workspace add
  [--repo]`, `project_info_t`, `index_list_projects`), the git CLI surface
  (`cmd_git`: status/commit/push/pull/branch/pr/clone/…), the forge-token
  broker (`forge_credentials.c` — provider-agnostic git auth), the
  workspace-resource-plane (detached provider / runner queue), the per-principal
  credential vault, and webchat's existing multi-user PAM auth + sessions.

## Problem

aimee-webchat is already a multi-user, server-hosted browser app (PAM auth,
sessions, `UserManager`, a Chat tab + Dashboard tab). But a webchat user can
only *chat*. They cannot:

1. **Connect a git repo from the browser.** Today the only way a repo enters
   aimee is `aimee workspace add --repo <url>` from a CLI, and the only way the
   server gets push/PR rights is a *filesystem-rich client* handing a
   short-lived forge token over `/v1` (`forge_credentials.c`). A browser user
   has no CLI and no local filesystem, so neither path is reachable. There is
   also **no `/v1` git surface** at all (`cli_v1_routes_gen.inc` has no `git.*`
   routes) — `cmd_git` shells out locally.

2. **Edit those repos.** There is no editor surface in webchat.

The user wants browser users to connect their own repos (any provider) and edit
them in a full in-browser VSCode, with a "workspace = collection of projects,
each git repo = a project" model.

## Goal

Two new capabilities in webchat, **git first, then VSCode**:

- **G. Git projects.** A webchat user connects git credentials, clones one or
  more remote repos as **projects** under their **per-user workspace**, and
  performs core git operations (pull/fetch/commit/push/branch/PR) and
  browse/manage from the UI.
- **V. VSCode tab.** A `/vscode` tab giving the user a full code-server bound to
  their workspace directory, auth-gated and reverse-proxied by webchat.

The model maps onto what exists: a **workspace** is the per-user root directory;
a **project** is a cloned git repo within it (already exactly `project_info_t`
{name, root, scanned_at} + `index_list_projects`).

## Background: what already exists (reuse, don't rebuild)

- **Clone + register + index:** `cmd_infra.c` `workspace add --repo <url>
  [--path <dest>]` clones then `register_and_index`. `aimee git clone` →
  `handle_git_clone`.
- **Git operations:** full `cmd_git` surface (status/commit/push/pull/fetch/
  branch/log/diff/pr/issue/stash/tag/reset/restore/clone/verify), shelling out
  via `run_cmd` with the thread-local cwd.
- **Git auth (provider-agnostic):** `forge_credentials.c` brokers a short-lived,
  narrowly-scoped forge token (GitHub/GitLab App token, fine-grained PAT, or
  `gh auth token`) **in memory only**, injected as `GH_TOKEN` + a `GIT_ASKPASS`
  shim. Scope lattice: global < workspace < project < user.
- **Per-principal credential vault:** encrypted-at-rest, keyed by principal
  (`uid:`/`webuser:`) — the natural durable home for a user's git credential.
- **Co-located execution:** the shared workspace provider runs ops directly on
  the server filesystem (no detached/reverse-channel needed when the files live
  server-side, which is our case).

The **gaps** are: (a) workspaces are a single global list (`cfg.workspaces[]`),
not per-user; (b) no web credential-intake; (c) no `/v1` git surface; (d) no
editor.

## Design

### 0. Per-user workspaces (the foundational gap — shared by G and V)

aimee's workspace registry is global/single-user. Webchat is multi-user, so a
user's projects must be **isolated per principal** on disk and in config.

- On-disk root: `${AIMEE_WORKSPACES_DIR}/webusers/<principal>/` (principal =
  the webchat `webuser:<name>`). Each connected repo clones to
  `…/<principal>/<project>/`.
- Scope the workspace registry by principal. Either (a) a per-principal
  `workspace_count`/`workspaces[]` partition keyed by the authenticated
  principal, or (b) keep the global registry but tag each workspace/project with
  an owning principal and filter every read/write by the caller's principal.
  Recommendation: **(b)** — least churn to the existing config + index, and the
  index already stores absolute roots we can prefix-match per user.
- Every `/v1` workspace/git call resolves its target from `(principal, project)`
  → absolute root, and **rejects** roots outside the caller's own tree (mirrors
  `workspace_turn_reject_foreign_cwd`).

### G. Git projects (Phase 1)

**G1. Credential intake (web).** New webchat settings panel "Git". A user
provides **either**:
- a fine-grained **PAT** / `gh`-style token (provider-agnostic, MVP), or
- an **SSH key** (deploy/user key), or
- (later) a "Connect GitHub" **OAuth** flow.

The credential is POSTed over the authed webchat session to a new
`/v1/forge/credential` route and stored **exclusively in the per-principal
credential vault** (encrypted at rest, keyed by `webuser:<name>`). **All
credentials live in the vault — PATs, SSH private keys, and any later OAuth
tokens alike.** There is no other store: no plaintext on disk, no
`agent-keys.json`-style sidecar, no config file. Scope defaults to `user`;
credentials are never returned to the browser after store, and zeroed from
memory on revoke/session close.

**Vault key derivation for `webuser:` principals.** Per-principal DEKs wrapped
under the server master key (`.server-master.key`, the dual-access wrap), **not**
any session/cookie-derived secret. A `webuser:<name>` principal's vault entries
are sealed server-side and decryptable only by the co-located server, never by
the browser.

> **PREREQUISITE — SATISFIED (verified against `origin/testing`).** This design
> depends on a *persistent, per-principal, encrypted* credential vault, and it
> already exists in mainline: `src/server/server_vault.c`,
> `vault_principal.{c,h}`, `vault_crypto.c`, `vault_server_key.c`
> (`.server-master.key` seal), `vault_kek_cache.c`, `vault_service.c`,
> `vault_capability.c`. Critically, the vault is **already keyed by a
> `webuser:<name>` principal** for webchat users (`vault_principal.h`;
> `ATTEST_WEBCHAT_TRUSTED`), asserted via the `server.token`-gated
> `X-Aimee-Webuser` header — the exact integration seam this feature needs. It
> also already refuses a **tokenless** webuser as a spoof rather than leaking
> across users (a threat the security lens raised). So Phase 1 *uses* the vault
> as-is; the work is wiring webchat's credential-intake/`GIT_ASKPASS`/ssh-agent
> through this existing `webuser:` vault, not building or forward-porting one.

### G1b. Credential injection — never on disk, even for code-server

The roundtable's central blocker: a transient temp file is still a plaintext
sidecar, and a long-lived code-server needs its *own* git auth. Both are solved
**without ever writing a credential to disk**:

- **HTTPS tokens (PAT / OAuth):** a `GIT_ASKPASS` shim binary that, on demand,
  fetches the live token from the in-memory broker (populated from the vault)
  over a per-process authenticated channel and echoes it. The token reaches git
  via the askpass stdout only — never a file, never a logged arg.
- **SSH keys:** a **per-user in-memory `ssh-agent`** (its own Unix-domain socket
  under a `0700` per-principal runtime dir, ideally on `tmpfs`). The private key
  is loaded into the agent **from the vault, in memory** (`ssh-add` from a pipe /
  agent protocol) and **never written to disk**. git uses it via
  `SSH_AUTH_SOCK`; nothing touches `~/.ssh`.
- **code-server gets the same seam:** its process environment carries
  `GIT_ASKPASS` (the vault-backed shim) and `SSH_AUTH_SOCK` (the user's agent).
  Editor-side git (terminal, SCM push) authenticates through exactly these — so
  a long-lived editor never holds a plaintext credential and the vault-only
  invariant holds end-to-end.

The in-memory `forge_credentials` broker / ssh-agent are **transient caches**
populated *from* the vault, scoped per principal, zeroed on revoke/session close
— never the source of truth, never persisted. No credential ever appears as a
file or a process arg; logging masks all credential material.

**G2. Connect a repo = clone as project.** `POST /api/git/connect {url, name?}`
→ webchat → new `/v1/workspace/clone {principal, url, dest}` →
server clones into the user's per-principal root with the user's forge token
injected, then `register_and_index`. Provider-agnostic (any `git clone`-able
URL; https uses the token via `GIT_ASKPASS`, ssh uses the stored key).

**G3. Git operations over `/v1`.** Expose a **read/write-tiered** git surface
(the gap today): `git.status/log/diff/branch` (read) and
`git.pull/fetch/commit/push/checkout/pr` (write), each resolving
`(principal, project)` → root and gated by the existing per-route capability
matrix (`server_auth.c` / route caps) **plus** `forge_cred_scope_allows` for
push/PR. Reuse `cmd_git`'s `handle_git_*` cores behind the routes. webchat adds
`/api/git/*` thin proxies.

**G4. Web UI.** Extend the existing Chat "projects" concept into a **Projects**
panel/tab: list the user's workspace → projects (reusing
`handleChatProjects`/`workspace list` shape), a "Connect repo" button, and
per-project actions: branch switch, pull, commit message + push, open PR, and
status/diff view. Browse/manage repos (the "nice-to-have"): list connected
projects, disconnect (unregister + optionally delete the clone).

**G5. Revocation & no-logging.** A "Disconnect git"/"Remove credential" UI action
→ `DELETE /v1/forge/credential` → **vault entry deleted, broker cache zeroed,
the key removed from the user's ssh-agent, and the user's code-server env
refreshed (or process recycled)** so no live handle survives. Credential
material is masked in all log paths; a build-integrity check (see acceptance
criteria) asserts no credential ever reaches a log or the filesystem.

### V. In-browser VSCode (Phase 2)

**V1. Editor engine: code-server**, full VSCode, running on aimee-server (per
the decided approach). Open VSX registry only; no MS Marketplace / MS-branded
binaries (licensing — both code-server and openvscode-server share that
constraint).

**V2. Per-user process, OS-isolated.** webchat launches/owns a **code-server
process per active user**, bound (`--user-data-dir`, `--extensions-dir`, working
dir) under that user's per-principal workspace root, listening on a loopback
port. Crucially, isolation is enforced at the **OS level, not just by path
binding** (roundtable blocker — the editor terminal can otherwise `cd` out):
each code-server runs **as the user's own PAM UID** (webchat already creates a
per-user OS account) so filesystem permissions deny cross-user reads, and is
additionally confined to the workspace subtree via a namespace/bind jail
(`bwrap`/`unshare`) where available. Telemetry off; no MS Marketplace. Its
`--user-data-dir` lives under the user's tree (or is ephemeral) so editor state
never captures another user's data. Credential env per G1b (`GIT_ASKPASS` +
`SSH_AUTH_SOCK`) — no plaintext at rest. Lifecycle: lazily started on first
`/vscode` open; **idle = no HTTP/WebSocket traffic for N minutes** (not UI focus,
so an open-but-quiet editor isn't killed); torn down on logout. A supervisor in
webchat (mirroring `webchat-lib.sh`) tracks `{principal → port, pid, agent
socket}`.

**V3. Reverse proxy + auth.** A `/vscode/*` route in webchat reverse-proxies
(HTTP + WebSocket upgrade) to the user's code-server, **gated by the existing
webchat session** (`requireAuth`) so code-server is never directly reachable and
inherits webchat's PAM identity. The proxy resolves `{session cookie →
principal → that principal's port}` **per request** (never a client-supplied
port/path), and the **WebSocket upgrade traverses the same `requireAuth` gate**
(no unauthenticated upgrade path). Because the editor terminal is a powerful
sink reachable over a same-cookie WebSocket, the upgrade path **also enforces a
same-origin `Origin` allow-list** (webchat's own host): a cross-origin or
missing-`Origin` upgrade is rejected, closing the cross-origin WS-CSRF surface
that `requireAuth` alone (cookie-based) does not. Webchat session cookies are **stripped**
before forwarding upstream so code-server never receives webchat session
material. code-server's own auth is disabled (webchat is the sole gate); it binds
loopback only.

**V4. Tab.** Add `{ label: 'VSCode', route: '/vscode' }` to the SPA `NAV_ITEMS`;
the page is an `<iframe src="/vscode/">`. (The recent ErrorBoundary work means a
load failure degrades gracefully instead of blanking the app.)

**V5. Shared filesystem = agent + editor coherence.** Because projects live in
the user's server-side workspace root and aimee already indexes that root, the
chat agent and the editor operate on **the same files** — edits in VSCode are
visible to `code_search`/ingest and vice versa.

## Deployment (aimee-combined / .254)

- code-server is a new binary in the `aimee-server-kb` image (or a sidecar
  container). Add to `Dockerfile.combined`; gate behind a build flag
  (`WITH_VSCODE=1`) like `WITH_WEBCHAT`. Open-VSX-only, pinned version.
- `aimee-combined-entrypoint` / `webchat-lib.sh` gain the supervisor hooks; only
  the webchat port stays published (code-server is loopback-only behind the
  proxy).
- Per-user disk lives under the persistent `AIMEE_WORKSPACES_DIR` volume.

## Out of scope

- GitHub/GitLab **OAuth "Connect"** flow (G1 ships PAT/SSH first; OAuth layers on
  the same vault seam later).
- Multi-tenant resource isolation beyond per-user dirs + per-process code-server
  (no per-user containers/cgroups in v1).
- Merge-conflict UI, code review UI, terminal-sharing — code-server's built-in
  terminal/SCM covers the editor side.
- Thin-client (CLI) users: they keep the existing detached-workspace path; this
  proposal is the **server-hosted browser** path.

## Risks

- **Multi-user isolation.** Per-principal path scoping must be airtight —
  every git/workspace/proxy route rejects cross-principal roots/ports. A bug
  here is a cross-tenant file/edit leak. (Mirror `reject_foreign_cwd`; add
  tests.)
- **Credential durability vs. exposure.** Moving credentials into a *persistent*
  vault (vs. the pre-existing in-memory-only `forge_credentials` broker this
  design builds on) widens their lifetime, so the mitigation is the **vault-only
  invariant**: encryption-at-rest, per-principal keys, never-return-to-browser,
  never-logged, and exec-time materialization that — *as the target* — never
  reaches the filesystem or a child's environment.
  **Status by credential type at close-out:**
  - *SSH keys — invariant MET.* Loaded into the per-user in-memory ssh-agent over
    a non-filesystem channel (`memfd`/agent-protocol, decrypt buffer zeroed after
    load, `RLIMIT_CORE=0`), never written as a named path (G1b).
  - *HTTPS tokens — PARTIAL.* Reach git via the `GIT_ASKPASS` shim **and** as
    `GH_TOKEN` in the git child's environment, so the token is visible in that
    short-lived child's `/proc/<pid>/{cmdline,environ}` (cmdline is argv-only and
    clean; environ is not). Moving HTTPS auth to the broker-fed askpass channel
    the G1b design describes — so `GH_TOKEN` never enters a child env — is
    **deferred item 3**.

  The **binding acceptance check** (a build-integrity / leak test) is the *target*
  gate: no credential material outside the sealed vault — not a file (incl.
  `/tmp`, `~/.ssh`, `--user-data-dir`), not a log, not `/proc/<pid>/{environ,fd,
  maps}` — surviving a mid-exec `SIGKILL` with no orphaned key file. The shipped
  `test_webchat_git_leak.c` covers the no-file-under-`$AIMEE_HOME` + vault
  round-trip + cross-principal slice; extending it to the full `/proc` + push +
  SIGKILL assertions (and closing the HTTPS-environ gap it would then catch) is
  **deferred item 6**.
- **Resource cost.** N concurrent users = N code-server processes. Mitigate with
  idle timeout + a configurable max-concurrent cap (log when capped, per the
  no-silent-truncation rule).
- **code-server licensing.** Open VSX only; no MS Marketplace / MS-branded
  build. Document the constraint.
- **Proxy correctness.** WebSocket upgrade + large payloads through the webchat
  proxy; reuse a vetted Go reverse-proxy and test the upgrade path.

## Acceptance criteria

- A webchat user connects a git credential from the browser; it is stored
  **only** in the encrypted per-principal vault, persists across sessions, and is
  never returned to the client. Verified by an automated leak check: after a
  connect + a git push from both the API and the code-server terminal, **no
  credential material exists anywhere outside the sealed vault** — not as a file
  (incl. `/tmp`, `~/.ssh`, code-server's `--user-data-dir`), not in any log, and
  not in a process arg/`/proc/<pid>/environ`. The SSH key is present only inside
  the in-memory ssh-agent (reachable via `SSH_AUTH_SOCK`), never on disk. The
  check survives a mid-exec `SIGKILL` (no orphaned key file).
- The user clones ≥2 repos as projects under their **own** isolated workspace;
  another user cannot see or reach them (verified by a cross-principal denial
  test).
- The user performs pull / commit / push / branch / open-PR from the UI against
  a real remote, provider-agnostically.
- A `/vscode` tab opens a full code-server bound to the user's workspace; edits
  there are visible to the chat agent's `code_search`, and vice versa.
- code-server is unreachable except through the authed webchat proxy.
- All default-off behind a flag until validated; CI green incl. the isolation
  tests.

## Prerequisites & deferred hardening (roundtable rev.2)

The architecture above closed every fundamental blocker from roundtable rev.1.
The rev.2 panel converged on a **gating prerequisite** plus a set of
mechanism-level hardening items that belong in the **implementation plan** (which
gets its own roundtable), not the proposal:

- **Gating prerequisite — persistent vault.** See the PREREQUISITE box in §G1.
  Resolve before Phase 1 implementation.
- **No-plaintext-window on key load.** Loading the SSH key from the vault into the
  ssh-agent must avoid *any* filesystem-backed plaintext: prefer `memfd`/agent
  protocol over a named pipe, zero the decrypt buffer after `ssh-add`, set
  `RLIMIT_CORE=0` to bar core dumps, and verify no exposure via `/proc/<pid>/fd`.
- **Extension-host credential containment.** code-server's extension host is
  untrusted: sanitize `GIT_ASKPASS`/`SSH_AUTH_SOCK` out of the extension-host
  environment, disable git `credential.helper` caching, and forbid
  credential-caching extensions — a malicious/over-eager extension must not be
  able to read or persist credentials.
- **Per-process credential scoping.** The askpass channel and agent socket are
  per-principal, `0700`, owned by that user's UID (not a shared global path); a
  spoofed askpass from another user must fail.
- **Child-process escape.** Constrain code-server's terminal so it can't escape
  the workspace jail via `sudo`/`docker`/`nsenter`.
- **Agent survival across restarts** (idle-kill → relaunch) re-loads the key from
  the vault, never from disk.
- **`tmpfs` is mandatory** for the per-principal runtime dir (agent socket); fail
  closed if unavailable rather than spilling to persistent disk.

## Phasing

1. **Phase 1 (Git):** §0 per-user workspaces + G1–G4. Independently shippable
   and useful without the editor.
2. **Phase 2 (VSCode):** V1–V5 on top of the Phase-1 workspace model.

## Status at close-out (2026-06-29)

**IMPLEMENTED + CLOSED OUT.** Both phases shipped across a series of merged PRs;
the design above is what was built. This section records the shipped surface, the
acceptance-criteria audit, and the follow-ups carried as future work
(**done ≠ GA**). A final roundtable review of the *shipped code* (not just the
design) drove the last two hardening PRs (#877, #878) before close-out.

### Shipped

- **§0 per-user workspaces** — WP-A (#451): `workspace_scope.{c,h}` resolves
  `(principal, project) → root` from the *attested* `webuser:` principal (never a
  client-supplied path), with `realpath` + `O_NOFOLLOW`/`openat` containment.
  Every `/v1` workspace/git route resolves through it; cross-principal denial is
  tested (`test_workspace_scope`, `test_webchat_git_leak`).
- **G1 credential intake + vault** — per-host token store + GitHub OAuth
  device-flow sign-in (#486–#489), keyed by the `webuser:` principal, write-only
  (host list returns names, never secrets; tokens never returned to the browser).
- **G1b injection** — `GIT_ASKPASS` shim + a per-user in-memory `ssh-agent`
  (`memfd_create`, `RLIMIT_CORE=0`, `PR_SET_DUMPABLE=0`, key buffer zeroed after
  `ssh-add`); git's on-disk `credential.helper` disabled; per-principal runtime
  dir is **tmpfs-mandatory, fail-closed** (`webuser_runtime.c`).
- **G2/G3 clone + git ops** — `/v1/workspace/clone` (clone → `register_and_index`)
  and `/v1/workspace/git` (status/log/diff/branch/fetch/pull/commit/push/checkout),
  argv-only (no shell), with per-session worktree isolation (#611).
- **G4 UI** — Projects panel (`Projects.tsx`); **G5 revoke** — vault delete +
  editor/ssh-agent recycle so no live handle survives revocation (#877).
- **V code-server** — per-webuser supervisor WP-I (`webuser_editor.c`) + reverse
  proxy WP-J (#467); `/vscode` tab; loopback-only bind; `requireAuth` **plus** a
  same-origin gate on the WS upgrade (#877); idle-reaping wired (#878);
  `WITH_VSCODE` image flag. The editor process env is curated of the server's own
  secrets (`AIMEE_DB2_URL`, `AIMEE_SERVER_TOKEN`, provider keys) and hardened
  with `no_new_privs`/`DUMPABLE=0` — but `GIT_ASKPASS`/`SSH_AUTH_SOCK` are still
  inherited by the untrusted extension host (deferred item 4), and the process
  runs as the server UID with the server `PATH` (deferred item 5).

### Acceptance-criteria audit

- **MET:** cross-principal isolation at the `/v1` route level (WP-A `realpath` +
  `openat`/`O_NOFOLLOW` containment, + denial test), tmpfs fail-closed, the
  loopback-only editor being reachable only through the authed proxy *at the
  route boundary*, shared-FS agent⇄editor coherence, clone ≥2 repos as projects,
  read/write git ops from the UI, and the SSH-key leak invariant *for the
  implemented + tested agent-load path* (agent-only, never on disk).
- **PARTIAL:** the **HTTPS token** is injected as `GH_TOKEN` in the git child's
  environment, so it is present in that short-lived child's `/proc/<pid>/environ`
  (deferred item 3). **Browser-facing SSH-key intake** is unreachable (only PAT/
  token + OAuth have intake), so the agent path above can't yet be driven from
  the UI (deferred item 1). The **editor process** runs as the server UID with
  the server `PATH` and an extension host that inherits the git-cred env, so the
  loopback/proxy boundary above is the *route-level* guarantee, not yet an
  OS-jail one (deferred items 4, 5). **Open-PR** from the UI is not wired
  (deferred item 2).
- **DEVIATION (deliberate):** *default-off behind a flag* — the feature ships
  **default-ON**, an operator decision (branch `feat/webchat-git-editor-default-on`).
  The two surfaces are asymmetric and an operator should know it:
  - the **editor** is toggleable (`AIMEE_WEBCHAT_EDITOR=0` disables it);
  - the **git surface** (`/v1/git/*`, `/v1/workspace/{clone,git}`) ships
    **always-on with no operator opt-out**.
  Because two of the default-ON decision's risk-mitigating prerequisites —
  extension-host credential containment (item 4) and editor OS-jail hardening
  (item 5) — are themselves deferred, the default-ON posture carries a residual
  risk window; re-evaluating it is **deferred item 7**.

### Deferred follow-ups (future work; not blocking close-out)

1. **SSH private-key web intake.** The ssh-agent load path is fully built +
   tested but unreachable from the browser (only PAT/token + OAuth have intake).
   Add `POST/DELETE /v1/git/credential {type:"ssh_key"}` writing the SSH-key cred
   under the `webuser:` principal + a UI key field — completes G1's SSH option.
2. **Open-PR op.** `/v1/workspace/git` has no `pr` op end-to-end; provider-specific
   (`gh`/`glab`), needs a real forge to validate.
3. **HTTPS token out of `/proc/<pid>/environ`.** Move the askpass to a
   socket-fed channel (the design's "fetches the live token from the in-memory
   broker over a per-process authenticated channel") so `GH_TOKEN` never enters a
   child env.
4. **Extension-host credential containment (WP-K).** `GIT_ASKPASS`/`SSH_AUTH_SOCK`
   are inherited by code-server's extension host; sanitize them out of the
   extension-host env (keep them only for the integrated terminal + SCM).
5. **Editor OS-jail hardening.** `PATH` sanitization (drop `sudo`/`docker`/
   `nsenter`), a `bwrap`/`unshare` namespace jail, `seccomp`, cgroup CPU/mem
   limits, and per-user UID drop by default (today the editor runs as the server
   UID with the server `PATH`; `no_new_privs` + `DUMPABLE=0` + a secret-free
   curated env are already applied).
6. **Leak-test depth.** Extend `test_webchat_git_leak.c` to a real `git push` and
   assert clean `/proc/<pid>/{environ,fd}`, `~/.ssh`, `--user-data-dir`, and
   `SIGKILL` survival (the binding acceptance check above).
7. **Re-evaluate the default-ON posture** once the extension-host containment (4)
   and editor OS-jail hardening (5) land, and add an operator opt-out for the
   git surface (it ships always-on today) — see the DEVIATION note.
</content>
