# aimee: Technical Reference

Architecture, internals, and build instructions. For usage and getting started,
see the [root README](../README.md) and the full [Manual](../MANUAL.md). For the
system-level architecture (process topology, trust/storage boundaries, request
lifecycles) see [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md). This document is
the code-level companion: module map, server internals, the RPC and HTTP
surfaces, the KB split, the build system, and testing.

aimee's core services are written in C11 for performance and minimal footprint. The current tree is a large C codebase (hundreds of `.c` files, plus generated headers and split `*.inc` units), shipped with the standalone Go `aimee-webchat` service plus `aimee`, `aimee-server`, and `aimee-kb`. Runtime DB ownership is pinned: clients are DB-free, the local server owns DB1 (sqlite), and KB owns DB2 (postgres + pgvector), which can be local or shared by deployment. MCP support is built into `aimee mcp-serve`.

## Architecture overview

aimee supports two agent roles. The **primary agent** is the AI coding surface the user interacts with (Claude Code, Gemini CLI, direct Codex, direct Mistral, Codex CLI legacy mode, Mistral Vibe, GitHub Copilot). aimee integrates through hooks, provider-CLI adapters, or direct primary-session adapters, intercepting tool calls, injecting memory and rules, enforcing guardrails, and managing session isolation. **Delegate agents** are sub-agents configured in `agents.json` that handle offloaded work via `aimee delegate <role> <prompt>`.

```mermaid
graph TB
    subgraph Primary["Primary Agent Integration"]
        PA[Claude Code / Gemini / Codex / Vibe / Copilot]
        SS[SessionStart<br/>Load rules, assemble context,<br/>create worktrees]
        Pre[PreToolUse<br/>Classify path, check plan mode,<br/>worktree enforce, anti-patterns]
        Post[PostToolUse<br/>Re-index edited files]
    end

    subgraph Core["aimee Core"]
        Mem[(DB2 Knowledge<br/>local/shared KB scope<br/>memories, rules, tasks)]
        Vec[(pgvector<br/>memory + KB indexes)]
        Guard[Guardrails<br/>Path classification,<br/>sensitive file blocking]
        Idx[Code Index<br/>Symbol lookup,<br/>blast radius]
        Rules[Rules Engine<br/>Feedback-derived rules]
        WM[DB1 Local State<br/>sessions, windows, caches]
    end

    subgraph Delegate["Delegate Agent System"]
        Router[Agent Router<br/>Cost-based selection]
        Loop[Tool-use Loop<br/>bash, read, write]
        HTTP[HTTP Client<br/>OpenAI / ChatGPT / Anthropic]
    end

    subgraph Server["Server"]
        Srv[aimee-server<br/>HTTP /v1<br/>UDS + optional TCP]
        MCP[aimee mcp-serve<br/>Stdio MCP server]
    end

    PA --> SS
    PA --> Pre
    PA --> Post
    SS --> Mem
    SS --> Rules
    Pre --> Guard
    Pre --> Idx
    Post --> Idx
    Mem --> Vec
    Router --> HTTP
    Router --> Loop
    Srv --> Core
    MCP --> Core
```

### Knowledge flow

```mermaid
flowchart LR
    M[DB2 knowledge<br/>memories + rules<br/>local/shared KB scope] -->|session-start| CTX[Context Assembly<br/>facts + rules + network]
    V[pgvector] -->|recall| CTX
    AJ[agents.json] -->|delegate context| DCTX[Delegate Context<br/>hosts, SSH, networks]
    AL[agent_log] -->|auto-promote| DP[Delegation Patterns<br/>L2 facts]
    MCP2[aimee mcp-serve] -->|tools| TOOLS[search, hosts,<br/>symbols, delegate]
    PD[project descriptions] -->|inject| CTX
    PD -->|inject| DCTX
```

## Module map

### Shared runtime (linked into owning binaries only)

These modules are linked only where their ownership boundary allows them. The
thin `aimee` and `aimee-webchat` targets do not link database,
`kb_client`, command, data, or agent objects.

| File | Responsibility |
|------|----------------|
| `config.c` | App config, `session_id()`, atomic writes |
| `util.c` | Core utilities, option parsing, security helpers |
| `text.c` | Text similarity, stemming, tokenization, search |
| `render.c` | JSON output, struct-to-JSON converters |
| `log.c` | Structured logging with level control |
| `platform_*.c` | Platform abstractions (events, IPC, random, process) |
| `client_integrations.c` | AI tool detection and hook registration |
| `mcp_tools.c` | MCP tool definitions and dispatch |
| `cJSON.c` | Vendored JSON parser |

### KB-owned data layer (linked into `aimee-kb`)

| File | Responsibility |
|------|----------------|
| `db2/db2.h` | DB2 project/workspace/global knowledge API |
| `db2/vector_status.h` | pgvector status and repair API |
| `memory_core.c` | High-level memory orchestration over DB2 + pgvector |
| `memory_logic.c` | Promotion, demotion, expiry, conflict detection, delegation pattern synthesis |
| `memory_assemble.c` | Context assembly, cache, compaction |
| `memory_scan.c` | Conversation scanning, JSONL parsing |
| `db2/memory_entity_graph.c` | Entity-relationship graph queries behind DB2 |
| `memory_advanced.c` | Anti-patterns, style learning, compaction |
| `cmd_index.c` | Code indexing CLI through aimee-kb |
| `extractors.c` | Source parsing, definition extraction (JS/TS/Python/Go) |
| `extractors_extra.c` | Language extractors (C#/Shell/CSS/Dart/C/C++/Lua) |
| `db2/rules.c` | Rule storage and tier classification behind DB2 |
| `db2/feedback.c` | Feedback recording and reinforcement behind DB2 |
| `guardrails.c` | Path classification, pre-tool safety checks, worktree enforcement |
| `workspace.c` | Workspace manifest parsing, provisioning, context generation |
| `db1/wm.c` | Session-scoped key-value store with TTL |
| `db2/tasks.c` | Project/workspace task graph |
| `db1/checkpoints.c` | Local checkpoints |

### Agent layer (directly linked into `aimee-server`)

| File | Responsibility |
|------|----------------|
| `server/agent_runtime.c`, `server/agent_loop.c` | Core delegate runtime and tool-use loop |
| `server/agent_policy.c` | Tool validation, policy, trace, metrics, env, manifest, contract |
| `server/delegate_prompt.c`, `server/delegate_routing.c` | Delegate prompt/context shaping and route selection |
| `server/agent_config.c` | Delegate config loading, routing, auth resolution |
| `server/agent_tools.c` | Tool execution (bash, read, write), checkpoints |
| `server/delegate_plan.c` | Delegate packet planning |
| `agent_eval.c` | Eval harness, task suites |
| `agent_coord.c` | Multi-delegate coordination, voting, directives |
| `server/server_compute*.c`, `server/server_jobs_aux.c` | Durable/background delegate jobs and compute dispatch |
| `server/delegate_openai.c`, `server/http_retry.c` | Provider HTTP shaping, retry, fallback |

### Server-side and legacy command modules (not linked into clients)

| File | Responsibility |
|------|----------------|
| `cmd_core.c`, `cmd_init.c`, `cmd_infra.c` | Core setup, lifecycle, and infrastructure helpers |
| `cmd_memory*.c` | Memory subsystem handlers and legacy/maintenance surfaces |
| `cmd_index.c`, `cmd_graph.c`, `cmd_kb*.c` | Code index, graph, and KB command handlers |
| `cmd_rules.c`, `cmd_review.c` | Rules and review-surface helpers |
| `cmd_hooks.c` | Primary agent integration: hooks, session-start context, wrapup |
| `cmd_roadmap.c`, `cmd_auto.c`, `cmd_autopilot.c` | Roadmap/autonomous implementation modules; not all are routed in the thin CLI |
| `cmd_skill.c`, `cmd_job.c`, `cmd_trigger.c` | Skill, coordinated-job, and automation helpers |
| `cmd_util.c` | Shared helpers: db open/close, subcommand dispatch |
| `dashboard.c` | Embedded HTTP dashboard server |

### Server binaries

| File | Responsibility |
|------|----------------|
| `cli_main.c` | Thin client entry point and local command fallbacks |
| `posix/cli_client.c`, `windows/cli_client.c` | Socket client libraries and explicit server start/restart fallback |
| `cli_mcp_serve.c` | MCP server entry point |
| `server/server.c` | dispatch table + server_dispatch (shared by the /v1 surface) |
| `server/server_main.c` | aimee-server entry point, signal handling |
| `server/server_auth.c` | Authentication (peer credentials, capability tokens) |
| `server_session.c` | Session management |
| `server_state.c` | Handlers: memory, index, rules, working memory, dashboard |
| `server_compute.c` | Async compute pool for delegate execution and tool calls |
| `compute_pool.c` | Thread pool |

### Header policy

`aimee.h` includes shared types and non-agent subsystem headers. Agent headers are NOT in `aimee.h`. Each `.c` file includes only the narrow agent headers it needs (`agent_types.h`, `agent_config.h`, `agent_exec.h`, etc.).

### Command pattern

Commands are registered in `main.c` as a `command_t` table. All handlers take `(app_ctx_t *ctx, int argc, char **argv)`. Complex commands use `subcmd_t` subtables with `subcmd_dispatch()`.

### Module decomposition roadmap

The largest source files are tracked for decomposition in waves (split the biggest modules first while preserving their public APIs).

**Current hotspots (>1000 lines, approximate):**

| File | Lines | Target wave |
|------|-------|-------------|
| `server/server_mcp.c` | ~1999 | Wave 3, split remaining MCP tool families from dispatch |
| `server/agent_tools.c` | ~1825 | Wave 2, split schema generation from tool execution |
| `server/kb_client.c` | ~1781 | Wave 4, split remaining KB client families |
| `server/kb_client_memory.c` | ~1457 | Wave 4, split remaining memory RPC wrappers |
| `memory_advanced.c` | ~1079 |, (single responsibility, acceptable size) |
| `posix/cli_client.c` | ~1073 |, (single responsibility, acceptable size) |

**Wave sequence:** Wave 1 (abstractions) → Wave 2 (tools/hooks) → Wave 3 (service/UI) → Wave 4 (data layer). Each wave is an independent PR.

## Memory system

```mermaid
graph TD
    L0[L0: Session Memory<br/>Scratch, in-progress state<br/>Lifetime: session only]
    L1[L1: Recent Memory<br/>Decisions, context, checkpoints<br/>Lifetime: 30 days]
    L2[L2: Long-term Memory<br/>Stable facts, preferences, patterns<br/>Lifetime: persistent]
    L3[L3: Episodic Memory<br/>Past attempts, outcomes, failures<br/>Lifetime: persistent with decay]

    L0 -->|accordion fold<br/>at session end| L1
    L1 -->|3 reuses or<br/>confidence >= 0.9| L2
    L2 -->|unused 60 days +<br/>confidence < 0.7| L1
    L3 -.->|auto from<br/>session end| L3

    AL[agent_log] -->|delegation patterns<br/>auto-synthesized| L2
```

### Memory kinds

| Kind | Description | Example |
|------|-------------|---------|
| `fact` | Verified information | "PostgreSQL runs on port 5432" |
| `preference` | User style/behavior | "User prefers concise responses" |
| `decision` | Choice with rationale | "Chose JWT over sessions: stateless" |
| `episode` | Past attempt narrative | "Session: refactored auth module" |
| `task` | Work item or goal | "Implement user authentication" |
| `scratch` | Temporary working data | Current task state |

### Deduplication pipeline

New memories pass through canonicalization before storage to prevent duplicates:

```mermaid
flowchart TD
    Input[New memory input] --> Norm[1. Normalize<br/>lowercase, strip fillers,<br/>collapse whitespace]
    Norm --> Exact{2. Exact key match<br/>through DB2 memory API?}
    Exact -->|yes| Merge1[Merge: keep higher confidence,<br/>increment use_count]
    Exact -->|no| Trigram{3. Trigram similarity >= 0.7<br/>vs top 100 same-kind?}
    Trigram -->|yes| Merge2[Merge into existing]
    Trigram -->|no| Insert[4. Insert new memory<br/>with tier, kind, confidence,<br/>provenance]
```

### Contradiction detection

When memories conflict, aimee detects the contradiction via negation asymmetry analysis and word similarity. Both conflicting memories get their confidence reduced (`*= 0.7`), with the conflict recorded in `memory_conflicts`.

### Temporal fact versioning

Facts change over time. The KB-side memory pipeline can supersede a fact: the old version gets a `#vN` suffix and `valid_until` timestamp, and the new version takes the canonical key with `valid_from`. Thin clients see this through routed memory responses rather than a direct storage command.

## Search

### Fact search

Fact search uses the high-level memory pipeline: DB2 supplies lexical and structured candidates, pgvector (inside DB2) supplies dense vector candidates, and the caller sees a ranked result set without depending on either path's query primitives.

### Window search

Conversation history search combines four scoring signals:

```mermaid
flowchart TD
    Q[Query] --> TM[Term Match<br/>exact lowercase match<br/>through DB1 API]
    Q --> LM[DB1 Lexical Match<br/>summary-token recall<br/>through DB1 API]
    Q --> GB[Graph Boost<br/>2-hop entity edges<br/>decay by 1/hop]

    TM --> Combine[Combine scores]
    LM --> Combine
    GB --> Combine

    Combine --> TD2[Apply time decay<br/>1 / 1 + days * 0.02]
    TD2 --> TW[Apply tier weight<br/>raw:1.0 summary:0.7 fact:0.4]
    TW --> Final[Final score<br/>sorted, top N]
```

## Guardrails

Before every tool call from the primary agent or delegate agents, aimee classifies the target path:

```mermaid
flowchart TD
    TC[Tool call: Edit/Write/Bash] --> Classify[Classify target path]
    Classify --> Sensitive{Sensitive file?<br/>.env, credentials, keys}
    Sensitive -->|yes| Block1[BLOCK exit 2]
    Sensitive -->|no| WTCheck{Real workspace path<br/>has worktree?}
    WTCheck -->|yes| Block2[BLOCK + redirect<br/>to worktree path]
    WTCheck -->|no| PlanMode{Planning mode<br/>active?}
    PlanMode -->|yes, write op| Block3[BLOCK writes]
    PlanMode -->|no| AntiPat[Check anti-patterns<br/>WARN on stderr]
    AntiPat --> Drift[Check drift detection<br/>WARN if off-scope]
    Drift --> Allow[ALLOW exit 0]
```

### Guardrail modes

| Mode | Yellow/Amber | Red/Block |
|------|-------------|-----------|
| `approve` (default) | Silently allow | Block + inform |
| `prompt` | Block + inform | Block + inform |
| `deny` | Silently block | Silently block |

### Planning mode

When active, blocks all write operations (Edit, Write, MultiEdit, destructive commands). Read-only operations always allowed.

## Anti-pattern detection

aimee maintains a database of known bad patterns extracted from negative feedback rules (weight >= 50), failed decisions, and manual additions. Before every tool call, the pre-hook checks for matches and emits warnings on stderr (non-blocking).

## Drift detection

When a task is active (`state = "in_progress"`), aimee monitors whether tool calls stay within the task's scope. Scope is determined from key terms in the task title, referenced projects, and subtask titles. Off-scope edits trigger warnings on stderr.

## Task graph

```mermaid
graph LR
    A[Design auth API<br/>done] -->|depends_on| B[Implement auth<br/>in_progress]
    B -->|depends_on| C[Write unit tests<br/>todo]
```

Edge types: `depends_on`, `supersedes`, `decided_by`, `evidence_for`, `failed_because`, `blocks`. Decisions are logged with rationale and assumptions.

## Context assembly

aimee assembles different context for the primary agent and delegate agents. By injecting pre-assembled facts, rules, project descriptions, and network info at session start, agents avoid spending tokens on re-discovery.

### Primary agent context (32KB buffer)

Assembled by `build_session_context()` in `cmd_session_lifecycle.c`, printed to stdout at session start.
The default brief is scoped to the hook client's current working directory so it stays focused on
the active project. Set `AIMEE_SESSION_START_VERBOSE=1` to include broad diagnostic sections.

| Section | Source | Content |
|---------|--------|---------|
| Code Principles | `prompt_code_principles_text()` | Engineering rules that should apply everywhere |
| Aimee Context | hardcoded tips | Short reminders for index, memory, delegation, and brief inspection |
| Rules | `db2_rules_generate()` | Behavioral rules from feedback |
| Project Context | DB2 memory scope APIs | Project-scoped facts |
| Delegation | agent config | Delegate instructions + examples |
| Key Facts | `memory_list(L2, fact)` | Verbose only: top global facts |
| Network | `agents.json` | Verbose only: SSH entry, hosts, networks |
| Shared Context | DB2 memory scope APIs | Verbose only: global/shared memories |
| Recent Delegations | `agent_log` (last 5) | Verbose only: delegate success/failure history |
| Project Descriptions | `workspace_build_context()` | Verbose only: all indexed workspace descriptions |
| Capabilities | `build_capabilities_text()` | Verbose only: available aimee commands |

### Delegate agent context (16KB budget)

Assembled by `agent_build_exec_context()` / `agent_build_exec_context_ex()` in
the platform agent runtime, sent as system prompt:

| Section | Source | Content |
|---------|--------|---------|
| Custom Prompt | caller-provided | Task-specific instructions |
| Rules | `db2_rules_generate()` | Behavioral rules |
| Relevant Memory | `memory_find_facts()` | Top matching facts |
| Code Index | aimee-kb index RPCs | Relevant symbols |
| Network Access | `agents.json` | All hosts, SSH entry, networks |
| Working Memory | `wm_assemble_context()` | Session key-values |
| Active Tasks | `db2_task_list()` | In-progress work |
| Recent Failures | `agent_log` (5min window) | Last 3 failures |

## MCP server

The `aimee mcp-serve` command exposes knowledge as JSON-RPC 2.0 tools over stdio for MCP-compatible primary agents:

| Tool | Source | Returns |
|------|--------|---------|
| `search_memory` | `memory_find_facts()` (DB2 lexical + pgvector dense recall + reranking) | Matching facts by keyword and phrase |
| `list_facts` | `memory_list(L2, fact)` | All stored L2 facts |
| `memory_briefing` / `memory_recall` | KB memory briefing/recall RPCs | Structured task/session memory bundles |
| `get_host` | `agents.json` | Single host by name |
| `list_hosts` | `agents.json` | All hosts + networks |
| `find_symbol` | DB2 code-index API / aimee-kb RPC | Code symbol locations |
| `ast_grep_search` | bundled `sg` binary | Structural code-search matches |
| `preview_blast_radius` | DB2 code-index API / aimee-kb RPC | File dependency impact |
| `learning_review` / `session_search` / `autopilot` | server-side tools | Learning review, local session search, and internal autopilot actions |
| `ast_grep_search` | `sg --json` | Structural AST pattern matches |
| `delegate` | `popen("aimee delegate")` | Delegation result |
| `preview_blast_radius` | `index_blast_radius()` | File dependency impact |
| `record_attempt` | `agent_log` | Log a delegation attempt |
| `list_attempts` | `agent_log` | Recent delegation history |
| `delegate_reply` | `agent_log` | Follow-up on prior delegation |

Codex CLI legacy support is layered on top of the same `aimee mcp-serve`
command. When `~/.codex` is present, `install.sh` and `update.sh` refresh a
local marketplace entry, mirror the plugin payload, and write an activation
entry in `~/.codex/config.toml`. Direct Codex primary sessions use the
server-side primary session adapter instead of the CLI.

## Interaction style learning

aimee analyzes negative feedback for recurring patterns:

| Keywords detected 2+ times | Auto-generated preference |
|----------------------------|--------------------------|
| "verbose", "too long", "wordy" | "User prefers concise responses" |
| "too short", "more detail" | "User prefers detailed explanations" |
| "format", "structure" | "User wants consistent formatting" |

Learned preferences are stored as L2 memories and injected into both primary and delegate agent contexts.

## Delegate agent system

```mermaid
flowchart TD
    PA[Primary Agent] -->|aimee delegate role prompt| CLI[aimee CLI]
    CLI --> Route[agent_route<br/>find cheapest for role]
    Route --> D1[Ollama<br/>tier 0, free]
    Route --> D2[Codex / ChatGPT<br/>tier 0, subscription]
    Route --> D3[OpenAI-compatible<br/>tier N]

    D2 -->|HTTP 400?| FB[Retry with<br/>fallback_model]
    D1 -->|failure?| Chain[Next in<br/>fallback chain]
    D3 -->|--retry N?| Retry[Auto-retry<br/>transient failures]
```

### Provider types

| Provider | Endpoint | Request format |
|----------|----------|---------------|
| `openai` (default) | `/v1/chat/completions` | `messages` array, `max_tokens` |
| `chatgpt` | `/backend-api/codex/responses` | `input` array, `instructions` |
| `anthropic` | `/v1/messages` | `messages` array, `system` (top-level) |

### Authentication

| Auth type | Config | Use case |
|-----------|--------|----------|
| `none` | `"auth_type": "none"` | Local Ollama |
| `bearer` | `"api_key": "$OPENAI_API_KEY"` | OpenAI, Together, Groq |
| `oauth` | provider-managed token file or helper | Codex/Gemini subscription |
| `x-api-key` | `"auth_cmd": "cat ~/.config/aimee/claude.key"` | Anthropic Claude |

### Delegate roles

| Role | Description |
|------|-------------|
| `code` | Write or edit code |
| `review` | Analyze code/plans |
| `explain` | Explain concepts |
| `refactor` | Restructure code |
| `draft` | Generate content |
| `execute` | Run agentic tasks (multi-turn tool-use) |
| `summarize` | Compress text |
| `format` | Reformat data |
| `search` | Find information |
| `reason` | Complex reasoning |

### Delegate options

| Flag | Description |
|------|-------------|
| `--tools` | Enable tool-use mode (bash, read_file, write_file) |
| `--retry N` | Auto-retry on transient failures |
| `--verify CMD` | Run CMD after delegation; exit 3 on failure |
| `--context-dir DIR` | Bundle directory into prompt |
| `--prompt-file PATH` | Read prompt from file |
| `--files F` | Pre-load comma-separated file contents |
| `--background` | Create a server-owned job and return job_id immediately |
| `--timeout N` | Override per-call timeout (ms) |

### Cross-verification

| Direction | Trigger | Action |
|-----------|---------|--------|
| Verify delegate output | Automatic after `aimee delegate` | Runs `verify_cmd` (build/test) |
| Delegate reviews supervisor diff | `aimee verify` | Sends diff to delegate for review |

## Database schema

```
Tier-owned schemas:

DB1: local sessions, windows, checkpoints, caches, delegate jobs, eval
     results, local secrets, local interaction/learning signals, and
     same-user runtime state owned by the local aimee-server.
DB2: memories, rules, KB metadata, tasks, decisions, code index metadata,
     learning records, project/workspace/global facts, and the pgvector
     collections + payload indexes derived from those records. DB2 belongs to
     aimee-kb and can be local or shared; local evidence is reflected or
     promoted there only when it is appropriate for the configured KB scope.
```

## Server architecture

```mermaid
graph TD
    subgraph Binaries
        Client["aimee<br/>thin CLI + MCP serve"]
        Webchat["aimee-webchat<br/>DB-free browser client"]
        Server["aimee-server<br/>DB1 + compute pool"]
        KB["aimee-kb<br/>DB2 + pgvector<br/>local/shared KB"]
    end

    subgraph Libraries
        Core["Core runtime objects<br/>db, config, util, text,<br/>render, cJSON, platform,<br/>MCP git handlers"]
        Data["Server-linked data layer<br/>memory, index, rules, tasks,<br/>guardrails, workspace"]
        Agent["Server-linked agent layer<br/>delegate loop, tools, HTTP,<br/>policy, plan, eval"]
        Cmd["Server-linked RPC handlers<br/>session, compute, MCP,<br/>dashboard"]
    end

    Client -.->|socket| Server
    Webchat -.->|socket| Server
    Client -->|mcp-serve| Core
    Server --> Core
    Server --> Data
    Server --> Agent
    Server --> Cmd
    Server -.->|typed KB /v1 HTTP| KB
    Core --> Data
    Data --> Agent
    Agent --> Cmd
```

Each layer depends only on layers below it. No circular dependencies.

### Server features

- HTTP `/v1` surface over `~/.config/aimee/aimee-http.sock` (+ optional localhost TCP)
- per-connection worker threads for the HTTP listener (never blocks the accept loop)
- 8-thread bounded compute pool for delegate executions, chat, and tool calls
- `SO_PEERCRED` authentication with 16 capability flags
- Trust levels: unattested (read-only), attested (full access via capability token)
- Rate limiting on auth failures
- Request dispatch through tier-owned DB APIs
- Graceful shutdown with SIGTERM

### Protocol methods (27+)

| Category | Methods |
|----------|---------|
| Server | `server.info`, `server.health` |
| Auth | `auth` |
| Hooks | `hooks.pre`, `hooks.post` |
| Sessions | `session.create`, `session.list`, `session.get`, `session.close` |
| Memory | `memory.search`, `memory.store`, `memory.list`, `memory.get` |
| Index | `index.find`, `index.blast_radius`, `index.list` |
| Rules | `rules.list`, `rules.generate` |
| Working memory | `wm.set`, `wm.get`, `wm.list`, `wm.context` |
| Dashboard | `dashboard.metrics`, `dashboard.delegations` |
| Workspace | `workspace.context` |
| Tool execution | `tool.execute` |
| Delegation | `delegate` |
| Streaming chat | `chat.send_stream` |

### Server lifecycle

The server is normally owned by service management: `install.sh` installs and enables systemd user units on Linux and launchd plists on macOS. The thin client no longer auto-spawns a server for ordinary RPC commands; `aimee server start` is an explicit unmanaged fallback, and `aimee server restart` is the explicit version-drift recovery path.

## Delegate agent configuration

Delegates are defined in `~/.config/aimee/agents.json`. Codex, Gemini, and
Mistral can use direct HTTP adapters. `codex-cli` and Claude use installed
provider CLIs, while `gemini-cli`, `mistral-cli`, and `mistral-plan` keep the
provider-CLI configuration shape but execute through native HTTP adapters
instead of launching the provider binaries. Gemini uses `GEMINI_API_KEY` or
`GOOGLE_API_KEY`; Mistral routes use `MISTRAL_API_KEY` or `~/.vibe/.env`.
These entries are still delegate agents; the user-facing primary agent is
configured separately by the launch, CLI chat, or webchat surface.
Provider-CLI routes that still launch a local CLI are available only when
`cli_cmd` resolves to an executable on `PATH` or an executable absolute path.
Native Gemini and Mistral routes are routable without a CLI on `PATH`.

```json
{
  "agents": [
    {
      "name": "codex",
      "provider": "codex",
      "backend": "provider-cli",
      "cli_kind": "codex",
      "cli_cmd": "codex",
      "roles": ["code", "review", "explain", "refactor", "draft", "execute"],
      "cost_tier": 0,
      "enabled": true
    },
    {
      "name": "claude",
      "provider": "claude",
      "backend": "tmux-cli",
      "cli_cmd": "claude",
      "roles": ["code", "review", "explain", "refactor", "draft", "execute"],
      "cost_tier": 1,
      "enabled": true
    },
    {
      "name": "gemini-cli",
      "provider": "gemini",
      "backend": "provider-cli",
      "cli_kind": "gemini",
      "roles": ["code", "review", "explain", "refactor", "draft", "execute"],
      "cost_tier": 0,
      "enabled": true
    },
    {
      "name": "mistral-cli",
      "provider": "mistral",
      "backend": "provider-cli",
      "cli_kind": "mistral",
      "roles": ["code", "review", "explain", "refactor", "draft", "execute"],
      "cost_tier": 0,
      "enabled": true
    },
    {
      "name": "mistral-plan",
      "provider": "mistral",
      "backend": "provider-cli",
      "cli_kind": "mistral-plan",
      "roles": ["code", "review", "explain", "refactor", "draft", "execute"],
      "cost_tier": 0,
      "enabled": true
    },
    {
      "name": "local-llama",
      "endpoint": "http://localhost:11434/v1",
      "model": "llama3.2",
      "auth_type": "none",
      "roles": ["summarize", "format", "draft"],
      "cost_tier": 0,
      "enabled": true
    }
  ],
  "default_agent": "codex",
  "fallback_chain": ["codex", "claude", "local-llama"]
}
```

## Storage paths

```
~/.config/aimee/
  aimee.yaml                # Workspaces, guardrail mode, primary agent provider
  agents.json               # Delegate agent config + network inventory
  aimee.db                  # DB1 local server/user/session state
  aimee-http.sock           # aimee-server /v1 HTTP Unix socket
  server.log                # Server debug log
  session-<id>.state        # Per-session state
  worktrees/<id>/<project>/ # Per-session git worktrees
  projects/<name>.md        # Auto-generated project descriptions
  tls/cert.pem, key.pem    # Self-signed TLS certs (webchat)

<project>/.mcp.json           # MCP server config (auto-generated)
<project>/.aimee/project.yaml # Per-project metadata (build, test, lint)
```

## Building

```bash
cd src
make                # Build DB-free clients (-> ../aimee, ../aimee-webchat)
make server         # Build server and KB binaries (-> ../aimee-server, ../aimee-kb)
make                # MCP serve is built into the aimee binary
make install        # Install aimee, aimee-webchat, aimee-server, and aimee-kb
make lint           # Check clang-format
make format         # Auto-fix formatting
make unit-tests     # Build and run all tests
make clean          # Remove build artifacts
```

### Dependencies

| Library | Purpose | Linking | Platform |
|---------|---------|---------|----------|
| SQLite3 | DB1 local store | `-lsqlite3` | `aimee-server` only |
| libpq | DB2 service access (incl. pgvector) | discovered with `pkg-config` | `aimee-kb` only |
| libm | Math functions | `-lm` | All |
| libpthread | Parallel delegates, server | `-lpthread` | Linux/macOS |
| libpam | PAM authentication | `-lpam` | Linux |
| libssl/libcrypto | TLS and auth provider support | `-lssl -lcrypto` | `aimee-server`, `aimee-kb` |
| libcurl | HTTP client (delegates) | `-lcurl` | `aimee-server` |
| cJSON | JSON parsing | Compiled in (vendored) | All |

### Code style

Enforced by `.clang-format`:

- 3-space indentation, no tabs
- Allman brace style
- `snake_case` functions, `SCREAMING_CASE` constants
- Right-aligned pointers (`char *str`)
- 100-character line limit
- No automatic include sorting

## Repository layout

The C source lives in `src/`; the rest of the repository supports building,
testing, deploying, and documenting it.

| Path | Contents |
|------|----------|
| `src/` | All C sources. Hundreds of `.c` files plus generated headers and `*.inc` split units. |
| `src/db1/` | DB1 (SQLite) data layer, owned by `aimee-server`. |
| `src/db2/` | DB2 (Postgres + pgvector) data layer, owned by `aimee-kb`. |
| `src/server/` | `aimee-server` internals: event loop, auth, sessions, compute pool, HTTP /v1, KB client. |
| `src/kb/` | `aimee-kb` internals: service dispatch, ingest/curator workers, KB HTTP. |
| `src/gateway/` | Optional `aimee-gateway` (ambient presence: Telegram/ntfy/webhook, STT/TTS). |
| `src/linux/`, `src/mac/`, `src/windows/`, `src/posix/` | Platform abstractions (events, IPC, paths, process). |
| `src/headers/`, `src/shared/`, `src/vendor/` | Internal headers, shared helpers, vendored libs (cJSON). |
| `src/tests/` | C unit tests (`test_*.c`), `fuzz_corpus/`, `support/`, and `test_build_integrity.sh`. |
| `webchat/` | Standalone Go browser service (thin client to `aimee-server`). |
| `frontend/` | React 19 + Vite UI, bundled to a single HTML file for webchat. |
| `api/` | OpenAPI specs (`openapi-server-v1.yaml`, `openapi-v1.yaml`), the API contract. |
| `skills/` | Bundled process skills (markdown + frontmatter). |
| `session_templates/` | Multi-agent session templates (code-review, debate, design-critique, planning). |
| `plugins/` | Plugin interface + example plugin manifest. |
| `benchmarks/`, `tools/`, `data/` | Eval suites (`catalog.toml`, `suite/`, `targets/`), replay harnesses, dataset cache. |
| `scripts/` | Sidecars (embeddings, LLM chat, curator, synthesis, ranker, planner) + build/boundary check scripts. |
| `systemd/`, `service/`, `deploy/` | systemd user units, macOS launchd plists, container deploy config. |
| `docs/` | Reference docs + the `proposals/` lifecycle (`pending` → `reviews`/`accepted` → `done`/`rejected`). |

Generated headers (checked in, regenerated on build): `schema_data.h` (DB schema
constants), `agent_help_data.h` (CLI help text), `tool_prompts_data.h` (tool
prompt templates). The generators are the `gen_*.py` scripts in `src/`.

## Server internals

`aimee-server` fronts its `/v1` HTTP surface with a per-connection-worker accept
loop and a bounded compute pool. The accept loop only hands each connection to a
worker; any handler that may block runs on the compute pool.

### Listener and connections

- The HTTP listener (`src/server/server_http.c`) owns the accept loop over the
  `/v1` UDS (`~/.config/aimee/aimee-http.sock`) and the optional localhost TCP
  port, handing each accepted connection to its own detached worker thread.
- `server_run()` (`src/server/server.c`) no longer runs an accept loop (the
  legacy NDJSON RPC socket was removed) and simply parks until shutdown. The
  `server_conn_t` buffered read/write helpers (`conn_flush()` etc.) survive
  because the in-process `/v1` dispatch bridge reuses them with a stack-allocated
  connection.
- `--socket=PATH` is accepted for compatibility but ignored (only the pid file is
  derived from it); the server always serves `/v1` over `aimee-http.sock`.

### Concurrency model

- **HTTP listener threads:** the accept loop hands each connection to its own
  detached worker, so a slow or streaming request never blocks the listener.
- **Compute pool:** bounded worker threads run handlers that touch DB, network,
  subprocesses, or delegate inference. Sizing is tunable
  (`compute_threads`/`worker_threads`/`background_threads`/`session_threads`).
- **Compute budget gate:** `server_compute_budget_acquire/release` serializes
  heavyweight inference so concurrent delegates cannot exhaust resources.

### Authentication and capabilities

The local `/v1` UDS (`aimee-http.sock`) is filesystem-permission gated and fully
trusted: it reaches the entire dispatch surface with no token. The optional TCP
listener requires a configured bearer token (`server_http_authorize`,
constant-time compare). Authorization is a 16-bit capability mask declared per
method in a registry (`src/server/server_auth.c`); unknown methods default to
deny, and TCP connections are capability-scoped.

| Flag | Bit | Grants |
|------|-----|--------|
| `CAP_CHAT` | 0x0001 | chat streaming |
| `CAP_DELEGATE` | 0x0002 | delegate / inference |
| `CAP_TOOL_EXECUTE` | 0x0004 | tool invocation |
| `CAP_TOOL_BASH` | 0x0008 | bash tool |
| `CAP_TOOL_WRITE` | 0x0010 | file-write tools |
| `CAP_MEMORY_READ` | 0x0020 | memory recall/search |
| `CAP_MEMORY_WRITE` | 0x0040 | memory store |
| `CAP_RULES_READ` | 0x0080 | rules list |
| `CAP_RULES_ADMIN` | 0x0100 | rules delete/approve |
| `CAP_DESCRIBE_READ` | 0x0200 | graph describe |
| `CAP_DESCRIBE_ADMIN` | 0x0400 | graph describe mutation |
| `CAP_INDEX_READ` | 0x0800 | code-index reads |
| `CAP_INDEX_ADMIN` | 0x1000 | index scan/rebuild |
| `CAP_SESSION_READ` | 0x2000 | session / wm reads |
| `CAP_SESSION_ADMIN` | 0x4000 | session / tool mutation |
| `CAP_DASHBOARD_READ` | 0x8000 | dashboard metrics |

Composite sets exist for authenticated (all-but-index-admin) and read-only
callers. Auth failures are rate-limited per UID (5 / 60 s → cooldown); the HTTP
surface can rate-limit per source. SIGTERM/SIGINT drain pending I/O and record
unclean-exit forensics (`shutdown_forensics.c`) to DB1.

## RPC method catalog

The dispatch table in `src/server/server.c` currently registers 162 typed server
methods; the thin CLI maps supported commands to them in `cli_v1_routes.inc`
(144 routes/aliases at this revision). Commands implemented only in legacy
`cmd_*` modules are not user-facing until they have a typed route. By family:

| Family | Representative methods |
|--------|------------------------|
| Server / meta | `server.info`, `server.health`, `hud.status`, `workers` |
| Auth / launch | `auth`, `launch.run`, `init.run` |
| Sessions | `session.create/list/get/close/brief` |
| Memory | `memory.search/store/list/get/read`, `memory.recall` |
| Index / graph | `index.scan/find/list/blast_radius/structure/find_callers`, `graph.sync_code/explain`, `blast_radius.preview` |
| Knowledge base | `kb.search/build/update/status`, `kb.docs.push`, `kb.ingest(.status)` |
| Rules / collab | `rules.list/generate/delete`, `collab_rules.list(_active)/approve/reject/retire` |
| Skills / toolsets | `skill.list/show/lint/eval/create/edit/patch/archive/pin/unpin/lifecycle/autostub`, `toolset.list/show/resolve` |
| Working memory | `wm.set/get/list/context` |
| Work queue | `work.add/add_batch/claim/complete/fail/list/board/cancel/release/clear/gc/sync_proposals/stats` |
| Agents / delegation | `agent.list/add/local/remove/enable/disable/probe/setup(_poll)/episodes`, `delegate(.launch/.status/.log/.reply/.backend_list/.backend_exec)` |
| Chat | `chat.send_stream` |
| Jobs | `jobs.list/status/logs/cancel`, `job.start/list/status/cancel` |
| Attempts / episodes | `attempt.record/list`, `episode.list` |
| Dashboard / LSP | `dashboard.*`, `lsp.diagnostics_summary` |
| Workspace | `workspace.context/add/list/remove`, `worktree.gc` |
| Eval / dogfood | `eval.run/results`, `dogfood.tag/review/report` |
| Identity / config | `identity.show/snapshot/diff`, `aux.config_show/test` |
| Tools / MCP | `tool.execute`, `mcp.tools_list/audit/recheck/call` |
| Triggers / cron | `trigger.fire/list/status/cancel`, `cron.list/add/show/history/run/enable/disable/remove` |
| Providers / models | `provider.list/show/models/test/quota/get/set`, `provider.slot_acquire/release`, `model.list/show/refresh` |
| Insights / migrate | `insights.overview`, `migrate.v2` |

Request shape: `{"method": "...", "protocol_version": 2, ...params}`. Success:
`{"status": "ok", ...data}`. Error: `{"status": "error", "error": "...",
"message": "..."}`. Streaming methods emit NDJSON events ending in a final status
object. `auth` must precede any capability-gated call.

## HTTP /v1 seam

`src/server/server_http.c` and `server_api.c` implement a native HTTP /v1 REST
surface (OpenAI-compatible inference, run management, and read surfaces). UDS
callers authenticate by peer UID; TCP callers present a bearer token (optionally
`scope:`-prefixed to constrain capabilities, a scoped token is denied on
inference with 403). `X-Request-ID` is echoed and access-logged. The contract
lives in `api/openapi-server-v1.yaml`. `docs/gen/api-v1.md` is the generated
reference for the separate KB API (`api/openapi-v1.yaml`). See the
[Manual §24](../MANUAL.md#24-the-http-v1-api) for the server endpoint list.

## The KB split

`aimee-server` owns no DB2 SQL. It reaches DB2 through the **KB client**
(`src/server/kb_client*.c`), typed wrappers (memory, index, docs, agent,
dashboard, roadmap, notes, curiosity, status) that call `aimee-kb` over its
HTTP `/v1` API (`AIMEE_KB_API_URL`, port 8741). The legacy Unix-socket transport
was retired in #2747; HTTP is now the only KB transport. DB1 remains local to the server;
DB2 can be a local single-user KB or a shared KB, and server-side reflection or
promotion flows send only scoped knowledge artifacts across that boundary.
`aimee-kb` (`src/kb/`) owns the DB2 schema, the memory inference pipeline, the
vector collections, the code index, document ingest, and the curator. It exposes
43 HTTP /v1 endpoints
(`/v1/code/*`, `/v1/docs/*`, `/v1/search`, `/v1/ingest/*`, `/v1/entities/*`,
`/v1/reflections/*`, `/v1/maintenance/*`, `/v1/releases/*`), see
`api/openapi-v1.yaml`. The server no longer auto-starts the KB: `AIMEE_KB_API_URL`
must point at an already-running `aimee-kb` (started by its service unit, container,
or `aimee-kb --http-port`). `AIMEE_KB_NO_AUTOSTART` is now a deprecated no-op.

## Scaling model

The scaling boundary is the storage boundary: the per-user server and the shared
knowledge service scale independently and in different ways.

**`aimee-server` is 1:1 with a user.** It owns DB1 (local SQLite, same-user
runtime state) and authenticates by `SO_PEERCRED` peer UID, so it is single-tenant
and local by construction: one per OS login, never sharded or load-balanced. Its
only scaling axis is *vertical*: the compute pool and concurrency knobs
(`compute_threads` / `worker_threads` / `background_threads` / `session_threads`,
`concurrency.per_model`) raise a single user's in-flight delegate and tool-call
throughput.

**`aimee-kb` is the horizontally-scalable, many-user tier.** All durable state
lives in DB2 (Postgres); a KB process holds only connection pools and worker
loops, making each instance an effectively stateless request server over a shared
database. Many `aimee-server` instances (many users) reach one KB over HTTP `:8741`
(`AIMEE_KB_API_URL`). Critically, the background pipeline claims work straight off
DB2 with `FOR UPDATE SKIP LOCKED` (`db2_kb_ingest_queue_claim_next` in
`src/kb/kb_ingest_workers.c`; the curator drain in `kb_curator_extract_code.c`), so
concurrent KB replicas never double-claim a row; Postgres is the only
coordinator. The scale-out recipe is therefore: **run N `aimee-kb` replicas behind
a load balancer over one Postgres.**

**DB2 is standard Postgres.** It is one `aimee_shared` database with `pg_trgm` +
`vector` (pgvector), scaled with the normal Postgres playbook (instance sizing,
pooling, read replicas; `db2_pool_size` bounds each instance's pool, 1-32). Vector
search scales inside it: pgvector HNSW by default (and always for memory vectors),
with **pgvectorscale (StreamingDiskANN)** as an additive, opt-in scale-up for large
corpus tables. The index type is chosen per corpus table by
`db2.vector.corpus_index` (`auto`|`hnsw`|`diskann`) /
`db2.vector.corpus_diskann_threshold` (`pgvec_corpus_index_choose` in
`src/db2/pgvec_transport.c`); because the `vector` column and queries are identical
across index types, switching is an index-only reindex
(`aimee kb repair --reindex-corpus`), never a data migration. See the
[Manual §27.5](../MANUAL.md#275-scaling-and-multi-user-deployment).

## Build system internals

`src/Makefile` is canonical; `CMakeLists.txt` mirrors it for Windows (MinGW) and
macOS. Objects are partitioned into sets and linked into only their owning
binary:

| Binary | Object sets | Link flags |
|--------|-------------|------------|
| `aimee` | client + support + platform | `-lpthread` (no SQLite, no libpq) |
| `aimee-server` | server + kb_client + agent + data + cmd-stubs + core + db1 + platform + mcp_git | `-lsqlite3 -lssl -lcrypto -lpam` (no libpq); built `-DAIMEE_DB2_DISABLED` |
| `aimee-kb` | kb + kb-data + db2-pg + db2 + core + platform | `-lpq -lzstd -lssl -lcrypto` (no SQLite); built `-DAIMEE_DB1_DISABLED -DAIMEE_DISABLE_DB2_SQLITE_SHIM` |
| `aimee-webchat` | (Go) | `cd webchat && go build` |
| `aimee-gateway` | gateway + platform | `-lpthread -lssl -lcrypto` |

Common flags: `-Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections
-s`, `-Wall -Wextra -Werror`, dependency generation (`-MMD -MP`). Optional
features are pkg-config-gated: `-DWITH_PAM`, `-DWITH_LIBSECRET`. The version
string is embedded from `git describe`.

Boundary gates (run in CI): `make check-linking` (no SQLite in client, no libpq
in server, no SQLite in KB, verified with `ldd`/`otool` and `readelf` symbol
checks), `make module-boundary-check`, `make kb-target-isolation-check`,
`make kb-container-packaging-check`, plus `scripts/check_tier_deps.sh` and
`scripts/check-module-boundary.py`. Layer-include violations are caught by
`src/tests/test_build_integrity.sh` against the exemption table in
[OWNERS.md](../OWNERS.md).

## Testing and CI

- **Unit tests:** `make unit-tests` builds and runs `src/tests/test_*.c`.
- **Fuzzing:** `src/tests/fuzz_corpus/` seeds AFL/libFuzzer targets; the
  `fuzz-nightly.yml` workflow runs them on a schedule.
- **Build integrity:** `test_build_integrity.sh` enforces layering; the
  `make` boundary gates enforce storage isolation.
- **Benchmarks:** `benchmarks/` (driven by `catalog.toml`, `suite/runner.py`)
  and the replay harnesses in `tools/` cover memory recall, planning, reasoning,
  guardrails, and KB quality; `bench-smoke.yml` runs a smoke subset in CI. See
  [docs/BENCHMARKS.md](../docs/BENCHMARKS.md).
- **CI workflows:** `.github/workflows/`, `ci.yml` (build + tests + boundary
  gates), `bench-smoke.yml`, `fuzz-nightly.yml`, `release.yml`.
