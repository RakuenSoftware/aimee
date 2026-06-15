# Proposal: Server-hosted OAuth CLI agents (claude / codex)

- **State:** reviewed R1 (roundtable 2026-06-15: 5-model panel — security /
  architect / QA / contrarian / constructive — 76 findings; the §"Security &
  robustness contract" below resolves every blocking theme). Pending re-review.
- **Author:** JBailes / Claude

## Goal

`aimee agent add claude-oauth` and `aimee agent add codex-oauth` provision the
**actual vendor CLI on the aimee-server host**, drive its OAuth login **on the
server**, and register it as a **server-side delegate**. The payoff: claude and
codex become real server-side panelists (today claude is excluded from the
roundtable because it is client-CLI-only, and codex rides a client-held token).

This supersedes the current client-side behaviour: `codex-oauth` today runs a
device-code flow on the *client* and stores the token client-side; `claude-oauth`
does not exist. Both become server-hosted.

## Constraints (from the user)

- **No PTY forwarding.** Everything happens locally to aimee-server. The
  interactive login runs in a **server-side tmux session**; we **scrape its OAuth
  URL + device/user code and surface those to the user** (as the existing
  device-code flow already does). The user completes the browser step themselves.
- **Provisioning:** bake the *enablers* (tmux + node/npm) into the aimee-server
  image; **install the CLI on-demand** at `agent add` time into a home-volume
  prefix so the CLI and its OAuth token persist across container recreate.

## Design

### 1. Image enablers (Dockerfile.server / .combined)
Add to the runtime stage: `tmux`, `nodejs`, `npm` (Debian bookworm packages).
Set an npm prefix on the persistent home volume so global installs land there:
`ENV NPM_CONFIG_PREFIX=/var/lib/aimee/.npm-global` and prepend
`/var/lib/aimee/.npm-global/bin` to `PATH`. (~150 MB image growth; enablers only,
not the CLIs.)

### 2. Server op: install + login (new `/v1` route)
A server-side operation `agent.cli_oauth_setup` (op-run, async — it shells out and
waits on a human) parameterised by `vendor ∈ {claude, codex}`:
1. **Install** the CLI if absent: `npm i -g @anthropic-ai/claude-code` (claude) /
   the codex CLI (codex) into the home-volume prefix. Idempotent (skip if present).
2. **Launch login in a detached tmux session** (`tmux new-session -d -s
   cli-oauth-<vendor> '<cli> login'`), with the CLI's config dir under
   `/var/lib/aimee` (`HOME=/var/lib/aimee`, so `~/.claude` / `~/.codex` persist).
3. **Scrape** the session's pane (`tmux capture-pane -p`) for the verification URL
   + user code; return them in the op-run status.
4. **Poll** the tmux pane / the CLI's auth state until login completes or times
   out; surface `pending` → `authenticated` → `failed`.

### 3. Client command (cmd_agent_setup.c)
`setup_claude_oauth` / rework `setup_codex_oauth` to:
- POST `agent.cli_oauth_setup {vendor}` to the server,
- print the returned **"Open <url> and enter code <code>"** instructions,
- poll the op-run, printing progress, until authenticated,
- on success, register the agent (kind=`claude`/`codex`, server-side CLI
  execution) and enable it.

### 4. Server-side CLI delegate execution
Route the registered agent's delegate calls to the server-installed CLI via the
existing `provider_cli_adapter` (claude/codex adapters), executing **on the
server** (not the client reverse channel). This is the path that lets the
roundtable seat claude/codex as server-side panelists. `agent_is_claude_cli` +
the #318 panel gate are updated so a *server-hosted, authenticated* claude is
panel-eligible (distinct from the client-only claude).

### 5. Persistence
CLI binaries (npm prefix) and OAuth tokens (`~/.claude`, `~/.codex`) both live
under `/var/lib/aimee` (the bind-mounted home volume), so a container recreate
keeps them — no re-auth after a redeploy.

## Existing infrastructure to build on (discovered 2026-06-15)

More of this exists than first assumed — the implementation *extends* it rather
than starting fresh:
- **`server_agent.c` already has a server-side `agent.setup` / `agent.setup_poll`
  codex-oauth device flow** (start + poll handlers, `SAGENT_CODEX_*` device-auth
  URLs). The OAuth state/poll skeleton is there.
- **`sagent_configure_tmux_cli_agent`** already configures a server-side tmux-CLI
  agent for `codex-cli` / `claude` / `claude-code` (`cli_kind`, `cli_cmd`, empty
  HTTP endpoint). The server-side CLI agent shape exists.
- The `claude` provider path (server_agent.c ~576) already sets up a tmux-run
  `claude` agent — it just assumes `claude` is on PATH (it is not, on the minimal
  image — hence the enablers + on-demand install).

So the remaining work, concretely:
1. **On-demand `npm i -g` install** of the vendor CLI when absent (new step in the
   setup flow; idempotent).
2. **`claude-oauth`**: a setup path that installs claude-code, runs
   `claude setup-token` in a server tmux session, scrapes the URL, accepts the
   pasted code back (`tmux send-keys`), and registers the tmux-CLI agent.
3. **`codex-oauth`**: prefer the installed CLI's `codex login --device-auth`
   (or keep the existing HTTP device flow) + register the tmux-CLI agent.
4. **Panel eligibility**: relax the #318 `ensemble_default_panel_from_agents`
   exclusion so a *server-hosted, authenticated* claude (distinct from the
   client-only one) can be seated.

## Security & robustness contract (resolves roundtable R1)

This section is the binding contract; where it conflicts with the prose above, it
wins.

### Vendor matrix (the one source of truth — resolves the flow inconsistency)
`vendor` is a **closed enum** `{claude, codex}` validated server-side *before* any
process is spawned; any other value is rejected. Per vendor (derived from the
2026-06-15 spike against claude-code 2.1.177 / codex-cli 0.139.0):

| | claude | codex |
|---|---|---|
| npm package (pinned) | `@anthropic-ai/claude-code@<pin>` | `@openai/codex@<pin>` |
| executable | `claude` | `codex` |
| login cmd | `claude setup-token` | `codex login --device-auth` |
| user step | authorize at scraped URL → **paste the returned code back** | enter the scraped code at the scraped URL |
| code submission | `tmux send-keys` the user-supplied code (charset/length validated) | none — the CLI **polls** to completion |
| done check | `claude` token file present + a `--version`/auth probe | `codex login status` exits 0 |
| config dir | `$HOME/.claude` | `$CODEX_HOME` |

So claude uses `setup-token` (paste-back), codex uses `--device-auth` (poll) —
**not** bare `claude login` / `codex login` (the latter opens a localhost
callback, useless headless). No ambiguity remains.

### No shell — argv only
npm, tmux, and the CLI are spawned via `safe_exec_capture` (execve/argv), **never
`system()`/`sh -c`**. `vendor` maps through the closed enum to constant argv
arrays and constant binary paths resolved from the npm prefix; no string is ever
interpolated into a shell. (Resolves command-injection findings.)

### Install (separate, idempotent, pinned, retriable)
Split into `agent.cli_install {vendor}` and `agent.cli_login {vendor}`. Install:
`npm i -g <pkg>@<pinned-version> --ignore-scripts` (no lifecycle scripts run with
server privileges); "present" = the pinned executable runs `--version` 0, not mere
dir existence; failure is an op-run error with **redacted, bounded** logs and a
`--reinstall` recovery flag. The pinned version is recorded on the agent record.

### Token custody & tenancy
**aimee-server is single-tenant per deployment** (stated explicitly; the existing
delegate model already assumes one operator's vault). Tokens live in
**per-vendor config dirs** under the home volume (`$HOME=/var/lib/aimee`,
`$CODEX_HOME=/var/lib/aimee/.codex`), created `0700`, token files `chmod 0600`,
`chown` to the aimee runtime UID, under `umask 077`. `XDG_*` and `CODEX_HOME` are
pinned to the volume in the setup env (CLIs may write outside `$HOME`; verified
empirically before GA). The home volume must be a private named volume, not a
world-readable host bind. Backups exclude the token dirs.

### Secret handling in the op-run
The pane scrape extracts **only** the verification URL and user code via a
per-vendor regex (after ANSI-stripping); raw pane dumps are **never** persisted or
logged. The URL+code are returned **once**, then dropped from subsequent poll
responses. A token-shape redactor (`sk-…`, `eyJ…`, `sk-ant-…`) runs over op-run
status serialization as defense-in-depth.

### Concurrency, timeout & cleanup
A per-vendor file lock under `/var/lib/aimee/lock/` serializes setups; a second
concurrent call gets `setup_already_in_progress`. The tmux session name carries
the op-run id (`aimee-oauth-<vendor>-<opid>`) on a **private tmux socket**
(`-S /var/lib/aimee/.tmux/<opid>.sock`, dir `0700`). A hard wall-clock timeout
(default 10 min, < the codex 15-min code TTL) kills the session, releases the
lock, and fails the op. Server startup GCs any stale `aimee-oauth-*` sessions and
reconciles their op-runs to `failed`.

### Delegate execution & panel eligibility
Server-side CLI delegates run as the non-root aimee service user in a constrained
working dir with the same env-construction function as setup, a per-call timeout,
and an output cap. A new agent field **`is_server_hosted`** (+ `auth_status`,
`cli_executable_path`) distinguishes a server-hosted authenticated claude from the
client-only one; `ensemble_default_panel_from_agents` seats claude **only** when
`is_server_hosted && auth_status==ok` — so the #318 exclusion is *narrowed*, not
removed. A `401`/expired from a delegate marks the agent `needs_reauth` (suppress
repeat calls) and surfaces `aimee agent reauth <vendor>`.

### Authorization & audit
`agent.cli_install` / `cli_login` / register / reauth require operator
authorization (the existing server-principal/admin gate) and a **per-vendor
opt-in**: `claude_cli_delegate_enabled` (exists) and a new
`codex_cli_delegate_enabled`. Enabling either, and each setup/reauth/revoke, is
audit-logged (who/when, never the secret). Revocation = delete token dir + kill
session + vendor-side revoke.

## Out of scope
- Refreshing/rotating OAuth tokens automatically (the CLIs handle their own
  refresh; we surface re-auth instructions if a call 401s).
- Non-OAuth CLIs.

## Risks / open questions
- **Headless login UX per vendor — RESOLVED by spike (2026-06-15).** Confirmed in
  isolated HOME+tmux: `claude setup-token` prints a scrapable
  `claude.com/cai/oauth/authorize?…` URL with a **hosted** callback (no localhost)
  and a paste-back code; `codex login --device-auth` prints a scrapable
  `auth.openai.com/codex/device` URL + one-time code and polls. (Bare
  `codex login` uses a localhost:1455 callback — unusable headless — so the matrix
  pins `--device-auth`.) Both scrape cleanly from `tmux capture-pane -p`.
- **ToS / automation:** running the vendor CLIs headless on a server may bump
  their terms; gate behind explicit operator opt-in (as claude-cli delegate
  already is via `claude_cli_delegate_enabled`).
- **Image size / build time** from node+npm+tmux.
