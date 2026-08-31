# Event bus: arena payloads (developer guide)

This is the practical guide to sending and receiving **large** event-bus payloads,
those that do not fit the inline budget. It complements the design rationale in
[`EVENT_BUS_DECISIONS.md`](EVENT_BUS_DECISIONS.md) (D3 leases, D7 blast radius, D10
capture) and the delivery map in
[`EVENT_BUS_FEATURE_TREE.md`](EVENT_BUS_FEATURE_TREE.md).

If your payload fits the inline budget, you do not need any of this. Use
`bus_client_publish` / `bus_client_request` / `bus_client_reply` and read
`ev.payload` from `bus_client_poll`. The audit/guardrail/memory bridges are all
inline; their rows are bounded well under the budget.

## When to use the arena

A frame's payload is either **inline** (it rides in the ring slot, after the
64-byte header, and must fit `inline_budget`) or **arena** (the bytes live in the
shared arena region; the frame carries only a *lease reference*). Use the arena
when a single event's payload can exceed `inline_budget`, for example a tool
call's arguments or results. Choose per event: a small payload of the same kind
still goes inline.

## The one hard constraint: producers are co-located and trusted

The arena's lease **table** is host-private (D3): it lives in the host process's
own memory, not in shared memory, so a client cannot forge a lease's lifetime.
The consequence is that **only code in the same process as the host can produce or
resolve an arena payload**. It needs a pointer to the host's `bus_arena_t`. A
thin cross-process `bus_client` can attach and exchange inline frames, but it
cannot allocate a lease. This is deliberate and consistent with D7: the bus is
linked only into the trusted daemons (aimee-server, aimee-kb), and arena
production is a co-located side channel within one of them.

So in practice you hold both the `bus_host_t` and the `bus_client_t` (as obs_bus
does with `g.host` and `g.producer`/`g.consumer`).

## Producing an arena payload

```c
/* 1. Allocate a lease on the host arena, owned by this producer's slot. */
uint32_t lease;
if (bus_arena_alloc(&host->arena, producer->reply.handle_id, len, &lease) != BUS_ARENA_OK)
    /* ERR_CAP (at the per-client live-lease cap), ERR_NOSPACE, ERR_NOLEASE:
       transient backpressure — a consumer release frees room. Retry or drop. */;

/* 2. Fill the span. The write happens OUTSIDE any lock; you own the span until
      you send the frame. */
uint8_t *p;
bus_arena_fill_ptr(&host->arena, lease, &p);
memcpy(p, my_bytes, len);

/* 3. Read the generation, then emit the reference frame. No bytes cross the ring. */
bus_arena_ref_t ref;
bus_arena_ref(&host->arena, lease, &ref);
bus_client_publish_arena(producer, KIND, lease, ref.generation, len);
/*   or, correlated:  build the frame with BUS_F_REQUEST|BUS_F_ARENA yourself —
     see route_arena_request. A request/reply arena API can be added when a
     consumer needs it. */
```

**Once you send the frame, the lease is the host's.** Do not touch it again. The
host takes ownership by publishing it during routing. If you allocate a lease and
then decide *not* to send it (an error path), call `bus_arena_cancel` to release
your producer reference.

`len` is bounded by the arena, not by `inline_budget`. The lease table already
refuses an over-large allocation, so there is no second size check in
`bus_client_publish_arena` (it only rejects a zero length).

## Consuming an arena payload

`bus_client_poll` returns an arena frame with `ev.payload == NULL`. It does not
auto-resolve arena bytes (only the co-located host arena can). Resolve it
yourself, then release:

```c
bus_event_t ev;
if (bus_client_poll(consumer, &ev) == BUS_CLIENT_OK && (ev.frame.hdr_flags & BUS_F_ARENA)) {
    const uint8_t *p;
    if (bus_arena_read_ptr(&host->arena, (uint32_t)ev.frame.payload_ref,
                           ev.frame.generation, consumer->reply.handle_id, &p) == BUS_ARENA_OK) {
        /* Read up to ev.frame.payload_len bytes from p, in place. The host has
           already validated payload_len <= the span, so the read is in bounds. */
        handle(p, ev.frame.payload_len);
        bus_arena_release(&host->arena, (uint32_t)ev.frame.payload_ref,
                          ev.frame.generation, consumer->reply.handle_id);
    }
}
```

`read_ptr` is gated on the generation (ERR_STALE if the lease was reused) and on
this slot actually holding a reference (ERR_NOTHOLDER). **Release exactly once**
after you are done reading; the span stays valid until you do. The lease is
reclaimed when the last consumer releases (or when a consumer is reaped).

## What the host does with it (routing)

You do not call this (it is what happens when you `pump`) but it is worth
knowing:

- **Notification:** published to a snapshot of the kind's observers (refcount =
  that set). Delivered by reference under BLOCK/SHED like an inline fan-out. A
  shed observer's reference is released so the lease still drains. No observers ⇒
  reclaimed immediately.
- **Request:** published to the kind's server and delivered point-to-point; the
  correlation is registered. No server ⇒ the lease is reclaimed and the requester
  gets `capability_absent`.
- **Reply:** only the kind's server may answer; published to the original
  requester and the correlation retired on delivery. A forged/unmatched reply is
  reclaimed and dropped.
- **Cancel:** carries no meaningful payload; an arena cancel's lease is reclaimed
  and the frame dropped.

Every arena frame is validated against the authoritative lease (generation match,
`payload_len ≤ span`) before routing; a mismatch is dropped-with-count.

## Threading

The host's lease table is guarded by an in-process mutex, so you may `alloc` /
`fill` from a producer thread while the pump thread routes and a consumer thread
reads, exactly obs_bus's shape (`consumer_main` runs pump + drain; emit runs on
any thread). The lock covers only the table bookkeeping; your `memcpy` into the
span and your read out of it happen outside it, kept safe by the refcount. The
ThreadSanitizer lane `scripts/run-bus-arena-tsan.sh` exercises this.

## Capture / replay

Arena events are captured like any other: the tap materializes the payload bytes
into the record (D10), so a captured arena event replays byte-exact from the
record and never needs its long-gone lease. You get this for free, the capture
tap and the audit replay path already handle it.

## Quick reference

| Call | Who | When |
| --- | --- | --- |
| `bus_arena_alloc` | producer (co-located) | reserve a span |
| `bus_arena_fill_ptr` | producer | get the writable pointer, fill it |
| `bus_arena_ref` | producer | read the generation for the frame |
| `bus_client_publish_arena` | producer | emit the reference frame (notification) |
| `bus_arena_cancel` | producer | abandon an allocated-but-unsent lease |
| `bus_arena_read_ptr` | consumer (co-located) | resolve the span to read in place |
| `bus_arena_release` | consumer | release after reading (exactly once) |

All of these are in [`bus_arena.h`](../../src/core/event_bus/include/aimee/core/event_bus/bus_arena.h)
and [`bus_client.h`](../../src/core/event_bus/include/aimee/core/event_bus/bus_client.h); see
[`test_bus_route.c`](../../src/tests/test_bus_route.c) and
[`test_bus_client.c`](../../src/tests/test_bus_client.c) for worked examples.
