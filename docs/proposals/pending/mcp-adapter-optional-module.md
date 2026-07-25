# Proposal: MCP adapter as an optional module — install into aimee-server or aimee-kb

## Context

The MCP client adapter (`modules/protocols/mcp/`) connects out to external MCP
servers and exposes their tools to aimee as namespaced `server:tool` names. Today
it is **server-only**: `mcp_client_registry_boot()` is called once from
`server_main.c:291`, the sessions belong to that one aimee-server process, and the
tools are merged into that server's per-session tool list
(`agent_tools.c` → `mcp_client_registry_build_namespaced_tools`). `aimee-kb` boots
no MCP registry and exposes no external MCP tools.

Two facts make a different topology cheap:

1. The adapter is already a self-contained module with a clean boot/shutdown/
   call/list surface (`mcp_client_registry.h`) — nothing about it is intrinsically
   server-bound.
2. A **kb → server tool-registry federation already exists**:
   `kb_client_tool_registry_snapshot_json()` / `kb_client_tool_registry_lookup()`
   let a server pull a tool surface *from* its kb. That is precisely the channel a
   kb-hosted MCP surface would ride.

## The requirement

Make the MCP adapter an **optional module that either aimee-server or aimee-kb can
install**, with the install target deciding the exposure scope:

- **Installed in aimee-server** — the external MCP tools are exposed **only to the
  sessions of that aimee-server** (today's behavior).
- **Installed in aimee-kb** — the external MCP tools are exposed to **everything
  hooked up to that aimee-kb**: every aimee-server (and every thin client) that
  connects to the kb sees the same shared MCP tool surface, backed by one set of
  connections the kb owns.

The kb case is the valuable new one: a single operator-configured MCP server (a
GitHub server, a filesystem server, a company-internal tool server) becomes a
shared capability of the whole deployment, configured and connected once, rather
than re-declared and re-connected in every aimee-server.

## Decision

Make `modules/protocols/mcp` a module both binaries may link and boot, gated by an
**install target** in config; add a kb-side tool surface + a forwarding call path
so a kb-hosted MCP tool executes **at the kb** (one shared connection) while being
*callable from* any server.

### Install target (config)

Add an install scope to the MCP client config — the simplest form is a per-client
`install: server | kb` (default `server`, preserving today's behavior), or a
single top-level `mcp.install` when the whole set moves together. `config_mcp_client_t`
(`config.h`) gains the field; both `server_main.c` and `kb_main.c` read their own
subset.

- aimee-server boots `mcp_client_registry` for its `install: server` clients only.
- aimee-kb boots `mcp_client_registry` for its `install: kb` clients only.

A client is served by exactly one host — never both — so a tool name resolves
unambiguously.

### Where a kb-installed tool executes (the crux)

"Shared to everything on the kb" means the external connection is **owned by the
kb**, not re-opened per server. So a kb-installed tool call must run at the kb:

- **Tool surface federation.** The kb publishes its namespaced MCP tools through
  the existing tool-registry snapshot (`kb_client_tool_registry_snapshot_json`);
  each connected server merges them into its session tool list alongside its own
  `install: server` MCP tools and native tools. Namespacing (`server:tool`) keeps
  the two sources collision-free; a name collision between a server-local and a
  kb-shared client is a config error, surfaced at boot.
- **Forwarding dispatch.** When a session calls a kb-hosted tool, the server's
  dispatcher (the `strchr(name,':')` branch in `agent_tools_dispatch.c`) resolves
  the name to a *kb-hosted* client and forwards the call over `kb_client` to a new
  kb method (`kb.mcp.call` / `mcp_client_registry_call_tool` on the kb side),
  rather than calling `mcp_client_registry_call_tool` locally. The kb runs the
  actual `tools/call` on its shared session and returns the result.
- A server-installed tool keeps executing locally, exactly as today.

### Audit follows the install target (composes with #1955)

The tool-call outcome audit (#1955) and the governed-action identity audit are
per-process on `obs_bus`. This proposal keeps that invariant:

- A **server-installed** MCP call is dispatched and audited on the **server's**
  `obs_bus` (mode `outbound:stdio|sse`), exactly as it is now.
- A **kb-installed** MCP call executes at the kb, so the kb records the outbound
  completion on the **kb's** `obs_bus` (the same NULL-default hook + server-only
  bridge pattern, installed in `kb_main.c` — mirroring the memory-kb bridge). The
  forwarding server still records that the *session* invoked a remote tool
  (identity), and the kb records the *execution outcome*. No content crosses in
  either row (fingerprint + enums only, per #1955).

This means the completion-audit bridge and its hook become linkable into kb as
well — which is why this proposal lands *after* #1955, not inside it.

## Non-goals

- **No new external egress surface.** The kb already makes outbound connections
  (embedder, forge); a kb-hosted MCP client is another operator-configured
  outbound, gated by the same config trust boundary. It does not expose the kb's
  own API to the MCP server.
- **No per-session MCP servers.** Install scope is deployment config (server or
  kb), not a per-session or per-principal dynamic — dynamic/tenant-scoped MCP is a
  separate concern.
- **Not a change to the MCP wire or the tool schema.** Tools stay namespaced
  `server:tool`; only *where the registry lives and who sees it* changes.
- **D7 unchanged.** The bus stays confined to the two trusted daemons; a
  kb-hosted MCP client audits on the kb's bus, still never a thin client.

## Binding checks

- A kb-installed client's tool appears in a connected server's session tool list
  (via the registry snapshot) and is absent when installed server-side on a
  *different* server — proving the scope boundary.
- A forwarded kb-hosted tool call returns the same result as a direct one, and its
  **completion row lands on the KB ledger** (mode `outbound:*`), while a
  server-installed call's row lands on the **server ledger** — proving audit
  follows the install target, still content-free.
- A name collision between a `server` and a `kb` client is refused at boot, not
  silently shadowed.
- `check_bus_blast_radius.sh` stays green with the MCP registry linked into kb.

## Rollout

1. Add the `install` config field + validation; keep default `server` so nothing
   changes without opt-in.
2. Boot the registry conditionally in `kb_main.c`; publish its tools through the
   kb tool-registry snapshot.
3. Add the `kb.mcp.call` forwarding method + the server-side dispatcher routing to
   it for kb-hosted names.
4. Install the completion-audit bridge in kb (the hook/TU from #1955 already link
   cleanly); wire the outbound-completion emit at the kb call site.
5. Multi-agent convergence review (scope boundary + the forwarding/audit split),
   then merge.
