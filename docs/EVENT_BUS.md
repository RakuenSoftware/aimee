# Event bus

The event bus is the new spine inside `aimee-server` and `aimee-kb`. It replaces scattered
in-process side channels with one typed, ordered, bounded transport.

Today it carries observability and audit traffic as well as production module request/reply
decisions. It is also the contract that lets C and Go modules attach without sharing
implementation details.

## Why it exists

Before the bus, every new module needed its own queue, callback, logging path, and shutdown rules.
Completeness depended on finding every call site.

The bus gives each daemon one host and one full-stream tap:

```text
producer -> private outbound ring -> host -> private inbound ring -> consumer
                                  |
                                  +-> ordered capture and audit tap
```

That buys us:

- one place to order, observe, meter, and audit inter-module work;
- C and pure-Go clients with byte-for-byte conformance tests and no cgo;
- bounded backpressure instead of unbounded queues;
- point-to-point request/reply, fan-out events, cancellation, and typed capability errors;
- large payloads by shared-arena lease instead of another copy;
- capture files that preserve the accepted stream for inspection and audit replay;
- a clean path for policy checks, workflow events, telemetry, and separately shipped modules.

The last item is an extension surface, not a claim that every subsystem has moved already.

Sixteen production C-to-Go process batches now cover every supervised process:
`memory`, `learning`,
`routing`, `delegates`, `tools`, `workspace`, `git`, `skills`,
`response-composition`, `governance`, `workflows`, `roundtable`, `kb-synthesis`,
`runtime-web`, `control-web`, and `benchmarks`.
Each keeps its existing event kind and AMOD body contract, but the supervisor now
starts an authenticated Go process for that identity. C adapters serve as parity
fixtures; the bounded memory rerank, response-composition key, roundtable
verification-rubric, and benchmark IR-scoring stages do not mean those modules'
storage-heavy or daemon orchestration code has all moved to Go. Governance moves
the bounded response tool-policy decision; parsed-response mutation and its
broader identity/OIDC plane remain in their current C owners. Workflows moves
only the pure advance admission classification; the Go WFE remains the sole
lifecycle, persistence, scheduling, and transition owner. KB synthesis moves
only the deterministic code-unit grounding gate; curator queues, model calls,
storage, linking, promotion, and scheduling remain in their current owners.
Runtime web moves the bounded RPC-fault-to-HTTP-status decision. The server asks
that process over the event bus and places the returned status in its error
envelope; the physical Go HTTPS provider consumes the result without importing
or reimplementing the policy. Listener, authentication, sessions, proxying, and
assets remain provider-owned.
Control web moves bounded console-admin and fleet proxy-route authorization; its
physical Go provider and isolated process consume the same policy package. The
KB requests console-admin decisions from that process over its local event bus
and keeps no duplicate C allowlist. The production `memory.benchmark` RPC also
requests its MRR/NDCG/recall scoring from the benchmarks process; missing or
invalid module responses fail the benchmark instead of falling back locally.
Skills trigger-frontmatter matching is also process-owned: the filesystem resolver
loads the bounded skill body, then guardrails requests the match over event `7682`.
There is no local trigger parser on the production path; a missing or malformed
reply emits the conservative advisory rather than silently skipping it.
Learning signal sink selection is likewise event-bus-only. Before a signal is
persisted or any reranker, supersede, rule, or workflow proposal is queued, the
router requests the sink mask from the supervised learning process over event
`6145`. A missing or invalid response aborts ingestion; the C router keeps no
local signal-to-sink table.
Memory pre-injection confidence comes only from event `5893`. The server does
not replace an unavailable or malformed response with a locally selected tier;
it omits the context envelope, and the formatter rejects missing confidence.

## What is on it now

The server and KB publish these through the observability bridge:

- governed action audit rows;
- semantic guardrail decisions;
- server- and KB-side memory mutations;
- vault credential access;
- sandbox isolation degradation;
- MCP and tool-call activity;
- tool completion outcomes.

Separately, production request/reply traffic uses the module bridge. This includes governance's
response tool-policy decision: the server sends the policy gate, tool names, and upstream stop
reason to the supervised governance process and applies only the returned decision. A missing,
failed, or malformed governance reply fails closed; the response path does not evaluate that
policy locally. Workflow advance admission follows the same rule: the server supplies the
authoritative binding, current stage/state, and replay nonce to the supervised workflows process.
Only an explicit successful module decision can reach the workflow engine; bus failure returns an
error without advancing the work item.

Those bridges use two wire kinds: governed actions and semantic guardrail events. The action row is
PII-bounded; memory identities are fingerprinted before publication. The consumer drains accepted
events to their durable sinks. Graceful shutdown stops publishers, drains the rings, flushes
capture, then tears down the host.

A full queue is never a silent success. Publishers retry bounded backpressure; a stuck consumer
increments a visible drop counter and logs the failure.

## Ordering and delivery

The host assigns a monotonic sequence number before routing. The tap sees accepted events in that
order. Producers keep FIFO order; consumers only receive event kinds they subscribed to.

`publish` success means the producer ring accepted the event. It does not mean a consumer has
finished its work. Callers that need read-after-write behavior use the bridge flush point.

Requests and replies use a correlation ID. A missing server returns `capability_absent`. A reply
goes only to its requester. Notifications fan out to the authorized observers registered for that
kind. Wire version 3 permits a correlated request or reply to span ordered inline fragments:
`BUS_F_MORE` is set on every non-final fragment, and the first frame without it completes the
message. The host keeps the route pending until the request is complete, and cancellation retires
any partial request or reply.

Each client has its own queue pair. One slow consumer does not create an unbounded host queue.
Kinds declare whether they block or may shed under pressure. Sheds become typed overflow records in
the ordered tap.

## Payloads

Small payloads ride inside a ring slot. Trusted in-daemon publishers can put larger event payloads
in a lease in the shared arena:

1. the producer allocates and fills a span;
2. the frame carries its offset, length, and generation;
3. the host assigns references to the destination set;
4. each consumer releases its reference after reading;
5. the last release reclaims the span.

Generation checks reject stale references. Client reap releases abandoned references. Capture
materializes arena bytes into the record, so replay never depends on a live arena.

Arena allocation is for trusted code co-located with the host. Separately shipped module processes
do not allocate arena leases: the module protocol fragments request and reply bodies above the
negotiated inline budget and reassembles them at the endpoints. The current module-message limit is
16 MiB; an oversized or malformed stream is rejected and drained without being delivered to a
handler.

## Capture and replay

The host tap writes CRC-checked, length-framed capture files under the aimee config directory. A
record contains the original frame and materialized payload. Retention keeps the newest capture
sessions and prunes older files when a new capture starts.

Replay is observational: it presents the exact accepted stream to an inspector. It does not execute
tools or drive a module again.

The durable WORM audit ledger remains the security record. Capture is the ordered diagnostic and
replay layer above it. An off-host witness or anchor is still required for evidence against a fully
compromised host.

## Trust boundary

The v0 bus is Linux-only. The host creates anonymous `memfd` regions and admits clients over a Unix
`SOCK_SEQPACKET` socket, passing descriptors with `SCM_RIGHTS` only after admission.

The control region is read-only. Every admitted client maps only its queue pair plus the shared
arena; it cannot enumerate or map another client's rings. The arena is cooperative isolation for
trusted native modules, not a sandbox for hostile code.

The bus is intra-daemon. `aimee-server` and `aimee-kb` each host their own bus from the same
`libaimee-core-event-bus.a`; the thin client does not link it. Traffic between `aimee-server`, `aimee-kb`,
browsers, thin clients, and providers uses the authenticated `/v1` network surfaces through the
shared connection layer.

## Adding a consumer

Keep the contract small:

- assign a stable event kind and bounded schema;
- choose notification or correlated request/reply;
- declare block or shed behavior;
- register only the observers that need the kind;
- keep action policy before delivery and storage off the hot path;
- add C/Go vectors when the wire contract changes;
- test shutdown, queue exhaustion, malformed frames, client reap, and capture replay.

Use `bus_client_publish` for ordinary inline events. Use the arena for a trusted co-located event
publisher that needs a lease. Use the module request API for module calls; it selects ordered inline
fragmentation when a request or reply exceeds the inline budget.

Code lives under `src/core/event_bus/`. The public C client is `bus_client.h`; the pure-Go client and
module process runtime are under `server-go/bus/`. Both runtimes implement the same AMOD envelope,
fragmentation, deadline, cancellation, heartbeat, and host-epoch behavior. The source headers hold
the wire and arena invariants.
