# Technical reference

This is the code map. Start with the [architecture](../docs/ARCHITECTURE.md) for process and trust
boundaries and the [manual](../MANUAL.md) for use.

## Runtime artifacts

| Artifact | Language | Owner |
| --- | --- | --- |
| `aimee` | C11 | DB-free thin CLI, hooks, MCP/ACP stdio, local transport |
| `aimee-server` | C11 | DB1, resource API, agents, tools, policy, vault, provider calls |
| `aimee-kb` | C11 | DB2, memory, documents, code graph, retrieval, curation |
| `aimee-wfe` | Go | workflow definitions, lifecycle, artifacts, scheduler, worktrees, forge |
| `aimee-runtime-web` | Go | authenticated browser proxy and UI service |

The server image supervises `aimee-server` and `aimee-wfe` as peers. Workflow lifecycle has one
writer: Go. The C server supplies typed agent, credential, policy, and forge resources; it does not
advance workflow state.

## Source map

```text
src/
  core/                 extraction-ready shared C libraries
    connection/         TCP, deadline/cancel, HTTP/1, auth, TLS/mTLS
    event_bus/           local shared-memory host and module clients
  cli_*                 thin-client commands and transport
  server/               server listeners, handlers, agent/resource plane
  kb/                   KB daemon and HTTP surface
  db1_client/           typed client for the server store module
  modules/              product modules and public include trees
    aimee/              PostgreSQL-backed server store contract
    postgres/           shared PostgreSQL transport module
    db2/                KB PostgreSQL/pgvector owner
    audit/              WORM audit, replay, observability bridge
    sandbox/            delegate isolation
  tests/                C unit and integration tests

server-go/
  bus/                   pure-Go event-bus client and conformance
  internal/wfe/          definitions, catalog, canonical snapshots
  internal/engine/       scheduler, runners, worktrees, forge, roundtables
  internal/db1/          workflow view of the store-module contract
  internal/api/          workflow/control-plane routes

runtime-web/             Go browser service
control-web/             Go knowledge-base administration service
frontend/                browser application
api/                     OpenAPI sources and SDK generation
scripts/                 checks, generation, deployment, smoke tests
```

Embedding and synthesis are KB-owned roles. Each can run inside the selected KB container or use a
remote endpoint. The current server configuration names one KB URL; fleet selection is the next
routing boundary. There is no standalone inference runtime artifact.

## Event bus

`src/core/event_bus/` provides the intra-daemon transport:

| File | Contract |
| --- | --- |
| `bus_wire.*` | fixed little-endian frame codec and version validation |
| `bus_ring.*` | SPSC bounded ring with release/acquire publication |
| `bus_region.*` | read-only control, private queue pair, shared arena regions |
| `bus_arena.*` | generation-checked leases and consumer references |
| `bus_host.*` | admission, sequence, routing, correlation, flow control, reap, tap |
| `bus_client.*` | C attach, publish, subscribe, request/reply, poll |
| `bus_capture.*` | ordered CRC-checked capture and observational replay |
| `module_protocol.*` | versioned pointer-free feature request/result envelope |
| `module_runtime.*` | authenticated process loop, dispatch, deadline and cancellation |

One host owns all `memfd` creation. An admitted client receives only its queue pair and shared arena.
The host stamps `seq` before routing and invokes the tap for every accepted event. Per-source FIFO is
guaranteed; consumers see sparse global sequence numbers because they receive only authorized kinds.

The observability bridge under `modules/audit/` is the current production consumer. It serializes
governed actions and semantic guardrail events, drains them asynchronously to typed stores, and
flushes capture. Server and KB bridges map memory, vault, sandbox, MCP, and tool activity into that
contract.

The Go client consumes the same golden wire vectors and links no C. Wire or layout changes require
version negotiation, regenerated vectors only when the wire changes, and cross-language conformance.

See [Event bus](../docs/EVENT_BUS.md).

## Storage ownership

DB1 and DB2 cannot call through each other's storage layer.

- Server builds define the DB2-disabled boundary and never link `libpq`.
- The `aimee-kb` service never links SQLite; its separately credentialed WORM
  worker links the same SQLite implementation as `aimee-server`.
- The thin client links neither database.
- Cross-tier operations use typed `/v1` clients.
- `scripts/check_tier_deps.sh`, link checks, and symbol checks enforce the boundary.

The server and KB worker share the complete `modules/audit/audit_worm.c` SQLite
implementation. They use separate files and keys. The KB worker's narrow link
closure adds libpq only for its immutable producer outbox.

The Go workflow engine reaches DB1 through the same store module as the server;
it owns workflow behavior but does not open a database.

## Modules

An owned module has:

- a narrow public header under its include tree;
- private implementation files;
- a descriptor naming dependencies, capabilities, config, event kinds, and routes;
- focused tests;
- current module documentation generated or attested from the descriptor.

Dependency direction matters more than physical directory. A lower owner does not include a higher
owner to reach state. Cross-owner calls use the public contract.

The old arbitrary plugin loader is retired. Optional modules attach through declared module or event
contracts; loading code into the core is not an extension API.

## HTTP and CLI contracts

Named `/v1` routes are described once and checked against handlers, OpenAPI, the thin-client route
table, and generated docs. The generic `/v1/rpc` path is gone.

Commands follow the same rule: implement the local operation or typed server route, add canonical
help, then expose it in the command registry. Do not advertise server-internal handlers from the
DB-free client.

Configuration scalar fields come from the field descriptor table. Defaults, parsing, schema, CLI,
browser settings, and generated docs must agree.

Provider-specific request and response JSON ends in translation modules. Core stages operate on
canonical IR.

## Build

From the repository root:

```bash
make -C src -j4
```

Useful targets:

| Target | Result |
| --- | --- |
| `all` | client, web service, server, KB, gateway/forwarder targets for this checkout |
| `server` | server, KB, and required service artifacts |
| `kb` | KB only |
| `lean` | stripped size-gated build |
| `frontend` | browser assets |
| `install` | install built artifacts |
| `clean` | remove generated build output |

The GNU Make build is canonical for Linux. Keep CMake in sync for native thin-client and portable
test builds.

## Tests and checks

```bash
make -C src unit-tests
make -C src lint
make -C src check-linking
make -C src integration-tests
```

Run the narrow unit executable while iterating. Test names follow `unit-test-<area>` under the build
object tree.

High-value gates:

| Gate | Protects |
| --- | --- |
| tier and link checks | DB1/DB2/thin-client ownership |
| module boundary checks | public-header and dependency contracts |
| route/API checks | descriptor, handler, OpenAPI, and client parity |
| CLI help coverage | command implementation and help parity |
| docs generation check | generated reference drift |
| proposal reconciliation | pending/done state and links |
| sanitizer call-site check | required allocator/concurrency test coverage |
| build variants | compile-time feature boundaries |
| event-bus conformance | C/Go wire and behavior parity |
| event-bus perf gate | bounded dispatch overhead |

Use ASAN/UBSAN for ownership and parsing changes. Use TSAN for rings, arena leases, publishers,
shutdown, worker pools, and any new cross-thread lifetime.

```bash
make -C src unit-tests \
  OBJDIR=build/obj-asan \
  EXTRA_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
  EXTRA_L_FLAGS="-fsanitize=address,undefined"
```

Go work runs with the race detector for scheduler, store, bus, API, and recovery changes:

```bash
cd server-go
go test -race ./...
```

## Generated documentation and APIs

```bash
make -C src docs-gen
make -C src docs-gen-check
make -C src api-conformance-check
make -C src server-api-conformance-check
make -C src gen-sdks
make -C src sdk-parity-check
```

Do not hand-edit `docs/gen/` or generated SDKs. Change the registry, descriptor, or OpenAPI source.

## Adding a route

1. Choose the owning service.
2. Define the operation, auth, scope, write tier, bounds, and error contract.
3. Add the canonical descriptor and OpenAPI operation.
4. Implement the handler and typed client path.
5. Add negative auth/scope/body tests and one success test.
6. Regenerate docs and run route parity checks.

Internal workflow resource routes also need peer identity and worktree/forge confinement tests.

## Adding an event

1. Assign a stable kind and bounded schema.
2. Choose notification or correlated request/reply.
3. Declare block or shed behavior.
4. Register the minimum observer set.
5. Keep action authorization before delivery and storage off the hot path.
6. Test malformed frames, queue full, client reap, shutdown drain, and capture replay.
7. Update C/Go vectors if the wire changed.

Use inline payloads unless the real schema can exceed the inline budget. Arena producers must be
co-located with the host today.

## Adding configuration

Add one field descriptor with type, default, bounds, persistence, environment override, secret
classification, and restart behavior. Table-driven parsing and schema generation should pick it up.
Add custom code only when the field is not a flat scalar.

## Debugging

- Use request IDs across client, server, KB, workflow, and provider logs.
- Preserve the first failing delegate attempt.
- Inspect workflow lifecycle state before restarting a parked run.
- Treat a nonzero bus drop counter as lost observability, not harmless load.
- Classify capture as complete, open, truncated, or corrupt before replay.
- Run `aimee audit verify` before modifying audit state.
- Check the owner boundary before fixing a symptom in the wrong process.
