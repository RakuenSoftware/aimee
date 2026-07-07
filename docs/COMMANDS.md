# Command Reference

> **Complete, always-current references** (generated from the source-of-truth
> tables, see [`gen/cli-commands.md`](gen/cli-commands.md) and
> [`gen/configuration.md`](gen/configuration.md)):
> - **[Full CLI command list](gen/cli-commands.md)**, every command + subcommands,
>   from `src/cli_help_data.h`.
> - **[Configuration reference](gen/configuration.md)**, every config key (the
>   `aimee config get/set` allowlist + the config-file JSON sections).
>
> This page is a hand-written walkthrough of the common commands; the generated
> pages above are the authoritative, exhaustive lists.

`aimee` is a thin CLI. It either handles a small local-only operation
or forwards a typed request to `aimee-server`. Commands that are not listed
here are not part of the client contract until they have a typed server route.

Server-backed commands support `--json` for machine-parseable output.

## Command Tiers

- **Core**: everyday commands shown in `aimee --help`.
- **Advanced**: power-user status commands shown by `aimee help --all`.
- **Admin**: operational helpers shown by `aimee help --all`.

Run `aimee help --all` to see all tiers at once, or
`aimee help <command>` for command-specific help.

## Core Commands

### Stored Memory

- `aimee memory search <query>`: search stored memory through the server.
- `aimee memory store <key> <value> [--tier T] [--kind K]`: store a memory entry.
- `aimee memory list [--tier T] [--kind K] [--limit N]`: list stored memory entries.
- `aimee memory get <id>`: read one memory by id.

### Working Memory

- `aimee wm set <key> <value> [--session ID] [--category C] [--ttl N]`: store a session-scoped scratch value.
- `aimee wm get <key> [--session ID]`: read a session-scoped scratch value.
- `aimee wm list [--session ID] [--category C]`: list session-scoped scratch values.

### Manuscript

- `aimee manuscript scenes`: list detected scene/chapter markers.
- `aimee manuscript wordcount`: count words in manuscript files.
- `aimee manuscript outline`: print a structural outline.
- `aimee manuscript check [files...]`: run long-form writing checks.

### Rules

- `aimee rules list`: list active rules through the server.
- `aimee rules generate`: generate the rules prompt through the server.
- `aimee rules delete <id>`: delete one rule through the server.

### Roadmap, Auto, and Review Implementation Modules

The source tree contains roadmap/auto/review implementation modules, but
`aimee roadmap`, `aimee auto`, and unified `aimee review` are not current
thin-client commands because they do not have typed server RPC routes. The
current routed planning path is:

- `aimee delegate plan <proposal.md> [--json] [--output PATH] [--launch] [--parallel N]`: generate read-only work packets from a proposal.
- `aimee delegate launch <plan.json> [--json] [--parallel N]`: queue a reviewed packet plan into a coordinated job.
- `aimee job status <id>`: inspect queued packet progress.

Commands that return "has no typed server RPC route" are implementation work in
progress, not part of the installed CLI contract.

### Code Index

- `aimee index find <identifier>`: find a symbol or identifier.
- `aimee index list`: list indexed projects.
- `aimee index overview`: alias for `index list`.
- `aimee index scan [--force]`: scan workspaces and rebuild the code index.
- `aimee index blast-radius <file>`: show files affected by changing a file.
- `aimee index structure <file>`: show file structure.
- `aimee index callers <symbol>`: find callers of a symbol.
- `aimee code audit [dir] [--json] [--fix]`: run local file-health checks. `--fix` is intentionally non-mutating for now and reports that no safe automatic fixes are available.
- `aimee code audit --graph [--project P] [--json]`: request graph-derived dead-export, import-cycle, exact-clone, and near-clone findings from `aimee-server`/`aimee-kb`. Thin clients need a configured remote and an indexed project.

### Chat

- `aimee` or `aimee chat`: start the primary-agent chat session.

Bare `aimee` launches the OpenCode v2 TUI through `opencode attach`.
The server still owns provider routing, primary session IDs, memory, guardrails,
and tool execution.

Direct Codex and Mistral primary sessions use server-side structured
conversation state; explicit legacy routes such as `codex-cli` still use the
provider CLI path. Claude delegates run the installed `claude` CLI in tmux,
while `gemini-cli`, `mistral-cli`, and `mistral-plan` bridge to native HTTP
adapters behind provider-CLI-compatible config.

### Delegation

- `aimee delegate <role> <prompt>`: delegate a task through `aimee-server`.
- `aimee delegate plan <proposal.md> [--output PATH] [--launch]`: generate reviewed work packets.
- `aimee delegate launch <plan.json> [--parallel N]`: queue a packet plan into a coordinated job.
- `aimee delegate aggregate "<task>"`: run one Mixture-of-Agents fan-out and synthesis over `ensemble.reference_models`.
- `aimee delegate roundtable "<task>" [--mode draft|review] [--turns parallel|sequential] [--rounds N] [--brief TEXT] [--brief-json JSON] [--apply]`: run a bounded multi-round collaborative draft or review.
- `aimee delegate status <job_id> [job_id...]`: inspect background delegate status.
- `aimee delegate log` / `aimee delegate history`: show delegation episodes.
- `aimee delegate --list-roles`: list configured delegate roles.

Useful flags:

- `--tools`: allow tool-use mode for the delegate.
- `--prompt-file PATH`: read the prompt body from a file.
- `--max-tokens N`: pass a token budget hint.

### Skills and Toolsets

- `aimee skill list`: list available skills.
- `aimee skill show <name>`: print a skill body or support file.
- `aimee skill lint`, `skill eval`: validate or evaluate skills.
- `aimee skill create`, `edit`, `patch`, `archive`, `pin`, `unpin`, `lifecycle`, `autostub`: mutate skill state through routed RPCs.
- `aimee toolset list`, `toolset show <name>`, `toolset resolve <name>`: inspect composable toolsets.

### MCP Registry and Insights

- `aimee mcp audit`: list registered MCP servers and last OSV verdict.
- `aimee mcp recheck [name]`: force a fresh OSV query.
- `aimee insights [--days N]`: show token usage totals over the last N days.

### Knowledge Base

- `aimee kb search <query>`: search the KB through `aimee-server` and `aimee-kb`.
- `aimee kb status`: show KB/vector health.
- `aimee kb build [--path DIR] [--project NAME] [--force]`: build an index from a tree.
- `aimee kb update`: run an incremental KB update.
- `aimee kb docs push [--scope SCOPE] <file>...`: stage docs for ingest.
- `aimee kb ingest <file>` and `aimee kb ingest status`: queue and inspect ingest work.

## Advanced Commands

- `aimee session list [--limit N]`: list recent sessions through the server.
- `aimee session show <session-id>`: show one session through the server.
- `aimee session close <session-id>`: close one session through the server.
- `aimee session brief [--limit-tokens N]`: show the persisted session-start brief.
- `aimee status`: show `aimee-server` health. It requires an already-running
  server; use `systemctl --user start aimee-server` on systemd systems or
  `aimee server start` as the unmanaged fallback.
- `aimee hud`: show current session telemetry.
- `aimee workers`: show server/KB worker-pool state.
- `aimee server start`: explicitly spawn a server in unmanaged environments.
- `aimee server restart`: terminate the running server and spawn a fresh one.
- `aimee server status` / `server health`: aliases for server health.
- `aimee workspace add <path>`, `workspace list`, `workspace remove <path>`: manage indexed workspace roots.
- `aimee remote set <url> [token]`, `remote enroll`, `remote trust`, `remote status`,
  `remote clear`: point the thin client at a remote `aimee-server` over TCP. `set`
  persists the target to `<aimee_home>/remote.conf` and, for `https://`, pins the
  server's self-signed cert (trust-on-first-use); `status` shows the resolved
  transport plus a `GET /v1/health` probe; `trust` re-pins after a cert rotation;
  `clear` reverts to the local Unix socket. Precedence: `--server`/`--server-token=`
  flags > `AIMEE_SERVER_URL`/`AIMEE_SERVER_TOKEN` env > persisted `remote.conf`.
  `https://` is supported on Linux/macOS (OpenSSL, cert-verified;
  `AIMEE_TLS_INSECURE=1` to skip); Windows builds refuse it.
  - **Bootstrap enrollment:** the aimee-server image seeds a well-known one-time
    bearer `aimee-local-dev`. Connecting with it (`aimee remote set <url>
    aimee-local-dev`) auto-runs enrollment: the server mints a strong random
    per-deployment bearer (`api.rotate_bearer`), the client adopts it in
    `remote.conf`, and the bootstrap token immediately stops working, so the
    shared default is never a standing credential. `aimee remote enroll` forces a
    fresh rotation on the configured remote at any time. To skip the bootstrap
    entirely, set `AIMEE_API_BEARER_TOKEN` on the server (secret store) to pin your
    own bearer; an explicit env token disables the auto-rotation.
- `aimee worktree gc [--days N] [--force] [--dry-run]`: garbage-collect abandoned session worktrees.
- `aimee work add`, `add-batch`, `claim`, `complete`, `fail`, `list`, `board`, `cancel`, `release`, `clear`, `gc`, `sync-proposals`, `stats`: manage the inter-session work queue.
- `aimee jobs list [--limit N]`: list recent durable delegate jobs.
- `aimee jobs status <job-id>`: show one durable delegate job.
- `aimee jobs logs <job-id>`: print the recorded result/log body for one durable delegate job.
- `aimee jobs cancel <job-id>`: cancel a queued or running durable delegate job.
- `aimee job start <plan-id>`, `job list`, `job status <id>`, `job cancel <id>`: manage coordinated packet-plan jobs.
- `aimee delegate-backend list`, `delegate-backend exec ...`: inspect or drive local/SSH/docker delegate backends.
- `aimee agent list`, `add`, `local`, `remove`, `enable`, `disable`, `probe`, `setup`, `episodes`, `token`: manage delegate agents and credentials.
- `aimee provider list [--available] [--json]`: list registered model providers and credential availability.
- `aimee provider show <name>`: show one provider profile.
- `aimee provider models <name> [--json]`: fetch a provider model catalog.
- `aimee provider test <name>`: probe provider credentials and model-listing connectivity.
- `aimee provider quota [name]`: inspect credential-pool quota state.
- `aimee provider <name>` or `aimee use <name>`: switch the active provider profile.
- `aimee model list`, `model show <model>`, `model refresh`: inspect model capability metadata.
- `aimee graph sync-code`, `graph explain`: run code graph sync/explanation helpers.
- `aimee trajectory export`, `trajectory batch`: export or batch-generate trajectory data.
- `aimee aux`, `aux config show`, `aux test`: inspect or test auxiliary-model routing.
- `aimee optimize run --suite <suite> [--arm <arm>]`: run the `memory.benchmark` RPC for retrieval suites. Synchronous suites are `code-graph-fusion`, `memory`, `corpus`, `memory-retrieval`, and `live`; dataset and judge-style suites return an `async-only` pointer to the CLI/delegate benchmark path. Registered decision points include `briefing_style` and `guardrail_strictness`; these static points are promotion/replay driven while online exploration remains default-off via `intelligence.bandit.live_decision_enabled`.
- `aimee identity show`, `snapshot`, `diff`: inspect charter/local-operator/working-profile state.
- `aimee dogfood tag`, `review`, `report`: label and report dogfood review data.
- `aimee eval run <suite_dir>`, `eval results [suite]`: run or inspect eval suites.
- `aimee trigger fire`, `list`, `status`, `cancel`: manage event-triggered runs.
- `aimee cron list`, `add`, `show`, `history`, `run`, `enable`, `disable`, `remove`: manage server-side cron/watchdog jobs.
- `aimee profile create`, `list`, `show`, `delete`, `current`: manage profile-specific config roots.

The hidden `hooks` command is used by configured editor/agent integrations:

- `aimee hooks pre`
- `aimee hooks post`

## Admin Commands

- `aimee git verify`: verify current changes before merge.
- `aimee clean [--force]`: remove local aimee configuration and client integrations.

## Webchat

- `aimee-webchat --port 8080`: ask `aimee-server` to host the browser webchat surface.

`aimee-webchat` is also a thin client. It does not read storage directly.

## MCP Server

The MCP bridge runs as `aimee mcp-serve`. It is built into
`aimee`, speaks stdio JSON-RPC 2.0 to MCP-compatible clients, and
forwards tool calls to `aimee-server`.

Tools exposed:

- `search_memory`: server-backed retrieval over stored facts.
- `list_facts`: list stored facts.
- `get_host`: look up a host by name.
- `list_hosts`: list all hosts and networks.
- `find_symbol`: search code index for symbol locations.
- `delegate`: delegate a task to a sub-agent.
- `preview_blast_radius`: show file dependency impact.
- `record_attempt`: log a delegation attempt.
- `list_attempts`: list recent delegation history.
- `delegate_reply`: follow up on a prior delegation.
