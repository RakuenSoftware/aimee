# The aimee Manual

A complete, exhaustive reference for installing, configuring, operating, and
mastering aimee. If you want a quick overview, start with the
[README](README.md). If you want code-level internals, see
[`src/README.md`](src/README.md) and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

This manual is organized so you can read it front-to-back the first time and use
it as a lookup reference afterward.

---

## Table of contents

1. [What aimee is](#1-what-aimee-is)
2. [Concepts and mental model](#2-concepts-and-mental-model)
3. [Installation](#3-installation)
4. [Updating and uninstalling](#4-updating-and-uninstalling)
5. [Configuration reference](#5-configuration-reference)
6. [The command line](#6-the-command-line)
7. [Command reference](#7-command-reference)
8. [Memory](#8-memory)
9. [Working memory](#9-working-memory)
10. [Rules and feedback](#10-rules-and-feedback)
11. [Guardrails and safety](#11-guardrails-and-safety)
12. [The code index](#12-the-code-index)
13. [Delegation](#13-delegation)
14. [Roadmaps and the autonomous loop](#14-roadmaps-and-the-autonomous-loop)
15. [The work queue](#15-the-work-queue)
16. [Skills and toolsets](#16-skills-and-toolsets)
17. [Sessions and worktrees](#17-sessions-and-worktrees)
18. [Providers, models, and agents](#18-providers-models-and-agents)
19. [Identity, charter, and personas](#19-identity-charter-and-personas)
20. [Triggers, cron, and automation](#20-triggers-cron-and-automation)
21. [The knowledge base](#21-the-knowledge-base)
22. [The MCP server](#22-the-mcp-server)
23. [Webchat and the browser UI](#23-webchat-and-the-browser-ui)
24. [The HTTP /v1 API](#24-the-http-v1-api)
25. [Integrations](#25-integrations)
26. [Server and service management](#26-server-and-service-management)
27. [Operations and deployment](#27-operations-and-deployment)
28. [Troubleshooting](#28-troubleshooting)
29. [Appendix A, Environment variables](#appendix-a-environment-variables)
30. [Appendix B, Files and paths](#appendix-b-files-and-paths)
31. [Appendix C, Glossary](#appendix-c-glossary)

---

## 1. What aimee is

aimee gives your AI coding tool a memory, and lets you run that tool on any
model you like. It is a local-first layer that sits between you and tools like
Claude Code, Codex, OpenCode, Gemini CLI, Mistral Vibe, and GitHub Copilot. One
UI, any model, and a memory that travels with you between every model and
provider, so switching never starts from zero and is never locked to a vendor
(see [§25, Integrations](#25-integrations) for pointing a tool's front end at
aimee). On top of that it adds three things your tools lack:

- **Persistent memory** that compounds across sessions and across tools, so your
  AI starts every session already knowing your infrastructure, conventions,
  decisions, and past mistakes.
- **Guardrails** that classify and block dangerous edits (secrets, production
  configs), warn on known anti-patterns, and lock writes during planning.
- **Delegation** that routes routine work (summaries, formatting, review,
  boilerplate) to cheaper or free models, so your primary agent only handles
  what needs its full capability.

It integrates through **hooks** (intercepting tool calls) and **MCP** (exposing
memory and code knowledge as tools). The core is written in C for speed, the
CLI starts in under 10 ms and a pre-tool guardrail check adds about 1 ms, and it
has **zero cloud dependencies**: everything runs on your machine, and external
model providers are reached only when you explicitly delegate.

### The four artifacts

| Binary | Role |
|--------|------|
| `aimee` | The thin CLI you run. Holds no database; forwards typed requests to `aimee-server`. Also hosts the MCP bridge (`aimee mcp-serve`). |
| `aimee-server` | The persistent local hub. Owns local state (DB1/SQLite), runs the compute pool, routes delegates, serves hooks, exposes the RPC and HTTP /v1 surfaces. |
| `aimee-kb` | The knowledge service. Owns DB2 (Postgres + pgvector), which may be a local single-user database or a shared knowledge service: memories, rules, code index, embeddings, ingest/curator. |
| `aimee-webchat` | A standalone Go service that serves the browser UI and proxies to `aimee-server`. |

(An optional `aimee-gateway` adds ambient-presence delivery, Telegram/ntfy/
webhooks, speech-to-text/text-to-speech, and is not part of the core path.)

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for how these fit together.

---

## 2. Concepts and mental model

**Primary agent vs delegate agent.** The *primary agent* is the AI coding
surface you interact with (Claude Code, Gemini CLI, Codex, Vibe, Copilot, or
aimee's own chat). aimee integrates with it through hooks, provider-CLI
adapters, or direct primary-session adapters. *Delegate agents* are sub-agents
configured in `agents.json` that handle offloaded work via
`aimee delegate <role> <prompt>`.

**The two storage tiers.** DB1 (SQLite, owned by the local `aimee-server`) holds
same-user runtime state, sessions, working memory, conversation windows,
checkpoints, jobs, local signal streams, and secrets. DB2 (Postgres + pgvector,
owned by `aimee-kb`) holds durable knowledge, memories, rules, the code index,
tasks/decisions, and the vector embeddings of all of it. DB2 can run locally for
one user or as a shared KB; only knowledge that is appropriate for its configured
scope is reflected or promoted there from local server state. The boundary is
pinned and compile-enforced; you never choose a different backend for either
tier. It is also the **scaling boundary**: `aimee-server` is one-per-user, while
`aimee-kb` and its Postgres are the shared, horizontally-scalable tier (see
[§27.5](#275-scaling-and-multi-user-deployment)). See
[`docs/STORAGE_TIERS.md`](docs/STORAGE_TIERS.md).

**Memory tiers (L0-L3).** Stored knowledge moves through four lifetimes: L0
session scratch, L1 recent (≈30 days), L2 long-term stable facts, L3 episodic
(past attempts/outcomes with decay). Promotion and decay are automatic. This is
*different* from the storage tiers above, L0-L3 is the *semantic* lifecycle of a
memory; DB1/DB2 is *where bytes live*.

**Sessions and worktrees.** Each session gets its own git worktree, branch, and
state file, so parallel sessions never collide. Guardrails redirect writes that
target a workspace path with an active worktree.

**Hooks.** aimee registers `SessionStart`, `PreToolUse`, and `PostToolUse`
hooks with your tool. SessionStart injects context; PreToolUse classifies and
gates each tool call; PostToolUse re-indexes edited files.

---

## 3. Installation

aimee ships four binaries: the **`aimee`** thin client, **`aimee-server`**,
**`aimee-kb`**, and **`aimee-webchat`**. There are two ways to stand it up:

- **Docker services + thin client (recommended, [§3.1](#31-run-the-services-in-docker-recommended) to [§3.2](#32-install-the-thin-client)).**
  Run `aimee-server` and `aimee-kb` as containers (one combined container or two
  split) and install only the thin `aimee` CLI on each developer machine, pointed
  at the server. This is the intended deployment and the path the operations
  chapter ([§27](#27-operations-and-deployment)) details.
- **Single-box source build ([§3.3](#33-single-box-source-build)).** Build and run
  everything on one host with `install.sh`, no Docker. The classic
  single-developer setup.

### 3.1 Run the services in Docker (recommended)

The combined image co-locates both binaries in one container; Postgres (pgvector)
and a CPU embedder come up alongside it:

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.combined.yaml up --build -d
```

The server fronts the `/v1` API on `:8740` (default bearer `aimee-local-dev`) and
reaches the in-container kb on `:8741`:

```bash
curl -H 'Authorization: Bearer aimee-local-dev' http://localhost:8740/v1/health
curl -H 'Authorization: Bearer aimee-local-dev' http://localhost:8740/v1/kb/status
```

Split the two binaries into separate containers with `compose.server.yaml` when
you want to scale, update, or place them independently (the horizontal-scaling
model in [§27.5](#275-scaling-and-multi-user-deployment)). See
[§27.1](#271-containerized-deployment) for every compose file, the container
lifecycle (start/logs/stop/update), volumes, and the end-to-end smoke tests.
Override the default `aimee-local-dev` bearer on any networked deployment.

### 3.2 Install the thin client

Each developer installs only the `aimee` CLI. Prebuilt thin-client binaries for
Linux, macOS, and Windows are attached to every GitHub release; or build just the
client from a checkout (a C compiler is the only dependency, no Go, libpq, or
zstd):

```bash
cmake -B build -DAIMEE_THIN_CLIENT=ON -DAIMEE_LEAN=ON \
      -DWITH_PAM=OFF -DWITH_LIBSECRET=OFF -DWITH_UI=OFF
cmake --build build --target aimee
```

On macOS add `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`; on Windows (MinGW)
add `-G "MinGW Makefiles" -DWITH_TLS=OFF` (no TLS; terminate TLS at a proxy).
Point the client at the server per-invocation, via the environment, or persist it:

```bash
aimee --server http://my-host:8740 --server-token=aimee-local-dev status

export AIMEE_SERVER_URL=http://my-host:8740
export AIMEE_SERVER_TOKEN=aimee-local-dev
aimee status

aimee remote set http://my-host:8740 aimee-local-dev   # persists to <aimee_home>/remote.conf
aimee remote status                                    # resolved transport + /v1/health probe
aimee remote clear                                     # revert to a local Unix socket
```

Precedence is `--server` > `AIMEE_SERVER_URL` > `remote.conf`. `https://` works on
Linux/macOS builds (set `AIMEE_TLS_INSECURE=1` for self-signed certs); the Windows
client refuses `https://`. A remote thin client drives the data/RPC plane
(`memory`, `kb`, `rules`, `index`, `sessions`, `notes`, …); interactive `aimee
chat`/`aimee launch` need a co-located server, and **writes are off by default over
the network** until the server sets `aimee.api.remote_writes` (see
[§24](#24-the-http-v1-api)). Finally, register the hooks on each machine with
`./configure-hooks.sh` (or `configure-hooks.ps1`) so your AI coding tool calls
aimee.

### 3.3 Single-box source build

To build and run everything on one host without Docker:

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
./install-deps.sh   # system packages + PostgreSQL bootstrap (uses sudo)
./install.sh        # build + install + configure (no sudo)
```

`install-deps.sh` installs the build/runtime packages and bootstraps the
`aimee_shared` PostgreSQL database, the only steps that need root. (For a host
that uses a *remote* kb and needs no local database, run `AIMEE_KB_MODE=remote
./install-deps.sh`.) Prerequisites by platform:

| Need | Debian/Ubuntu | Fedora/RHEL | Arch | macOS |
|------|---------------|-------------|------|-------|
| C toolchain | `build-essential` | `gcc make` | `base-devel` | Xcode CLT |
| pkg-config | `pkg-config` | `pkgconf-pkg-config` | `pkgconf` | `brew install pkg-config` |
| SQLite3 (DB1) | `libsqlite3-dev sqlite3` | `sqlite-devel sqlite` | `sqlite` | system SQLite |
| libpq (DB2) | `libpq-dev` | `libpq-devel` | `postgresql-libs` | `brew install libpq` |
| libcurl | `libcurl4-openssl-dev` | `libcurl-devel` | `curl` | system curl |
| libzstd | `libzstd-dev` | `libzstd-devel` | `zstd` | `brew install zstd` |
| PAM (webchat) | `libpam0g-dev` | `pam-devel` | `pam` | built-in |
| Postgres + pgvector | `postgresql postgresql-contrib` + pgvector | `postgresql-server` + `pgvector_NN` | `postgresql pgvector` | `brew install postgresql pgvector` |
| ctags (indexing) | `universal-ctags` | `ctags` | `ctags` | `brew install universal-ctags` |
| ripgrep (search) | `ripgrep` | `ripgrep` | `ripgrep` | `brew install ripgrep` |

Go 1.25+ is required only to build `aimee-webchat`. Then `install.sh`, in order:

1. **Check prerequisites** (FTS5 + the dev headers); if any are missing it stops
   and points you back at `install-deps.sh` rather than escalating privileges.
2. **Build** (only if sources changed): `cd src && make all server`, producing
   `aimee`, `aimee-server`, and `aimee-kb` at the repo root, plus `aimee-webchat`
   when a Go toolchain is on `PATH` (otherwise `make` notes it was skipped and the
   C services still build — webchat is optional for a source install).
3. **Stop any running server/KB** gracefully to avoid "text file busy".
4. **Install binaries** to `~/.local/bin/` and remove retired binaries.
5. **Install bundled skills** to `~/.local/share/aimee/skills/` (idempotent).
6. **Choose a kb mode**: local sidecar (default, backed by local Postgres) or a
   remote `aimee-kb` over HTTP (persists `kb_client_url`/`kb_client_bearer_token`
   to `aimee.yaml`).
7. **Install service units**: systemd user units (Linux) or launchd plists
   (macOS), and enable `aimee-kb` then `aimee-server` (server only, in remote-kb
   mode).
8. **Install `ast-grep` (`sg`)** for structural code search.
9. **Configure your primary AI CLI** (Claude / Codex / Gemini / OpenAI-compatible),
   saved to `~/.config/aimee/aimee.yaml`.
10. **Optionally add a local delegate** via `add-local-delegate.sh` (Ollama /
    llama.cpp).
11. **Configure AI coding tools** via `configure-hooks.sh`: register hooks + MCP
    for Claude Code, Gemini CLI, Codex CLI, GitHub Copilot.

`install-deps.sh` also bootstraps Postgres (starts the service, creates
`aimee_shared`, enables `pg_trgm` + `vector`); `aimee-kb`'s startup auto-bootstrap
retries the same steps on first launch. On Windows, aimee runs as the thin client
only (server + kb live in Docker or on a Linux/macOS host): `install.ps1` builds
and installs just `aimee.exe`; point it at a server with `aimee remote set` (or
`AIMEE_SERVER_URL`), then run `configure-hooks.ps1`. Update with `./update.sh` (§4).

**Manual build targets:**

```bash
cd src
make                # DB-free clients  -> ../aimee, ../aimee-webchat
make server         # server + KB      -> ../aimee-server, ../aimee-kb
make install        # install all four to ~/.local/bin
make lean           # size-optimized variants (aimee-lean, ...)
make lint           # clang-format / static checks
make format         # auto-fix formatting
make unit-tests     # build and run the C test suite
make clean          # remove build artifacts
```

Build-boundary gates you can run: `make check-linking` (no SQLite in client,
no libpq in server, no SQLite in KB), `make module-boundary-check`,
`make kb-target-isolation-check`. See [`src/README.md`](src/README.md) for the
full target list and the layering rules.

### 3.4 Verify

```bash
aimee version
aimee status        # server, DB1, and kb health
```

Against a Docker server, set `AIMEE_SERVER_URL`/`AIMEE_SERVER_TOKEN` (or `aimee
remote set`) first. On a source install, the thin CLI no longer starts the server
implicitly; if no service is running, use `systemctl --user start aimee-server` on
systemd systems or `aimee server start` as the cross-platform fallback.

---

## 4. Updating and uninstalling

**Update:**

```bash
./update.sh         # fetch latest, rebuild if needed, restart services
```

`update.sh` pulls the latest source (SSH with HTTPS fallback), rebuilds only when
sources changed or `--force` is given, gracefully stops KB and server, then
reinstalls following the same flow as `install.sh`. Windows: `update.ps1`.

**Uninstall:**

```bash
aimee clean         # remove local aimee config + tool integrations
aimee clean --force # skip confirmation
```

`aimee clean` removes `~/.config/aimee` state and unregisters hooks/MCP from
your AI tools. It does not remove the binaries from `~/.local/bin` (delete those
manually) or drop the Postgres `aimee_shared` database.

---

## 5. Configuration reference

aimee reads three configuration files plus per-project metadata and a set of
environment variables. All live under `~/.config/aimee/` unless `AIMEE_HOME` is
set (see [Appendix A](#appendix-a-environment-variables)).

### 5.1 `aimee.yaml`, main configuration

Located at `~/.config/aimee/aimee.yaml` (YAML, parsed internally to JSON). A
minimal real-world file:

```yaml
guardrail_mode: approve
provider: claude
openai_endpoint: https://api.openai.com/v1
openai_model: gpt-4o
workspaces:
  - /home/me/dev
  - /home/me/work
concurrency:
  per_model:
    MiniMax-M2.7: 3
compact:
  enabled: false
```

The full set of recognized top-level keys (from `config_schema[]` in
`src/config.c`). Object/array keys have their own nested settings handled by the
corresponding `config_*.c` module.

**Storage**

| Key | Type | Meaning |
|-----|------|---------|
| `db1_path` | string | SQLite DB1 path (default `~/.config/aimee/aimee.db`). |
| `db2_url` | string | Postgres DB2 connection URL (`postgresql://user:pass@host:port/db`). |
| `db2_pool_size` | int | DB2 connection pool size (clamped 1-32; default 8). |
| `db2` | object | Additional DB2 settings. |

**Primary provider / models**

| Key | Type | Meaning |
|-----|------|---------|
| `provider` | string | Primary agent provider: `claude`, `codex`/`openai`, `gemini`, `mistral`, etc. |
| `use_builtin_cli` | bool | Use aimee's built-in HTTP adapter vs the provider's native CLI. |
| `claude_model` | string | Claude model override (empty = CLI default). |
| `codex_model` | string | Codex/OpenAI primary model override. |
| `model_reasoning_effort` | string | Thinking effort: `low`/`medium`/`high`/`xhigh`. |
| `openai_endpoint` | string | OpenAI-compatible endpoint URL. |
| `openai_model` | string | Model for that endpoint. |
| `openai_key_cmd` | string | Shell command that prints the API key. |
| `prompt_tier` / `delegate_prompt_tier` | string | System-prompt verbosity tier for primary / delegate. |
| `prompt_file` | string | Override system prompt from a file. |

**Embeddings**

| Key | Type | Meaning |
|-----|------|---------|
| `embedding_command` | string | External command that produces embeddings (sidecar). |
| `embedding_model` | string | Embedding model name. |
| `embedding_endpoint` | string | Embedding service URL. |
| `embedding_dim` | int | Embedding dimensionality; must match the embedder model. Default `2560` (pplx-embed-v1-4b); set `1024` for the pplx-embed-v1-0.6b tier. |

**Memory & retrieval**

| Key | Type | Meaning |
|-----|------|---------|
| `memory_weight_profile` | string | Named retrieval-weight profile used by the retrieval pipeline. |
| `memory_rerank_mode` / `memory_rerank` | string/object | Reranking strategy and parameters. |
| `memory_query_expansion` | object | Query-rewrite / HyDE / decomposition settings. |
| `memory_recall_lanes` | object | Recall-lane (lexical/dense/graph) configuration. |
| `memory_maintenance` | object | Promotion/decay/compaction schedule. |
| `memory` | object | General memory subsystem settings. |

**Guardrails & safety**

| Key | Type | Meaning |
|-----|------|---------|
| `guardrail_mode` | string | `approve` (default), `prompt`, or `deny`, see [§11](#11-guardrails-and-safety). |
| `guardrails` | object | Detailed guardrail policy (paths, semantic checks, TDD gate). |
| `sandbox` | object | Sandboxing policy for tool execution. |
| `integrity` | object | Integrity-gate settings. |

**Delegation & concurrency**

| Key | Type | Meaning |
|-----|------|---------|
| `autonomous` | bool | Allow autonomous multi-step operation. |
| `cross_verify` | object | Cross-verification of delegate output. |
| `retry` | object | Retry policy for transient delegate failures. |
| `max_iterations` / `max_iterations_delegate` | int | Tool-use loop caps. |
| `max_delegation_depth` | int | How deep delegates may sub-delegate. |
| `max_delegation_spawns` | int | Max concurrent delegate spawns. |
| `max_background_processes` | int | Cap on background processes. |
| `background_threads` / `compute_threads` / `session_threads` / `worker_threads` | int | Server thread-pool sizing. |
| `concurrency` | object | Per-model concurrency limits (`concurrency.per_model.<model>: N`). |
| `ensemble` | object | Ensemble/voting delegate settings. |
| `roundtable` | object | Agent roundtable loop settings: `max_rounds`, `converge_threshold`, `deadline_ms`, `turns`. |

**Sessions, tools, transport**

| Key | Type | Meaning |
|-----|------|---------|
| `workspaces` | array | Registered workspace root paths. |
| `sessions` / `session` | object | Session lifecycle settings. |
| `rewind` | object | Conversation rewind settings. |
| `search` | object | Search behavior. |
| `compact` | object | Context compaction (`compact.enabled`). |
| `ecomode` | bool | Reduced-resource mode. |
| `lsp_servers` | array | Language servers to launch for diagnostics. |
| `mcp` / `mcp_clients` | object/array | MCP server transport and downstream MCP clients. |
| `computer_use` | object | Computer-use tool settings. |
| `transport` | object | Server transport (socket/TCP) settings. |
| `otel` | object | OpenTelemetry export. |
| `proxy_url` / `proxy_token` | string | Outbound HTTP proxy. |

**Knowledge, learning, identity**

| Key | Type | Meaning |
|-----|------|---------|
| `kb` | object | Knowledge-base / curator settings. |
| `learning` | object | Implicit-learning pipeline settings. |
| `intelligence` | object | Calibration / bandit / demotion settings. |
| `charter` | object | Charter (epistemic directives) pipeline. |
| `identity` | object | Working-profile / identity settings. |
| `skills` | object | Skill discovery and lifecycle. |
| `toolsets` | object | Named composable toolsets. |
| `script` | object | Project build/test/lint command hints. |
| `auxiliary` | object | Auxiliary-model (task → provider/model) routing. |
| `model_meta` | object | Model-capability metadata overrides. |
| `dogfood` | object | Dogfood review settings. |
| `cron_jobs` | array | Scheduled cron jobs (see [§20](#20-triggers-cron-and-automation)). |

> Editing tip: many settings are written for you by commands such as
> `aimee agent setup`, `aimee provider set`, and `aimee workspace add`.
> Hand-edit `aimee.yaml` only when a setting has no routed command, and keep it
> valid YAML, the server reloads it on the next request.

### 5.2 `agents.json`, delegate agents and network inventory

Located at `~/.config/aimee/agents.json`. Defines delegate agents, the default
agent, the fallback chain, and (optionally) a host/network inventory used to
brief delegates. Example (secrets redacted):

```json
{
  "default_agent": "codex",
  "fallback_chain": ["codex", "claude", "local-llama"],
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
      "name": "local-llama",
      "endpoint": "http://localhost:11434/v1",
      "model": "llama3.2",
      "auth_type": "none",
      "roles": ["summarize", "format", "draft"],
      "cost_tier": 0,
      "enabled": true
    }
  ]
}
```

Per-agent fields:

| Field | Meaning |
|-------|---------|
| `name` | Unique agent id used by `--via`, routing, and the fallback chain. |
| `provider` | Provider family: `openai`, `chatgpt`/`codex`, `anthropic`/`claude`, `gemini`, `mistral`, `minimax`, … |
| `backend` | `provider-cli` (launch a CLI), `tmux-cli`, or omitted for direct HTTP. |
| `cli_kind` / `cli_cmd` | CLI adapter kind and executable (for provider-CLI backends). |
| `endpoint` | HTTP endpoint for direct API agents. |
| `model` | Model name to request. |
| `auth_type` | `none`, `bearer`, `oauth`, or `x-api-key`. |
| `api_key` | API key (or `$ENV_VAR`); **store secrets via env/`auth_cmd` where possible**. |
| `auth_cmd` | Command that prints the credential. |
| `fallback_model` | Model to retry with on HTTP 400. |
| `roles` | Roles this agent can serve (see [§13](#13-delegation)). |
| `cost_tier` | Lower = cheaper; the router prefers the lowest capable tier. |
| `max_tokens` / `max_turns` / `timeout_ms` | Per-agent budgets (`max_turns: -1` = unlimited). |
| `tools_enabled` | Whether tool-use mode is permitted. |
| `enabled` | Toggle without deleting. |

Top-level keys: `default_agent`, `fallback_chain`, `agents`, and optional
`hosts`/`networks` (SSH/host inventory injected into delegate context). Manage
these with `aimee agent add/local/remove/enable/disable` and
`aimee agent setup <provider>` rather than editing by hand. Full guidance:
[`docs/DELEGATES.md`](docs/DELEGATES.md).

### 5.3 Per-project and per-workspace configuration

| File | Purpose |
|------|---------|
| `<project>/.aimee/project.yaml` | Per-project metadata: build/test/lint commands, language, conventions. Used by guardrails, verification, and context. |
| `<project>/.mcp.json` | Auto-generated MCP server registration pointing at `aimee mcp-serve`. |
| `<workspace>/aimee.workspace.yaml` | Declarative multi-repo workspace manifest (repos, dependencies, required credentials). Consumed by `aimee setup`. See [`docs/WORKSPACES.md`](docs/WORKSPACES.md). |
| `<project>/.aimee/roadmap/` | Generated roadmap projections (`ROADMAP.md`, `STATE.md`, `reports/<id>.html`). |

### 5.4 Profiles

`aimee profile` manages alternate configuration sets (separate `aimee.yaml`,
`agents.json`, and state). Select one with `--profile=<name>` on any command or
the `AIMEE_PROFILE` environment variable. Useful for separating work and
personal setups, or for testing a new provider without disturbing your default.

---

## 6. The command line

```
aimee [--json] [--fields=FIELDS] [--profile=PROFILE] <command> [args...]
```

Global flags:

| Flag | Effect |
|------|--------|
| `--json` | Machine-parseable JSON output (supported by server-backed commands). |
| `--fields=FIELDS` | Restrict JSON output to the named fields. |
| `--profile=PROFILE` | Use a named profile (see [§5.4](#54-profiles)). |
| `--help`, `-h` | Command help. |

Discovery:

```bash
aimee help            # core commands
aimee help --all      # core + advanced + admin
aimee help <command>  # detailed help for one command
```

`aimee` is a *thin client*: it either performs a small local operation or
forwards a typed request to an already-running `aimee-server`. `install.sh`
installs and enables service units where supported; otherwise start the server
with `aimee server start`. Commands are grouped into tiers, **core** (shown by
`aimee help`), **advanced**, and **admin** (both shown by `aimee help --all`).

Bare `aimee` (or `aimee chat`) launches the interactive primary-agent chat TUI.

---

## 7. Command reference

This is the current thin-client command contract, grouped by family.
Server-backed commands support `--json`. Where useful, the routed RPC method is
noted in parentheses.

> The CLI is the contract: commands here are those registered in the dispatch
> table and RPC routes. `aimee help <command>` always reflects the installed
> build.
>
> Some older in-process command modules still exist in the source tree
> (`cmd_roadmap.c`, `cmd_auto.c`, extended `cmd_memory_*` maintenance verbs), but
> they are not part of the installed thin-client contract until they have a typed
> server RPC route. If `aimee <command>` says "has no typed server RPC route",
> treat that command as implementation work in progress rather than user-facing
> documentation.

### 7.1 Memory, `aimee memory`

Persistent, tiered knowledge (DB2, via the server → KB).

**Core CRUD**
- `memory search <query>`, search stored memory (`memory.search`).
- `memory store <key> <value> [--tier L0|L1|L2|L3] [--kind K]`, store a memory (`memory.store`).
- `memory list [--tier T] [--kind K] [--limit N] [--status proposed]`, list memories (`memory.list`).
- `memory get <id>` / `memory show <id>`, read one memory (`memory.get`).
- `memory read`, assemble the current memory context (`memory.read`).

The installed thin-client help currently exposes only `search`, `store`, `list`,
`get`/`show`, and `read` as the supported memory command family. The KB and
server contain additional maintenance, graph, provenance, vector repair,
benchmarking, and review implementation modules; those are internal or
not-yet-routed unless they appear in `aimee help memory` for your build.

See [§8](#8-memory) for the conceptual model.

### 7.2 Working memory, `aimee wm`

Session-scoped scratch (DB1, TTL'd).

- `wm set <key> <value> [--session ID] [--category C] [--ttl N]` (`wm.set`)
- `wm get <key> [--session ID]` (`wm.get`)
- `wm list [--session ID] [--category C]` (`wm.list`)

### 7.3 Code index, `aimee index`

Symbol-level code knowledge (DB2 code index, via KB).

- `index find <identifier>` (`index.find`)
- `index list` / `index overview` (`index.list`)
- `index scan [--force]` (`index.scan`)
- `index structure <file>` (`index.structure`)
- `index callers <symbol>` (`index.find_callers`)
- `index blast-radius <file>` (`index.blast_radius`)
- `code audit [dir] [--json] [--fix]`: local file-health checks. `--fix` is non-mutating until there are safe, reviewable mechanical fixes.
- `code audit --graph [--project P] [--json]`: graph-derived dead exports, import cycles, exact clones, and near clones via `aimee-server` and `aimee-kb`. Thin clients need a configured remote and a scanned/indexed project.

See [§12](#12-the-code-index).

### 7.4 Rules, `aimee rules`

Behavioral rules derived from feedback.

- `rules list` (`rules.list`) · `rules generate` (`rules.generate`) · `rules delete <id>` (`rules.delete`)

### 7.5 Delegation, `aimee delegate`

Offload work to a sub-agent (via the server compute pool).

- `delegate <role> <prompt>` (`delegate`), roles: `code`, `review`, `explain`,
  `refactor`, `draft`, `execute`, `summarize`, `format`, `search`, `diagnose`,
  `validate`, `reason` (aliases: `implement`/`build`→`code`, `test`/`check`→
  `validate`, `inspect`→`diagnose`, `research`→`execute`).
  Flags: `--tools`, `--json`, `--background`, `--durable`, `--prompt-file PATH`,
  `--prompt-stdin`, `--system S`, `--max-tokens N`, `--max-turns N`,
  `--timeout N`, `--handoff-json`, `--worktree BRANCH`, `--via AGENT`,
  `--provider NAME`, `--model NAME`, `--tier N`, `--retry N`, `--verify CMD`,
  `--context-dir DIR`, `--files F`.
- `delegate plan <proposal.md> [--json] [--output PATH] [--launch] [--parallel N]`, generate work packets from a proposal.
- `delegate launch <plan.json> [--json] [--parallel N]` (`delegate.launch`), queue reviewed packets.
- `delegate aggregate "<task>"` (`delegate.aggregate`), run one Mixture-of-Agents fan-out and synthesis over `ensemble.reference_models`.
- `delegate roundtable "<task>" [--mode draft|review] [--turns parallel|sequential] [--rounds N] [--brief TEXT] [--brief-json JSON] [--apply]` (`delegate.roundtable`), run a bounded multi-round collaborative draft or review. Directed review briefs may include focus/fixes/invariants/questions. The async run result includes `artifact`, `rounds_run`, `converged`, `degraded`, `truncated`, `cost_capped`, `deadline_hit`, `cancelled`, `best_round`, `items_round`, `artifact_round`, `cost_usd`, `items`, `answered_questions`, and `coverage_gaps`.
- `delegate status <job_id> [job_id...] [--full|--result-limit N]` (`delegate.status`).
- `delegate log` / `delegate history` (`delegate.log`) · `delegate --list-roles` (`agent.list`).

See [§13](#13-delegation) and [`docs/DELEGATES.md`](docs/DELEGATES.md).

### 7.6 Roadmaps and autonomous dispatch

Spec-driven decomposition (DB2, via KB) and a deterministic dispatch loop.

The source tree contains roadmap and auto-loop implementation modules
(`cmd_roadmap.c`, `cmd_auto.c`, KB-side `roadmap.*` handlers), but the current
thin CLI does not expose `aimee roadmap` or `aimee auto` through typed server RPC
routes. Use `aimee delegate plan` and `aimee delegate launch` for the currently
routed work-packet flow.

See [§14](#14-roadmaps-and-the-autonomous-loop).

### 7.7 Work queue, `aimee work`

Inter-session task queue (claim/complete coordination).

- `work add` · `work add-batch [--from-proposals]` · `work claim` · `work complete` · `work fail`.
- `work list` · `work board [--history ITEM]` · `work stats`.
- `work cancel` · `work release` · `work clear` · `work gc [--max-age N]` · `work sync-proposals`.

See [§15](#15-the-work-queue).

### 7.8 Coordinated jobs, `aimee job` and durable jobs, `aimee jobs`

- `job start <plan_id>` · `job list [--limit N]` · `job status <id>` · `job cancel <id>`, coordinated parallel jobs.
- `jobs list [--limit N]` · `jobs status <id>` · `jobs logs <id>` · `jobs cancel <id>`, durable delegate jobs (created by `--background`/`--durable`).

### 7.9 Skills, `aimee skill`

Project-scoped process skills.

- `skill list` · `skill show <name>` · `skill create` · `skill edit` · `skill patch`.
- `skill lint` · `skill eval` · `skill archive` · `skill lifecycle [--stale-days N] [--archive-days N] [--force]`.
- `skill autostub [--force]` · `skill pin <name>` · `skill unpin <name>`.

### 7.10 Toolsets, `aimee toolset`

- `toolset list` · `toolset show <name>` · `toolset resolve <name>`.

### 7.11 MCP registry, `aimee mcp`

- `mcp audit`, list registered MCP servers and OSV verdicts.
- `mcp recheck [name]`, force a fresh OSV query.

(The MCP *bridge* is `aimee mcp-serve`; see [§22](#22-the-mcp-server).)

### 7.12 Sessions, `aimee session`

- `session list [--limit N]` (`session.list`) · `session show <id>` / `session get <id>` (`session.get`) · `session close <id>` (`session.close`).
- `session brief [--limit-tokens N]`, show the persisted session-start brief.

### 7.13 Worktrees, `aimee worktree`

- `worktree gc [--days N] [--force] [--dry-run]` (`worktree.gc`), garbage-collect abandoned session worktrees.

### 7.14 Providers and models, `aimee provider`, `aimee model`, `aimee agent`

`provider`:
- `provider list [--available] [--json]` · `provider show <name>` · `provider models <name> [--json]` · `provider test <name>` · `provider quota [name]`.

`model`:
- `model list [--capability NAME] [--open-weights]` · `model show <model>` · `model refresh`.

`agent`:
- `agent list` · `agent add` · `agent local [--provider openai|llama-eval]` · `agent remove` · `agent enable` · `agent disable`.
- `agent probe` · `agent token <name>` · `agent setup <provider-oauth>` · `agent episodes`.

`delegate-backend`:
- `delegate-backend list` · `delegate-backend exec --backend X --task-id Y [--image I] [--host H] [--no-hibernate] "<cmd>"`.

See [§18](#18-providers-models-and-agents).

### 7.15 Identity, `aimee identity`

- `identity show` · `identity snapshot [--out DIR]` · `identity diff [--flip-threshold N]`.
- `identity working-profile review [--limit N] [--confidence-min F] [--json]`.

### 7.16 Knowledge base, `aimee kb`

- `kb search <query>` · `kb status` · `kb build [--path DIR] [--project NAME] [--force]` · `kb update`.
- `kb docs push [--scope SCOPE] <file>...` · `kb ingest <file>` · `kb ingest status`.

See [§21](#21-the-knowledge-base).

### 7.17 Automation, `aimee trigger`, `aimee cron`

`trigger`:
- `trigger fire [--source S] [--task T] [--workspace WS] [--token TOK]` · `trigger list` · `trigger status [id]` · `trigger cancel [id]`.

`cron`:
- `cron list` · `cron add <id>` · `cron show <id>` · `cron run <id>` · `cron history <id>` · `cron enable <id>` · `cron disable <id> [--all]` · `cron remove <id>`.

See [§20](#20-triggers-cron-and-automation).

### 7.18 Benchmarks and optimization, `aimee optimize`

- `optimize run --suite <suite> [--arm <arm>]`: run the server-side
  `memory.benchmark` RPC. Synchronous retrieval suites are `code-graph-fusion`,
  `memory`, `corpus`, `memory-retrieval`, and `live`. Dataset and judge-style
  suites such as `locomo`, `longmemeval`, `locomo-qa`, and `longmemeval-qa`
  return an `async-only` response that points to the CLI/delegate benchmark path.
- `optimize compare --baseline <arm> --candidate <arm>`: compare benchmark arm
  metrics.
- Registered decision points include `briefing_style` (`compact`,
  `evidence_heavy`) and `guardrail_strictness` (`balanced`, `strict`). These
  residual UX/safety points are promotion/replay driven; online exploration
  remains default-off unless `intelligence.bandit.live_decision_enabled` is set.

### 7.19 Review surfaces

Unified `aimee review` is implemented in the legacy command table but is not
currently routed through the thin CLI. Use routed review/status surfaces that are
visible in `aimee help --all` for your installed build.

### 7.20 Insights, HUD, status, dogfood, graph, trajectory

- `insights [--days N]`, token-usage totals (default 30 days) (`insights.overview`).
- `hud`, real-time session telemetry (`hud.status`).
- `status`, system health overview (`server.health`).
- `dogfood tag` · `dogfood review` · `dogfood report [--month YYYY-MM] [--json]`.
- `graph sync-code` · `graph explain`.
- `trajectory export` · `trajectory batch`.

### 7.20 Manuscript, `aimee manuscript`

Novel/long-form writing helpers (client-local): `manuscript scenes` ·
`manuscript wordcount` · `manuscript outline` · `manuscript check [files…]`.

### 7.21 Server, profiles, admin

- `server start` · `server restart` · `server status` / `server health`, manage `aimee-server`.
- `profile create|list|show|delete|current [--force]`, configuration profiles.
- `git verify` / `verify` (`git.verify`), verify current changes before merge.
- `clean [--force]`, remove local config and integrations.

### 7.22 Hidden / integration entry points

Used by tool integrations, not typed by users directly:

- `hooks pre` / `hooks post`, PreToolUse / PostToolUse hook handlers.
- `session-start`, SessionStart hook handler.
- `mcp-serve`, the stdio MCP bridge.
- `eval run <suite_dir> [--ablation <preset|all>] [--runs N]` · `eval results [suite]`, eval harness.
- `migrate v2`, data migration utility.

---

## 8. Memory

Memory is the on-ramp to aimee's larger goal: a self-learning, company-wide
knowledge base. The tiers below are the durable store; the curator pipeline,
scope lattice, knowledge graph, and reflection that turn stored facts into
*learned*, cross-domain knowledge are described in
[How aimee learns](docs/KNOWLEDGE.md).

### 8.1 The four tiers

```
L0  Session       scratch / in-progress state   - lifetime: session only
L1  Recent        decisions, context, checkpoints - lifetime: ~30 days
L2  Long-term     stable facts, preferences, patterns - persistent
L3  Episodic      past attempts, outcomes, failures - persistent, decays
```

Promotion and decay are automatic: L0 folds into L1 at session end; L1 promotes
to L2 after ~3 reuses or confidence ≥ 0.9; unused L2 (60 days, confidence < 0.7)
demotes back to L1; delegation patterns auto-synthesize from the agent log into
L2 facts.

### 8.2 Kinds

| Kind | Meaning | Example |
|------|---------|---------|
| `fact` | Verified information | "PostgreSQL runs on port 5432" |
| `preference` | User style/behavior | "Prefers concise responses" |
| `decision` | Choice + rationale | "Chose JWT over sessions: stateless" |
| `episode` | Past-attempt narrative | "Refactored auth module" |
| `task` | Work item / goal | "Implement user authentication" |
| `scratch` | Temporary working data | Current task state |

### 8.3 Deduplication, contradiction, and versioning

New memories are normalized (lowercase, strip fillers, collapse whitespace),
checked for an exact key match, then for trigram similarity ≥ 0.7 against the top
same-kind candidates; matches merge (keeping higher confidence, incrementing
use-count) instead of inserting duplicates. When two memories conflict, negation
and word-similarity analysis flags the contradiction, reduces both confidences
(×0.7), and records it. Facts that change over time are *superseded*: the old
version gets a `#vN` suffix and a `valid_until`, the new one takes the canonical
key with `valid_from`.

### 8.4 Search

Fact search fuses lexical (DB2 structured) and dense (pgvector) candidates with
reranking. Conversation-window search combines term match, lexical recall, and a
2-hop entity-graph boost, then applies time decay and a tier weight
(raw 1.0 / summary 0.7 / fact 0.4). Internal diagnostic and answer-generation
helpers exist in the KB memory pipeline, but they are not current thin-client
commands unless they appear in `aimee help memory`.

`memory.ask` also carries a default-off answerability gate. When
`memory.abstain.enabled` is set, weak retrieved evidence is refused with
`no_answer=true`, empty rendered citations, and an `evidence_trace` containing
candidate ids, grounding scores, thresholds, and the abstention reason. The
context path uses the same rollout switch to withhold weak memory context and
emit `## Memory Answerability` instead of passing weak evidence to the model.
L4/L5 curated anchors bypass the gate.

### 8.5 Everyday usage

```bash
aimee memory store db-host "PostgreSQL at 10.0.0.5:5432" --tier L2 --kind fact
aimee memory search "database"
aimee memory list --tier L2 --kind fact --limit 20
aimee memory get <id>
```

---

## 9. Working memory

Working memory (`aimee wm`) is fast, session-scoped scratch stored in DB1 with an
optional TTL. Use it for the current task, intermediate state, or anything the AI
should remember within a session but not promote to durable knowledge.

```bash
aimee wm set current-task "Review PR #42 for security issues"
aimee wm set scratch.findings "3 issues, see notes" --category review --ttl 3600
aimee wm get current-task
aimee wm list --category review
```

Values fold into L1 memory at session end if relevant; otherwise they expire.

---

## 10. Rules and feedback

Rules are behavioral instructions derived from your feedback and from negative
outcomes. `aimee rules generate` builds the rules prompt injected at session
start; `aimee rules list` shows active rules; `aimee rules delete <id>` removes
one. Proposed-rule review code exists in the source tree, but review commands
are not part of the current thin-client route table unless shown by
`aimee help rules`.

Interaction-style learning watches negative feedback for recurring patterns
(e.g. "too verbose" twice → "User prefers concise responses") and stores the
inferred preference as an L2 memory injected into both primary and delegate
contexts.

---

## 11. Guardrails and safety

Before every tool call from the primary agent or a delegate, aimee classifies
the target path and applies policy:

```
Edit/Write/Bash → classify path
  ├─ sensitive (.env, credentials, keys)         → BLOCK (exit 2)
  ├─ real workspace path with active worktree    → BLOCK + redirect to worktree
  ├─ planning mode active and write op           → BLOCK writes
  ├─ anti-pattern match                           → WARN (stderr, non-blocking)
  └─ off-scope vs active task (drift)             → WARN (stderr)
                                                  → otherwise ALLOW (exit 0)
```

**Modes** (`guardrail_mode` in `aimee.yaml`):

| Mode | Amber (warn) | Red (block) |
|------|--------------|-------------|
| `approve` (default) | silently allow | block + inform |
| `prompt` | block + inform | block + inform |
| `deny` | silently block | silently block |

**Planning mode** locks all write operations (Edit, Write, MultiEdit,
destructive commands); read-only operations are always allowed. **Anti-patterns**
come from negative-feedback rules (weight ≥ 50), failed decisions, and manual
additions; matches warn on stderr. **Drift detection** warns when edits stray
from the scope of the active in-progress task (key terms from the task title,
referenced projects, and subtasks).

Guardrails are enforced *server-side*, clients cannot bypass them. See
[`docs/SECURITY.md`](docs/SECURITY.md) for the trust model and
`src/guardrails.c` / `src/guardrails_orchestrator.c` for the implementation.

---

## 12. The code index

aimee indexes your code into DB2 so the AI can look up symbols instead of
grepping. It extracts definitions across many languages (JS/TS, Python, Go, C/
C++, C#, shell, CSS, Dart, Lua, and more).

```bash
aimee index scan --force            # build/refresh the index
aimee index find handleLogin        # locate a symbol
aimee index callers handleLogin     # who calls it
aimee index structure src/auth.c    # definitions in a file
aimee index blast-radius src/db.c   # files impacted by changing this one
aimee code audit --graph --project my-app --json
```

`aimee code audit` has two modes. The local mode scans a working tree for file
health signals such as untested files and TODO/FIXME markers; its `--fix` flag
does not rewrite files yet and reports that no safe automatic fixes are
available. The graph mode calls `/v1/code/audit` on `aimee-server`, which proxies
to `aimee-kb`; it requires the server and KB to be reachable and the project to
have code graph/index rows. The graph response includes dead exports, import
cycles, exact body-hash clone groups, near-clone pairs, and clone threshold
metadata such as `clone_min_lines`.

`find_symbol`, `preview_blast_radius`, and `ast_grep_search` are also exposed as
MCP tools (see [§22](#22-the-mcp-server)), so a tool that speaks MCP can use the
index directly. PostToolUse re-indexes edited files automatically.

---

## 13. Delegation

Delegation offloads bounded work to the cheapest capable model. The router reads
`agents.json`, finds the lowest `cost_tier` agent whose `roles` include the
requested role, and runs it; on failure it walks the `fallback_chain` or retries
with `fallback_model`.

```bash
aimee delegate review "Review this PR for security issues"
aimee delegate code --tools "Add tests for the auth module"
aimee delegate summarize --files notes.md "Summarize to 5 bullets"
aimee delegate execute --tools --background "Migrate config to YAML"
aimee delegate roundtable "Draft a migration proposal" --rounds 3
aimee delegate roundtable "Review this design" --mode review --turns sequential --brief "Check authorization and cancellation paths"
```

**Roles:** `code`, `review`, `explain`, `refactor`, `draft`, `execute`,
`summarize`, `format`, `search`, `diagnose`, `validate`, `reason`.

**Modes:** read-only delegates inspect the parent session worktree directly;
`--tools` enables bash/read/write in an isolated sibling worktree;
`--background`/`--durable` create a server-owned job that survives client exit
(inspect with `aimee jobs ...`).

**Provider request formats:** `openai` (`/v1/chat/completions`), `chatgpt`
(`/backend-api/codex/responses`), `anthropic` (`/v1/messages`). **Auth types:**
`none` (Ollama), `bearer` (OpenAI/Together/Groq), `oauth` (Codex/Gemini
subscriptions), `x-api-key` (Anthropic).

**Cross-verification:** `--verify CMD` runs a build/test after delegation and
fails (exit 3) if it does not pass; `aimee verify` can send your diff to a
delegate for review. Full guide: [`docs/DELEGATES.md`](docs/DELEGATES.md).

**Roundtable:** `aimee delegate roundtable` reuses `ensemble.enabled`,
`ensemble.reference_models`, `ensemble.aggregator`, `ensemble.min_successful`,
and `ensemble.max_cost_usd` (optional; unset or `0` means no cost cap, the
default — set a positive value to cap a run). The panel and aggregator ship
configured, so the roundtable runs with no setup. `roundtable.max_rounds` defaults to `3`,
`roundtable.converge_threshold` to `10`, `roundtable.deadline_ms` to `600000`,
and `roundtable.turns` to `parallel`. Draft mode returns a shared artifact;
review mode returns a consolidated review, and `--apply` asks a final draft turn
to apply that review.

---

## 14. Roadmaps and the autonomous loop

A roadmap decomposes a goal into a `milestone → slice → task` tree of validated
artifacts (DB2). The roadmap and auto-loop implementation modules are present,
but they are not current thin-client commands. The routed workflow today is
reviewed delegate packet planning plus coordinated jobs.

```bash
aimee delegate plan docs/proposals/pending/example.md --output /tmp/plan.json
aimee delegate launch /tmp/plan.json --parallel 3
aimee job status <id>
```

The older roadmap/auto modules describe validated milestone/slice/task trees and
single-track autonomous dispatch, but they are not current thin-client commands.
The routed path today is reviewed delegate packet planning plus coordinated jobs.

---

## 15. The work queue

The work queue coordinates tasks across sessions: one session adds items,
another claims and completes them, with stale-claim garbage collection.

```bash
aimee work add "Fix flaky test in auth_test.c"
aimee work add-batch --from-proposals      # seed from accepted proposals
aimee work claim                            # take the next ready item
aimee work complete <id>                    # or: work fail <id>
aimee work board                            # kanban view
aimee work gc --max-age 24                  # release stale claims
```

`work sync-proposals` closes items whose underlying proposal has moved;
`work stats` reports queue health.

---

## 16. Skills and toolsets

**Skills** are project-scoped process guides (markdown with frontmatter) that
shape agent behavior, e.g. *systematic-debugging*, *test-driven-development*,
*recall-before-asking*, *verification-before-completion*. Bundled skills install
to `~/.local/share/aimee/skills/`; manage them with `aimee skill ...`
(`list`/`show`/`create`/`edit`/`patch`/`lint`/`eval`/`archive`/`lifecycle`/`pin`).
`aimee skill autostub` proposes skill capabilities for tools that lack coverage.

**Toolsets** are composable named bundles of tools. `aimee toolset list/show/
resolve` inspect them; select one at runtime with the `AIMEE_ACTIVE_TOOLSET`
environment variable or `toolsets` config.

---

## 17. Sessions and worktrees

Each session has its own state file (`session-<id>.state`), git worktree, and
branch under `~/.config/aimee/worktrees/<id>/<project>/`. This is what lets two
sessions run in parallel without clobbering each other, and what guardrails
enforce by redirecting writes. Inspect sessions with `aimee session list/show`;
reclaim abandoned worktrees with `aimee worktree gc`. Multi-repo coordination is
described in [`docs/WORKSPACES.md`](docs/WORKSPACES.md).

---

## 18. Providers, models, and agents

- **Providers** are inference backends with credentials and catalogs. `aimee
  provider list --available` shows which have working credentials; `aimee
  provider test <name>` probes connectivity; `aimee provider models <name>` lists
  the catalog; `aimee provider quota` shows usage.
- **Models** carry capability metadata (context window, cost, cutoff, flags).
  `aimee model list --capability tools`, `aimee model show <model>`,
  `aimee model refresh`.
- **Agents** are delegate definitions in `agents.json`. Add one interactively
  with `aimee agent setup <provider>` (handles OAuth device flows for
  subscription providers), or directly with `aimee agent add` / `aimee agent
  local`. Toggle with `enable`/`disable`, inspect history with `agent episodes`.

The primary agent (the surface you chat with) is configured separately from
delegates, by `aimee.yaml`'s `provider`/model keys or by the launch/chat/webchat
surface, while `agents.json` defines the delegate fleet.

---

## 19. Identity, charter, and personas

The **charter** is aimee's pipeline for proposing durable behavioral artifacts
(rules, epistemic directives, anti-patterns, working-profile traits) that can be
reviewed before they take effect. The unified review loop is implemented in the
legacy command modules but is not currently routed by the thin CLI.
`aimee identity show/snapshot/diff` inspects the working profile and tracks how
it changes over time. **Personas** (see [`docs/personas.md`](docs/personas.md))
let a session adopt a named voice/behavior profile, settable per session in chat
and webchat.

---

## 20. Triggers, cron, and automation

- **Triggers** queue event-driven autopilot runs: `aimee trigger fire --source
  ci --task "fix build" --workspace myrepo`. List/inspect/cancel with
  `trigger list/status/cancel`. A common source is a webhook relay
  (`scripts/github-webhook-relay.py`).
- **Cron** schedules recurring server-side jobs: `aimee cron add <id>` (define),
  `cron list/show/history`, `cron run <id>` (run now), `cron enable/disable/
  remove`. Jobs are stored in `aimee.yaml`'s `cron_jobs` and run by the server's
  scheduler/watchdog.

---

## 21. The knowledge base

The KB (`aimee-kb`) ingests documents and code into DB2 and answers semantic and
full-text queries. A shared `aimee-kb` is what makes aimee a *company-wide*
knowledge base: it distills every team's documents, across all domains, into
one graph and synthesizes across them. See [How aimee learns](docs/KNOWLEDGE.md)
for the vision and the mechanisms; this section covers the commands.

```bash
aimee kb build --path ./docs --project myproj   # build from a doc tree
aimee kb update                                  # incremental refresh
aimee kb search "how does auth work"
aimee kb status                                  # vectors, models, health
aimee kb docs push --scope project README.md     # stage docs for ingest
```

Ingest runs through staged document upload → curator extraction (structured
knowledge from prose/code/API docs) → embedding → review. `aimee-kb` serves its
contract over an **HTTP `/v1` API on port 8741**: this is the only transport;
the legacy Unix-socket RPC was retired in #2747. The server's KB client, thin
clients, and containerized deployments all reach it that way, via
`AIMEE_KB_API_URL` (plus an optional bearer in `AIMEE_KB_API_BEARER_TOKEN`). The
KB must be running (its service unit, container, or `aimee-kb --http-port=8741`)
before the server can use kb-backed features; the server no longer autostarts it.
The contract is `api/openapi-v1.yaml`; the generated markdown is `docs/gen/api-v1.md`.

---

## 22. The MCP server

`aimee mcp-serve` is a stdio JSON-RPC 2.0 MCP server built into the `aimee`
binary. It exposes aimee's knowledge and delegation as MCP tools to any
MCP-capable client, forwarding calls to `aimee-server`.

| Tool | Returns |
|------|---------|
| `search_memory` | Matching facts (lexical + dense recall + rerank). |
| `list_facts` | Stored L2 facts. |
| `memory_briefing` / `memory_recall` | Structured memory context bundles for a task/session. |
| `get_host` / `list_hosts` | Host(s) and networks from `agents.json`. |
| `find_symbol` | Code symbol locations. |
| `ast_grep_search` | Structural AST pattern matches (`sg --json`). |
| `delegate` | Delegation result. |
| `preview_blast_radius` | File dependency impact. |
| `record_attempt` / `list_attempts` | Log / list delegation attempts. |
| `delegate_reply` | Follow up on a prior delegation. |
| `learning_review` | Review learning/curation candidates. |
| `session_search` | Search local session history. |
| `autopilot` | Drive the internal autopilot action surface for MCP callers. |

`install.sh`/`configure-hooks.sh` register `aimee mcp-serve` as an MCP server for
tools that support it (writing `<project>/.mcp.json`). `aimee mcp audit`/`mcp
recheck` gate registered MCP packages against OSV advisories.

---

## 23. Webchat and the browser UI

`aimee-webchat` serves a browser chat + dashboard. It is a thin client: it holds
no database and proxies to `aimee-server` over its `/v1` HTTP surface.

```bash
aimee-webchat --port 8080
```

It authenticates browser users with PAM (a self-signed TLS cert is generated if
none is provided), serves the React UI from `frontend/`, streams chat over SSE,
and proxies dashboard panels, collab-rule review, and a local channel board.
Treat it as a *semi-trusted*, same-host/trusted-LAN surface, see
[`docs/SECURITY.md`](docs/SECURITY.md).

---

## 24. The HTTP /v1 API

The server exposes a native HTTP /v1 REST surface (over `aimee-http.sock` or an
optional localhost TCP port) for programmatic and OpenAI-compatible access.

**OpenAI-compatible inference** (SSE streaming supported):
- `POST /v1/chat/completions`, `POST /v1/completions`, `POST /v1/embeddings`,
  `POST /v1/responses`.

**Runs:**
- `POST /v1/runs`, `GET /v1/runs/{id}`, `GET /v1/runs/{id}/events` (SSE replay),
  `POST /v1/runs/{id}/stop`.

**Read surfaces:**
- `GET /v1/health`, `/v1/version`, `/v1/capabilities`, `/v1/models`,
  `/v1/rules`, `/v1/notes`, `/v1/roadmap`, `/v1/agents`, `/v1/curiosity`,
  `/v1/kb/status`, `/v1/dashboard/memory`, `/v1/dashboard/reminders`,
  `/v1/persona`, `/v1/personas`, `/v1/personas/{name}`,
  `/v1/sessions/{id}/persona`.
- `POST /v1/kb/search`, `/v1/memory/recall`, `/v1/notes/search`.

**Conventions:** UDS callers authenticate by peer UID; TCP callers present an
`Authorization: Bearer <token>` (optionally `scope:`-prefixed to constrain
capabilities, a scoped token is denied on inference with a 403). `X-Request-ID`
is echoed and access is logged. The server contract source of truth is
[`api/openapi-server-v1.yaml`](api/openapi-server-v1.yaml). The generated
[`docs/gen/api-v1.md`](docs/gen/api-v1.md) documents the separate `aimee-kb`
HTTP API generated from [`api/openapi-v1.yaml`](api/openapi-v1.yaml).

---

## 25. Integrations

| Tool | Integration | Setup |
|------|-------------|-------|
| Claude Code | Hooks + MCP, or any primary model via the Anthropic ingress | `./install.sh` / `aimee claude-proxy enable` |
| Codex CLI | Hooks + MCP + local plugin, or any primary model via the Responses ingress | `./install.sh` / `~/.codex/config.toml` |
| OpenCode | TUI front end (`opencode attach`), any primary model via the OpenAI-compatible ingress | `./install.sh` |
| Gemini CLI | Hooks | `./install.sh` |
| Mistral Vibe | Provider-CLI primary + subscription-plan delegates (incl. `mistral-plan`) | `aimee agent setup mistral-plan` |
| GitHub Copilot | MCP server | `./install.sh` |
| VS Code | MCP tools in Copilot Chat agent mode, or aimee as an OpenAI-compatible model via the `/v1` API | [docs/VSCODE.md](docs/VSCODE.md) |

`configure-hooks.sh` registers PreToolUse/PostToolUse/SessionStart hooks and the
MCP server in each tool's settings. VS Code is wired MCP-only (no lifecycle
hooks); see [docs/VSCODE.md](docs/VSCODE.md) for both the MCP and OpenAI-endpoint
setups. Switching tools preserves all memory and
context because everything lives in the shared server/KB, not in the tool. Direct
Codex and Mistral primary sessions use server-side structured conversation state;
explicit legacy routes (e.g. `codex-cli`) still use the provider CLI.

### Claude Code on any primary model (Anthropic ingress)

aimee-server exposes the Anthropic Messages API at `POST /v1/messages` (with
streaming and `/v1/messages/count_tokens`). Point Claude Code at it and every
turn runs on aimee's configured **primary agent** — minimax, mistral, mimo,
gemini, openai, or anthropic — instead of Anthropic's models. It is a stateless
wire-format proxy: Claude Code keeps owning its own system prompt, history, and
tools (tool execution stays client-side); aimee only translates the wire format
and swaps the model. Switch models with `aimee primary <agent>`.

Enable it (writes `ANTHROPIC_BASE_URL` + `ANTHROPIC_AUTH_TOKEN` into
`~/.claude/settings.json`):

```sh
aimee claude-proxy enable http://127.0.0.1:8910 <server-bearer>
# or, with AIMEE_SERVER_URL / AIMEE_SERVER_TOKEN already set:
aimee claude-proxy enable
aimee claude-proxy disable   # restore Claude Code to Anthropic
```

This is **off by default** and reroutes **all** Claude Code sessions (including
running ones), so it is only ever changed by this explicit command. For a
one-off session without touching settings, set the env vars inline instead:

```sh
ANTHROPIC_BASE_URL=http://127.0.0.1:8910 ANTHROPIC_AUTH_TOKEN=<bearer> claude
```

(The server must be reachable over HTTP — loopback TCP with a bearer, or a
remote `AIMEE_SERVER_URL`.)

### Codex on any primary model (Responses ingress)

aimee-server also exposes the OpenAI Responses API at `POST /v1/responses` (with
SSE streaming), the sibling of the Claude Code ingress above. To the open-source
Codex CLI/TUI, aimee then behaves exactly like any other model: point Codex's
model provider at aimee and it is indistinguishable from `gpt-5.x`. Codex drives
its normal agent loop, aimee runs the turn on a registered primary-agent model
selected by the request `model` field, and aimee participates in Codex's tool
loop (it emits `function_call` items, Codex executes them in the user's
workspace, and returns `function_call_output`). `GET /v1/models` lists the
registered primary-agent models so Codex can list and switch models from its UI.

Add a provider to `~/.codex/config.toml` and select it:

```toml
[model_providers.aimee]
name = "aimee"
base_url = "http://<aimee-host>:<port>/v1"
wire_api = "responses"
env_key = "AIMEE_API_KEY"          # aimee loopback bearer
requires_openai_auth = false

model_provider = "aimee"
model = "aimee"                     # or any slug from GET /v1/models
```

Like the Anthropic ingress, this is a stateless wire-format proxy: Codex keeps
owning its prompt, history, and tool execution; aimee translates the wire format
and swaps the model. Switch models with `aimee primary <agent>`.

### Any OpenAI-compatible front end (OpenCode, VS Code, custom clients)

Tools that speak the OpenAI Chat Completions wire format point at
`POST /v1/chat/completions` (SSE streaming supported) and run on aimee's primary
agent the same way. This covers OpenCode (also launchable as a TUI through
`opencode attach`), VS Code configured with aimee as an OpenAI-compatible model
(see [docs/VSCODE.md](docs/VSCODE.md)), and any custom client. Whatever front end
you choose, the memory, guardrails, and delegation are identical because they
live in the shared server and KB, not in the tool.

---

## 26. Server and service management

`aimee-server` runs as a long-lived service. In the recommended Docker deployment
it is a container supervised by Docker's restart policy; manage it with `docker
compose` ([§27.1](#271-containerized-deployment)). On a source install it is a
systemd user unit, launchd agent, or Windows service wrapper installed by the
platform scripts. Either way the thin CLI does not implicitly spawn a server for
ordinary RPC commands. To manage a source-installed server explicitly:

```bash
aimee server status      # health
aimee server start       # cross-platform fallback spawn if no service is running
aimee server restart     # SIGTERM + respawn
aimee status             # full system health overview
aimee hud                # live session telemetry
```

**As a managed service:**

```bash
# Linux (systemd user units installed by install.sh)
systemctl --user enable --now aimee-kb.service aimee-server.service
systemctl --user status aimee-server
journalctl --user -u aimee-server -f

# macOS (launchd agents)
launchctl load ~/Library/LaunchAgents/com.aimee.kb.plist
launchctl load ~/Library/LaunchAgents/com.aimee.server.plist
```

`aimee-kb` starts first; `aimee-server` orders after it. Units set memory/task
limits and restart on failure. Logs go to the journal (Linux) or
`~/Library/Logs/aimee/` (macOS), plus
`~/.config/aimee/server.log`.

---

## 27. Operations and deployment

### 27.1 Containerized deployment

Running the services in containers is the recommended deployment: developers
install only the thin client ([§3.2](#32-install-the-thin-client)) and point it at
the server. Four compose files ship, built from three images: `aimee-server`
(`Dockerfile.server`), `aimee-kb` (`Dockerfile`), and the co-located
`aimee-server+kb` (`Dockerfile.combined`). Every stack also brings up a
`pgvector/pgvector:pg16` Postgres and a CPU embedder sidecar
(`Dockerfile.embedder`); the kb auto-applies its DB2 schema (`pg_trgm`/`vector`
extensions + tables) on first boot. The sidecar ships in two tiers (embedder +
matching reranker in one image): the default `aimee-embedder` (4b/2560 + 1b
reranker) and the lighter `aimee-embedder-0.6b` (0.6b/1024 + 400m reranker) via
`AIMEE_EMBEDDER_IMAGE` + `embedding_dim: 1024`. Trade-offs:
[retrieval-stack.md](docs/retrieval-stack.md#choosing-a-tier).

| Compose file | Brings up | Use when |
|--------------|-----------|----------|
| `compose.combined.yaml` | **`aimee-server+kb`** (one container) + Postgres + embedder | **Recommended default.** Both binaries co-located; server `/v1` on `:8740`, kb `:8741`. |
| `compose.server.yaml` | `aimee-server` + `aimee-kb` + Postgres + embedder | Split stack: scale/update/place server and kb independently. |
| `compose.yaml` | `aimee-kb` + Postgres + embedder | The knowledge service alone: building block for a shared/scaled kb. |
| `compose.server-standalone.yaml` | `aimee-server` only (SQLite DB1, no kb) | DB1-backed `/v1` with no shared knowledge. |

**Bring up the recommended (combined) stack:**

```bash
docker compose -f compose.combined.yaml up --build -d
curl -H 'Authorization: Bearer aimee-local-dev' http://localhost:8740/v1/health
curl -H 'Authorization: Bearer aimee-local-dev' http://localhost:8740/v1/kb/status
```

The combined container runs **both** aimee binaries: the kb on loopback `:8741`
inside the container, the server fronting `:8740` with
`AIMEE_KB_API_URL=http://127.0.0.1:8741`. The split stack
(`compose.server.yaml`) instead runs `aimee-kb` as its own container the server
reaches at `http://aimee-kb:8741`. Add `--profile llm` to any stack to bring up a
local llama.cpp server for the synthesis/curator passes. Each container runs as a
non-root user; the kb uses Python sidecars (`scripts/embed-remote.py`,
`llm-chat.py`, `curator-extract.py`, `learning-synthesize.py`) for embeddings,
synthesis, and curation, with container defaults from `deploy/container/aimee.yaml`.

**Container lifecycle**: the binaries run as long-lived container processes
(PID 1), supervised by Docker's restart policy, not systemd/launchd:

```bash
docker compose -f compose.combined.yaml ps           # container + health status
docker compose -f compose.combined.yaml logs -f      # follow logs (add a service to scope)
docker compose -f compose.combined.yaml restart aimee-server-kb
docker compose -f compose.combined.yaml down         # stop + remove containers (volumes persist)
docker compose -f compose.combined.yaml down -v      # also DROP data volumes (destroys DB2 + state)
docker compose -f compose.combined.yaml up -d --build # update: rebuild images, recreate containers
```

**State and volumes.** Durable data lives in named volumes, not the container
filesystem, so recreate is safe and only `down -v` erases it: `*-postgres` holds
DB2 (`aimee_shared`); `*-home` (`/var/lib/aimee`) holds server/kb runtime state
(DB1 SQLite + config); `*-workspaces` (`/var/lib/aimee-workspaces`,
`AIMEE_WORKSPACES_DIR`) holds the mirror-tier bare mirrors + reconstructed
worktrees; `*-models` volumes cache embedder/LLM weights. The server's worker
threads need a 64 MB stack, so every server container sets `ulimits.stack:
67108864` (a plain `docker run` must pass `--ulimit stack=67108864`). Override the
baked `aimee-local-dev` bearer and other config by mounting your own `aimee.yaml`
at `/var/lib/aimee/aimee.yaml` (see `compose.remote-writes.combined.yaml`).

**Verify a stack end to end.** Each topology ships a smoke test that brings it up,
waits for health, exercises the live surface (DB + vector readiness, search,
embeddings, and the server→kb path), then tears down; `e2e-matrix.sh` runs several
and prints one pass/fail table:

```bash
scripts/aimee-combined-docker-smoke.sh --up --down            # combined server+kb
scripts/aimee-server-docker-smoke.sh --up --down              # split server + kb
scripts/aimee-kb-docker-smoke.sh --up --down                  # kb-only
scripts/aimee-server-standalone-docker-smoke.sh --up --down   # server standalone
scripts/e2e-matrix.sh --only T1,T2,T3,T4                      # all Docker topologies
```

A proxy front-end is available via `Dockerfile.proxy` + `docker-compose.proxy.yml`.

### 27.2 Health and observability

- `GET /v1/health` (server and KB) for liveness.
- `aimee status`, `aimee hud`, `aimee insights --days 30` for operational state
  and token spend.
- `otel` config in `aimee.yaml` for OpenTelemetry export.
- `server.log`, `audit.log` under `~/.config/aimee/`.

### 27.3 Backup

DB2 (`aimee_shared`) holds durable knowledge, back it up with standard
Postgres tooling (`pg_dump`). In a shared KB deployment, treat it as shared
infrastructure and back it up accordingly. DB1 (`aimee.db`) is local
`aimee-server` runtime state and is generally reconstructable; back it up if you
depend on session history.

### 27.4 Resource tuning

Thread pools and concurrency are tunable in `aimee.yaml`
(`compute_threads`, `worker_threads`, `background_threads`, `concurrency.
per_model.<model>`). `ecomode: true` reduces resource use. Service-unit memory
limits cap the server and KB independently.

### 27.5 Scaling and multi-user deployment

aimee's scaling model follows its storage boundary exactly. The two tiers scale
differently *by design*, and understanding which is which is the whole story:

| Tier | Scope | How it scales |
|------|-------|---------------|
| `aimee-server` | **1:1 with a user** | Vertical only. One server per OS login, on that user's machine. |
| `aimee-kb` | **Many users** | Horizontal. N stateless replicas behind a load balancer over one Postgres. |
| Postgres (DB2) | The shared substrate | Standard Postgres scaling: instance sizing, pooling, read replicas; pgvector → pgvectorscale for large vector corpora. |

#### `aimee-server` is one-per-user

`aimee-server` owns DB1, the local, same-user runtime state (sessions, working
memory, conversation windows, checkpoints, jobs, secrets), and authenticates
callers by `SO_PEERCRED` peer UID. Its trust model is "same Unix user = same
principal." It is therefore **single-tenant and local by construction**: you do
not shard it, replicate it, or place it behind a load balancer. Each developer
(each OS login) runs exactly one `aimee-server`, normally as a systemd user unit
or launchd agent. You scale a single user's server *vertically* (more in-flight
delegates and parallel tool calls) with the thread-pool and concurrency knobs in
[§27.4](#274-resource-tuning); that does not, and is not meant to, make it serve
other users.

#### `aimee-kb` is the horizontally-scalable, many-user tier

`aimee-kb` owns DB2 and is where multi-user scale-out lives. Three properties make
it horizontally scalable:

1. **State lives in Postgres, not in the process.** A KB instance holds only
   transient state (connection pools, request buffers, worker loops); all durable
   knowledge is in DB2. Each instance is effectively a **stateless request server
   over a shared database**.
2. **It is reachable over the network.** KB serves its full `/v1` contract over
   HTTP on `:8741` (`AIMEE_KB_API_URL`, optional bearer token via
   `AIMEE_KB_API_BEARER_TOKEN`), the only transport since the Unix-socket RPC was
   retired in #2747. Many `aimee-server` instances (many users, on many
   machines) can point at one KB endpoint.
3. **Concurrent instances are safe with no external coordinator.** The background
   pipeline (ingest, curator, embedding, index-update) claims work directly off
   DB2 queues with `FOR UPDATE SKIP LOCKED`, so two workers never claim the same
   row. Postgres *is* the coordinator.

So the scale-out recipe is the standard stateless-web-tier one: **run several
`aimee-kb` replicas behind a load balancer, all pointed at one Postgres.** Query
traffic (search, recall, index reads, the hot path the servers hit) fans out
across replicas; the workers on each replica drain the shared queues
cooperatively. Point every server at the load-balanced KB with `AIMEE_KB_API_URL`
(or the `kb` block in `aimee.yaml`).

#### The database is just standard Postgres

DB2 is one ordinary Postgres database (`aimee_shared` by default) with the
`pg_trgm` and `vector` (pgvector) extensions; there is no bespoke datastore. You
scale it with the normal Postgres playbook:

- **Vertical sizing** of the Postgres instance (CPU/RAM/IOPS).
- **Connection pooling** (e.g. PgBouncer) in front of Postgres; each KB instance's
  own pool is bounded by `db2_pool_size` (1-32, default 8).
- **Read replicas** to fan out query load when one primary is saturated.

Vector search rides inside the same Postgres and scales with it:

- **pgvector HNSW is the default**: for memory vectors always, and for all
  small-to-medium corpora. Nothing extra to install; this is what a fresh
  `install.sh` gives you.
- **pgvectorscale (StreamingDiskANN) is the opt-in scale-up** for large corpora.
  It is *additive*: built on top of pgvector, it reuses the same `vector` column
  type and distance operators and only adds the `USING diskann` index, disk-backed
  with bounded build memory, for the large/RAM-constrained regime where HNSW's
  in-memory graph becomes the bottleneck. Select it per corpus table:

  ```yaml
  db2:
    vector:
      corpus_index: auto          # auto | hnsw | diskann
      corpus_diskann_threshold: 1000000   # rows/table before "auto" picks diskann
  ```

  `auto` uses HNSW until a corpus table exceeds the threshold *and* the
  `vectorscale` extension is installed, then picks diskann; it falls back to HNSW
  with a notice when the extension is absent (so schema apply never hard-fails).
  Memory/hot-path vectors stay HNSW unconditionally.
- **Switching index type is a reindex, not a migration.** Data and queries are
  identical across HNSW and diskann; `aimee kb repair --reindex-corpus` rebuilds
  corpus indexes to the configured/`auto` type with no re-embedding and no query
  rewrite. This is what makes "started local on pgvector, grew, flipped to
  pgvectorscale" a config change rather than a data move.

#### Deployment topologies

| Topology | Shape | When |
|----------|-------|------|
| **Containerized server + KB (default)** | `aimee-server` + `aimee-kb` as containers (combined or split) + Postgres + embedder ([§27.1](#271-containerized-deployment)); developers run only the thin client. | The recommended deployment for one user or a team. |
| **Single developer, source build** | One `aimee-server` + one local `aimee-kb` + local Postgres, all on one host via service units. | The `install.sh` no-Docker setup. |
| **Shared KB** | Many users' `aimee-server` instances point at one `aimee-kb`/Postgres over HTTP. DB1 stays per-user/per-machine; only KB-scoped knowledge crosses the boundary. | A team or a single user across several machines wanting shared knowledge. |
| **Scaled KB** | Several `aimee-kb` replicas behind a load balancer (`:8741`) over one Postgres (with pooling and, if needed, read replicas + pgvectorscale). | Many users and/or a large corpus where one KB instance or plain HNSW is the bottleneck. |

In every topology the contracts are identical: thin clients speak `/v1` to their
per-user server (local `aimee-http.sock` or a remote `host:port`); servers speak
the typed KB `/v1` HTTP API to `aimee-kb`; the storage boundary holds. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §11 for the architectural view.

---

## 28. Troubleshooting

| Symptom | Check |
|---------|-------|
| `aimee status` cannot reach the server | Is `aimee-server` running? `aimee server start`; check `~/.config/aimee/server.log`; confirm `aimee-http.sock` exists and is owned by you. |
| KB queries fail / `kb status` errors | Is Postgres up with `aimee_shared` and the `vector`/`pg_trgm` extensions? Is `aimee-kb` running and `db2_url` correct? |
| Memory search returns nothing | Check `aimee memory list` to confirm stored entries, `aimee kb status` for vector/KB health, and `aimee index scan --force` when the missing evidence is code-derived. |
| Symbol lookups empty | `aimee index scan --force`; confirm `universal-ctags` is installed. |
| Delegate fails immediately | `aimee provider test <name>`; check credentials/`auth_cmd`; inspect `aimee delegate log`. |
| Writes blocked unexpectedly | Planning mode active, or a worktree redirect, check `guardrail_mode` and `aimee session show`. Set `AIMEE_ANTIPATTERNS_BYPASS=1` only to confirm a diagnosis. |
| Build fails on `make check-linking` | A tier boundary was violated (libpq in server, SQLite in KB). See [`src/README.md`](src/README.md). |
| Webchat login fails | PAM service `aimee-webchat` configured? Check login rate limiting and TLS cert. |

Raise log verbosity with `AIMEE_LOG_LEVEL=debug`. For unclean server exits,
shutdown forensics are recorded to DB1 and surfaced by `aimee status`.

Feature implementation status is tracked in [`docs/STATUS.md`](docs/STATUS.md);
platform support in [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md).

---

## Appendix A, Environment variables

| Variable | Effect |
|----------|--------|
| `AIMEE_HOME` | Override the config/state root (default `~/.config/aimee`). |
| `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN` | Thin-client target: remote `aimee-server` URL (`http[s]://host:port`) + bearer. Overridden by `--server`/`--server-token`; persisted form is `<aimee_home>/remote.conf`. |
| `AIMEE_TLS_INSECURE` | Skip TLS certificate verification for the thin client's `https://` connections (self-signed/dev only). |
| `AIMEE_API_ENDPOINT` / `AIMEE_API_BEARER` / `AIMEE_API_CLIENT_TRANSPORT` | Lower-level CLI transport: endpoint (`tcp:host:port` or `unix:/path`), bearer, and `http`/`socket` selector. Mirrors `aimee.api.client_endpoint` / `bearer_token` / `client_transport` in `aimee.yaml`. |
| `AIMEE_PROFILE` | Select a configuration profile. |
| `AIMEE_MODE` | Operating mode (e.g. planning vs implement). |
| `AIMEE_MODEL` / `AIMEE_EFFORT` | Override model / reasoning effort for a run. |
| `AIMEE_LOG_LEVEL` | Log verbosity (`debug`, `info`, …). |
| `AIMEE_SESSION_ID` | Bind the current process to a session id. |
| `AIMEE_SESSION_START_VERBOSE` | Include broad diagnostic sections in the session brief. |
| `AIMEE_HOOK_CLIENT` | Identify the calling tool to the hook layer. |
| `AIMEE_MCP_CWD` | Working directory for the MCP bridge. |
| `AIMEE_ACTIVE_TOOLSET` / `AIMEE_TOOLSETS_CONFIG` | Select / locate the active toolset. |
| `AIMEE_GUARDRAILS_PATH` | Override guardrail policy path. |
| `AIMEE_ANTIPATTERNS_BYPASS` | Bypass anti-pattern warnings (diagnostic only). |
| `AIMEE_CONTEXT_NO_KB` / `AIMEE_NO_CACHE` | Skip KB context / disable caches. |
| `AIMEE_DELEGATE_DEPTH` / `AIMEE_PARENT_DELEGATION_ID` | Delegation nesting controls. |
| `AIMEE_DELEGATE_WORKTREE_ROOT` | Root for delegate worktrees. |
| `AIMEE_DELEGATE_SOURCE_PATHS` / `AIMEE_DELEGATE_SOURCE_AUTHORITY` | Delegate source-access policy. |
| `AIMEE_DELEGATE_HEARTBEAT_MONITOR` | Enable delegate heartbeat monitoring. |
| `AIMEE_PARALLEL_MAX` / `AIMEE_VERIFY_PARALLEL` | Parallelism caps for delegates / verification. |
| `AIMEE_VERIFY_STEP_TIMEOUT_MS` | Per-step verification timeout. |
| `AIMEE_MEMORY_WEIGHT_PROFILE` | Retrieval-weight profile override. |
| `AIMEE_KB_API_URL` / `AIMEE_KB_API_BEARER_TOKEN` | KB `/v1` HTTP endpoint and bearer token. Required for kb-backed (shared/vector) features; the server no longer autostarts `aimee-kb`. |
| `AIMEE_KB_NO_AUTOSTART` | Deprecated no-op (the kb socket autostart was retired in #2747; the server only reaches kb via `AIMEE_KB_API_URL`). |
| `AIMEE_DB1_URL` / `AIMEE_DB2_URL` | Storage URLs used by containerized/explicit deploys: DB1 `sqlite:///…` (server) and DB2 `postgresql://…/aimee_shared` (kb). |
| `AIMEE_EMBEDDER_URL` | Embedding service endpoint for the kb (e.g. the embedder sidecar `http://embedder:8080`). |
| `AIMEE_SERVER_HTTP_BIND` / `AIMEE_KB_HTTP_BIND` | Bind the server/kb `/v1` listener on `0.0.0.0` (set `1` in containers so the published port is reachable). |
| `AIMEE_WORKSPACES_DIR` | Mirror-tier workspace root (bare mirrors + reconstructed worktrees); containers mount a volume here (`/var/lib/aimee-workspaces`). |
| `AIMEE_KB_MODE` | Install-time selector (`local`/`remote`) consumed by `install-deps.sh`/`install.sh`. |
| `AIMEE_SERVER_STARTUP_FD` | Internal: server startup handshake fd. |
| `AIMEE_DOCKER_BIN` / `AIMEE_DOCKER_WORKDIR` | Docker backend for delegate execution. |
| `AIMEE_SSH_BIN` | SSH binary for remote delegate/host access. |
| `AIMEE_WORKTREE_GC` / `AIMEE_WORKTREE_GC_DAYS` | Worktree GC policy. |
| `AIMEE_FORENSICS_DIR` | Shutdown-forensics output directory. |
| `AIMEE_BUNDLED_SKILLS_DIR` | Override bundled-skills location. |
| `AIMEE_ENABLE_PROJECT_PLUGINS` | Allow project-local plugins. |
| `AIMEE_MODELS_DEV_SNAPSHOT` / `AIMEE_MODEL_CAPABILITY_OVERRIDES` | Model metadata overrides. |
| `AIMEE_INSTALL_PREFIX` | Install prefix used by scripts. |

Provider credentials are also read from the usual provider variables
(`OPENAI_API_KEY`, `GEMINI_API_KEY`/`GOOGLE_API_KEY`, `MISTRAL_API_KEY`, …) and
from `~/.vibe/.env` for Mistral routes.

---

## Appendix B, Files and paths

```
~/.config/aimee/
  aimee.yaml                  # main configuration
  agents.json                 # delegate agents + network inventory
  aimee.db (+ -wal, -shm)     # DB1 local state (SQLite)
  aimee-http.sock             # aimee-server /v1 HTTP Unix socket (loopback)
  remote.conf                 # persisted thin-client remote target (aimee remote set)
  server.token                # (legacy; unused since the NDJSON RPC socket was removed)
  server.log / audit.log      # server + audit logs
  aimee.pid / *.lock          # process/lifecycle files
  session-<id>.state          # per-session state
  worktrees/<id>/<project>/   # per-session git worktrees
  projects/<name>.md          # auto-generated project descriptions
  tls/cert.pem, key.pem       # self-signed TLS (webchat)

~/.local/bin/                 # installed binaries (aimee, aimee-server, ...)
~/.local/share/aimee/skills/  # bundled skills

<project>/.aimee/project.yaml     # per-project metadata (build/test/lint)
<project>/.aimee/roadmap/         # roadmap projections + HTML reports
<project>/.mcp.json               # MCP server registration
<workspace>/aimee.workspace.yaml  # multi-repo workspace manifest
```

DB2 lives in PostgreSQL (database `aimee_shared` by default), not on the
filesystem under `~/.config/aimee`. That Postgres instance can be local to one
machine or shared by multiple `aimee-server` instances, depending on deployment.

---

## Appendix C, Glossary

| Term | Meaning |
|------|---------|
| **Primary agent** | The AI coding tool you interact with directly. |
| **Delegate agent** | A sub-agent in `agents.json` that handles offloaded work. |
| **DB1 / DB2** | Pinned storage tiers: local SQLite owned by `aimee-server` / local-or-shared Postgres + pgvector owned by `aimee-kb`. |
| **L0-L3** | Memory lifecycle tiers: session / recent / long-term / episodic. |
| **Worktree** | Per-session isolated git working tree + branch. |
| **Guardrail** | Server-side safety check run before each tool call. |
| **Hook** | SessionStart / PreToolUse / PostToolUse integration point. |
| **MCP** | Model Context Protocol; aimee exposes tools via `aimee mcp-serve`. |
| **Capability token** | Bearer token granting scoped server operations. |
| **Charter** | Pipeline proposing durable behavioral artifacts for review. |
| **Roadmap / auto loop** | Goal decomposition tree + deterministic dispatch loop. |
| **Toolset** | A named, composable bundle of tools. |
| **KB** | The knowledge base owned by `aimee-kb` (DB2), local or shared depending on deployment. |

---

*This manual reflects the codebase at the time of writing. The installed build is
always authoritative, use `aimee help --all` and `aimee help <command>` for the
exact commands and flags in your version.*
