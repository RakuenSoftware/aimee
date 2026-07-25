# Proposal: an event-bus MCP-plugin adapter — optional, install into aimee-server or aimee-kb

## The idea

An **adapter into the event bus that runs MCP plugins.** An MCP plugin (an external
MCP server — GitHub, filesystem, a company tool server) is run by an adapter that
is a participant on the event bus: a plugin tool-call travels the bus as a
**request event**, the adapter serves it against the plugin's live MCP session and
publishes the **reply event**, and the **thin-client advertises those plugin tools
over MCP/CLI exactly as it advertises modules today**. The adapter is an *optional
module* either trusted daemon can install, and the install target sets the scope:

- **Installed in aimee-server** — the adapter runs on the server's bus, so its
  MCP-plugin tools are exposed only to **that server's sessions**.
- **Installed in aimee-kb** — the adapter runs on the kb's bus, so its tools are
  exposed to **everything hooked up to that kb**: every aimee-server and every thin
  client on that kb sees the same shared plugin surface, backed by one set of
  plugin connections the kb owns.

This is the "MCP interface for MCP plugins over the event bus" that opened this
work, now buildable because the substrate exists: the bus carries correlated
request/reply (host routing), large tool arguments/results ride the arena
(payload-by-reference), and every call is audited content-free (the tool-call
outcome audit). The MCP-plugin adapter is the **first real producer** of the arena
payload path.

## Why the bus is the right substrate here

A plugin tool-call is a natural bus request/reply, and routing it over the bus buys
exactly the properties an in-process plugin loader could not give:

- **Scope by construction.** The bus a call rides *is* its blast radius. Put the
  adapter on the server bus and its tools are that server's; put it on the kb bus
  and they are the whole deployment's. Exposure is not a separate ACL layer — it is
  which bus the adapter subscribed to.
- **Isolation + backpressure.** The plugin's connection lives behind the adapter,
  not in every caller. A slow or dead plugin is BLOCK/SHED-managed by the host,
  never a caller hang; a plugin crash never takes a session with it.
- **Large payloads, safely.** MCP arguments and results routinely exceed the inline
  budget (file contents, diffs, tool output). They ride the arena by reference — no
  copy through the host, refcounted, generation-gated — which is what the arena work
  was for.
- **Audited for free.** The request and reply are governed bus events, so the
  tool-call outcome audit records identity + outcome (content-free) on whichever
  daemon's bus the adapter runs — server-installed audits on the server, kb-installed
  on the kb, mirroring the memory-kb bridge.

(This does not turn the bus into a general RPC fabric: it carries *governed
tool-calls as events*, the same shape as every other bus event, not arbitrary
transport.)

## Decision

Make `modules/protocols/mcp` (plus a thin bus-adapter layer) an optional module
both binaries may link and boot, driven by an **install target** in config, with a
bus request/reply contract for plugin calls and thin-client advertisement of the
resulting tools.

### 1. Install target (config)

`config_mcp_client_t` (`config.h`) gains an `install: server | kb` field
(default `server`, so nothing changes without opt-in). Each daemon boots the
adapter for **its own** clients only, so a plugin is owned by exactly one host and
a tool name resolves unambiguously.

### 2. The adapter as a bus participant

Per hosting daemon, the adapter:
- opens/owns the MCP plugin sessions (stdio/sse), as `mcp_client_registry` does now;
- **subscribes** to a plugin-tool-call request kind on that daemon's bus and, on a
  request, runs `tools/call` against the plugin session and publishes the reply
  (result inline, or arena for a large payload); errors/timeouts become typed reply
  outcomes, never a caller hang;
- a caller (a session's tool dispatch) that targets a plugin tool **publishes the
  request** to that bus and awaits the correlated reply. A server-hosted plugin's
  request/reply stays on the server bus; a kb-hosted plugin's rides the kb bus
  (reached from a server over the existing kb_client transport).

The correlated-arena routing already delivers request→server and reply→requester by
reference with correct refcounting; this is its first non-test consumer.

### 3. Thin-client advertisement (as for modules)

The plugin tools must surface to a thin client the same way module tools do today:
the tool catalog the CLI serves over MCP (`cli_mcp_serve.c` forwarding `tools/list`,
and the CLI's own tool listing) includes the adapter's namespaced plugin tools
(`plugin:tool`) alongside module and native tools. A kb-hosted plugin's tools reach
a server's catalog through the existing kb→server tool-registry federation
(`kb_client_tool_registry_snapshot_json`); a server-hosted plugin's are local. Either
way the thin client advertises them with no new advertisement mechanism — it is the
same `tools/list` surface, one more source feeding it.

### 4. Audit follows the install target

Composes with the tool-call outcome audit: a server-installed plugin call is
dispatched and audited on the **server's** obs_bus (mode `outbound:stdio|sse`); a
kb-installed call runs at the kb and records its outcome on the **kb's** obs_bus
(the same NULL-default completion hook + server-only bridge, installed in
`kb_main.c`, mirroring the memory-kb bridge). Content-free either way — fingerprint
+ enums, never the plugin's arguments or results, even when those ride the arena.
This reuse of the completion hook + bridge on the kb side is why this lands *after*
that audit PR.

## Non-goals

- **No new external egress surface.** The kb already makes operator-configured
  outbound connections (embedder, forge); a kb-hosted MCP plugin is another, gated
  by the same config trust boundary. It does not expose the kb's own API to the
  plugin.
- **No per-session / per-tenant plugins.** Install scope is deployment config
  (server or kb), not a dynamic per-principal surface.
- **No general-purpose RPC over the bus.** The bus carries governed tool-calls as
  events, not arbitrary transport; a plugin call is the same event shape as the
  rest of the arc.
- **No revival of the removed in-process plugin loader.** MCP *is* the plugin
  interface; the bus adapter is how a plugin runs and is scoped. Nothing is
  dynamically `dlopen`'d.
- **D7 unchanged.** The bus stays confined to the two trusted daemons; a kb-hosted
  plugin audits on the kb's bus, never a thin client.

## Binding checks

- A kb-installed plugin's tool appears in a connected server's `tools/list` (via the
  registry federation) and in the thin client's advertised catalog; a
  server-installed plugin's tool is absent from a *different* server — proving scope.
- A plugin call round-trips as a bus request/reply, with a large argument/result
  carried by arena reference (refcount drains to zero); the completion row lands on
  the **hosting daemon's** ledger (server vs kb), content-free.
- A slow/dead plugin BLOCK/SHED-resolves at the host without hanging the caller,
  and a plugin crash does not fault a session.
- A name collision between a `server` and a `kb` plugin is refused at boot.
- `check_bus_blast_radius.sh` stays green with the adapter linked into kb.

## Rollout

1. `install` config field + validation (default `server`; today's behavior).
2. The bus request/reply contract for a plugin call (kind + correlated reply +
   arena for large payloads), served by the adapter; caller-side publish/await in
   the dispatcher's `plugin:tool` branch.
3. Boot the adapter conditionally in `kb_main.c`; federate its tool catalog to
   servers and the thin-client `tools/list`.
4. Install the completion-audit bridge in kb for the kb-hosted call outcome.
5. Multi-agent convergence review (scope boundary, the request/reply + arena
   refcount lifecycle, the audit split), then merge.
