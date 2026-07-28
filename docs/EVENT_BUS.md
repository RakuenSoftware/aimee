# Event bus

The event bus is the new spine inside `aimee-server` and `aimee-kb`. It replaces scattered
in-process side channels with one typed, ordered, bounded transport.

Today it carries observability and audit traffic. It is also the contract that lets C and Go
modules attach without sharing implementation details.

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

## What is on it now

The server and KB publish these through the observability bridge:

- governed action audit rows;
- semantic guardrail decisions;
- server- and KB-side memory mutations;
- vault credential access;
- sandbox isolation degradation;
- MCP and tool-call activity;
- tool completion outcomes.

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
kind.

Each client has its own queue pair. One slow consumer does not create an unbounded host queue.
Kinds declare whether they block or may shed under pressure. Sheds become typed overflow records in
the ordered tap.

## Payloads

Small payloads ride inside a ring slot. Large payloads use a lease in the shared arena:

1. the producer allocates and fills a span;
2. the frame carries its offset, length, and generation;
3. the host assigns references to the destination set;
4. each consumer releases its reference after reading;
5. the last release reclaims the span.

Generation checks reject stale references. Client reap releases abandoned references. Capture
materializes arena bytes into the record, so replay never depends on a live arena.

Arena allocation is currently for trusted code co-located with the host. A follow-on branch adds
cross-process attachment for inline events; it does not expose arena allocation or lease resolution.

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

The bus is intra-daemon. Traffic between `aimee-server`, `aimee-kb`, browsers, thin clients, and
providers still uses the authenticated `/v1` network surfaces.

## Adding a consumer

Keep the contract small:

- assign a stable event kind and bounded schema;
- choose notification or correlated request/reply;
- declare block or shed behavior;
- register only the observers that need the kind;
- keep action policy before delivery and storage off the hot path;
- add C/Go vectors when the wire contract changes;
- test shutdown, queue exhaustion, malformed frames, client reap, and capture replay.

Use `bus_client_publish` for inline events. Use the arena only when a real payload can exceed the
inline budget.

Code lives under `src/modules/bus/`. The public C client is `bus_client.h`; the pure-Go client is
under `server-go/bus/`. The source headers hold the wire and arena invariants.
