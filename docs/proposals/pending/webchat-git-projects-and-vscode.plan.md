# Implementation plan — Webchat git projects + in-browser VSCode

- **Source:** `docs/proposals/pending/webchat-git-projects-and-vscode.md` (USER-APPROVED).
- **State:** draft plan; awaiting plan-roundtable, then implementation.
- **Shippable milestone:** Phase 1 (Git) — WP-0..WP-G. Phase 2 (VSCode) —
  WP-H..WP-K — rides on the Phase-1 workspace/credential model.
- **Default-off** behind a config flag until validated.

Each WP is a bounded, delegate-sized unit. WP-0 is a hard gate (verify the
assumptions the rest of the plan rests on). The auto-planner produced no
implementation packets (it only detects edits to existing files; this is mostly
greenfield), so packets are authored here.

## Existing primitives to build on (verified in `origin/testing`)

| Primitive | Where | Use |
|---|---|---|
| `vault_principal_resolve(is_tcp,is_tls,peer_uid,webuser,webuser_token_ok,…)` | `server/vault_principal.c` | THE single identity point; already mints `webuser:<name>` under the `server.token` boundary, refuses a tokenless webuser as a spoof |
| `ATTEST_WEBCHAT_TRUSTED` + `X-Aimee-Webuser` (server.token-gated) | `headers/vault_principal.h`, `server/server_http_identity.c` | the webchat→server principal assertion seam |
| `vault_service_*` (sealed set/get), `vault_store_*`, `VAULT_*` consts, `.server-master.key` | `server/vault_service.c`, `vault_store.h`, `vault_server_key.c` | persistent per-principal encrypted credential storage |
| `forge_cred_install/get/scope/revoke`, `forge_cred_scope_allows` | `forge_credentials.{c,h}` | in-memory forge-token broker → `GH_TOKEN` + `GIT_ASKPASS`; scope lattice. **Extend** to load from the vault |
| `workspace add --repo` clone+index; `index_list_projects`, `project_info_t`; `cfg.workspaces[]`; `/v1/workspaces` | `cmd_infra.c`, `index.h`, `cli_workspace_serve.c` | workspace=projects model; **extend** to per-principal scoping |
| `cmd_git` → `handle_git_*` cores | `cmd_infra.c` | reuse cores behind new `/v1/git/*` routes |
| `workspace_turn_reject_foreign_cwd` | `server/workspace_turn.c` | precedent for cross-principal root rejection |
| webchat `requireAuth`, `handleChatProjects`, auth principal | `webchat/chat.go`, `webchat/server.go` | session→principal; project listing shape |
| `Dockerfile.combined`, `aimee-combined-entrypoint`, `webchat-lib.sh` | repo root / deploy | image + process supervision seam |

## WP-0 — Assumption gate (HARD GATE, audit-only)

Confirm before building: (1) does webchat already send `X-Aimee-Webuser` (and
hold `server.token`), or must we wire it (WP-B dependency)? (2) is the forge
broker currently fed only from a client over `/v1`, with no vault backing yet?
(3) confirm `vault_principal_resolve` mints `webuser:` exactly as the proposal
assumes. (4) Phase-2 input: how should a per-principal **service UID** be
allocated for `webuser:` principals (which are not PAM login users) for WP-I's
OS-level isolation? Output: a short findings note; adjust WP-A/B/I owners if any
assumption is false. **No code.** Gate: clears WP-A (and the Phase-2 UID model).

**WP-0 FINDINGS (done 2026-06-17, verified on `origin/testing`):**
1. The `webuser:` path is **fully wired server-side** — `server_http_identity.c`
   reads `X-Aimee-Webuser` (gated by the `server.token` secret), feeds
   `vault_principal_resolve(...)` → `conn->vault_principal`; caps/vault/audit key
   off that. WP-A/B use the resolved principal directly.
2. webchat sends `server.token` (bearer) but **not** `X-Aimee-Webuser` →
   **WP-B owns adding the header** on credential/git calls.
3. `POST /v1/workspaces/{id}/forge-token` (`rh_workspace_forge_token`) exists but
   is **workspace_id-keyed + in-memory only** → WP-B re-keys to principal + adds
   vault persistence; that route is the template.
4. `forge_cred_askpass_shim()` + `forge_cred_build_env()` already inject
   `GH_TOKEN`+`GIT_ASKPASS` (used in `workspace_turn.c`) → **WP-C reuses these for
   the HTTPS half; the in-memory ssh-agent is the only new piece.**
5. Phase-2 UID model: `webuser:` are not PAM login users → WP-I allocates a
   per-principal service UID from a map (no `getpwnam` path exists).

## Phase 1 — Git

### WP-A — Per-user workspace scoping (`§0`)
- Owned: `server/workspace_scope.{c,h}` (new) + minimal touches to the workspace
  registry/config read path and `index_list_projects` callers.
- Tag each workspace/project with an owning principal; resolver
  `ws_scope_resolve(principal, project) → abs_root`; reject roots outside the
  caller's `${AIMEE_WORKSPACES_DIR}/webusers/<principal>/` tree (mirror
  `reject_foreign_cwd`). Create the per-principal root `0700` lazily.
- **Traversal/TOCTOU hardening (plan-roundtable):** canonicalize with
  `realpath(3)` and re-check the prefix on the *resolved* path; reject `..`,
  `//`, and **symlinks that escape** the principal root (resolve symlinks, or
  open with `O_NOFOLLOW`/`openat2(RESOLVE_BENEATH)`); resolve-then-use on the
  *same* fd to close the TOCTOU window between check and use.
- Accept: a `webuser:` sees only its own workspaces/projects; a symlink or `..`
  crafted to escape the root is rejected (covered by WP-G).

### WP-B — Web credential intake → vault
- Owned: `server/server_forge_cred_route.c` (new) `POST/DELETE /v1/forge/credential`;
  extend `forge_credentials.c` to **persist to / hydrate from** the sealed vault
  under the caller's `webuser:` principal; `webchat/api.go` `/api/git/credential`
  + a settings panel field (frontend).
- Store PAT/SSH-key/(later OAuth) **only** in the vault; never returned to the
  browser; masked in logs. **`.server-master.key` access control:** assert it is
  readable only by the server process (`0600`, server UID) and is never reachable
  by webchat-frontend, code-server, or its UID — a test guards this.
- Accept: connect a credential; it survives a server restart **and a server
  master-key rotation** (re-encrypt round-trip preserves `webuser:` entries);
  absent from disk outside the vault (feeds WP-G leak test).

### WP-C — No-disk credential injection (roundtable-critical)
- Owned: `server/git_cred_inject.{c,h}` (new): (a) a `GIT_ASKPASS` shim that
  fetches the live token from the broker over a per-principal `0700` socket;
  (b) a per-user in-memory **`ssh-agent`** (socket in the WP-L tmpfs dir),
  exposed as `SSH_AUTH_SOCK`.
- **Explicit no-disk steps (each verifiable, owned here — plan-roundtable):**
  1. Decrypt the SSH key from the vault into an **anonymous `memfd`** (never a
     named pipe or file); load it into the agent via the agent protocol.
  2. **Zero** the plaintext decrypt buffer **before** `ssh-add` returns, and
     **close/unlink the `memfd`** immediately after the agent has the key.
  3. Set **`RLIMIT_CORE=0`** on the agent process *and* on git/`ssh` children
     (no core dump can capture key material); set `PR_SET_DUMPABLE=0`.
  4. **Unlink** the askpass + agent sockets on session close / process exit
     (and on crash via the WP-L dir being tmpfs + a reaper); no orphans.
- Accept: git push over https (token) and ssh (agent) with **zero** plaintext on
  disk *or in `/proc/<pid>/fd`, `/proc/<pid>/environ`, or a core dump*; survives
  mid-exec `SIGKILL` (verified by WP-G).

### WP-L — tmpfs runtime dir + fail-closed (dependency of WP-C/WP-I)
- Owned: provisioning of `${RUNTIME}/webusers/<principal>/` as a `0700`
  **tmpfs** mount (agent sockets, askpass socket). **Owns the tmpfs check**:
  if tmpfs is unavailable, **fail closed** — refuse git/ssh credential ops rather
  than spill sockets/state to persistent disk. A reaper unlinks dirs on logout.
- Accept: with tmpfs forced unavailable, credential ops are refused (not spilled).

### WP-D — Clone-as-project over `/v1`
- Owned: `POST /v1/workspace/clone` handler (route + reuse the `cmd_infra.c`
  clone core) — clones into the caller's per-principal root with WP-C injection,
  then `register_and_index`.
- Accept: a `webuser:` clones a private repo as a project, provider-agnostically.

### WP-E — Git operations over `/v1`
- Owned: `/v1/git/*` routes (read tier: `status/log/diff/branch`; write tier:
  `pull/fetch/commit/push/checkout/pr`) wrapping `handle_git_*`; resolve
  `(principal, project)→root`; gate writes by route caps **and**
  `forge_cred_scope_allows`. Regen `cli_v1_routes_gen.inc` + openapi.
- Accept: each op works scoped to the caller's project; a read-only cap can't push.

### WP-F — Webchat Projects UI + `/api/git/*`
- Owned: `webchat/api.go` `/api/git/*` proxies; frontend Projects panel/tab
  (connect repo, list/browse/disconnect, branch switch, pull, commit+push, PR).
- Accept: full connect→clone→edit-less git lifecycle from the browser.

### WP-G — Isolation + leak tests
- Owned: tests. Cross-principal **denial** tests (user B cannot read/clone/git
  into user A's project, incl. a **symlink/`..` escape** attempt per WP-A); the
  **credential leak** acceptance check after push from the API: no cred as a
  file, in a log, in `/proc/<pid>/environ`, in **`/proc/<pid>/fd`**, or in a
  **core dump** (assert `RLIMIT_CORE=0` is in effect); survives mid-exec
  `SIGKILL`.
- Accept: maps to proposal acceptance criteria 1–3 + isolation.

## Phase 2 — VSCode

### WP-H — code-server in the image
- Owned: `Dockerfile.combined` (+ `WITH_VSCODE=1` flag, like `WITH_WEBCHAT`),
  pinned version, Open-VSX only, telemetry off.

### WP-I — Per-user code-server supervisor
- Owned: `webchat-lib.sh` + a webchat supervisor (`{principal→port,pid,agent}`).
  Launch under a **dedicated per-principal low-privilege service UID** (mapped
  from `webuser:<name>` — these are *not* PAM login users, so the supervisor
  allocates/looks up a uid from a per-principal map, not `getpwnam`) + a
  namespace/bind jail to the workspace root; env carries WP-C `GIT_ASKPASS` +
  `SSH_AUTH_SOCK`; idle = no HTTP/WS traffic for N min; killed on logout.
- **Child-escape + restart (plan-roundtable):** `no_new_privs` + a seccomp/jail
  that bars `sudo`/`nsenter`/namespace breakout from the terminal & extensions;
  on idle-kill→relaunch, **re-load the SSH key from the vault into a fresh
  in-memory agent** (never a disk fallback) and re-derive the env.
- Accept: editor-side git authenticates via the vault seam; OS perms + jail deny
  cross-user reads and child escape; a restarted editor re-auths with no disk.

### WP-J — `/vscode` reverse-proxy + tab
- Owned: `webchat/server.go` `/vscode/*` (HTTP+WS) behind `requireAuth`,
  per-request `cookie→principal→port`, strip webchat cookies upstream; SPA
  `/vscode` nav tab (iframe).
- Accept: code-server reachable only through the authed proxy; another user's
  port is unreachable.

### WP-K — Extension-host containment + editor leak tests
- Owned: strip `GIT_ASKPASS`/`SSH_AUTH_SOCK` from the env of the extension host
  **and every child it spawns** (terminals, tasks, extensions), disable git
  `credential.helper` caching. Tests: a hostile extension cannot read
  `SSH_AUTH_SOCK`/`/proc/<ppid>/environ` or spawn a child that inherits the
  agent; editor push leak test (same surfaces as WP-G); child-process escape test.
- Accept: maps to proposal acceptance criteria 4–6.

## Sequencing & risk
- **Order:** WP-0 → WP-A → WP-B → **WP-L** → WP-C → (WP-D, WP-E parallel) → WP-F
  → WP-G ⇒ Phase 1 ships. Then WP-H → WP-I → WP-J → WP-K. **WP-0 also gates
  Phase 2** (its assumption audit covers the PAM/UID-mapping question WP-I needs).
  WP-L lands before WP-C/WP-I (both depend on the tmpfs runtime dir).
- **WP-C is the highest-risk** (in-memory ssh-agent + no-disk invariant) and the
  load-bearing security piece — give it the most adversarial plan-review and its
  own leak test before WP-D/E depend on it.
- **WP-A is foundational** to every later WP; a scoping bug is a cross-tenant
  leak — review the resolver and reject path hardest.
- Each WP lands default-off; isolation/leak tests (WP-G, WP-K) gate enabling.

## Tests (summary)
- Unit: scope resolver reject path (WP-A); vault hydrate/persist round-trip (WP-B);
  askpass + ssh-agent no-disk + SIGKILL (WP-C); scope-gated git write deny (WP-E).
- Integration: cross-principal denial (WP-G); editor push via vault seam +
  extension-host env sanitized (WP-K).
</content>
