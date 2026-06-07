# Proposal: Self-sufficient thin client — in-process execution plane + `/v1` data plane

One binary an end user installs to use *any* aimee (local files or a shared
remote), with **no separate `aimee-server` process** to run or manage.

## Summary

Today the user-facing `aimee` binary is a pure client. The **data plane**
(memory / kb / rules / index / sessions / notes) already works against any
aimee over first-class `/v1` HTTP — proven by thin-client → remote NAS. But the
**execution plane** (the SessionStart/pre/post hooks, `chat`, `mcp-serve`,
`launch`, `delegate`, `agent setup`) routes through
`cli_ensure_server_for_method()`, which only discovers a **co-located** server
over a local socket. So a remote/thin deployment cannot run the agent at all,
and a normal dev box needs a second component (`aimee-server`, systemd-managed)
just to make the Claude Code integration work.

This proposal folds the execution engine into the `aimee` binary so it runs the
agent/hooks/tools/MCP **in-process, locally**, while reaching shared memory/kb
over `/v1` against a configurable backend (local file or remote). Result: a
single install that is "everything an end user needs," and the execution plane
stops being hard-bound to a co-located daemon.

Phase 0 already shipped (#69): `session-start` now serves itself over remote
`/v1` recall when no local server is present — the first, purely-data-plane
slice of this vision.

## Motivation

- **Remote/shared deployments are first-class for the data plane but broken for
  the agent.** After moving memory+kb to a shared host (e.g. a SmoothNAS
  aimee-kb), the dev box's `aimee` can query everything over `/v1`, but
  `session-start`, `chat`, and `mcp-serve` fail with "server unavailable"
  because they want a local socket. (This is exactly the regression #69 fixed
  for one method.)
- **"Install one thing" is the product goal.** An end user should `install aimee`
  and have the full surface — not "install the CLI *and* stand up/manage an
  `aimee-server` service."
- **The split that caused this was a lifecycle fix, not a design north star.**
  `cli_ensure_server_for_method()` documents #1660: the CLI stopped spawning
  servers on demand after "five iterations of orphan-listener fixes … every CLI
  invocation was a potential server-spawner," handing lifecycle to systemd. That
  solved orphaned **listeners** — but the answer for the execution plane is to
  run it **in-process with no listener at all**, which sidesteps the orphan
  problem entirely rather than requiring a managed daemon.

## Background: what exists today

- **Data plane (done).** `cli_rpc_lookup` + `cli_rpc_forward` resolve a method to
  a first-class `/v1` route (`cli_v1_routes_gen.inc`) and POST it to the
  configured endpoint — local UDS *or* remote TCP (`cli_rpc_client_endpoint`,
  `cli_rpc_client_bearer`, `cli_rpc_has_remote_endpoint`). `kb_client_*` already
  speaks to a remote kb. No socket-RPC bridge remains (`/v1/rpc` retired).
- **Execution plane (local-only).** `handle_session_start`, `chat`
  (`chat.send_stream`), `cli_mcp_serve` (`mcp.call`), `launch.run`,
  `agent.setup`, `trigger.list` all call `cli_ensure_server_for_method()` →
  `cli_existing_server_for_method()`, which only probes `AIMEE_SOCK` and the
  well-known local socket. The actual work runs **server-side**, in
  `server/*.c`: `agent_loop.c`, `agent_runtime.c`, `agent_tools.c`,
  `delegate_backend{,_local,_ssh,_docker}.c`, `server_hooks.c`, the MCP dispatch
  table (`server/server_mcp_call_table.inc`), plus `CORE_SRCS` (db1, config).
- **Why it can't just point at the remote.** These operations execute tools on
  the **local working tree** — `git pull --ff-only`, worktree isolation,
  file/exec tools. They must run where the files are. So "route execution to the
  NAS" is wrong; "run execution locally, fetch memory remotely" is right.
- **Build.** `$(BINARY)` = `CLI_OBJS + CLIENT_SUPPORT_OBJS` (links `L_CLIENT`, no
  libpq). The engine is `SERVER_OBJS` + `CORE_SRCS`. `-DAIMEE_THIN_CLIENT=ON`
  builds the client only.

## Design

The single `aimee` binary gains an **embedded execution engine** invoked
**in-process** (no socket, no daemon) for execution-plane commands, with the
data plane (kb/memory and any DB2-backed reads/writes) flowing over `/v1` to a
configurable backend via the existing client.

Core seam: replace, for execution-plane commands, the
`cli_ensure_server_for_method()` → forward-to-socket step with a dispatch that
prefers, in order:
1. a co-located server if one is already running (back-compat, unchanged);
2. otherwise an **in-process engine call** (`engine_session_start()`,
   `engine_mcp_serve()`, `engine_chat()`, `engine_delegate()`), where the engine
   is configured with `AIMEE_KB_API_URL`/remote `/v1` for shared memory and a
   local DB1 path for per-host session state.

DB1 (SQLite, per-host session/working state) stays local to the binary — it is
inherently host-scoped (session_state, working memory). Durable *memory* is DB2
(the kb) and already remote. So "one binary + a shared remote kb" loses nothing
semantic.

### Phase 0 — SessionStart over remote `/v1` recall — **shipped (#69)**
`session-start` emits proactive recall via `POST /v1/memory/recall` when only a
remote endpoint is configured. Pure data plane; no engine needed.

### Phase 1 — Read-only hooks + recall completeness
Route the remaining read-only hook surfaces (pre-tool advisory reads, recall
variants) over `/v1` the same way. No engine link yet. Low risk.

### Phase 2 — In-process `mcp-serve`
Link the MCP dispatch (`server_mcp_call_table.inc` + the tool implementations it
needs) into the binary and have `aimee mcp-serve` run it in-process. Pure-data
tools call `/v1`; local-fs/exec tools run locally. This is what Claude Code's
MCP integration actually needs and is more self-contained than `chat`.

### Phase 3 — In-process `chat` / primary agent
Link `agent_loop.c` / `agent_runtime.c` / `agent_request_shaping.c` + provider
calls + tool execution. `aimee chat` runs the agent loop in-process against the
local working tree, fetching memory/kb over `/v1`. Largest phase; gated behind a
build flag until proven.

### Phase 4 — In-process `delegate`
Link `delegate_backend*` so `aimee delegate` runs sub-agents locally (it is
currently "no /v1 route" — foreground delegate is socket-bound). Backends
(local/ssh/docker) are already modular.

## Build / packaging

- Introduce a build profile that produces the **full single binary** (client +
  embedded engine) as the default end-user artifact; keep
  `-DAIMEE_THIN_CLIENT=ON` for a minimal data-only client where binary size
  matters. Reuse `SERVER_OBJS`/`CORE_SRCS` — no logic duplication; the engine
  becomes a library both `aimee` and `aimee-server` link.
- `aimee-server` remains for multi-tenant/shared deployments (the NAS plugin).

## Scope / phasing

Ship Phase 0 (done) → 1 → 2 first; they cover the Claude Code integration
(hooks + MCP), which is the highest-value, lowest-risk part. Phases 3–4 (chat,
delegate in-process) are larger and land behind a flag with their own
validation. Each phase is independently shippable.

## Risks / non-goals

- **Binary size / deps.** Embedding the engine pulls libpq-free engine code but
  grows the binary; mitigate with the thin-only profile staying available.
- **No orphan-listener regression.** In-process execution publishes **no
  socket** and exits with the command — it does not reintroduce the #1660
  spawner problem. A persistent local listener is explicitly a non-goal.
- **DB1 locality.** Per-host session state stays in a local SQLite file; we do
  not attempt to remote DB1. Durable memory is DB2/kb (already remote).
- **Not a security boundary change.** Remote `/v1` writes remain governed by the
  server's `remote_writes` / capability model; the in-process engine uses local
  creds for local DB1 and the configured bearer for `/v1`.

## Verification

- Per phase: `-Wall -Wextra -Werror` clean; unit tests; and an end-to-end check
  with the binary pointed at a remote aimee (no local server) — e.g. Phase 2:
  `aimee mcp-serve` answers an MCP `tools/call` with memory fetched over `/v1`;
  Phase 3: `aimee chat` completes a turn with tools on the local tree and recall
  from a remote kb.
- Back-compat: with a co-located server running, every command behaves exactly
  as today (local socket preferred).
