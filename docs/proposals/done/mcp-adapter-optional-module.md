# Proposal: MCP-plugin adapter as an optional module (install into aimee-server or aimee-kb)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. Delivered scope archived 2026-07-26.

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. Remaining work is tracked in
> [`mcp-adapter-bus-routing-residual.md`](../pending/mcp-adapter-bus-routing-residual.md).

> **Revised after a five-lens design roundtable (2026-07-25), verdict needs-rework →
> reworked.** The first draft framed this as "an adapter *into the event bus* that
> runs MCP plugins," with plugin calls "riding the bus" across daemons; the
> roundtable verified against the code that **the event bus is intra-daemon
> shared-memory only** (memfd regions handed over a local socketpair via SCM_RIGHTS;
> each daemon runs its own host. D1/D7); it never crosses the server↔kb process
> boundary, and there is no general bus host today (only obs_bus's private 4-slot
> audit singleton). So the bus cannot be the cross-daemon plugin-RPC transport. This
> revision splits the design accordingly and states the one governing rule up front.
> (The alarming security/audit claims did **not** survive verification, content-free
> audit is structurally enforced on the separate audit host; scope-by-daemon is
> real. See "What the review corrected.")

## Governing rule

**The event bus is an intra-daemon mechanism. Every server↔kb hop is `kb_client`
HTTP/mTLS.** The bus earns its keep *inside* one daemon; the "shared to the whole
deployment" value of a kb-hosted plugin comes from HTTP federation + HTTP
invocation, with the kb-local bus as at most the last in-kb hop.

## The requirement (unchanged)

Make the MCP-plugin adapter (`modules/protocols/mcp`) an **optional module either
`aimee-server` or `aimee-kb` can install**, the install target deciding scope:

- **Server-install**: the plugin's tools are exposed only to **that server's
  sessions** (this is essentially today's behavior, made optional + explicit).
- **KB-install**: the plugin's tools are exposed to **everything hooked up to that
  kb**: every server and thin client on that kb sees one shared plugin surface,
  backed by connections the kb owns.

The thin-client advertises the plugin tools over MCP/CLI the same way it advertises
modules today.

## Core design (Phase 1: buildable now, no new bus infra)

### Install target (config)

`config_mcp_client_t` (`config.h`) gains `install: server | kb` (default `server`).
Each daemon boots `mcp_client_registry` for its own clients only, so a plugin is
owned by exactly one daemon.

### Server-install: direct, in-process

The dispatcher and the registry are co-located, so a server-hosted plugin call is
the **direct in-process `mcp_client_registry_call_tool`** it is today; its tools
merge into the session `tools/list` via `mcp_client_registry_build_namespaced_tools`
(no bus). Crash isolation comes from the plugin **subprocess** boundary, not the
bus. Nothing new is required here beyond honoring the install target.

### KB-install: the kb owns the plugins, servers call over HTTP

- **Invocation:** a session that targets a kb-hosted `plugin:tool` reaches it over the
  **existing mTLS action-RPC family**, a new `mcp.call` method on `POST
  /v1/actions/<method>`, dispatched by the same kb service table as
  `tool_registry.snapshot`/`lookup`. (The design review found this cleaner than a
  bespoke `/v1/mcp/call` REST endpoint: it reuses the action channel's auth, framing,
  and timeout, strictly less new surface, same security envelope as every other kb
  RPC.) `kb_client_mcp_call` builds `{name, arguments?, timeout_ms?}`; the kb handler
  runs `tools/call` against its live plugin session (timeout clamped ≤120s) and
  returns `{status:"ok", result}`. Args/results copy across the request/response body.
- **Advertisement:** the kb-hosted tool **definitions** reach a server through an
  **extended tool-registry snapshot RPC**. (Today's `kb_client_tool_registry_snapshot_json`
  carries *prompt* strings, not tool defs, and feeds the system prompt. It does
  **not** feed `tools/list`; this is new work, not existing federation.) The
  extension carries `name / description / inputSchema / side_effect`, and a new
  server-side merge folds them into `mcp_build_tools_list` alongside local plugin +
  native tools.
- **Collision handling** happens at that **runtime catalog merge** (not at boot; the
  two daemons are independent processes and neither sees the other's config), with
  an explicit precedence rule + diagnostic. The namespaced-name grammar and buffer
  bounds (`client[64]:tool[128]` vs `qualified[160]`) are defined so nothing
  truncates.

### Audit (reuses the tool-call outcome audit)

A server-install call audits its outcome on the **server's** obs_bus; a kb-install
call runs at the kb and records its outcome on the **kb's** obs_bus, the same
NULL-default completion hook + server-only bridge, installed in `kb_main.c`,
mirroring the memory-kb bridge. Content-free either way (fingerprint + enums). This
is the event bus's genuine role in this feature, and it is why this lands *after*
the tool-call outcome audit PR.

## Phase 2 (optional, later): intra-daemon bus routing of plugin calls

Routing a plugin call over a daemon's *own* bus (correlated request/reply, large
args/results by arena reference) buys isolation + backpressure + payload-by-
reference **within one daemon**. It is deliberately **opt-in and deferred**, because
it is not free and is not required for the feature above:

- It needs a **general per-daemon bus host**. One does not exist; obs_bus's host is
  a private, 4-slot, notification-only audit singleton. Standing up a
  general-purpose host (slot budget, kind registration, attach model, pump/heartbeat
  owner, coexistence with the audit host) is first-class new infrastructure.
- It needs the **client-visible arena request/reply surface**, `bus_client` today
  exposes only one-way `bus_client_publish_arena`, and arena allocation is
  host-private/co-located (D3). Either that surface is built, or the caller+adapter
  must be co-located and use the in-process host-arena API. Arena payload-by-
  reference is **intra-daemon only**, it never helps the server↔kb hop.
- It is a **governed D7 blast-radius amendment**: a bus participant in
  `modules/protocols/mcp` must include a bus header, which `check_bus_blast_radius.sh`
  (a hard allowlist) *fails* by design. Widening the allowlist is a roundtable-
  governed D7 revision that explicitly re-states the widened blast radius (external
  stdio/sse/JSON I/O now co-resident with the arena), **not** a check that "stays
  green."
- The correlated request/reply + arena routing (built, wired, TSAN/conformance-
  tested) has **zero shipping consumers**; this would be its first production driver,
  so its full refcount / timeout / crash lifecycle must be integration-tested before
  anything depends on it, and bus routing stays opt-in per client with the direct
  call as the default fallback + observable parity.

## Non-goals

- No new external egress model beyond the operator-configured outbound the kb
  already makes (embedder, forge); a kb-hosted plugin is another such outbound.
- No per-session / per-tenant plugins; install scope is deployment config.
- No revival of the removed in-process `dlopen` plugin loader, MCP *is* the plugin
  interface.
- **D7 unchanged in Phase 1** (the bus stays audit-only, confined to the two trusted
  daemons); Phase 2 explicitly amends it.

## Binding checks

- A kb-installed plugin's tool appears in a connected server's `tools/list` (via the
  extended snapshot RPC + merge) and in the thin client's advertised catalog; a
  server-installed plugin's tool is absent from a *different* server, proving scope.
- A kb-hosted `plugin:tool` call round-trips over the `mcp.call` action and returns the
  same result as a direct call; its completion row lands on the **kb** ledger,
  content-free; a server-installed call's row lands on the **server** ledger.
- A local↔kb tool-name collision is resolved at the runtime merge by the stated
  precedence, with a diagnostic, not silently first-wins.
- Phase 1 keeps `check_bus_blast_radius.sh` green (no bus header in the adapter).
  Phase 2, if pursued, ships the D7 amendment + updated gate.

## Failure-mode & precedence e2e (observed 2026-07-25, CT 132 @ .253)

Against a live kb hosting a multi-tool plugin (ok / error / slow tools):
- **Error path:** a plugin that returns a JSON-RPC error → kb reply is an error and
  the audit row is `verdict:"error", reason_code:"tool_error"`.
- **Timeout path:** a tool that sleeps 3s called with `timeout_ms:1000` → the kb
  aborts and records `verdict:"error", reason_code:"tool_error"` (the clamp/timeout
  is enforced, not ignored).
- **Concurrency:** 12 parallel `mcp.call`s to one stdio plugin all succeed
  (`verdict:"ok"`) and each is audited. The per-client stdio pipe is serialized
  safely.
- **Local precedence (collision), LIVE:** with the SAME tool name `echo` hosted on
  BOTH the server (install:server → a "LOCAL-echo" plugin) and the kb (install:kb),
  a real model turn calling `echo:echo` returned `LOCAL-echo: LIVE-TURN` and the kb
  logged ZERO `mcp.call`s. The server's local plugin wins and the kb is never
  contacted. This also exercises the install:server invocation (local dispatch
  branch) end to end. The selection primitive (`mcp_client_registry_get`) is
  additionally unit-tested for hosted/not-hosted both ways.

## Phase 1 verification (observed 2026-07-25, CT 132 @ .253)

Real `aimee-kb` daemon (fresh DB2, dim pinned) hosting an `install: kb` stdio MCP
plugin (a python echo server), exercised over the live `/v1/actions/*` channel:

- **Boot filtering + hosting** (increments 1–2): the kb booted **only** the
  `install: kb` plugin; its OSV startup scan ran and did not block. Unit test
  `test_install_target_filtering` + the registry's real-subprocess
  `call_tool`/`build_namespaced_tools` cases pass on the CT.
- **Federation advertise** (increment 4, kb side): `POST
  /v1/actions/tool_registry.snapshot` → HTTP 200 with
  `mcp_tools:[{"name":"echo:echo","description":…,"inputSchema":…}]`, the exact
  bytes the server's `append_federated_kb_tools` consumes.
- **Invocation** (increment 3): `POST /v1/actions/mcp.call`
  `{"name":"echo:echo","arguments":{"text":"federation-works"}}` → HTTP 200
  `{"status":"ok","result":{"content":[{"type":"text","text":"echo: federation-works"}]}}`,
the handler dispatched to the registry, which round-tripped to the real plugin
  subprocess. This is the exact endpoint `kb_client_mcp_call` wraps.

**Server side, verified end to end** (added 2026-07-25): against the same live
kb, the real server functions run through their paths via two gated integration
cases in `unit-test-mcp-native-dispatch` (`AIMEE_KB_API_URL` set):
- **Advertise:** `build_tools_array()` (the LLM-facing tool array) contains
  `echo:echo`, `append_federated_kb_tools` fetched the kb snapshot and merged the
  federated def. PASS.
- **Dispatch:** `dispatch_tool_call("echo:echo", {"text":"kb-routed"})` returned
  `{"content":[{"type":"text","text":"echo: kb-routed"}]}`, the server resolved
  the client as non-local and routed to `kb_client_mcp_call` → kb → plugin. PASS.
- **Scope isolation:** with a config declaring `echo` (install:kb) + `locecho`
  (install:server), the kb's federated `mcp_tools` is exactly `["echo:echo"]`. The
  server-installed plugin is never federated; only the install:kb plugin runs on
  the kb. Matches `test_install_target_filtering`.
- **Regression:** the full `make unit-tests` suite passes on the CT (real
  Postgres), SUITE_EXIT=0.

**kb-side audit, implemented + verified** (added 2026-07-25): the kb now records
every hosted plugin tool-call OUTCOME on its own audit ledger, content-free. This
did NOT need #1955, the kb already hosts obs_bus and an allowlisted bridge pattern
(`kb_memory_audit_bridge.c`), so `kb/kb_mcp_audit_bridge.c` reuses it and
`kb_handle_mcp_call` fires it on every path. Observed rows in the kb's `audit.log`:
- success → `{"kind":"tool_action","actor":"session-XYZ","tool":"echo:echo",`
  `"args_hash":"v1-c336…","command":"","mode":"outbound","reason_code":"","verdict":"ok"}`
- error → `{…,"actor":"mcp","tool":"nope:tool","reason_code":"tool_error","verdict":"error"}`
Only a name-only fingerprint + classified enums cross (no argument/result content).
The caller's dispatch role is threaded over the mcp.call request as the audit actor
(default `"mcp"`). Blast-radius gate: all six C shipping binaries inspect clean.

**Live-model agent turn. Run and verified** (added 2026-07-25): a real agent turn
was driven through `aimee-server`'s agentic loop against a stub OpenAI-compatible
model (aimed at via the `openai` provider's configurable `openai_endpoint`, no key
needed). The stub emitted an `echo:echo` tool_call; the full loop closed:
- delegate job → `status: done, turns: 1`, model final answer
  `TOOL_RESULT_SEEN: {"content":[{"type":"text","text":"echo: LIVE-TURN"}]}`. The
  string is the KB-hosted plugin's own output, so the model's call reached the kb.
- the kb logged `POST /v1/actions/mcp.call status=200` for that turn.
- the kb audit ledger recorded the model-driven call end to end: a server-side
  authorization row `{"actor":"delegate","tool":"echo:echo","verdict":"allow"}` and
  the kb-side outcome row `{"actor":"mcp","tool":"echo:echo","mode":"outbound","verdict":"ok"}`.

So the complete chain is exercised by a real model: model emits `echo:echo` →
server authorizes + dispatches → kb executes the plugin → kb audits the outcome →
result returns to the model. (In this delegate harness the tool array the stub
received was empty, a delegate role-toolset filtering artifact, orthogonal to the
adapter; the federation of `echo:echo` into `build_tools_array` is separately
verified by the integration test above.)

> Harness note: mcp_client config command lists must use **block-style** YAML
> (`command:\n  - python3\n  - script.py`); the flow-style inline form
> (`command: [python3, script.py]`) is not parsed into an array by the config
> reader, so the client is silently skipped. Worth a follow-up hardening in the
> YAML/flow-list parsing, independent of this feature.

## What the review corrected (for the record)

- **The bus is not a cross-daemon transport.** Rewrote every "server publishes onto
  the kb's bus" to the real path: `kb_client` mTLS to a new `mcp.call` action method,
  kb-local bus at most the last hop.
- **No bespoke REST endpoint.** Implementation review of the seams showed invocation
  belongs in the existing `/v1/actions/<method>` RPC family (an `mcp.call` method),
  not a hand-rolled `/v1/mcp/call`, reusing the action channel's auth/framing/timeout
  rather than duplicating them.
- **No general bus host exists**: Phase 2 makes it an explicit prerequisite.
- **The tool federation carries prompts, not tool defs**: the snapshot RPC
  extension is new work, not existing federation.
- **`check_bus_blast_radius.sh` cannot "stay green"** with a bus participant in the
  adapter. That is a governed D7 amendment (Phase 2).
- **Name collisions can't be caught at boot** across two processes, moved to the
  runtime catalog merge.
- **Kept (verified):** content-free audit is structural (separate audit host, hash-
  only emit); scope-by-daemon is real; the arena/correlated routing is implemented
  and this would be its first production consumer.
