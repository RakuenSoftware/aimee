# Plugin modules: one Go module per MCP server or pluggy plugin, registering into the command list

- **State:** PENDING (proposed). Supersedes the C-oriented draft of this scope, which put the
  adapter in `src/modules/` on the C MCP client — wrong under the standing direction that only
  the event bus and its communication stay in C and everything else becomes a Go module. Extends
  the delivered [`mcp-adapter-optional-module`](../done/mcp-adapter-optional-module.md) from
  *per-daemon* plugin ownership to *per-plugin module processes*. Does not touch the deferred
  [`mcp-adapter-bus-routing-residual`](./mcp-adapter-bus-routing-residual.md).
- **Author:** JBailes
- **Date:** 2026-08-21

## Problem

Two gaps, and the second is the load-bearing one.

**1. A plugin cannot be owned by a module.** MCP servers are declared as
`config_mcp_client_t` entries in `aimee.yaml`, booted process-wide by
`mcp_client_registry_boot(host)`, and scoped only by `install: server | kb`. The unit of
ownership is the daemon. Nothing ties a plugin's lifetime, identity, failure domain, or
removal to a module. Pluggy has no support at all.

**2. The command list exists, but nothing registers into it.** *(Corrected from this
proposal's first draft, which claimed no command list existed. It does, and the design is
better for it — this builds on the registry rather than inventing a parallel one.)*

`src/headers/command_registry.h` is THE command table, and its stated invariant is exactly
what a plugin module needs: *"A capability is registered ONCE, here, by the module that owns
it. CLI, the v1 RPC routes, MCP and ACP all route from this table."* The declaration
contract over the bus exists too — `server-go/modules/memory/commands.go` answers event 5894
/ stage 6 with a `DCMD`→`DCMR` frame carrying `surfaces`, `visibility`, `group`, `verb`,
`summary` — and `src/modules/protocols/mcp/mcp_group_tool.c` already enumerates the registry
to build its surface.

What is missing is the middle:

- **Nothing in production calls `aimee_command_register`.** The only callers are
  `src/tests/test_command_registry.c`. `src/server/server_main.c:376-379` documents the
  design ("Modules declare their commands over the EVENT BUS when they connect") next to
  code that does not yet do it, so the memory module's declaration is answered by nobody.
- **`GET /v1/cli/manifest` does not read the registry.** It walks `g_v1_routes`, the
  statically compiled route table (`src/server/server_http_config_routes.c:1168-1200`), so
  even a populated registry would not reach a CLI client.
- **Registration is append-only with no withdrawal.** The registry has
  `aimee_command_registry_reset()` for teardown and nothing to remove one module's entries,
  which a plugin that disconnects requires.

The target shape is fifteen-ish independent processes — say 10 MCP modules and 5 pluggy
modules, **one plugin each** — that come and go at runtime. The registry is the right
destination; the driver that fills it, the manifest that reads it, and per-module withdrawal
are the work.

## Current state — the trace (all file:line verified on branch `testing` @ `553e859cbe`)

### The Go module substrate (exists, and is the target)

- **Multicall host.** `server-go/cmd/aimee-module/main.go` — `aimee-module-NAME
  DAEMON_MODULE_BUS_SOCKET`. One binary, N processes, dispatched by argv[0]. This is already
  the multiplicity mechanism this proposal needs.
- **Module runtime.** `server-go/bus/module_runtime.go` — `ModuleRuntimeConfig{SocketPath,
  ModuleName, PrincipalClass, PrincipalRef, Stages []ModuleStage}`, where each
  `ModuleStage{EventKind, StageID}` binds one event kind to a stage. The runtime attaches to
  the module bus socket and serves invocations; unknown stage, bad decode, or wrong operation
  are all refused (`:378-431`).
- **Module shape.** `server-go/modules/tools/tools.go` is the reference: a package declaring
  its `EventKind`, its stage constants, and its wire encode/decode. 20 modules exist under
  `server-go/modules/`.
- **Outbound identity precedent.** `main.go` already shows that a module needing to *call*
  another stage carries a second principal — `economizerStorePrincipalRef` (66),
  `roundtableDelegatePrincipalRef` (65) — because "a module's serving grant requests nothing".
  Registering into a command list is an outbound call, so it needs this pattern.

### The migration rule this obeys

`docs/proposals/pending/db2-as-a-go-module.md` states it plainly: *"There is no cgo bridge.
The module boundary is language-neutral: phase one uses the C bus client; phase two uses the
Go bus client."* New work skips phase one — a plugin module is new, so it is Go from the
start and never becomes C debt to port later.

### What exists in C and is deliberately not extended

`src/modules/protocols/mcp/` — `mcp_client.c` (transport vtable: stdio + SSE, JSON-RPC
framing, session handshake, `tools/list` caching, `tools/call`), `mcp_client_registry.c`
(process-wide registry, `client[64]:tool[128]` → `qualified[160]` namespacing). This is the
behavioural specification for the Go implementation and the conformance baseline to prove
against. It is not the thing to build on.

### The supply-chain gate that must not be bypassed

`src/cmd_mcp.c` + `osv_check.h` + `mcp_osv_enabled / _offline / _enforce / _cache_ttl_hours /
_allow[]` (`config.h:1306-1312`), backed by `src/db1/mcp_osv_cache.c`, with targets inferred
from a client's argv via `osv_infer_target_from_argv`. Today, running an MCP server requires
editing operator-owned `aimee.yaml`, and that edit is scanned.

## Target architecture

### One plugin, one module, one process

```
aimee-module-mcp-github      ── hosts exactly one MCP server
aimee-module-mcp-jira        ── hosts exactly one MCP server
...                             (10 of these)
aimee-module-pluggy-lint     ── hosts exactly one pluggy plugin
...                             (5 of these)
```

Each is an instance of one of two Go packages — `server-go/modules/mcp` and
`server-go/modules/pluggy` — with per-instance configuration. One plugin per module is a
hard rule, not a default: it makes the failure domain, the identity, the command namespace,
and the lifetime all the same boundary. A module whose plugin dies has lost its only job,
so it withdraws its commands and reports failed; nothing else is affected.

### The module exposes the interface; the plugin connects to it

The module owns a local endpoint and the plugin attaches to it, rather than the module
reaching out to find a plugin. For an MCP server that means the module owns the transport
endpoint and speaks MCP across it; for pluggy it means the module owns a socket that
`aimee-pluggy-host.py` — a thin Python shim loading exactly one pinned plugin distribution —
connects back to, reflects its hookspecs over, and then serves.

Both sides then look identical to everything upstream: a set of named, schema'd callables
with a module identity in front of them. Pluggy's hookspec-to-callable mapping is explicit —
one hookspec becomes one command, a `firstresult` hookspec returns the single result and a
non-`firstresult` one returns the list in pluggy's own call order. Hookwrappers are not
callables and are reported rather than silently dropped.

### Dynamic command registration — the new infrastructure

This is the part that does not exist. A command list gains a runtime layer over the static
one:

1. On plugin attach, the module enumerates its plugin's commands (name, description, input
   schema, side-effect class).
2. The module **registers** them, over the bus, using an outbound principal granted exactly
   that one request — the `economizerStorePrincipalRef` pattern.
3. The registry merges dynamic entries over the static `g_v1_routes`-derived manifest and
   serves the union from `GET /v1/cli/manifest` and `GET /v1/capabilities`.
4. Connected clients are notified so a long-lived session sees a plugin appear without
   reconnecting. A client that only polls the manifest still converges.
5. On plugin detach or module exit, registration is withdrawn and clients are notified again.

Commands namespace as `<module-instance>:<command>`, and with 15 instances that namespace is
the collision boundary — the existing `client[64]:tool[128]` grammar already sizes for it.
Precedence at the merge: static route > dynamic registration, and a dynamic command that
would shadow a static one is refused with a diagnostic, never silently shadowed.

### Instance identity is the open mechanical problem

`ModuleName` is the bus identity and `PrincipalRef` is a `uint32` assigned as a **compile-time
constant** today (65, 66). Fifteen instances that vary by deployment cannot use compile-time
constants. Either principal refs become allocatable at provisioning time with the grant
scoped per instance, or instances draw from a pre-provisioned pool with a declared ceiling.
This must be settled before the first instance ships, because it is the authorization
boundary, not a naming convenience.

## Slices

1. **[DONE] The Go MCP module, single instance.**
   `server-go/modules/mcp`: transport interface mirroring the C vtable, stdio transport,
   JSON-RPC 2.0 framing, handshake, `tools/list`, `tools/call`, served over two bus stages
   (invoke, declare-commands). Declaration reuses the memory module's `DCMD`/`DCMR` wire
   format so the C decoder stays single. Plugin tool names are folded onto the registry's
   `[a-z0-9_]` grammar, collisions are skipped rather than overwritten, and a skipped tool is
   not callable by the back door. Instanced dispatch in `cmd/aimee-module` via the `mcp-`
   prefix, with per-instance `ModuleName`.
   **Delivered with:** 21 package tests plus 4 host-dispatch tests, `-race` clean. The
   round-trip serialisation is mutation-verified (removing `Client.callMu` fails the
   out-of-order concurrency test).
   **Not included:** the plugin is never spawned — see Slice 4. The module serves its stages
   inert, declaring zero commands, until the supply-chain gate exists.

2. **[DONE] Instance identity, principal refs, and EVENT KINDS.**
   Two allocations, not one, and both are supplied rather than derived.
   `bus_host_serve_kind()` (core/event_bus/bus_route.c:109) binds one event kind
   to exactly ONE serving slot, so package-level event kinds would mean the first
   instance attaches and every other one is refused. Each instance is therefore
   allocated an aligned pair from a reserved range (`PluginEventBase` 11264), and
   its principal ref separately; both must agree with its `.grant` file
   (`principal_ref=` and the `serve=` list). Both are read from the environment
   the provisioning owns, and a module missing either **refuses to start** —
   deriving a base by hashing the instance name would collide two plugins onto
   one kind, and the bus refuses the second at attach with no indication of why.
   **Gate met:** the scale e2e runs 10 instances concurrently, each answering only
   for its own plugin, plus a deliberate collider on an allocated base which is
   denied at attach and does not displace the owner.

3. **[DONE] The registration driver, dispatch, every surface, and push.**
   Finishing this slice turned up that "registered" was doing far less than it read
   like. Four things were missing, not one:

   - **The driver.** `src/module_commands.c` probes the reserved range for attached
     declare stages, decodes `DCMR`, and registers into the command registry.
     Nothing in production had ever called `aimee_command_register`.
   - **Withdrawal.** `aimee_command_unregister_module()`; the registry was
     append-only, so a disconnected plugin could neither drop its commands nor
     re-register them (a duplicate is refused).
   - **DISPATCH — the one that made the rest decorative.** `aimee_command_find_method`
     and a command's `fn` had **no production caller at all**, so a registered
     command was listed everywhere and invocable nowhere. `POST /v1/commands/<group>.<verb>`
     now resolves through the registry and calls the handler, which for a plugin
     module dispatches back over the bus to its invoke stage. *(An earlier draft of
     this proposal made that worse: the manifest advertised a bare `/v1/commands/`
     prefix — a route no client could reach, which is exactly the "listed but
     unroutable" failure the registry exists to remove. Fixed to the full path,
     documented in `api/openapi-server-v1.yaml`, and the conformance gate now
     covers it.)*
   - **MCP tools/list.** Registry commands reached the CLI manifest and nothing
     else, so a plugin was invisible to the MCP clients that are the reason to host
     one. `mcp_group_tool_build()` existed for exactly this and had no callers; one
     multiplexed tool per group is now appended, which is also what keeps ~15 plugin
     modules from each adding N entries to a list `mcp_tool_profile.c` measured as a
     per-session tax.

   **Push.** `handle_initialize` has always advertised `tools.listChanged` — a promise
   that the client will be told rather than have to poll — and the notification was
   never sent, so a client that trusted the capability never re-listed. The registry
   now carries a generation, `mcp.tools_list` returns it, and the stdio bridge samples
   it and emits `notifications/tools/list_changed` when it moves. Polling under the
   hood because the bridge has no inbound channel; from the client's side it is push,
   which is what the capability actually promises. A server too old to report a
   generation simply never triggers one.

   **Gate met:** with nothing declared, `GET /v1/cli/manifest` and `tools/list` are
   unchanged; a registered command resolves by its RPC spelling and its handler is
   callable; a withdrawn one stops resolving so its route 404s rather than dispatching
   into a plugin that is gone.

4. **[DONE] Supply-chain admission — and a correction to what "parity" meant.**
   The original slice claimed OSV **and** permission **and** egress parity. Checking the
   code first showed two of those three were vacuous:

   - **OSV: real, and the actual blocker.** `mcp_client_registry.c` has always refused to
     START a client whose package carries malware advisories. Module-hosted plugins now go
     through the **same function**, not a copy: the gate was extracted to
     `modules/protocols/mcp/mcp_osv_gate.c` and both callers use it. A duplicate would have
     been the dangerous kind of bug — passing every test while enforcing a different policy
     on the path that runs untrusted code.
   - **Permission: NOT parity — new.** `plugin_permission_t` is parsed and stored and
     **never checked** anywhere; aimee.yaml clients have no per-call permission enforcement
     to be at parity with. Added anyway, because a plugin module's whole job is running
     someone else's tools: an instance declares a ceiling, a tool's required permission is
     read from its MCP `readOnlyHint` / `destructiveHint` annotations, and anything above
     the ceiling is neither declared nor callable. Unannotated tools count as `write`, and
     the default ceiling is `read`, so being wrong fails closed.
   - **Egress: NOT APPLICABLE.** `kb_egress_admission` is KB HTTP egress with org leases.
     `aimee.yaml` MCP clients do not go through it and neither should these. Claiming this
     as a deliverable would have meant bolting on something false.

   **Control is inverted to make the gate real.** A module no longer starts its plugin;
   it reports the argv it wants to run (`DCMP`) and waits. The daemon runs the gate and
   answers with a verdict bound to a SHA-256 of exactly those bytes — without that binding
   a module could report a benign command, collect an admit, and spawn something else.
   **Gate met:** the e2e proves a refused plugin never executes by OBSERVING it — the
   refused instance's plugin would create a sentinel file as its first action, and the file
   never appears. Mutation-verified: flipping the verdict to allow makes the sentinel
   appear and the assertion fire.

5. **[DONE] The pluggy module — which turned out to need no Go module at all.**
   `scripts/aimee-pluggy-host.py` serves one pluggy plugin as an MCP server: it builds a
   `PluginManager`, registers the host application's hookspecs, loads exactly one plugin,
   and reflects each implemented hookspec into one MCP tool. A `firstresult` hookspec
   returns its single result; a plain one returns the list in pluggy's own call order;
   hookwrappers are reported and NOT exposed, because a wrapper is not a callable surface.
   Pins (`--version`, `--sha256`) are verified BEFORE import, since importing executes the
   code and a check afterwards checks nothing.
   **The design claim held**: no `server-go/modules/pluggy` was needed. The existing MCP
   module hosts it unchanged, so there is no pluggy-specific transport, dispatch, or audit
   path anywhere above the shim.
   **Gate met:** 15 assertions against real pluggy 1.5.0, plus an e2e leg proving a pluggy
   plugin declares and is invoked over the unchanged MCP module, on both hosts.

6. **[DONE] Operator surface, provisioning, and transport parity.
   Retirement analysed and correctly NOT executed.**

   Delivered:
   - **Provisioning.** `scripts/provision-plugin-module.py` allocates the two things
     an instance cannot allocate for itself — `principal_ref` and an aligned
     event-kind pair — refusing to reuse either, writes the `.grant` the daemon
     reads, and prints the environment to start it with. Re-running is an update,
     not a second allocation. `unit-test-plugin-grant-provisioning` feeds its output
     to the daemon's OWN parser (`bus_runtime_policy_load_dir`), so the file format
     the two sides must agree on is checked rather than assumed.
   - **Operator surface.** `GET /v1/dashboard/metrics` carries a `plugins` array:
     event base, group, command count, state, last error. The state is the point —
     `refused` / `silent` / `pending` / `active` / `error` all otherwise present as
     "zero commands" and each needs a different action.
   - **SSE transport (`server-go/modules/mcp/sse.go`).** The last capability gap.

   ### Why the retirement is not a deletion, and what this proposal got wrong

   Slice 6 originally said "retirement of `src/modules/protocols/mcp` and the
   `config_mcp_client_t` boot path". That wording was wrong, and acting on it would
   have broken working features. `src/modules/protocols/mcp/` holds eight files, and
   only **two** are the remote-client path (`mcp_client.c`, `mcp_client_registry.c`).
   The rest are the native MCP tool surface (`mcp_tools*.c`, `mcp_tool_profile.c`,
   `mcp_skill_tools.c`, `mcp_group_tool.c` — which Slice 3 just wired into
   `tools/list`) plus `mcp_osv_gate.c`, which Slice 4 made the SHARED supply-chain
   gate for both paths. Deleting the directory would remove the gate that guards
   the very thing replacing it.

   Even narrowed to the client half, the registry has **live consumers** beyond boot:
   remote tools in the served list (`mcp_tools.c:1907`, `server/agent_tools.c:333`),
   remote dispatch (`modules/tools/agent_tools_dispatch.c`, three sites), and schema
   lookup (`server/agent_policy.c:259`).

   And the decisive one: the C client supports **SSE**, and this module did not.
   A plugin module that could only spawn a local process could not host a remote MCP
   server at all, so retirement would have deleted that capability outright — a
   functional gap, not merely a missing evidence gate. **That gap is now closed**
   (`sse.go`, six tests including bearer-on-every-request and relative-endpoint
   resolution, plus a module-level test proving the `Transport` seam makes the two
   transports interchangeable). The provisioner speaks `--sse-url` with a
   `--bearer-env`, carrying the env var NAME rather than the secret, because the
   declared argv is reported over the bus and logged.

   **Live-server evidence now exists** (`docs/validation/plugin-modules-live-on-252.md`):
   a real daemon on `.252`, a provisioned instance, and an `aimee.yaml` client on the
   SAME server — provisioning, admission, registration, `POST /v1/commands/...`
   dispatch to a real plugin, withdrawal, and the operator surface all verified over
   HTTP. It also found two defects nothing else had: an `aimee-kb` link regression
   from extracting the OSV gate, and an operator endpoint that reported nothing until
   another surface happened to trigger a collect.

   **What still blocks deletion, and it is not code:**
   1. **One instance on a scratch server is not sustained parity.** The path is now
      demonstrated to work end to end; a deletion should rest on real traffic over
      time, not a single green run.
   2. **No migration.** An `aimee.yaml` `mcp_clients` entry is one config line; a
      plugin module is a provisioned instance with a grant, a principal ref, an event
      pair and its own process. Retiring without a converter strands every deployment.
   3. **The gate must move first.** `mcp_osv_gate.c` has to leave
      `modules/protocols/mcp/` before that directory can shrink.

   These are operational sequencing, not missing implementation. Executing the
   deletion now would trade a working, shipped feature for an unproven one — the
   opposite of what an evidence-gated retirement is for. It stays open deliberately,
   with the blocking list above made concrete rather than left as "needs evidence".

## Risks / open questions

- **Principal-ref allocation (Slice 2) is the real blocker.** Everything else is
  straightforward; this one is an authorization design decision and should be settled first.
- **A module descriptor becomes a code-execution surface.** Recommend requiring an operator
  opt-in allowlist in `aimee.yaml` naming which plugin modules may run. One config key, and
  it keeps "deploy a module" from silently meaning "run this vendor's code".
- **15 processes is real cost.** Each is a Go runtime plus a plugin subprocess. Bounded and
  measurable, but it should be measured before the count grows, not after.
- **Client notification needs a defined delivery contract.** Fan-out to "any relevant
  connected clients" needs the relevance rule stated (all clients? scoped by workspace? by
  capability grant?) and a defined behaviour for a client that misses a notification. Polling
  convergence is the floor; it should be explicit that it is the floor.
- **The Go port must not fork MCP behaviour.** The C client is the specification. Slice 1's
  conformance gate exists so the Go implementation inherits its semantics rather than
  re-deriving them.

## Non-goals

- More than one plugin per module.
- Routing plugin calls over the event bus arena (the residual proposal owns that; still deferred).
- A general Go plugin API for Aimee. The pluggy module runs *someone else's* pluggy plugin;
  it does not define an Aimee hookspec surface.
- Hot-swapping a plugin inside a live module without a restart.

## Acceptance

- One Go module process hosts exactly one MCP server or one pluggy plugin, with per-instance
  bus identity and its own grant, at a scale of at least 15 concurrent instances.
- A plugin's commands appear in the command list at runtime and reach connected clients;
  they are withdrawn when the plugin or module goes away.
- With no dynamic registrations present, the command list is byte-identical to today.
- A pluggy plugin traverses the same registration and invocation path as an MCP plugin, with
  no pluggy-specific transport, dispatch, or audit code.
- Module-hosted plugins pass the identical OSV, permission, and egress gates as
  `aimee.yaml`-declared MCP clients.
- No new C is added outside `core/event_bus` and its communication surface.

```yaml acceptance
- {id: 1, tier: mechanical, check: "cd server-go && go test ./modules/mcp/..."}
- {id: 2, tier: mechanical, check: "cd server-go && go test ./modules/pluggy/..."}
- {id: 3, tier: mechanical, check: "cd server-go && go test ./bus/... -run ModuleInstance"}
- {id: 4, tier: mechanical, check: "make unit-tests TEST=test_cli_manifest_static_parity"}
- {id: 5, tier: integration, check: "curl -sk $AIMEE_API/v1/cli/manifest | jq -e '.routes | length > 0'"}
- {id: 6, tier: integration, check: "curl -sk $AIMEE_API/v1/capabilities | jq -e '.plugins != null'"}
```
