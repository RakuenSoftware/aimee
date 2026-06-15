# Proposal: Server-hosted OAuth CLI agents (claude / codex)

- **State:** draft — design locked with the user 2026-06-15 (no PTY forwarding;
  bake enablers + install CLIs on-demand)
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

## Out of scope
- Refreshing/rotating OAuth tokens automatically (the CLIs handle their own
  refresh; we surface re-auth instructions if a call 401s).
- Non-OAuth CLIs.

## Risks / open questions
- **Headless login UX per vendor:** the claude CLI and codex CLI each have their
  own `login` UX; we must confirm each prints a scrapable URL+code in a
  non-interactive tmux pane (vs. requiring a localhost browser redirect). If a
  vendor only supports a localhost-callback flow, we surface a tunnelled URL or
  fall back to a token-paste step.
- **ToS / automation:** running the vendor CLIs headless on a server may bump
  their terms; gate behind explicit operator opt-in (as claude-cli delegate
  already is via `claude_cli_delegate_enabled`).
- **Image size / build time** from node+npm+tmux.
