# Spec: Aimee shared-memory event bus — wire and segment specification (v0 DRAFT)

- **State:** DRAFT v0 — 2026-07-23. Normative for the bus host and every bus client; exact byte
  offsets and sizes are frozen by the conformance vectors, not by prose, and may change until v1.
- **Owner:** `module-runtime` (per
  [`module-runtime-source-ownership-and-build.md`](module-runtime-source-ownership-and-build.md)).
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)
- **Validated by:** the single in-source **C bus host**, the **C and Go reference clients**, and the
  cross-language conformance suite. Two independent client implementations, not one, keep this spec
  honest.
- **Date:** 2026-07-23

## Scope

This spec defines the intra-service shared-memory event bus: the segment layout, the ring buffers,
the event wire encoding, the attach/admission handshake, routing, flow control, ordering, versioning,
the governance/audit tap, and capture/replay format. It is what a bus client in any language
implements to interoperate with the single in-source C bus host.

Out of scope (owned elsewhere): the descriptor and **event-contract schema** — which event kinds a
module publishes/subscribes/requests — owned by `module-runtime`; **admission policy** (identity,
install, `execution-policy`) — owned by core, invoked at attach; **cross-service** (Runtime↔Control)
transport — the network path, not this bus; and module business logic.

## Terminology

- **Bus host** — the single in-source C implementation that owns the segment, admits clients, routes
  events, and runs the tap. Exactly one per service (Runtime, Control Plane).
- **Bus client** — a per-language library a module uses to attach and publish/subscribe/request.
- **Segment** — the shared-memory region host and admitted clients map.
- **Ring** — a single-producer/single-consumer (SPSC) lock-free queue in the segment.
- **Queue pair** — one client's inbound ring (host→client) and outbound ring (client→host).
- **Event** — one typed message: a fixed header plus a payload reference.
- **Handle** — the opaque per-client identity core grants at admission; indexes the client's queue
  pair and its authorization.

## Topology

Per service: one host, N admitted clients. The host is the only participant that reads or writes more
than one client's queues; each client maps **only its own queue pair** and the shared payload arena.
There is no client↔client shared ring — all routing goes through the host, which is what makes
observer routing and the audit tap total (suite invariants 13, 18). A cross-service request does not
use this segment; it leaves on the network transport carrying the same event encoding.

## Segment layout

```
+===========================================================+
|  Control block (fixed, cache-line aligned)                |
|    magic | spec_version | layout_version | flags          |
|    segment_size | ctrl_size | arena_off | arena_size       |
|    queue_dir_off | queue_slot_count | host_epoch           |
|    host_heartbeat (monotonic) | host_pid/liveness          |
+-----------------------------------------------------------+
|  Queue directory: queue_slot_count entries, one per        |
|  admittable client slot:                                    |
|    handle_id | state | principal_ref | inbound_ring_off     |
|    outbound_ring_off | ring_capacity | client_heartbeat     |
+-----------------------------------------------------------+
|  Rings region: per slot, inbound + outbound SPSC rings      |
+-----------------------------------------------------------+
|  Payload arena: shared, allocatable regions referenced by   |
|  events too large to inline (see Payloads)                  |
+===========================================================+
```

The control block is versioned and read-mostly. `host_epoch` changes on host restart and invalidates
every prior handle and mapping (clients must re-attach). Heartbeats give liveness both ways: a client
whose heartbeat stalls is reaped by the host; a stalled `host_heartbeat`/`host_epoch` change tells a
client the host is gone.

## Rings

Each ring is a lock-free **SPSC** queue with a producer index and a consumer index on separate cache
lines to avoid false sharing. Because every ring has exactly one writer (the client for its outbound,
the host for the client's inbound) and one reader, no lock is needed — publication uses release
stores and consumption uses acquire loads. A ring is a power-of-two slot array; `full` and `empty`
are distinguished by the producer/consumer index pair (not by a count that could alias). Slot size is
fixed per segment and carries the event header plus a small inline payload budget; larger payloads go
to the arena.

## Event encoding

Fixed-size little-endian header (illustrative field set; exact offsets frozen by vectors):

| Field | Purpose |
|---|---|
| `magic` / `hdr_flags` | frame sync + flags (inline-payload, arena-payload, request, reply, notification, cancel) |
| `wire_version` | encoding version (matches negotiated version) |
| `event_kind` | numeric id resolved against the event-contract kind registry |
| `correlation_id` | ties a reply to its request; zero for one-way notifications |
| `src_handle` / `dst_handle` | source client and (host-filled) destination; clients set src, host sets dst on routing |
| `principal_ref` | the attested principal for this event (for the tap/policy) |
| `seq` | host-assigned monotonic dispatch sequence (authoritative order for capture/replay) |
| `logical_ts` | logical clock for ordering across sources without wall-clock trust |
| `payload_len` | length of payload |
| `payload_ref` | inline (in-slot) offset, or arena `(offset,len)` when the arena flag is set |

Payload bytes are the IR event body; their per-kind schema is owned by the event-contract schema, not
this spec. This spec defines only the framing.

## Message patterns

- **Notification** — one-way; `correlation_id = 0`; no reply expected.
- **Request/reply** — the requester sets a fresh `correlation_id`; the serving module replies with the
  same `correlation_id`; the host routes the reply point-to-point back to the requester only.
- **capability_absent** — if the target kind has no ready authorized server, the host synthesizes a
  typed `capability_absent` reply rather than dropping the request.
- **cancel** — a requester may cancel an outstanding `correlation_id`; delivery of cancel is
  best-effort and idempotent.

## Attach and admission handshake

Attach is **not** a free mmap. A client requests attach over a small control channel; the host invokes
core admission (identity/attestation, installation, `execution-policy`) — owned by `module-runtime`,
not this spec — and only on success allocates the client a queue-directory slot, maps **only** that
slot's queue pair and the arena into the client, and returns the `handle_id` and negotiated version.
An unadmitted process gets no slot, no handle, and no mapping. Detach (graceful or by reaped
heartbeat) frees the slot; `host_epoch` bumps invalidate all handles at once.

## Routing and the tap

The host drains each client's outbound ring, and for each event: stamps `seq`, checks
`execution-policy` (synchronously for action-class kinds), offers it to the **governance/audit tap**,
resolves authorized observers of `event_kind` (observer routing, suite invariant 18), and writes the
event into each authorized subscriber's inbound ring (or the single requester's ring for a reply). A
client never sees an event for a kind it is not an authorized observer of. The tap is the sole
full-stream reader and is host/trust-kernel infrastructure, not a module.

## Flow control and backpressure

Rings are bounded, so backpressure is explicit. v0 uses **credit-based** flow control: a producer may
publish only up to the consumer's advertised free slots; when credits are exhausted it blocks with a
bounded wait or returns `would_block` (the client library chooses per call). The host protects itself
from a slow client by bounding per-client in-flight and, past a threshold, applying the descriptor's
declared overflow policy for that client's kinds (block the producer, or shed with a typed
`overflow` event) — never an unbounded host-side queue. A wedged client is reaped by heartbeat; its
slot is reclaimed.

## Ordering and delivery

- Per source ring: strict FIFO.
- Global order is defined only by the host's `seq`; there is no cross-ring order before the host
  stamps it. The **capture/replay** stream is exactly the host's `seq` order.
- Delivery within a live segment is exactly-once per destination; on `host_epoch` change (restart),
  in-flight events not yet `seq`-stamped are lost and clients re-attach — the durable audit chain,
  not the ring, is the persistence boundary.

## Versioning and negotiation

`spec_version`/`layout_version` are in the control block; `wire_version` is per event. A client
declares its supported version range at attach; the host accepts the highest common version or refuses
the attach with a typed reason. Within a major version, fields may be added only in reserved space and
ignored by older readers; a layout-incompatible change bumps `layout_version` and requires re-attach.

## Payloads and zero-copy (open design point)

Small payloads inline in the ring slot (a copy, but cheap). Payloads above the inline budget go to the
shared **arena** and are referenced by `(offset, len)`. Zero-copy across a process boundary needs an
ownership/lifetime discipline: v0's baseline is **host-mediated arena leases** — the producer requests
an arena region, fills it, publishes the ref; the consumer reads and releases; the host tracks the
lease and reclaims on release or on client reap. Whether to allow direct producer→consumer zero-copy
without a copy through the host, and how to bound arena fragmentation and huge/streamed payloads, are
**open** and must be settled with the conformance vectors before v1.

## Security

- The segment is created by the host with restrictive OS permissions; only admitted clients receive a
  handle, and each maps only its own queue pair plus the arena — never another client's rings.
- The host is the only multi-queue reader/writer; a client cannot read another client's traffic,
  matching observer routing at the memory level, not only the API level.
- Arena access is bounded to leased regions; a client cannot read arbitrary arena bytes outside its
  active leases (enforced by the client library and validated by conformance; a hostile native client
  in the trusted tier is out of this boundary's threat model — untrusted clients run sandboxed per
  [`module-loader.md`](module-loader.md)).

## Conformance

The spec is validated, not defined, by its implementations:

- **Wire vectors** — shared encode/decode fixtures for headers, each message pattern, version
  negotiation, and error frames; the C and Go reference clients must both produce and accept the exact
  bytes.
- **Interop** — the single in-source C host driven with a C client and a Go client on one segment,
  exchanging notifications and request/replies in both directions, including `capability_absent`,
  cancel, backpressure/credit exhaustion, and reaped-client recovery.
- **No-second-host** — there is exactly one host implementation; conformance forbids a second.
- **Third-language proof** — a client in a language other than C/Go, written only from this spec and
  the vectors, attaches and interoperates — the credibility test for "any language."

## Non-goals

- Cross-service transport (network path between Runtime and Control Plane).
- The event-contract/kind schema and descriptor graph (owned by `module-runtime`).
- Admission policy semantics (owned by core; invoked here at attach).
- Freezing exact byte offsets in prose — the vectors are authoritative.

## Open questions

- Direct zero-copy vs host-mediated arena leases; arena allocation/fragmentation strategy.
- Huge and streamed payloads (chunking over the ring vs a dedicated arena stream).
- Final backpressure policy (credit-based confirmed for v0; shed-vs-block defaults per kind).
- NUMA/placement of the segment and rings on multi-socket hosts.
- Exact slot size and inline-payload budget (set with the performance-budget baseline).
