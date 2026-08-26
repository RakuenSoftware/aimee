# Event bus v0: rendered decisions (D1–D10)

- **State:** DECIDED. 2026-07-23, revision 7, after six rounds of roundtable review.
- **Companion:** [`EVENT_BUS_FEATURE_TREE.md`](EVENT_BUS_FEATURE_TREE.md). Scope, the twelve slices,
  the dependency graph, and **acceptance ids 1–6**, each of which names the decisions it enforces.
  This document holds only the decisions; the tree references them by id and is reviewed alongside it.
- **Implements:** [`event-bus-wire-spec.md`](../proposals/done/event-bus-wire-spec.md) v0 DRAFT,
  under [`core-substrate-and-source-module-boundaries.md`](../proposals/done/core-substrate-and-source-module-boundaries.md)
  invariants 12, 13, 15, 17, 18, 19.

Every decision below is rendered, not deferred. Where a decision changes the wire spec, it is marked
and gated in [D1](#d1--segment-access-multiple-memfds-not-one-named-segment).

---

## D1: Segment access: multiple `memfd`s, not one named segment

**Decision: the bus is realized as several fd-backed regions. A client receives fds only for what it
may map.**

Two alternatives were considered and rejected:

- `shm_open` with a filesystem path. Rejected: the segment has a name, so invariant 17's "an
  unadmitted process cannot map the segment" rests entirely on permission bits and is one
  misconfiguration away from failing.
- One anonymous `memfd_create` segment, fd passed after admission. Rejected on review: it makes
  invariant 17 structural (nothing to open), but an *admitted* client holding the whole-segment fd can
  map and read every other client's rings and the whole queue directory. Invariant 18's "cannot
  enumerate" would be an API-level courtesy presented as a memory-level guarantee.

| Region | Backing | Client mapping | Contents |
|---|---|---|---|
| Control | one `memfd` | `PROT_READ` | magic, `spec_version`, `layout_version`, flags, `slot_size`, `inline_budget`, `arena_size`, `host_epoch`, `host_heartbeat` |
| Queue pair | one `memfd` **per admitted slot** | `PROT_READ\|PROT_WRITE`, own slot only | that client's inbound and outbound rings, its credit counters, its `client_heartbeat` |
| Arena | one `memfd` | `PROT_READ\|PROT_WRITE` | shared payload arena addressed by `(offset, len)` |

The **queue directory is not shared at all**. It becomes ordinary host-private memory. A client
learns its own slot geometry from the attach reply and has no fd, no mapping, and no address through
which to observe that another slot exists. Enumeration is structurally impossible rather than merely
unauthorized; cross-client ring access is an MMU fault rather than a policy violation.

Transport: a `SOCK_SEQPACKET` attach socket. **Each attach carries three fds, control, arena, and
that client's queue pair, in a single `sendmsg`**, passed by `SCM_RIGHTS` only after `bus_admit_fn`
returns success. The host's own fd table grows by one per admitted slot plus the two shared regions,
so `N+2` describes the host's fd count, not the size of any message. Both costs are trivial.
`memfd_create` is Linux-only; accepted for v0, since both bus-hosting services are containerized
Linux. A `shm_open` portability fallback is a later slice, not a v0 requirement, and reintroduces the
named-segment weakness, so it must carry its own threat-model note when written.

### Spec amendment and its gate

D1 changes the wire spec's **Segment layout** from one shared segment to several fd-backed regions and
moves the queue directory out of shared memory. That document is owned by `module-runtime`, so the
change is **requested, not made unilaterally**.

The change is argued to be *more* faithful to the spec's intent: the spec's Security section already
requires that "a client cannot read another client's traffic, matching observer routing at the memory
level, not only the API level", and a single shared segment cannot deliver that; the spec also states
that exact offsets are frozen by the conformance vectors rather than by prose, and the **frame
encoding (which is what the vectors cover) is unchanged by D1.**

**Gate.** Slices 1 and 2 are layout-independent and may proceed now: slice 1 produces frame vectors
only (see [D4](#d4--slot-size-and-inline-budget-control-region-parameters-not-wire-fields)) and slice
2 produces a ring primitive that is agnostic to which fd backs it. **Slices 3 and later do not begin
until `module-runtime` accepts or declines the D1 amendment.**

If `module-runtime` declines, this is **not** a one-slice fallback. Reverting to a single segment
invalidates the enforced half of [D2](#d2--threat-model)'s threat model, re-opens cross-client ring
access and slot enumeration, and re-states slice 3, slice 5, and acceptance id 3. That path therefore
requires **re-running the roundtable on D1 and D2** and re-issuing this document. It may not be taken
by silently widening D2. Revision 2 of the tree understated this; it is corrected here.

The gate is mechanically observable, not a matter of prose: `scripts/check_bus_d1_gate.sh` runs at
slice-3 PR open and passes only when the `module-runtime` decision record names the D1 amendment as
accepted. Until that record exists, slice 3 cannot open.

---

## D2: Threat model

**Decision: state the limits of v0 isolation explicitly rather than implying uniform enforcement.**

D1 closes cross-client ring access and slot enumeration. It does not close the arena: zero-copy
fan-out requires every admitted client to map the arena read-write, so arena lease bounds are a
**cooperative contract among admitted clients, not an enforced one**. An admitted client that ignores
the client library can read or corrupt any arena byte. This is consistent with the wire spec, which
already places "a hostile native client in the trusted tier" outside this boundary's threat model.

| Class | Property | How it holds |
|---|---|---|
| **Enforced** (fd / MMU) | An unadmitted process can map nothing | it holds no fd |
| **Enforced** | An admitted client cannot map or address another client's rings | it holds no fd for them |
| **Enforced** | An admitted client cannot enumerate other slots | the queue directory is host-private |
| **Enforced** | The control region is read-only to clients | `PROT_READ` mapping |
| **Cooperative** | Arena access stays within active leases | client-library contract, validated by conformance, not by hardware |
| **Out of scope for v0** | A malicious admitted *native* client | untrusted code does not run as a native admitted client |

Untrusted code runs sandboxed under `module-loader`, whose tier will need either per-lease arena
sub-regions or a copy-on-deliver mode. That is `module-loader`'s obligation; it is named here so it is
not forgotten.

Acceptance id 3 tests the two classes differently: enforced properties are tested as faults,
cooperative properties as contracts.

---

## D3: Arena leases: host-mediated, with generations and refcounts

**Decision: the spec's v0 baseline stands. The producer requests a lease, fills it, publishes the
ref; the consumer reads in place and releases; the host tracks the lease.** Payload bytes are never
copied through the host; only lease bookkeeping is host-mediated. Direct producer→consumer allocation
without host bookkeeping is **overruled** for v0: it has no ownership story that survives a client
dying mid-write.

Review found four lifetime gaps in earlier revisions. All four are closed here.

**Generations and refcounts.** Every lease carries a generation and a refcount. A consumer validates
the lease generation before reading; a stale generation is a typed error, never a read of whatever now
occupies those bytes. A region returns to the free pool only at refcount zero.

**The producer holds the first reference, and publishing transfers it.** The full lifecycle, so no
state is left implicit:

| Transition | Refcount effect |
|---|---|
| `bus_arena_alloc` succeeds | region created with refcount **1**, held by the allocating producer |
| `bus_publish` referencing it, routed to `k` authorized observers | **+k** consumer references, **−1** producer reference: ownership transfers to the consumers |
| a consumer releases | **−1** |
| `bus_arena_release` on an allocated-but-unpublished lease | **−1** (the producer's): the ordinary cancellation and error path |
| producer reap | drops any producer reference it still holds |
| consumer reap | drops every reference attributed to that consumer |

Publishing to zero authorized observers takes the refcount straight to zero and reclaims the region
immediately, which is correct. Nobody can read it. Because allocation always takes a reference and
every path that ends a producer's interest drops exactly one, **an allocated-but-unpublished lease
cannot leak without the producer either cancelling or dying**; it is never permanently live by
omission. *(Added in revision 6; earlier revisions defined consumer references only and left the
producer's own reference undefined between `bus_arena_alloc` and `bus_publish`.)*

**Producer reap does not reclaim live regions.** Reaping a producer drops its own reference but leaves
consumer references intact. Its leases become orphaned-but-live, drain as consumers release, and are
reclaimed at zero.

**Consumer reap releases that consumer's references.** Each reference is attributed to the consumer
slot that holds it. Reaping a consumer drops every reference attributed to it, which may take a region
to zero and reclaim it. Without this rule a dead consumer's references would be permanently
unreleasable and repeated consumer deaths would exhaust the arena. *(Added in revision 3; earlier
revisions defined producer reap only.)*

**The per-client live-lease cap is enforced synchronously at allocation, not at reap.** The call
surface is `bus_arena_alloc`, allocation is split from publish, so a producer requests a region,
fills it, and only then publishes a frame referencing it. A client at its cap is refused by
`bus_arena_alloc` immediately, by the host, before any bytes are written. Reap is
heartbeat-driven and therefore lagging; enforcing the cap only at reap would leave a heartbeat-long
window in which a misbehaving client keeps allocating. The cap is the synchronous gate; reap is the
eventual cleanup. *(Added in revision 3.)*

**The lease table is thread-safe (revision 5).** D3 originally read as host-private-and-single-threaded,
which held while the only table writers were the pump thread's publish/release/reap. Arena payload
routing makes the model concrete: a co-located producer (D7) allocates and fills a lease from its OWN
thread while the pump thread publishes and reaps and a consumer thread reads and releases. An
in-process mutex now guards every table transition (`bus_arena.c`). It covers only the bookkeeping.
The producer's fill and the consumer's read of the payload span happen outside the lock, kept safe by
the refcount (a live reference cannot be reclaimed mid-read). A ThreadSanitizer lane
(`scripts/run-bus-arena-tsan.sh`) runs producer/pump/consumer concurrently and is clean with the lock,
a verified race without it. *(The arena is only writable by co-located, trusted producers in the same
process as the host. A thin cross-process client has no access to the host-private lease table, so
arena production is a co-located side channel, consistent with D7.)*

**Routing an arena payload (revision 5).** The host forwards a lease by reference, never copying bytes.
On an arena notification it snapshots the kind's observers and publishes the lease to exactly them
(refcount = the set that will read); a shed observer's reference is released so the lease still drains;
zero observers reclaims immediately. Requests/replies route point-to-point (server / requester) with
the same publish-once, refcount-correct discipline. The host validates every arena frame against the
authoritative lease (generation match, `payload_len ≤ span`) before routing, which is what lets a
consumer trust `payload_len` as an in-bounds read length. The host never dereferences an arena offset
to route (only the lease table's id/generation bookkeeping) with one exception, the capture tap (D10).

---

## D4: Slot size and inline budget: control-region parameters, not wire fields

**Decision: `slot_size`, `inline_budget`, and `arena_size` are control-region parameters read by a
client at attach. They are not wire-frame fields and they do not participate in `layout_version`.**

Provisional values set in slice 3, **256-byte slots, 192-byte inline budget, 1024-slot default ring
capacity**, re-set from the slice-12 measurement before the tree closes.

The frame carries `payload_len`, a `payload_ref` (in-slot offset, or arena `(offset,len)`), and an
inline-vs-arena flag. It does **not** carry the threshold at which a producer chooses between them.
A frame produced under a 192-byte budget and one produced under a 1024-byte budget are byte-identical
for the same payload placement, so slice 1's golden vectors are stable under any later re-tuning.
Slice 1's vector set must include an inline-flagged and an arena-flagged frame at the budget boundary,
so this independence is asserted rather than assumed.

**`layout_version` rule (normative).** `layout_version` changes only when the *structure* of the
regions changes, fields added, moved, or removed from the control region, the queue-pair region, or
the ring header. It does **not** change when a control-region *parameter value* changes.

The wire-frame header is deliberately **not** in `layout_version`'s scope: it is versioned by
`wire_version`, which is per-event and negotiated at attach. A change to the frame header therefore
bumps `wire_version` and re-issues slice 1's vectors, while leaving `layout_version` alone unless the
region structure also moved. The two versions answer different questions, "can I map this?" and "can
I decode this?": and conflating them would force a re-attach for a decode-only change; Re-tuning
`slot_size` or `inline_budget` in slice 12 therefore does not bump `layout_version`, does not re-issue
slice 1's vectors, and does not invalidate the conformance suite. A client reads these values at
attach and adapts; it is never compiled against them. *(Stated normatively in revision 3; revision 2
left it as inference.)*

---

## D5: Backpressure: block by default, per-client, never silently

**Decision: block-with-bounded-wait is the default; shed-with-typed-`overflow` only where a kind's
descriptor declares it.** Shedding by default loses events silently, which invariant 13 forbids. The
spec leaves shed-vs-block per kind; the default must be the safe one.

### What `bus_publish` means

**`bus_publish` returning OK means the event was accepted into the calling client's outbound ring. It
never means delivered.** Producer-side credits are space in that one ring and nothing else. A bounded
wait blocks the calling client until credits free or the deadline expires; at expiry the call returns
`would_block`. It never blocks the host, never blocks another client, and never silently degrades into
a shed.

### Fan-out is not atomic, and that is the contract

*(This section answers the fan-out/credit-reservation gap found in revision 2, which left it
undefined.)*

An event is popped from the producer's outbound ring, `seq`-stamped, and tapped **once**. Delivery to
each of its `k` destinations is then **independent** and may complete at different times. **Partial
delivery is permitted and is exactly what the `overflow` events record.** There is no cross-destination
credit reservation and no atomic fan-out, providing one would require the host to hold events for
absent credit across all destinations, which is the unbounded host queue invariant 15 forbids.

For a destination whose inbound ring is full, the host applies that kind's declared policy **to that
destination only**:

- **shed-declared:** emit a typed `overflow` immediately and move on.
- **block-declared:** the event stays in that event's *residual destination set*. The host holds at
  most **one** in-flight event per producer, so the structure is bounded by slot count, not by
  traffic. The host stops popping that producer's outbound ring until the residual set drains, which
  propagates backpressure to that producer naturally. Its credits stop returning and its next
  `bus_publish` blocks or returns `would_block`.

**The host drain loop never blocks, and never busy-polls.** It services outbound rings round-robin
while there is work; when every ring is empty it waits on an `eventfd` that a publishing client signals,
with a bounded idle timeout so heartbeat reaping still runs on a quiet bus. "Never blocks" means it
never waits on a *destination*; waiting for work to exist is not the same thing. A stalled producer
stalls only itself. A slow consumer therefore stalls exactly those producers that publish block-declared kinds
to it, and no one else. That is head-of-line blocking scoped to the pair that caused it, and it is
accepted: the alternative is either an unbounded host queue or silent loss.

**"In-flight" means popped from the producer's outbound ring and not yet fully resolved across all of
its destinations.** The residual destination set is the per-producer, per-event tracker that holds
such an event, and there is exactly one tracker per slot, which is what bounds the structure by slot
count rather than by traffic.

### Reap under in-flight publishes

*(Added in revision 3; revision 2 left the fate of in-flight publishes on reap undefined.)*

- **Producer reaped:** its outbound ring and its one residual in-flight event are discarded, and a
  typed `producer_reaped` event is tapped naming the discarded `seq`. Nothing is lost silently.
- **Destination reaped:** its entries in any residual destination set are dropped and tapped as
  `overflow` with reason `destination_reaped`.
- A client blocked in `bus_publish` when it is reaped observes the reap through `host_epoch` /
  attach-state and must re-attach; its pending publish is not delivered and is accounted by the
  `producer_reaped` event.

---

## D6: The tap records the intended stream; sheds are events

**Decision: the tap is invoked once per event immediately after `seq` stamping and before any routing
decision, and every divergence between intended and delivered is itself an event in the stream.**

After routing there are `k` per-destination outcomes rather than one event, so pre-routing is the only
point at which a total, totally-ordered view exists.

**"Every event" means every event the host has `seq`-stamped.** A producer-side `would_block` is an
event the host never accepted; it is **not** a tap miss and acceptance id 4 must not be read as
requiring it. Callers that need visibility into their own rejected publishes read a per-client
producer-reject counter in their queue-pair region. *(This scoping was ambiguous in revision 2 and is
made explicit here.)*

**Timing applies to published events; host-generated records join the same stream.** The pre-routing
rule above describes when a *published* event is stamped and tapped. `overflow` and `producer_reaped`
are generated by the host after a routing or reap decision; they are then stamped and tapped under the
same total-stream rule, so they are ordinary members of the ordered stream and not a side channel.

**Sheds are recorded, delivered, and actionable.** Every shed emits its own typed `overflow` event,
`seq`-stamped and tapped, carrying **the shed event's `seq`, its kind, and the destination slot id**.

Tapping alone would make a shed visible to governance but not to the consumer that lost it, so the
delivery path is decided rather than left open: **`overflow` is a control-class event, delivered into
the losing consumer's own inbound ring.**

**`producer_reaped` is tap-only.** It describes a client that no longer exists and whose queue pair
has already been discarded, so there is no inbound ring to deliver it to and no other client with a
stake in it. Naming a substitute recipient would invent a subscriber the bus does not have. It is
recorded for governance and forensics and delivered to no one. *(Revision 5 wrongly grouped it with
`overflow` as deliverable, which contradicted D5's own reap semantics.)*

Control-class events differ from ordinary traffic in exactly two ways, both of which exist to stop
the pathological recursion of an overflow notice itself overflowing:

- They draw on a **reserved credit pool** in each queue-pair region that ordinary traffic cannot
  consume, so a client saturated with data events still has room to be told it lost some. Provisional
  size: **4 control-class events per client**, re-tuned by slice 12 alongside D4's parameters.
- They are **never shed**. The per-destination shed/block policy does not apply to them, so there is
  no overflow-of-overflow to recurse into.

If the reserved pool is nonetheless exhausted, the host sets a sticky `control_lost` flag and counter
in that client's queue-pair region. **That flag is observable only by a live client**: a reaped
client's queue pair is gone, so the flag does not survive it. This is deliberate. The flag exists so
a running client can discover it missed a notice, not as a durable record. The durable record of every
shed is the tap stream, which outlives any client.

**An `overflow` whose destination is reaped between the routing decision and delivery becomes
tap-only**, for the same reason `producer_reaped` is: there is no ring left to deliver into. The shed
remains recorded; only the notification is dropped.

**A consumer learns of loss from `overflow` events, never by inspecting `seq`.** A consumer receives
only the kinds it is an authorized observer of, so the `seq` values in its inbound ring are inherently
sparse, gaps there are the normal case and carry no information. Contiguity is a property of the tap
stream alone. This is why `overflow` must be delivered rather than merely tapped, and it is also why
`producer_reaped` being tap-only costs a consumer nothing: it could not have inferred the reap from
`seq` either way. Governance derives both facts from the tap; neither path depends on the other.

**The delivered stream is derivable:** delivered = intended, minus the sheds the stream itself records,
minus the `producer_reaped` discards it records. A ledger built on this answers both "what was
published" and "what arrived" from one capture point.

---

## D7: The bus does not link into any shipping binary in this tree

**Decision: build as a static library with its own test-harness binaries.** Linking an unproven IPC
substrate into a shipping binary before slices 10 and 12 are green buys nothing and risks a live
process.

Enforced by `scripts/check_bus_blast_radius.sh`, which runs **as a required check on every slice PR**,
not only at tree level. A tree-level-only gate would let a regression between slices 10 and 12 link the
bus into a shipping binary and still pass every per-slice gate. *(Per-PR enforcement added in revision
3.)*

That pre-shipping rule is retained here as history. The current shipping boundary is below.

### D7: revision 5: one shared local-bus library for both daemons

The rule above ("the bus links into **no** shipping binary") held for the whole twelve-slice feature
tree. **Delivery step 3 (the first real module migration onto the bus) deliberately ends it.** The
per-action governed-action audit row (`modules/guardrails/guardrails_action_audit.c`) no longer calls
the file writer directly; it publishes the row over the bus via `modules/audit/obs_bus.c`, whose
consumer thread drains it to the ledger. This is an all-or-nothing migration. There is no flagged
parallel direct path, so the bus becomes load-bearing for one real, off-critical-path operation.

The blast-radius invariant is now the product link graph:

- **Source.** Only `src/core/event_bus/*` and `src/modules/audit/obs_bus.c` may include a bus header.
- **Library.** All bus implementation objects enter `libaimee-core-event-bus.a`; no shipping target names
  individual bus objects.
- **Consumers.** `aimee-server` and `aimee-kb` both link that same archive and host independent local
  buses for modules in their own containers.
- **Non-consumers.** The thin client does not link the bus. The bus never carries traffic between
  machines; the shared connection library owns that traffic.

The daemon-owned observability runtime and storage sinks remain outside the archive. In particular,
the server installs its DB1 guardrail sink through `server/obs_bus_adapter.c`; the KB needs no fake
DB1 stub. `scripts/check_bus_blast_radius.sh` enforces the library membership and consumer graph.

**Durability note.** The old direct write `fflush`'d each row, so it was durable the instant it
returned. The bus path is asynchronous: the producer publishes and returns, and the consumer writes
sub-millisecond later. A graceful shutdown drains losslessly (`obs_bus_stop`, also via `atexit`),
but a hard kill (`SIGKILL`/crash) can lose the few rows still in the ring. This is consistent with the
audit row's pre-existing best-effort contract (`audit_action_log` already dropped silently when the
log was not open) and is the accepted trade for moving the write off the caller's thread.

---

## D8: The Go reference client is pure Go

**Decision: a new top-level `server-go/bus` package; `golang.org/x/sys/unix` for `mmap`, `recvmsg`,
and `SCM_RIGHTS`; no cgo.**

Suite invariant 12 excludes cgo ("no shared header, symbol, link edge, cgo boundary, or side channel"),
and a Go client that linked the C implementation would defeat the conformance suite's purpose. It
would test the C code twice and prove nothing about the spec.

`scripts/check_bus_single_host.sh` enforces the corollary that there is exactly one host: `bus_host_create`
is defined in exactly one translation unit; `memfd_create` is called from no file outside
`src/core/event_bus/bus_region_host.c`; no Go file in `server-go/bus` creates regions or accepts attaches; and
**no test in `server-go/bus` regenerates or shadows the slice-1 vectors**. The last clause matters
because the first three catch only a new violating *file*. They would not catch a future drift in the
C host's frame bytes being papered over by a Go-local vector override. Slice 1's vectors are the single
conformance authority.

---

## D9: The tap in this tree is a callback plus a recording test double

**Decision: the host invokes a registered `bus_tap_fn` per D6.**

"The tap contract" splits across two trees, and the line is drawn here so it is traceable: **this
tree decides the callback's invocation contract**, when it fires (D6), what it receives, and the
proof that it is total (**and the later tree decides what is done with the stream**) durable
chaining, `policy_rev`, signing, and the attestation bundle. Nothing about the callback's timing or
totality is left to that tree, and nothing about ledger semantics is decided here.

This tree owes the seam and the proof that the seam is total, every `seq`-stamped event, exactly once, in `seq` order. Durable chaining,
`policy_rev`, and the attestation bundle belong to
[`event-bus-governance-and-capture.md`](../proposals/done/event-bus-governance-and-capture.md) and
are not built here. The recording test double is the conformance vehicle, not a shipped feature.

---

## D10: The v0 capture stream format

**Decision: the capture format is specified normatively here and *validated* by slice 11's tests. It
is the contract the later governance-and-capture tree must adopt.**

Earlier revisions said only that slice 11 would "freeze the format", which deferred the decision while
claiming to render it. The format is defined below.

### Framing

All integers little-endian. The stream is a fixed file header followed by records.

**File header (64 bytes):**

| Offset | Size | Field | Value / meaning |
|---|---|---|---|
| 0 | 8 | `magic` | ASCII `AIMEECAP` |
| 8 | 2 | `format_version` | `1` for v0 |
| 10 | 2 | `header_len` | `64`; a reader skips to `header_len` so a later version may extend the header |
| 12 | 4 | `flags` | reserved, must be `0` |
| 16 | 4 | `spec_version` | the bus `spec_version` this stream was captured under |
| 20 | 4 | `layout_version` | the region `layout_version` at capture time |
| 24 | 8 | `host_epoch` | the epoch this stream belongs to |
| 32 | 8 | `first_seq` | `seq` of the first record; a stream may start mid-run |
| 40 | 8 | `created_unix_nanos` | wall clock at stream open, informational only: ordering comes from `seq` |
| 48 | 16 | `reserved` | must be zero on write; a reader **must ignore** nonzero reserved bytes and must not let them influence parsing |

**Record:**

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `record_len`: total record bytes including this field and the trailing CRC |
| 4 | 2 | `record_type` |
| 6 | 2 | `record_flags`: bit 0 `payload_materialized`; all other bits reserved, zero |
| 8 | `BUS_WIRE_HDR_LEN` | the bus wire frame header, **verbatim** |
| 8 + `BUS_WIRE_HDR_LEN` | `payload_len` | materialized payload bytes |
| `record_len` − 4 | 4 | `crc32c` (Castagnoli, u32 LE) over every preceding byte of the record |

`record_type`: `0` event, `1` overflow, `2` producer_reaped, `3` epoch_change.

**Sizes are not free variables.** `BUS_WIRE_HDR_LEN` is the fixed frame-header size defined by slice 1
and frozen by its vectors; the capture format does not re-declare it. The materialized payload length
is **exactly the embedded frame's own `payload_len` field**. There is no second length in the record.
A reader therefore validates

```
record_len == 8 + BUS_WIRE_HDR_LEN + payload_len + 4
```

and treats any mismatch as **corrupt**. Slice 11's vectors must exercise this boundary, including a
record whose `record_len` disagrees with its `payload_len` and a record whose CRC covers the wrong
extent.

### Rules

- **Payloads are materialized.** An arena region does not outlive the host, so a capture that stored
  only a `payload_ref` would be unreadable the moment the run ended. The frame header is preserved
  **byte-identical**, including its original `payload_ref`, and the *resolved* payload bytes follow
  inline with `payload_materialized` set. A reader **must** use the materialized bytes and **must
  never** dereference `payload_ref` at replay time. That offset names an arena that no longer
  exists. The preserved ref is forensic detail only. *(Realized for arena in revision 5: the pump
  resolves a producer-held lease once, pre-routing, via `bus_arena_producer_bytes` and hands the span
  to the tap, which materializes it exactly like an inline payload. This is the single exception to
  "the host never dereferences an arena offset", bounded by the span, for the capture tap only, before
  the lease is published.)*
- **`seq` is contiguous within a stream, and that is provable, not aspirational.** A `seq` is assigned
  only at stamping, and D6 requires every stamped event to be tapped, so the tapped set *is* the
  complete set of assigned `seq` values. Within one stream, `seq` therefore runs contiguously upward
  from `first_seq`, and a gap is a defect the reader must report rather than interpolate. Two things
  that might look like gaps are not: a producer-side `would_block` never received a `seq` at all (D6),
  so it cannot create one; and records before `first_seq` belong to an earlier stream, not to a hole
  in this one. This contiguity is what makes the delivered stream derivable per D6.
- **`overflow` and `producer_reaped` records are ordinary `seq`-stamped records**, so the derivation
  in D6 is a single pass over one ordered stream.
- **`epoch_change` terminates the stream.** Its body is 16 bytes, `old_epoch` (u64 LE) then
  `new_epoch` (u64 LE), carried in the ordinary payload position with the embedded frame header's
  `payload_len = 16` and `payload_materialized` **set**, so the `record_len` identity above holds
  with no special case. *(Revision 4 initially described a zero-payload header, which contradicted
  the identity; there is deliberately no carve-out.)* It carries the `seq` immediately after the last
  stamped event, so contiguity holds through it. It **must be the final record**; any record after it
  is corrupt. Handles and mappings do not survive a host restart, so a capture does not either. The
  next epoch is a new stream with a new `first_seq`.
- **`seq` assignment is serialized, which is what the contiguity claim rests on.** `seq` is assigned
  in the host's single tap thread, and the sequence {assign `seq` N, invoke the tap, make the record
  durable} completes before `seq` N+1 is assigned. Contiguity is therefore a consequence of that
  serialization, not an assumption layered on top of it.
- **The recorder must not be the source of a gap.** The contiguity rule above is a property of the
  bus; a recorder sitting between the tap callback and the file could still lose an event and
  manufacture a gap that looks like corruption. The recorder's obligation is therefore explicit: an
  event's record must be durable before the host stamps the next `seq`. A reader that finds a gap
  anywhere but at the end reports **corrupt** and refuses the stream. It does not attempt to
  distinguish a bus defect from a recorder defect, because from the reader's position they are the
  same fact. Only an incomplete *final* record is truncation.
### Terminal states, decided from the bytes alone

"Truncated" and "corrupt" must be separable by a reader with nothing but the file, so the rule is an
algorithm rather than a description. `REC_MIN` is `8 + BUS_WIRE_HDR_LEN + 4` and `REC_MAX` is
`REC_MIN + max_payload`; both are constants of the format.

**These are byte-observable classifications, not claims about what happened.** A reader cannot know
whether a given file was interrupted or damaged; it can only classify the bytes in front of it. The
rules below are chosen so that the common interrupted-write case lands on `truncated` and structural
impossibilities land on `corrupt`, but neither label asserts a cause. A torn final write whose CRC-32C
coincidentally validates (~2⁻³²) and whose contents are structurally inconsistent will be reported
`corrupt`; that is the correct and safe outcome, and it is not a contradiction in the rule.

0. Validate the file header: `magic`, and `format_version == 1`. An unknown `format_version` is
   refused outright. The reader must not scan records under framing it does not know. Skip to
   `header_len`.

Then, at each record position `p`, with `remaining = filesize − p`:

1. `remaining == 0` → the stream ends cleanly. **complete** if the previous record was
   `epoch_change`, otherwise **open**, a valid capture of a still-running host, not an error.
2. `remaining < 8` → **truncated** at `p`. There is not even a length prefix.
3. Read `record_len`.
   - `record_len < REC_MIN` → **corrupt**, regardless of `remaining`. A short-but-present length
     prefix is a malformed header, not a partial write: no prefix of a valid record can name a length
     below the minimum.
   - `record_len > REC_MAX` → **truncated** when `remaining ≤ REC_MAX`, **corrupt** otherwise.
4. `remaining < record_len` → **truncated** at `p`.
5. `record_len` bytes are present but the CRC fails → **truncated** if `remaining == record_len`
   (this record ends exactly at EOF); **corrupt** otherwise, since bytes follow it.
6. CRC passes, but any of: `record_len ≠ 8 + BUS_WIRE_HDR_LEN + payload_len + 4`; a reserved bit set
   in `record_flags`; `seq` not exactly one greater than the previous record's; the previous record
   was `epoch_change` → **corrupt**.

The procedure is total: every byte sequence lands in exactly one of complete, open, truncated, or
corrupt, and only the final record can ever be classified truncated.

**A corrupt or truncated result is reported, never returned as data.** The report carries the last
good `seq`, the byte offset of the offending record, and the rule number above that fired, so slice
11 has a normative error shape to implement rather than inventing one.
- **`format_version` is independent** of `wire_version` and `layout_version`. A reader refuses an
  unknown `format_version` outright rather than guessing at the framing.

### What slice 11 owes

Slice 11 *validates* this contract rather than defining it: round-trip of every record type,
contiguous-`seq` enforcement, materialized-payload correctness for both inline and arena frames,
`record_len`/`payload_len`/CRC boundary vectors, open-vs-truncated-vs-corrupt discrimination,
`epoch_change` termination including rejection of a record after it, and rejection of an unknown
`format_version`. Acceptance id 4 links this to the governance tree, which must adopt the format
rather than redefine it.

Module replay (re-driving a module against recorded inbound events) is **not** delivered by this tree.
Slice 11 delivers observational replay only, which is exact by construction because nothing is
re-executed.

---

## Revision history

- **r1** (2026-07-23), seven decisions presented as recommendations for the roundtable to converge.
  Panel returned `drifted`: a decided plan was asked for, not an agenda. Four blocking technical
  findings: intra-segment isolation overclaimed, arena reclaim raced in-flight readers, the tap/shed
  contract silently pre-decided, inline-budget re-tune appeared to threaten the frozen vectors.
- **r2** (2026-07-23), nine decisions rendered; D1 multi-region layout and D2 threat model adopted;
  lease generations and refcounts added; D6 added; D4 clarified. Panel found six further blocking
  gaps: fan-out credit contract undefined, consumer reap did not release references, the spec-amendment
  fallback understated its blast radius, the blast-radius check was tree-level only, in-flight
  publishes on reap were undefined, and `layout_version` under re-tune was inferred rather than stated.
- **r3** (2026-07-23), all six closed: D5 gained the fan-out and reap contracts, D3 consumer-reap
  release and synchronous cap enforcement, D1 a hard gate on the spec amendment, D7 per-PR
  enforcement, D4 the normative `layout_version` rule, D6 the seq-stamped scoping and
  actionable-overflow rule. D10 was added but **deferred the capture format to slice-11 tests**,
which the panel correctly called out as claiming to render a decision while still deferring it.
  Split from the feature tree into this document so neither exceeds the review payload limit.
- **r4** (2026-07-23), this revision. D10 now renders the capture stream format normatively: file
  header and record framing with exact offsets, `BUS_WIRE_HDR_LEN` and the single-`payload_len` rule
  with the `record_len` identity a reader validates, CRC-32C extent, the proof that `seq` is
  contiguous within a stream (and why `would_block` and `first_seq` do not break it), the
  `epoch_change` body and termination state machine, and the three terminal states
  open / truncated / corrupt. Slice 11 now validates the contract instead of defining it. Also:
  reserved bytes given a reader rule, `payload_ref` explicitly non-dereferenceable at replay, and the
  acceptance-id surface pointed at the companion tree.
- **r5** (2026-07-23), closes the r4 findings. `epoch_change` now carries `payload_len = 16` with
  `payload_materialized` set, so the `record_len` identity holds with no carve-out. D6 decides the
  delivery path for sheds: `overflow` and `producer_reaped` are control-class events delivered into
  the losing client's inbound ring from a reserved credit pool, never shed, with a sticky
  `control_lost` flag as the degenerate-case backstop, which also closes the overflow-of-overflow
  recursion. D6 clarifies that pre-routing timing describes published events while host-generated
  records join the same ordered stream. D10 states the recorder's durability obligation so the
  contiguity claim does not rest on unstated recorder correctness. D1's fd accounting is reworded to
  separate the host's fd table from the per-attach message, and D3 names `bus_arena_alloc` as the
  cap's enforcement surface.
- **r6** (2026-07-23), this revision, closing the r5 findings. D10's terminal states become a
  six-rule byte-decidable algorithm over `remaining` and `record_len`, total over every input, with a
  normative error report shape (last good `seq`, offending offset, rule number). D3 renders the full
  refcount lifecycle as a table: `bus_arena_alloc` takes the producer's reference, `bus_publish`
  transfers it to the `k` consumers, `bus_arena_release` is the cancellation path, so an
  allocated-but-unpublished lease cannot leak by omission. D6 corrects `producer_reaped` to tap-only,
  since r5 required delivering it into a queue pair the reap had already discarded. Plus: D1 names
  `check_bus_d1_gate.sh` as the mechanical gate, D9 draws the tap-contract boundary explicitly, and
  D5 pins what "in-flight" means.
- **r7** (2026-07-23), closes the r6 findings. D10's algorithm gains step 0 (`format_version`
  validated before any record is scanned), splits step 3 so `record_len < REC_MIN` is corrupt
  regardless of `remaining`, validates reserved `record_flags` bits in step 6, and drops the
  overreaching claim that interrupted writes cannot produce rule-6 conditions. The states are now
  presented as byte-observable classifications, which is all a reader can determine. D6 decides the
  post-routing reap race (`overflow` to a dead destination is tap-only), scopes `control_lost` to a
  live client, sizes the reserved control pool provisionally at 4, and explains why a consumer never
  infers loss from `seq`. Its inbound `seq` values are inherently sparse, which is exactly why
  `overflow` must be delivered and why `producer_reaped` being tap-only costs a consumer nothing.
  D5 gives the drain loop an `eventfd` wait so "never blocks" does not mean "busy-polls". D4 places
  the wire-frame header under `wire_version` rather than `layout_version`. D10 states the `seq`
  serialization the contiguity claim rests on, and D7 describes what its check actually inspects.
