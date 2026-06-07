# Proposal: Self-sufficient thin client — one binary, strict 3-tier, server-side execution

One `aimee` binary an end user installs to use *any* aimee. The client is a
**pure `/v1` client to aimee-server** — it knows nothing about DB1 or DB2, runs
no engine, and never talks to aimee-kb directly. All work happens on
aimee-server; for operations that touch the user's local files, the client
exposes its working tree to the server over the existing **detached workspace
reverse-channel** — it does not absorb the engine.

> Supersedes the earlier draft of this proposal, which argued for embedding the
> engine *into* the client. That was the wrong direction: it would have put DB/
> engine knowledge in the client. The architecture below is the opposite and
> matches the intended 3-tier separation.

## The invariant (3-tier)

- **Thin client** — pure `/v1` HTTP to **aimee-server only**. No sqlite (DB1),
  no libpq (DB2), no `kb_client`/`AIMEE_KB_API_URL`, no direct aimee-kb access.
- **aimee-server** — owns **DB1** (sqlite), runs the engine (agent loop, tools,
  hooks, MCP, delegate), and **proxies all kb/DB2 traffic to aimee-kb itself**.
- **aimee-kb** — owns **DB2** (postgres/pgvector).

The client routes everything through aimee-server; aimee-server fans out to
aimee-kb. The client is transport + local-workspace access, nothing more.

## Current state (verified)

- The thin client **already satisfies the invariant**: no sqlite/libpq refs in
  the client TUs, `L_CLIENT = L_MINIMAL + TLS` (no DB libs), and `aimee kb …`
  routes to method `kb.search`/`kb.status` against the configured **aimee-server**
  endpoint (`:8740`), whose `/v1/kb/*` proxies to aimee-kb. The client never hits
  aimee-kb (`:8741`) directly.
- **Data plane (done):** all read/data commands work thin-client → aimee-server
  over first-class `/v1` (Phase 1: `memory recall`, `notes`, plus the already-
  routed `index`/`curator`/`graph`/`api`/`memory search`/`mcp audit`).
- **Reverse-channel foundation (exists):** `aimee workspace serve` (client) polls
  `/v1/runner/poll`, executes ops on the local tree, replies `/v1/runner/respond`
  — remote-capable; the detached provider is already in `CLIENT_RUNNER_OBJS`.
  Server-side, `workspace_turn_bind_active()` binds a client's detached workspace
  to a turn and **refuses** a remote turn that acts outside one
  (`server_compute_async.c`).
- **The blocker:** `chat`/`launch`/`mcp` hard-refuse a remote endpoint
  (`cli_main.c:1239,1744`, `cli_rpc_remote_endpoint_is_tcp()` → "interactive
  needs a co-located aimee-server"). That refusal — not any missing capability —
  is why the execution plane can't run against a remote server today.

## Design

Make the execution-plane commands run on a **remote** aimee-server against the
client's **served workspace**, instead of refusing. The client orchestrates:

1. **Register** the client's cwd as a `detached` workspace on aimee-server
   (`workspace add --provider detached` over `/v1`), obtaining a `workspace_id`.
2. **Serve** it: start the `workspace serve <id>` reverse-channel loop in the
   background (poll/respond over `/v1`), so the server can do file/exec/git on
   the client's tree.
3. **Run** the turn over `/v1`: `chat.send_stream` / `mcp.call` execute on the
   server, which binds the served workspace for the turn (existing
   `workspace_turn_bind_active`) and streams output back to the client.
4. **Tear down** the workspace/serve loop when the command exits.

No engine and no DB ever enter the client; the server does the work and reaches
the client's files only within the registered detached workspace (already
enforced server-side).

### Phase 0 — SessionStart over remote `/v1` — shipped (#69, v0.2.27)
`session-start` emits proactive recall via `POST /v1/memory/recall` (client →
aimee-server → aimee-kb). Pure data plane, no co-located server.

### Phase 1 — Data-plane command coverage — shipped (#72, v0.2.28)
`memory recall`, `notes`/`notes search` wired over `/v1`; full read surface now
works thin-client → aimee-server.

### Phase 2 — `mcp-serve` against a remote server + served workspace
Replace the remote refusal in `cli_mcp_serve` with the register→serve→run→
teardown flow above. `aimee mcp-serve` drives Claude Code's MCP against a remote
aimee-server; tool calls that touch files run on the client via the reverse
channel; data/kb tools run on the server (→ aimee-kb). Highest-value (Claude
Code integration) and the smallest execution-plane command.

### Phase 3 — `chat` against a remote server + served workspace
Same orchestration for `chat.send_stream`; lift the `cli_main.c:1239` refusal.

### Phase 4 — `delegate` against a remote server
`delegate` runs server-side already; expose it over `/v1` for thin clients
(it is currently "no /v1 route") so a remote client can launch/poll delegate
jobs that execute on the server.

## Capability / bearer note

Execution-plane `/v1` routes need caps the baked dev bearer `aimee-local-dev`
lacks over TCP (read/query/chat only). A deployment that wants remote execution
must issue a higher-scope bearer (or set `remote_writes`) — a config concern,
not a client change. `mcp audit`/`tools_list`/`agent`/`provider test` 403 today
for the same reason.

## Scope / phasing

Phase 2 first (MCP = the Claude Code integration), then 3 (chat), then 4
(delegate). Each is independently shippable and reuses the workspace
reverse-channel; no build-profile or engine-link changes — the client stays thin.

## Risks / non-goals

- **Non-goal: engine or DB in the client.** Explicitly reversed from the earlier
  draft. The client never links sqlite/libpq and never speaks to aimee-kb.
- **Reverse-channel latency.** Every server-side file/exec op is a `/v1` round
  trip to the client; fine for interactive use, watch for chatty tool loops.
- **Security.** The server may only act on the client's fs within a registered
  detached workspace (already enforced); foreign-cwd turns are refused.
- **Bearer scope.** Remote execution needs an appropriately-scoped bearer (see
  above); the dev default is intentionally narrow.

## Verification

- Phase 2: with the client pointed at a remote aimee-server (no co-located
  server), `aimee mcp-serve` answers an MCP `tools/call` whose file tool runs on
  the client (via `workspace serve`) and whose memory tool resolves on the server
  (→ aimee-kb). Phase 3: `aimee chat` completes a turn editing a local file and
  recalling from the remote kb. Back-compat: a co-located server still uses the
  local socket path unchanged.
