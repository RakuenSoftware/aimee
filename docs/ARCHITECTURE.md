# Architecture

aimee is a local-first runtime between AI tools, model providers, code, and durable knowledge. It
keeps the fast client separate from stateful services, gives storage one owner, and sends internal
module events through one bounded bus.

## Processes

```mermaid
flowchart LR
    T[AI tool] -->|hooks / MCP / ACP| C[aimee thin client]
    B[Browser] --> W[aimee-runtime-web]
    C -->|local UDS or authenticated /v1| S[aimee-server]
    W -->|authenticated /v1| S
    W -->|workflow API| F[aimee-wfe]
    F -->|typed resource calls| S
    S -->|typed /v1| K[aimee-kb]
    S -->|provider API| P[model providers]
    K -->|embed / rerank / synth| L[aimee-llm]
    S --> D1[(DB1 SQLite)]
    F --> WF[(workflow SQLite)]
    K --> D2[(DB2 PostgreSQL + pgvector)]
```

| Process | Owns | Does not own |
| --- | --- | --- |
| `aimee` | CLI parsing, local hooks, MCP/ACP stdio, client filesystem access | databases, server policy, provider credentials |
| `aimee-server` | sessions, DB1, agents, tools, policy, vault, provider calls, `/v1` resource plane | DB2, workflow lifecycle |
| `aimee-wfe` | workflow definitions, scheduling, artifacts, retries, gates, worktrees, forge lifecycle | agent credentials, KB data, general chat |
| `aimee-kb` | DB2, memory, documents, code graph, retrieval, curation | DB1, workflow state, model serving |
| `aimee-runtime-web` | browser auth, session proxying, UI delivery | product databases and workflow decisions |
| `aimee-llm` | embedding, reranking, synthesis inference | knowledge storage and curation policy |

`aimee-server` and `aimee-wfe` run as supervised peers in the server image. If either exits, the
container terminates both and fails. The C server returns `410 Gone` for retired workflow lifecycle
routes; there is one workflow writer.

The browser, KB console, and optional ambient gateway are clients. They do not bypass the service
that owns the data they display.

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

```mermaid
flowchart LR
    P[producer] --> O[private outbound ring]
    O --> H[bus host]
    H --> I[private inbound ring]
    I --> C[consumer]
    H --> A[ordered audit / capture tap]
    H <--> R[shared payload arena]
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

The host tap sees accepted events before routing. It writes an ordered capture stream while the
consumer drains typed records to the WORM ledger or DB1. Storage stays off the request path.

Small events are inline. Large events use generation-checked arena leases. Backpressure is bounded;
a producer blocks or receives `would_block`, and shed delivery is represented by an overflow event.
There is no unbounded host queue.

See [Event bus](EVENT_BUS.md).

## Storage

There are two product data tiers and one workflow store.

| Store | Owner | Contents |
| --- | --- | --- |
| DB1, SQLite | `aimee-server` | sessions, working memory, local state, agent jobs, policy and audit state, caches |
| Workflow SQLite | `aimee-wfe` | definitions, immutable snapshots, work items, lifecycle events, artifacts, retries and parks |
| DB2, PostgreSQL + pgvector | `aimee-kb` | durable memories, documents, facts, evidence, code graph, embeddings, curation state |

The DB1/DB2 boundary is compile-enforced:

- server builds disable DB2 and never link libpq;
- KB builds disable DB1 and never link SQLite;
- thin clients link neither;
- calls across the boundary use public typed APIs.

The WORM hash primitive is the narrow exception shared by both stores. It contains hashing only—no
database handles or queries—so both stores produce the same chain format.

New KB containers run a private PostgreSQL 18 cluster when no external `AIMEE_DB2_URL` is set. It is
still DB2, still owned by the KB, and still independently exportable.

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
5. Recall returns bounded evidence; optional rerank and synthesis go through `aimee-llm`.

### Delegate turn

1. The server admits a role/persona request against agent limits, budget, policy, and credentials.
2. The workspace authority selects a local, remote-runner, or isolated-container backend.
3. Provider requests become canonical IR, then one provider translation at the edge.
4. Tool calls pass schema, policy, worktree, and sandbox checks.
5. Tool activity and outcomes publish to the event bus.
6. The server returns a compact result and preserves the durable job, cost, and audit record.

### Workflow run

1. `aimee-wfe` validates and snapshots the definition and admitted request.
2. The scheduler persists a transition before dispatching work.
3. Agent and roundtable work uses typed resource calls to the C server; credentials never cross
   back into the workflow store.
4. Each slice gets a confined worktree and branch.
5. Verification, review, merge, and forge operations produce separate artifacts.
6. A human gate parks until a signed human decision arrives. A crash resumes from the durable event
   log.

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
| Managed server | One server container launches KB and inference from the browser | Needs the host Docker socket |
| Split stack | Separate server, KB, and inference containers | More explicit; no server Docker control required |
| External DB2 | KB uses managed PostgreSQL | Operator owns backup, TLS, extensions, and latency |
| Local source install | Development and debugging | Host owns dependencies and services |
| Thin client | Normal developer machine | Needs a reachable server; keeps state off the client |

The old combined appliance image is gone.

## Code boundaries

- `src/modules/bus/`: event transport, arena, host, client, capture.
- `src/modules/`: owned C modules and public headers.
- `src/server/`: C resource plane and `/v1` handlers.
- `src/kb/`: KB daemon and DB2-facing routes.
- `server-go/`: workflow control plane and pure-Go bus client.
- `runtime-web/`: browser-facing Go service.
- `frontend/`: browser application.
- `api/`: OpenAPI sources and generated SDKs.

[Technical reference](../src/README.md) covers build targets, linkage, tests, and source ownership.
