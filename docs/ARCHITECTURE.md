# Architecture

aimee is a local-first runtime between AI tools, model providers, code, and durable knowledge. It
keeps the fast client separate from stateful services, gives storage one owner, and sends internal
module events through one bounded bus.

## Processes

```mermaid
flowchart LR
    T[AI tool] -->|hooks / MCP / ACP| C[aimee thin client]
    B[Browser] --> W[aimee-runtime-web]

    subgraph RUNTIME[Server container]
        S[aimee-server resource plane]
        F[aimee-wfe workflow harness]
        SB[server event-bus host]
        SM[supervised process modules]
        M[aimee store module]
        PG[postgres module]

        S <--> SB
        F -->|typed DB1 calls| SB
        SB <--> SM
        SB <--> M
        M --> PG
    end

    subgraph KNOWLEDGE[KB container]
        K[aimee-kb resource plane]
        KB[kb event-bus host]
        KM[supervised process modules]
        K <--> KB
        KB <--> KM
    end

    C -->|local UDS or authenticated /v1| S
    W -->|authenticated /v1| S
    W -->|workflow API| F
    F -->|typed resource calls| S
    S -.->|optional typed /v1| K
    S -->|provider API| P[model providers]
    K -->|local sidecar or remote synthesis endpoint| X[synthesis model]
    PG --> D1[(PostgreSQL + personal vectors)]
    KM --> KPG[postgres module]
    KPG --> D2[(PostgreSQL + vectors)]
```

Both containers use the same application image. A Go `server` or `kb` composition module
establishes the immutable first-boot identity and supervises the standard module processes.
Core's event bus rejects duplicate or conflicting roles. Both compositions have the same
PostgreSQL and memory modules, a local Vault, and independent model identities. The existing
C resource hosts remain during the transition of their domain handlers into Go modules.

| Process | Owns | Does not own |
| --- | --- | --- |
| `aimee` | CLI parsing, local hooks, MCP/ACP stdio, client filesystem access | databases, server policy, provider credentials |
| `aimee-server` | sessions, DB1, agents, tools, policy, vault, provider calls, `/v1` resource plane | DB2, workflow lifecycle |
| `aimee-wfe` | workflow definitions, scheduling, artifacts, retries, gates, worktrees, forge lifecycle | agent credentials, KB data, general chat |
| `aimee-kb` | shared DB2 knowledge, documents, code graph, retrieval, curation, and local or external model services | Server personal memory, workflow state, another KB's corpus |
| `aimee-runtime-web` | browser auth, session proxying, UI delivery | product databases and workflow decisions |

`aimee-server` and `aimee-wfe` run as supervised peers in the server image. If either exits, the
container terminates both and fails. The C server returns `410 Gone` for retired workflow lifecycle
routes; there is one workflow writer.

The browser, KB console, and optional ambient gateway are clients. They do not bypass the service
that owns the data they display.

The diagram shows an optional KB connection. A standalone Server uses its own embedding and
optional synthesis services with no KB. Routing among several KBs remains a target topology.
See [KB fleet and model placement](KB_FLEET.md).

## Two transports

aimee has two distinct communication layers.

### Between processes and machines

Named `/v1` HTTP routes carry client, browser, server-to-KB, and provider traffic. Local clients use
a filesystem-protected Unix socket. Remote clients use TLS plus bearer or mTLS identity and route
capabilities. The generic `/v1/rpc` endpoint is retired.

The server-to-KB boundary is typed HTTP. `aimee-server` never links libpq or sends SQL. The KB never
opens DB1.

### Inside a daemon

The event bus carries typed module events. It is not a network transport and does not replace
authenticated `/v1` calls.

## Module doctrine

This is the target the migration is moving toward. Where it disagrees with the process table
above, the doctrine is the design and the table is the current state.

C owns exactly four things:

1. the event bus, for inter- and intra-module communication;
2. mTLS, MCP, and communication with the outside world;
3. the HTTP and related APIs that expose internal information;
4. the tap for auditing and governance.

C ends up a small codebase. Everything else, meaning all logic and all state, including registries,
queues, rendezvous, provider and turn binding, and policy, lives in a Go module under
`server-go/modules/`. A concurrency primitive is not transport: a condition variable or a
FIFO is logic, and belongs in Go as channels.

Modules get no HTTP APIs. A module never binds a socket and never accepts a connection.
`runtime-web` and `control-web` are modules and get no exemption: their HTTP surface belongs
to C, while their logic stays in the module and speaks only the bus.

Direct module-to-module communication is forbidden. Every exchange between modules goes over
the event bus, so that governance and auditing see all of it.

External communication is banned from every module, with two structural doors: communication
initiated from outside arrives over the event bus through C, and communication a module
initiates leaves through the `egress` module. Direction selects the door; it never grants a
module the right to open a connection itself. Until `egress` exists, a module may make
internally-initiated outbound calls (the delegate module reaching an LLM and the git module
reaching its forge are both valid and load-bearing), but every such call must be logged to
the event bus. There is no unmonitored external communication. See [One egress
module](proposals/pending/module-egress-single-point.md).

```mermaid
flowchart LR
    P[producer or module bridge] --> O[private outbound ring]
    O --> H[bus host]
    H --> I[private inbound ring]
    I --> C[consumer]
    H --> T[ordered full-stream tap]
    H <--> R[shared payload arena]
    T --> CAP[diagnostic capture]
    T --> OBS[authorized observability drain]
    P -. declared ledger metadata event .-> O
    H --> DS[durability sink consumer]
    DS --> WORM[(daemon WORM ledger)]
    H -. overflow / producer-reap facts .-> WORM
    CAP -. gap / prune facts .-> WORM
```

Each daemon creates one host. Admitted clients receive a read-only control region, their own queue
pair, and the shared arena. The host owns admission, sequence numbers, subscriptions, routing,
correlations, credits, and client reap.

The current load-bearing consumer is observability:

- governed actions;
- semantic guardrail events;
- server and KB memory mutations;
- vault credential access;
- sandbox isolation degradation;
- MCP and tool-call activity;
- tool completion outcomes.

The host tap writes its own ordered diagnostic stream. Ledger-classified module
calls emit bounded intent/reply metadata through the durable emitter, while
capture gaps, pruning, overflow, and producer reap use direct rare-event sinks.
Those WORM paths do not depend on capture being healthy. Capture can therefore
remain prunable without making an absent interval look idle.

Small events are inline. Large events use generation-checked arena leases. Backpressure is bounded;
a producer blocks or receives `would_block`, and shed delivery is represented by an overflow event.
There is no unbounded host queue.

See [Event bus](EVENT_BUS.md).

## Storage

There are two product data tiers and separate WORM evidence stores.

| Store | Owner | Contents |
| --- | --- | --- |
| DB1, PostgreSQL | `aimee` domain module through `postgres` | sessions, working memory, local state, agent jobs, policy and audit state, caches, workflow definitions and lifecycle rows |
| DB2, PostgreSQL + pgvector | `aimee-kb` | shared memories, documents, facts, evidence, code graph, embeddings, curation state |
| Server WORM, SQLite | `aimee-server` | append-only evidence chain, keyed checkpoints, sealed snapshots |
| KB WORM, SQLite | `aimee-kb-worm` | append-only KB evidence chain, keyed checkpoints, sealed snapshots |

The DB1/DB2 boundary is compile-enforced:

- the server links no database driver at all: it reaches DB1 through the store module
  over the bus, and DB2 through typed `/v1` calls;
- KB builds never open DB1;
- thin clients link neither;
- calls across the boundary use public typed APIs.

The server and KB worker share the complete SQLite WORM implementation, not two
engine-specific approximations. Their files, keys, and process compartments are
separate. PostgreSQL DB2 retains only the immutable producer outbox and delivery
ledger needed for atomic KB mutation intent and idempotent delivery.

Both compositions use a separate standard PostgreSQL 18 container with ordinary storage by
default and opt-in LUKS2 encryption. When LUKS is enabled, the local Vault unlocks the store
before SQL initialization; the encryption passphrase persists only in Vault. Personal memory and
its vectors stay in Server storage, even when a shared KB is connected.

See [Storage tiers](STORAGE_TIERS.md).

## Request paths

### Local tool hook

1. The coding tool starts the thin client with a small JSON event.
2. The client sends it over the local Unix socket.
3. The server authenticates the socket by filesystem ownership.
4. Policy and guardrails return allow, deny, or context.
5. The action and verdict enter the event-bus audit path.

The client opens no database and starts no daemon. Warm state stays in `aimee-server`.

### Remote thin client

1. `remote.conf` resolves the server URL, certificate pin, bearer, and client identity.
2. Native TLS verifies the endpoint.
3. The server maps the principal to route capabilities and a write tier.
4. Read operations dispatch normally. A write also needs a KB-signed identity token, matching
   server/team trust, and the user's grant.
5. Workspace and document commands upload bytes from the client; the server never resolves a path
   on the client's machine.

### Memory write and recall

1. A client calls the server `/v1` surface.
2. The server authorizes the principal and calls the KB's typed endpoint.
3. The KB owns the transaction, lexical/dense indexes, and evidence.
4. Mutations publish a PII-safe audit identity on the KB bus.
5. Recall returns bounded evidence; optional synthesis runs in that KB's model-specific sidecar or
   at its configured remote endpoint.

### Delegate turn

1. The server admits a role/persona request against agent limits, budget, policy, and credentials.
2. The workspace authority selects a local, remote-runner, or isolated-container backend.
3. Provider requests become canonical IR, then one provider translation at the edge.
4. Tool calls pass schema, policy, worktree, and sandbox checks.
5. Tool activity and outcomes publish to the event bus.
6. The server returns a compact result and preserves the durable job, cost, and audit record.

### Workflow run

1. **Admit immutable input.** `aimee-wfe` validates and snapshots the definition and request.
2. **Persist before dispatch.** The scheduler writes the transition before starting work.
3. **Cross a typed resource boundary.** Agent and roundtable work calls the C server; credentials never enter
   the workflow rows in DB1.
4. **Confine each slice.** Every child gets its own worktree and branch.
5. **Keep evidence separate.** Verification, review, merge, and forge operations produce distinct artifacts.
6. **Stop at human authority.** A human gate parks until a browser or API decision arrives. The service stores
   a hashed approval artifact and lifecycle event, not a cryptographic principal signature. A crash
   resumes from the durable event log.

## Trust boundaries

The important boundaries are:

- **local user to local socket:** filesystem permissions are the identity;
- **remote client to server:** TLS, certificate pinning, bearer/mTLS identity, route capabilities,
  and write grants;
- **browser to server:** browser login, CSRF protection, principal propagation, and the same server
  authorization;
- **server to KB:** service bearer/TLS plus typed APIs;
- **workflow to resource plane:** local attested peer plus narrow internal operations;
- **agent to workspace:** assigned worktree and path policy;
- **delegate to host:** container/process sandbox, no ambient credentials, explicit egress;
- **service to provider:** vault resolution, provider allowlist, budget, rate, and egress policy;
- **module to bus:** admission, per-client rings, authorized event kinds, bounded flow control.

An admitted native bus module is trusted code. The shared arena is cooperative isolation, not a
sandbox. An external WORM witness is required if the threat includes full host compromise.

See [Security](SECURITY.md).

## Deployment shapes

| Shape | Use | Tradeoff |
| --- | --- | --- |
| Standard local Server | Single-user operation with local PostgreSQL and embedding | No KB or Docker-socket access required |
| Managed Server | Browser-managed local embedding and optional synthesis | Requires host Docker-socket access |
| Shared KB | Separate optional knowledge deployment using the same application image | Separate role, Vault, PostgreSQL, and access authority |
| KB fleet | Several capability-declaring KB containers | Target routing path; not integrated in this checkout |
| Local source install | Development and debugging | Host owns dependencies and services |
| Thin client | Developer machine | Needs a reachable Server |

The unified application image selects one role on first boot and cannot run both roles at once.
PostgreSQL and models remain separate service containers.

## Code boundaries

- `src/core/event_bus/`: event transport, arena, host, client, capture.
- `src/modules/`: owned C modules and public headers.
- `src/server/`: C resource plane and `/v1` handlers.
- `src/kb/`: KB daemon and DB2-facing routes.
- `server-go/`: workflow control plane and pure-Go bus client.
- `runtime-web/`: browser-facing Go service.
- `frontend/`: browser application.
- `api/`: OpenAPI sources and generated SDKs.

[Technical reference](../src/README.md) covers build targets, linkage, tests, and source ownership.
