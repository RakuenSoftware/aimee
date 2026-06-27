# Compatibility Reference

This document summarizes the environments, integrations, and external interfaces supported by aimee.

Status terms used throughout this document:

- Tested: Confirmed through direct use on the listed platform, tool, or provider.
- Expected: Not verified as part of routine testing, but expected to work based on matching protocols, compatible toolchains, or equivalent runtime behavior.

## Primary Agent Support

These are the primary AI coding tools that aimee integrates with for memory injection, guardrails, and session management. Tools can also point their front end at aimee's wire-format ingress and run every turn on aimee's configured primary model (any provider), instead of the tool's built-in vendor; see [MANUAL §25](../MANUAL.md#25-integrations).

| Primary agent | Integration points | Run on any model (ingress) | Tested versions | Support level | Notes |
|---|---|---|---:|---|---|
| Claude Code | `SessionStart`, `PreToolUse`, `PostToolUse`, `SessionEnd` | Anthropic Messages, `POST /v1/messages` | 1.x | Full | Full hook coverage; `aimee claude-proxy enable` reroutes to any primary model. |
| Codex CLI | `PreToolUse`, `PostToolUse`, `SessionStart`, local plugin, MCP | OpenAI Responses, `POST /v1/responses` | 1.x | Full | Hooks + MCP; `~/.codex/config.toml` model provider runs any primary model, incl. the function-call tool loop. |
| OpenCode | TUI front end via `opencode attach` | OpenAI-compatible, `POST /v1/chat/completions` | 2.x | Full | Front end onto aimee's primary model over the OpenAI-compatible ingress. |
| Gemini CLI | `BeforeTool`, `AfterTool`, `SessionStart` | Provider CLI | 2.x | Full | Uses Gemini CLI hook model rather than Claude-style hook names. |
| Mistral Vibe-compatible Mistral | native HTTP adapter using Vibe-compatible defaults | Provider CLI | 2.9.6 source-compatible defaults | Expected | Used by Aimee's built-in primary chat and `mistral-plan` delegate route without launching `vibe`. |

## Platform Support

### Operating Systems

| Platform | Architecture | Build status | Runtime status | Notes |
|---|---|---|---|---|
| Debian 13 (trixie) | x86_64 | Tested | Tested | Primary development platform. |
| Ubuntu 22.04 and newer | x86_64 | Expected | Expected | Expected to work with the same general toolchain as Debian. |
| Proxmox VE 8.x | x86_64 | Tested | Tested | Deployed in production. |
| macOS 14 and newer | arm64 | Expected | Expected | Requires Homebrew-managed dependencies. |
| Windows via WSL2 | x86_64 | Expected | Expected | Recommended Windows runtime environment. |
| Native Windows (experimental) | x86_64 | Tested | Expected | CMake + MinGW builds and portable unit tests run in CI, but many runtime features are still POSIX-first. |

### Shell Environments

| Shell | Hook execution status | Notes |
|---|---|---|
| `bash` | Tested | Primary shell environment. |
| `zsh` | Expected | Common default shell for Claude Code on macOS. |
| `sh` (`dash`) | Expected | Minimal POSIX shell environment. |

## Build Dependencies

These dependencies are required to build or run major aimee components.

| Dependency | Minimum version | Debian package | Required for | Notes |
|---|---:|---|---|---|
| `gcc` | 10+ | `build-essential` | All binaries | Required C compiler toolchain. |
| `make` | 4.0+ | `build-essential` | Build system | Used for project build orchestration. |
| SQLite3 | 3.35+ with FTS5 | `libsqlite3-dev` | Server-local storage | FTS5 is required for local session/window indexes. |
| `libcurl` | 7.68+ | `libcurl4-openssl-dev` | Delegate agent HTTP, server | Required for outbound HTTP integrations. |
| OpenSSL | 1.1+ | `libssl-dev` | Webchat TLS | Provides TLS support. |
| PAM | System version | `libpam0g-dev` | Webchat authentication | Uses the host platform PAM implementation. |

### Server SQLite Requirement

FTS5 support is required by local server indexes. Most system SQLite packages include FTS5, but it should be verified on minimal or custom builds.

Example check:

```bash
sqlite3 :memory: "SELECT fts5();" 2>&1 | grep -q "wrong number" && echo "FTS5 available"
```

## Delegate Providers

These are the API providers that delegate agents can connect to for task offloading. Codex, Gemini, and Mistral can also use the same provider layer through primary-session adapters; other primary-agent integrations happen through hooks, MCP, or remaining provider CLI routes.

| Provider | API format | Authentication | Models tested | Notes |
|---|---|---|---|---|
| OpenAI | `/chat/completions` | Bearer token | `gpt-4o`, `gpt-4o-mini` | OpenAI-compatible chat completions support. |
| ChatGPT (Codex) | `/backend-api/codex/responses` | OAuth device flow | `gpt-5.4`, `gpt-5.4-mini` | Uses Codex-specific backend API format. |
| Anthropic | `/v1/messages` | `x-api-key` header | `claude-sonnet-4-6`, `claude-haiku-4-5` | Native Anthropic messages API support. |
| Google (Gemini) | `/v1beta/models/{model}:generateContent` or `/v1beta/openai` | API key header or bearer token | `gemini-2.5-flash`, `gemini-2.5-flash-lite` | Native `gemini-cli` compatibility routes use GenerateContent without launching the Gemini CLI. |
| Mistral AI | `/v1/chat/completions` | Bearer token | `mistral-large-latest`, `mistral-small-latest`, `mistral-vibe-cli-latest` | Direct HTTP provider profile; `mistral-plan` uses Vibe-compatible defaults without launching Vibe. |
| Ollama | `/v1/chat/completions` | None | `llama3.2`, any local model | Local provider with no built-in auth requirement. |
| Groq | `/openai/v1` | Bearer token | `llama-3.3-70b-versatile` | OpenAI-compatible API variant. |

Provider-CLI delegates can also route through installed CLIs. `aimee agent setup
codex-cli` creates the legacy Codex CLI route. `aimee agent setup gemini-cli`,
`mistral-cli`, and `mistral-plan` create provider-CLI-compatible entries that
bridge to native HTTP adapters instead of launching provider binaries.

## MCP Protocol

The MCP server (`aimee mcp-serve`) exposes aimee knowledge and actions to primary agents that support MCP. It is built into the `aimee` binary.

| MCP capability | Version or scope | Status | Details |
|---|---|---|---|
| JSON-RPC 2.0 | `2024-11-05` | Implemented | Core RPC protocol support. |
| `stdio` transport | Standard input/output | Implemented | Primary supported transport. |
| `tools/list` | Tool discovery | Implemented | Exposes `search_memory`, `list_facts`, `get_host`, `list_hosts`, `find_symbol`, `delegate`, `preview_blast_radius`, `record_attempt`, `list_attempts`, and `delegate_reply`. |
| `tools/call` | Tool invocation | Implemented | Full tool execution support. |
| `resources/*` | MCP resources | Implemented | Resource endpoints are available. |
| `prompts/*` | MCP prompts | Implemented | Prompt endpoints are available. |
| HTTP SSE transport | Server-sent events | Not implemented | No HTTP transport support at this time. |

## Client Registration

This section covers how the aimee MCP server is registered with supported clients.

`install.sh` and `configure-hooks.sh` automatically detect installed clients and register
`aimee mcp-serve` for each one found. Running the installer is idempotent: if a client is
already configured, the registration is refreshed and reported as such rather than duplicated.
In the recommended Docker deployment (services in containers, thin client per machine),
each developer runs `configure-hooks.sh` directly to register hooks + MCP locally; the
hooks call the thin client, which reaches the configured `aimee-server`.

| Client | Detection method | Hook config path | MCP config path | Registration status |
|---|---|---|---|---|
| Claude Code | `~/.claude/` directory or `claude` in PATH | `~/.claude/settings.json` | `~/.claude/settings.json` | Implemented |
| Gemini CLI | `~/.gemini/` directory or `gemini` in PATH | `~/.gemini/settings.json` | `~/.gemini/settings.json` | Implemented |
| Codex CLI | `~/.codex/` directory or `codex` in PATH | `~/.codex/hooks.json` | `~/.codex/mcp-config.json` | Implemented |
| GitHub Copilot | `~/.copilot/` directory or `copilot` in PATH | `~/.copilot/config.json` | `~/.copilot/mcp-config.json` | Implemented |
| Claude Desktop | Config directory exists at OS-specific path | N/A (MCP only) | `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS) or `~/.config/Claude/claude_desktop_config.json` (Linux) | Implemented |
| VS Code | `code` in PATH or user config dir (`~/.config/Code/User/`, macOS `~/Library/Application Support/Code/User/`); Insiders and VSCodium variants detected | N/A (no hooks) | User-level `mcp.json` (uses the `servers` key, not `mcpServers`) | Implemented (MCP); also an ACP agent (`aimee acp-serve`) and an OpenAI-compatible model, see [VSCODE.md](VSCODE.md) |

### Hook events registered per client

| Client | Session start | Pre-tool | Post-tool | Matched tools |
|---|---|---|---|---|
| Claude Code | `SessionStart` | `PreToolUse` | `PostToolUse` | Pre: `Edit\|Write\|MultiEdit\|Bash\|Read\|Glob\|Grep`; Post: `Edit\|Write\|MultiEdit` |
| Gemini CLI | `SessionStart` | `BeforeTool` | `AfterTool` | Pre: `write_file\|replace\|shell`; Post: `write_file\|replace` |
| Codex CLI | `SessionStart` | `PreToolUse` | `PostToolUse` | Pre: `Bash`; Post: `Bash` |
| GitHub Copilot | `SessionStart` | `PreToolUse` | `PostToolUse` | Pre: `Bash\|Edit\|Write`; Post: `Edit\|Write` |
| Claude Desktop | n/a | n/a | n/a | MCP only; no hook events |
| VS Code | n/a | n/a | n/a | MCP / ACP / OpenAI-compatible model; no hook events |

### Installer output

The installer reports per-client registration outcomes:

- **Configured:** client was detected and aimee was newly registered.
- **Refreshed (already configured):** client was detected and registration already existed; config was updated.
- **Not detected:** no config directory and no binary found for the client; skipped silently.

Re-running `install.sh` or `configure-hooks.sh` is safe at any time and will not create duplicate entries.
When `install.sh` runs without an interactive TTY (for example in CI), it keeps the
current provider or default selections and skips optional prompts that would otherwise block.

## Command Tier Stability

Commands are grouped into three tiers with different stability expectations:

| Tier | Examples | Shown in | Stability |
|---|---|---|---|
| Core | `memory`, `wm`, `rules`, `index`, `delegate` | `aimee --help` | Stable. Breaking changes require a deprecation cycle. |
| Advanced | `status`, `hud` | `aimee help --all` | Stable. Flags and subcommand names may evolve with notice. |
| Admin | `git verify`, `clean` | `aimee help --all` | Best-effort. Use in scripts at your own risk. |

**Visibility rules:**
- `aimee --help` (or `aimee` with no arguments followed by `--help`) shows only Core commands.
- `aimee --advanced` or `aimee help --all` shows Core, Advanced, and Admin commands grouped by tier.
- A command appears in client help only after it has either a local client implementation or a typed `aimee-server` RPC route.

**Adding new commands:** New top-level commands should first add a typed server route (or a strictly local client implementation), then opt into the help table at the appropriate tier.  This prevents the DB-free client surface from advertising server-internal or unported commands.

## Known Limitations

| Area | Limitation | Impact |
|---|---|---|
| Native Windows | Build coverage exists, but support is still incomplete. | WSL2 remains the recommended Windows runtime until more POSIX-only runtime paths are ported. |
| macOS packaging | Homebrew-managed dependencies are required. | Manual dependency setup is needed outside Debian-like environments. |
| Server SQLite builds | FTS5 must be present. | Local text indexes are unavailable without FTS5. |
| Claude Desktop hooks | Claude Desktop is MCP-only. | Hook-based session events are not available for Claude Desktop. |
| MCP transport | HTTP SSE transport is not implemented. | MCP support is limited to available implemented transports, primarily `stdio`. |
