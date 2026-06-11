# What's New

Each section covers one release. The CLI reads the section matching the
current version and prints it once after an upgrade.

---

## Unreleased (testing)

Thin-client hardening — the client machine owns the working tree and the
secrets; see [THIN_CLIENT.md](THIN_CLIENT.md).

- **Client-held agent credentials**: agent/delegate API keys now live on the
  client (`~/.config/aimee/agent-keys.json`), not on the server. `aimee agent
  add … --key K` against a remote server keeps `K` local and strips it before
  forwarding the definition. Keys are pushed once per session to a **RAM-only**
  keyring on the server (`POST /v1/session/credentials`) and are **never written
  to disk** — so a compromised server holds no durable secret store. Auth
  resolution prefers the client-pushed session key over any server-stored key.
- **Codex OAuth from the thin client**: add a Codex agent with no key
  (`aimee agent add codex https://chatgpt.com/backend-api/codex gpt-5.5
  --provider codex`); the client supplies the OAuth token from this machine's
  `~/.codex/auth.json` per session. Works as a primary provider and a delegate.
- **Workspaces ingested from the client**: `aimee workspace add <path>` resolves
  the path locally, registers it as `detached`, and pushes the files to the
  server (`POST /v1/index/ingest`, chunked under aimee-kb's body cap) — the
  server never reads the client filesystem. `aimee index scan [path]` re-pushes.
- **Claude runs on the thin client (standard `claude` CLI over tmux)**: a
  `--provider claude` agent runs the standard `claude` CLI in a tmux session — it
  needs the `claude` binary, tmux, its login, and the working tree, none of which
  exist on a remote/containerized `aimee-server`. When the active workspace is
  `detached` (a thin client is serving it), aimee now runs that tmux session **on
  the client** by marshalling its tmux commands over the existing runner reverse
  channel, against the client's tree with the client's `~/.claude` login. No
  Claude credential is sent to or stored on the server, and `claude -p` print
  mode is not used. Co-located deployments are unchanged. See
  [DELEGATES.md](DELEGATES.md).
- **Claude via the CLI is primary-only by default**: Claude run via the `claude`
  CLI / tmux login (authenticated by the Claude subscription login, not an API
  key) can be your interactive primary but is **not** usable as a delegate unless
  you opt in with `aimee config set claude_cli_delegate_enabled true`. Driving a
  personal Claude subscription as an automated delegate may violate Anthropic's
  terms and risk account action; enabling the flag prints that warning once. This
  gate is Claude-CLI-specific — all other agents (API-key/HTTP, and other CLI
  agents like the Codex CLI) are unaffected.
- **Attention guard inert by default**: recursive raw scans flow freely unless a
  positive `ingress_max_raw_scans` cap is configured; the destructive-file guard
  is unchanged. The Claude Code integration also re-points stale hook/MCP command
  paths to the installed binary on reinstall.
- **`AIMEE_API_REMOTE_WRITES` env** lets a containerized server set its TCP write
  posture (`off|data|full`) without a writable `aimee.yaml`.
- **Fixes**: remote `/v1/index/scan` (and `/v1/index/ingest`) no longer fail with
  "rpc produced no response" (synchronous handlers under the op-run worker);
  `aimee agent add … --provider codex` is accepted as an alias for the Codex
  adapter.

---

## v0.2.0

- **Docker-first deployment**: run `aimee-server` + `aimee-kb` as containers, either one
  combined image (`compose.combined.yaml`, recommended) or split
  (`compose.server.yaml`), and install only the thin client on each developer
  machine. See the README "Run in Docker" and Manual §27.1.
- **Cross-platform thin client**: the `aimee` CLI now drives a remote server over
  HTTP from Linux, macOS, and Windows. Point it with `--server` /
  `AIMEE_SERVER_URL` (+ `--server-token` / `AIMEE_SERVER_TOKEN`), or persist it
  with `aimee remote set`. Prebuilt thin-client binaries ship with each release;
  build just the client with `-DAIMEE_THIN_CLIENT=ON`.
- **KB is HTTP-only**: `aimee-server` reaches `aimee-kb` exclusively over its `/v1`
  HTTP API (`AIMEE_KB_API_URL`); the legacy Unix-socket KB transport and on-demand
  autostart were retired. Deploys must run `aimee-kb --http-port=8741` (or its
  service unit / container).
- **Self-update notifier**: aimee now tells you what changed after each upgrade
  and warns when the binary is older than the source tree.
- **Stale binary detection**: a one-line warning appears when the binary predates
  the latest commit. Run `make` to rebuild.
- **Lean build profile** (`make lean`): a size-optimised, stripped binary pair for
  minimal deployments, capped at 1.5 MB.
- **Autonomous mode**: drive work without per-step prompts via `aimee auto` (roadmap dispatch loop) and `aimee autopilot` (end-to-end pipeline).
- **Slash command registry**: extensible `/command` system in aimee chat.
- **Event notifications**: fired at delegation and verification boundaries.
