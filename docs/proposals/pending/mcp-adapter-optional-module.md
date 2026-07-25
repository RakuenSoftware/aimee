# Proposal: MCP-plugin adapter as an optional module (install into aimee-server or aimee-kb)

> **Revised after a five-lens design roundtable (2026-07-25), verdict needs-rework →
> reworked.** The first draft framed this as "an adapter *into the event bus* that
> runs MCP plugins," with plugin calls "riding the bus" across daemons. The
> roundtable verified against the code that **the event bus is intra-daemon
> shared-memory only** (memfd regions handed over a local socketpair via SCM_RIGHTS;
> each daemon runs its own host — D1/D7); it never crosses the server↔kb process
> boundary, and there is no general bus host today (only obs_bus's private 4-slot
> audit singleton). So the bus cannot be the cross-daemon plugin-RPC transport. This
> revision splits the design accordingly and states the one governing rule up front.
> (The alarming security/audit claims did **not** survive verification — content-free
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

- **Server-install** — the plugin's tools are exposed only to **that server's
  sessions** (this is essentially today's behavior, made optional + explicit).
- **KB-install** — the plugin's tools are exposed to **everything hooked up to that
  kb**: every server and thin client on that kb sees one shared plugin surface,
  backed by connections the kb owns.

The thin-client advertises the plugin tools over MCP/CLI the same way it advertises
modules today.

## Core design (Phase 1 — buildable now, no new bus infra)

### Install target (config)

`config_mcp_client_t` (`config.h`) gains `install: server | kb` (default `server`).
Each daemon boots `mcp_client_registry` for its own clients only, so a plugin is
owned by exactly one daemon.

### Server-install — direct, in-process

The dispatcher and the registry are co-located, so a server-hosted plugin call is
the **direct in-process `mcp_client_registry_call_tool`** it is today; its tools
merge into the session `tools/list` via `mcp_client_registry_build_namespaced_tools`
(no bus). Crash isolation comes from the plugin **subprocess** boundary, not the
bus. Nothing new is required here beyond honoring the install target.

### KB-install — the kb owns the plugins, servers call over HTTP

- **Invocation:** a session that targets a kb-hosted `plugin:tool` reaches it over a
  **new `kb_client` HTTP action** (e.g. `POST /v1/mcp/call`), mirroring how the
  server already calls the kb for memory. The kb's handler runs `tools/call` against
  its live plugin session and returns JSON. Args/results copy across the HTTP body.
  The endpoint carries its own timeout / backpressure / auth (same mTLS bearer path
  as other kb actions).
- **Advertisement:** the kb-hosted tool **definitions** reach a server through an
  **extended tool-registry snapshot RPC**. (Today's `kb_client_tool_registry_snapshot_json`
  carries *prompt* strings, not tool defs, and feeds the system prompt — it does
  **not** feed `tools/list`; this is new work, not existing federation.) The
  extension carries `name / description / inputSchema / side_effect`, and a new
  server-side merge folds them into `mcp_build_tools_list` alongside local plugin +
  native tools.
- **Collision handling** happens at that **runtime catalog merge** (not at boot — the
  two daemons are independent processes and neither sees the other's config), with
  an explicit precedence rule + diagnostic. The namespaced-name grammar and buffer
  bounds (`client[64]:tool[128]` vs `qualified[160]`) are defined so nothing
  truncates.

### Audit (reuses the tool-call outcome audit)

A server-install call audits its outcome on the **server's** obs_bus; a kb-install
call runs at the kb and records its outcome on the **kb's** obs_bus — the same
NULL-default completion hook + server-only bridge, installed in `kb_main.c`,
mirroring the memory-kb bridge. Content-free either way (fingerprint + enums). This
is the event bus's genuine role in this feature, and it is why this lands *after*
the tool-call outcome audit PR.

## Phase 2 (optional, later) — intra-daemon bus routing of plugin calls

Routing a plugin call over a daemon's *own* bus (correlated request/reply, large
args/results by arena reference) buys isolation + backpressure + payload-by-
reference **within one daemon**. It is deliberately **opt-in and deferred**, because
it is not free and is not required for the feature above:

- It needs a **general per-daemon bus host** — one does not exist; obs_bus's host is
  a private, 4-slot, notification-only audit singleton. Standing up a
  general-purpose host (slot budget, kind registration, attach model, pump/heartbeat
  owner, coexistence with the audit host) is first-class new infrastructure.
- It needs the **client-visible arena request/reply surface** — `bus_client` today
  exposes only one-way `bus_client_publish_arena`, and arena allocation is
  host-private/co-located (D3). Either that surface is built, or the caller+adapter
  must be co-located and use the in-process host-arena API. Arena payload-by-
  reference is **intra-daemon only** — it never helps the server↔kb hop.
- It is a **governed D7 blast-radius amendment**: a bus participant in
  `modules/protocols/mcp` must include a bus header, which `check_bus_blast_radius.sh`
  (a hard allowlist) *fails* by design. Widening the allowlist is a roundtable-
  governed D7 revision that explicitly re-states the widened blast radius (external
  stdio/sse/JSON I/O now co-resident with the arena) — **not** a check that "stays
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
- No revival of the removed in-process `dlopen` plugin loader — MCP *is* the plugin
  interface.
- **D7 unchanged in Phase 1** (the bus stays audit-only, confined to the two trusted
  daemons); Phase 2 explicitly amends it.

## Binding checks

- A kb-installed plugin's tool appears in a connected server's `tools/list` (via the
  extended snapshot RPC + merge) and in the thin client's advertised catalog; a
  server-installed plugin's tool is absent from a *different* server — proving scope.
- A kb-hosted `plugin:tool` call round-trips over `POST /v1/mcp/call` and returns the
  same result as a direct call; its completion row lands on the **kb** ledger,
  content-free; a server-installed call's row lands on the **server** ledger.
- A local↔kb tool-name collision is resolved at the runtime merge by the stated
  precedence, with a diagnostic — not silently first-wins.
- Phase 1 keeps `check_bus_blast_radius.sh` green (no bus header in the adapter).
  Phase 2, if pursued, ships the D7 amendment + updated gate.

## What the review corrected (for the record)

- **The bus is not a cross-daemon transport.** Rewrote every "server publishes onto
  the kb's bus" to the real path: `kb_client` HTTP to a new `/v1/mcp/call`, kb-local
  bus at most the last hop.
- **No general bus host exists** — Phase 2 makes it an explicit prerequisite.
- **The tool federation carries prompts, not tool defs** — the snapshot RPC
  extension is new work, not existing federation.
- **`check_bus_blast_radius.sh` cannot "stay green"** with a bus participant in the
  adapter — that is a governed D7 amendment (Phase 2).
- **Name collisions can't be caught at boot** across two processes — moved to the
  runtime catalog merge.
- **Kept (verified):** content-free audit is structural (separate audit host, hash-
  only emit); scope-by-daemon is real; the arena/correlated routing is implemented
  and this would be its first production consumer.
