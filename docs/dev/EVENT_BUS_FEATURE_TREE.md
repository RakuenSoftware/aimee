# Feature tree: the shared-memory event bus (v0)

- **State:** DECIDED — 2026-07-23, revision 3, after two rounds of roundtable review.
- **Decisions:** [`EVENT_BUS_DECISIONS.md`](EVENT_BUS_DECISIONS.md) holds D1–D10 in full. This
  document holds scope, slices, dependencies, and acceptance, and references decisions by id.
- **Sequenced as:** step (2) of the 2026-07-23 amendment reconciliation in
  [`large-refactor-delivery-and-compatibility.md`](../proposals/pending/large-refactor-delivery-and-compatibility.md)
  — "the bus wire spec, the single in-source C bus host, the C and Go reference clients, and the
  cross-language conformance vectors — the foundation everything else attaches to".

## Scope

This tree builds the foundation only: a working, tested, benchmarked shared-memory event bus with one
C host, two reference clients, and a cross-language conformance suite, shipped as a static library
plus test harnesses. **No shipping binary links it in this tree** (D7, acceptance id 6).

That zero-integration boundary is deliberate. The bus is unproven until its conformance suite (slice
10) and its perf gate (slice 12) are green, and an unproven IPC substrate on a live server's hot path
is a self-inflicted outage. The cost of deferring integration is one later linking slice; the cost of
not deferring it is unbounded.

Explicitly **out of this tree**, each a later separately-reviewed tree:

| Out of scope | Owner / where it lands |
|---|---|
| Migrating any module onto the bus (`memory` first), gated by this tree's perf baseline | delivery step 3 |
| The descriptor graph, event-contract/kind schema, and admission *policy* | `module-runtime`; delivery step 2 |
| `module-loader`, OS-sandbox and WASM hosts, artifact verification, per-lease arena sub-regions for untrusted tiers (D2) | delivery step 4 |
| Chaining the tap into the WORM ledger | [`event-bus-governance-and-capture.md`](../proposals/pending/event-bus-governance-and-capture.md), delivery step 5; must adopt this tree's capture format (D10) |
| Cross-service (Runtime↔Control) paths over the network transport | delivery step 6 |
| Module replay (re-driving a module against recorded inbound events) | a later tree; slice 11 delivers observational replay only (D10) |

This tree defines the admission **seam** and calls an injected `bus_admit_fn`; it does not decide who
is admitted. It provides the tap **callback** and proves the callback is total; it chains nothing.

## Gate before slice 3

Slices 1 and 2 may begin now. **Slices 3 and later are blocked on `module-runtime` accepting or
declining the D1 spec amendment.** Slice 1 produces frame-only vectors and slice 2 produces a
layout-agnostic ring primitive, so neither depends on which layout wins. Declining the amendment is
not a one-slice fallback — see D1's gate section for what it re-opens and re-runs.

## Where the code lives

```
src/core/event_bus/          bus_wire.{c,h}    frame encode/decode
                          bus_ring.{c,h}    SPSC ring primitive
                          bus_region.{c,h}  region layout: control, queue-pair, arena
                          bus_arena.{c,h}   lease allocator
                          bus_host.{c,h}    admission, routing, tap, flow control
                          bus_client.{c,h}  C reference client
src/tests/test_bus_*.c    unit + integration tests
src/tests/fixtures/bus/   conformance vectors (shared with Go)
server-go/bus/            Go reference client (pure Go, no cgo)
scripts/test_bus_*.sh     integration + conformance harnesses
scripts/check_bus_*.sh    mechanical gates
bench/bus_baseline.json   committed perf baseline artifact
```

`src/core/event_bus/` follows the existing module convention: flat source list in `src/Makefile`,
mirrored in `CMakeLists.txt`, `-Icore/event_bus/include` on the include path, tests registered in
`src/tests/Rules.mk` and `src/tests/CMakeLists.txt`.

## Slices

Each slice is one PR into `feat/event-bus`, roundtable-reviewed before merge. Slice branches are
flat — `feat/event-bus-s1-wire-codec`, not `feat/event-bus/s1-...` — because git cannot hold a branch
and a directory of the same name.

A slice is not done until its tests pass under the normal build **and** the ASAN build; slices 2, 5,
6, 7 additionally require TSAN. `scripts/check_bus_blast_radius.sh` (D7) is a required check on
**every** slice PR, wired into `make lint`.

| # | Slice | Delivers | Proof obligations | Test command |
|---|---|---|---|---|
| 1 | **Wire codec + vectors** (D4) | `bus_wire`: fixed LE header, encode/decode, validation, version-negotiation and error frames. Golden vectors in `src/tests/fixtures/bus/`. | Round-trip for every message pattern. Rejection of truncated, mis-magic'd, over-length, and unknown-version frames. Vectors include inline- and arena-flagged frames at the budget boundary, asserting D4's independence from `slot_size`/`inline_budget`. | `make unit-test-bus-wire` |
| 2 | **SPSC ring** | `bus_ring`: power-of-two slot array, producer/consumer indices on separate cache lines, release/acquire publication, full/empty by index pair. Agnostic to which fd backs it. | SPSC correctness under threaded stress; no ABA on wrap; TSAN-clean. | `make unit-test-bus-ring` |
| 3 | **Region layout** (D1, D4) | `bus_region`: control region (RO), per-slot queue-pair regions, arena region; create, map, validate. Host-private queue directory. | A mapped region validates its own layout. A stale `host_epoch` is detected by a client. A corrupt control region is refused, not trusted. D4's provisional values are read from the control region, never compiled in. | `make unit-test-bus-region` |
| 4 | **Arena leases** (D3, D2) | `bus_arena`: lease alloc with generation + consumer refcount, publish by `(offset,len)`, consumer release, orphaned-but-live drain, per-client live-lease cap enforced at allocation, fragmentation bound. | Allocation fails closed rather than overlapping. A stale generation is a typed error, not a torn read. **Fault injection (a):** reap a producer with a reader in flight; the reader completes on intact bytes. **Fault injection (b):** a consumer dies without releasing; its references are dropped on reap and arena capacity is recovered. **Fault injection (c):** a client at its live-lease cap is refused a new lease synchronously, inside the heartbeat window, before any reap occurs. | `make unit-test-bus-arena` |
| 5 | **Host: admission** (D1, D2) | `bus_host`: region creation, the `SOCK_SEQPACKET` attach channel, the injected `bus_admit_fn` seam, slot allocation, three-fd grant via `SCM_RIGHTS`, heartbeat + reaping, epoch bump. | *Enforced:* an unadmitted process receives no fds and can map nothing; an admitted client cannot map or address another slot's queue pair; an admitted client cannot enumerate other slots; the control region faults on write. A refused attach returns a typed reason. A stalled client is reaped, its slot reclaimed, and its lease references released (D3). | `scripts/test_bus_isolation.sh` |
| 6 | **Host: routing + tap** (D6) | Drain loop, `seq` stamping, per-kind observer registry, point-to-point request/reply by `correlation_id`, synthesized `capability_absent`, `cancel`, `bus_tap_fn` invoked pre-routing. | A client never receives a kind it is not an authorized observer of. A reply reaches only its requester. The tap sees every `seq`-stamped event exactly once, in `seq` order, before routing. A producer-side `would_block` is not a tap miss and is counted in the client's producer-reject counter. | `scripts/test_bus_tap.sh` |
| 7 | **Flow control** (D5) | Credit-based backpressure, `would_block`, bounded per-producer residual destination set, declared overflow policy, typed `overflow` and `producer_reaped` events through the tap. | The host drain never stalls on a full destination; a wedged consumer affects only producers of block-declared kinds to it. A producer at zero credits blocks or returns `would_block`, never overwrites. Bounded-wait expiry returns `would_block`, never a silent shed. Every shed is a tapped `overflow` carrying the shed `seq`, kind, and destination slot id, and a consumer can enumerate its lost `seq` values from those alone. Reap-under-in-flight-publish behaves per D5. Re-exercises slice 5's slot-lifecycle cases by name. | `scripts/test_bus_flow.sh` |
| 8 | **C reference client** | `bus_client`: attach, publish, subscribe, request/reply, poll and bounded wait. | Drives the host through every pattern in slice 6 and every backpressure path in slice 7. | `make unit-test-bus-client` |
| 9 | **Go reference client** (D8) | `server-go/bus`: pure Go, mmap and `SCM_RIGHTS` via `x/sys/unix`, no cgo. | Produces and accepts the exact slice-1 vector bytes. Interoperates with the C host with no C linked into it — asserted by a build-constraint check, not by inspection. Does not regenerate or shadow the vectors. | `cd server-go && go test ./bus/...` |
| 10 | **Conformance suite** | `scripts/test_bus_conformance.sh`: the C host driven by a C client and a Go client on one bus, both directions, including `capability_absent`, cancel, credit exhaustion, and reaped-client recovery. Vectors consumed by both. | Two independent implementations agree byte-for-byte and behaviourally. `scripts/check_bus_single_host.sh` passes (D8). | `scripts/test_bus_conformance.sh` |
| 11 | **Capture + observational replay** (D6, D10) | `seq`-ordered capture stream format, frozen by this slice's tests; a replay tool that re-presents a captured window. | Capture is exactly the host's `seq` order and includes `overflow` and `producer_reaped` events, so the delivered stream is derivable per D6. Observational replay is exact by construction. Module replay is explicitly not delivered. | `scripts/test_bus_capture.sh` |
| 12 | **Perf baseline gate** | `bench/bus_baseline.json` with a named per-event dispatch-overhead ceiling (host enqueue → client dequeue, excluding module work), plus the gate script. Final D4 values set from this measurement. | "Within budget" names a real, checkable number. A red gate blocks merge. Re-tuning does not bump `layout_version` or re-issue vectors (D4). The `memory` round-trip p50/p99 rows are recorded as **pending** — they can only be measured against a pre-migration baseline in the later migration tree, and this tree must not fabricate them. | `scripts/check_bus_perf_gate.sh` |

### Dependency graph

```
1 ─┬─────────────> 8 ─┬─> 9 ─> 10
   │                  └─────────────> 12
2 ─┴─> 3 ─> 4 ─> 5 ─> 6 ─┬─> 7 ─────> 12
                          └─> 11
```

- **1, 2** are independent and may run in parallel; both are layout-independent and precede the D1 gate.
- **3** needs 2 (rings live in the queue-pair regions) and 1 (slot geometry must hold a frame).
- **4** needs 3 (the arena is a region).
- **5** needs 3 and 4 (reaping a client must release its lease references, D3).
- **6** needs 5 (routing needs admitted slots).
- **7** needs 6 (policy applies at the routing decision).
- **8** needs 6; its backpressure paths need 7.
- **9** needs **1** for the wire vectors and **8** for the observable host behaviour it must match.
- **10** needs 8 and 9.
- **11** needs 6 for the tap and 7 for `overflow` / `producer_reaped` events.
- **12** needs **6, 7, and 8** — the dispatch-overhead metric spans routing and flow control, not just
  the client.

### Decomposition note

Slices 5–7 all live in `bus_host.c` and land in sequence, so a defect in slice 5's slot lifecycle is
not fully exercised until slice 7's reap-under-load tests. This is accepted rather than restructured —
splitting the host across three files to make slices physically disjoint would be structure invented
for the review process, not for the code. The mitigation is mechanical, not prose:
`scripts/check_bus_slice7_covers_slice5.sh` asserts that slice 7's test executable contains slice 5's
slot-lifecycle cases by name, so a slice-7 author cannot silently trim them.

### Build configurations

```sh
# normal
make -C src unit-test-bus-<slice>

# ASAN — required for every slice
make -C src unit-tests OBJDIR=build/obj-asan \
     EXTRA_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
     EXTRA_L_FLAGS="-fsanitize=address,undefined"

# TSAN — required for slices 2, 5, 6, 7
make -C src unit-tests OBJDIR=build/obj-tsan TESTPREFIX=build/obj-tsan/tests \
     EXTRA_C_FLAGS="-fsanitize=thread -O1" EXTRA_L_FLAGS="-fsanitize=thread"

# On this host TSAN's own mapping guard trips on ASLR, so run the binary under:
setarch "$(uname -m)" -R build/obj-tsan/tests/unit-test-bus-ring
```

Two things implementation surfaced that are worth stating before the later slices repeat them:
`atomic_thread_fence` is invisible to TSAN, so any publication ordering must be carried by an atomic
field's release/acquire rather than a standalone fence if the pairing is to be verifiable; and a
header written by another process is a claim, so every geometry field must be checked against the
buffer actually mapped rather than trusted.

## Acceptance

Each check names the decisions and slices it enforces, so the decisions are mechanically verifiable
rather than merely asserted.

```yaml acceptance
- {id: 1, enforces: "D8, slices 1/9/10", tier: mechanical, check: "scripts/test_bus_conformance.sh --vectors --byte-exact-c-and-go && scripts/check_bus_single_host.sh --one-bus-host-create --memfd-only-in-bus-host --go-is-client-only --go-cannot-shadow-vectors"}
- {id: 2, enforces: "D5, slices 5/6/7/8/9/10", tier: integration, check: "scripts/test_bus_conformance.sh --interop c-host,c-client,go-client --both-directions --capability-absent --cancel --credit-exhaustion --reaped-client-recovery && scripts/check_bus_slice7_covers_slice5.sh"}
- {id: 3, enforces: "D1, D2, D3, slices 3/4/5", tier: integration, check: "scripts/test_bus_isolation.sh --enforced unadmitted-maps-nothing,no-cross-slot-mapping,no-slot-enumeration,control-region-read-only --cooperative arena-access-within-active-leases --lease-refs-survive-producer-reap --consumer-reap-releases-refs --lease-cap-enforced-at-allocation"}
- {id: 4, enforces: "D6, D9, D10, slices 6/7/11", tier: integration, check: "scripts/test_bus_tap.sh --tap-sees-every-seq-stamped-event-exactly-once --pre-routing --in-seq-order --producer-would-block-is-not-a-tap-miss --sheds-are-tapped-overflow-with-seq-and-destination --delivered-stream-derivable && scripts/test_bus_capture.sh --observational-replay-exact --capture-format-frozen-as-governance-contract"}
- {id: 5, enforces: "D4, slice 12", tier: mechanical, check: "scripts/check_bus_perf_gate.sh --baseline bench/bus_baseline.json --dispatch-overhead-ceiling-named --retune-does-not-bump-layout-version --memory-roundtrip-rows-marked-pending --gate-red-blocks-merge"}
- {id: 6, enforces: "D7, every slice PR", tier: mechanical, check: "scripts/check_bus_blast_radius.sh --no-shipping-binary-links-bus --required-on-every-slice-pr"}
```

## Progress

All twelve slices are merged into `feat/event-bus`. The D1 amendment was accepted (2026-07-24), which
unblocked slice 3 onward.

| Slice | State |
|---|---|
| 1 — wire codec + vectors | merged; normal + ASAN |
| 2 — SPSC ring | merged; normal + ASAN + TSAN |
| 3 — region layout | merged; normal + ASAN (fork+SIGSEGV read-only proof) |
| 4 — arena leases | merged; normal + ASAN |
| 5 — host admission | merged; normal + ASAN + TSAN |
| 6 — routing + tap | merged; normal + ASAN + TSAN |
| 7 — flow control | merged; normal + ASAN + TSAN |
| 8 — C reference client | merged; normal + ASAN + TSAN |
| 9 — Go reference client | merged; pure Go, vectors byte-exact |
| 10 — conformance suite | merged; real Go client ↔ C host interop |
| 11 — capture + replay | merged; normal + ASAN + TSAN |
| 12 — perf baseline gate | merged; ~134 ns/event dispatch, 1000 ns ceiling |

The suite ran under self-review from slice 4 onward (the shared roundtable panel was only partly
working); every slice's adversarial self-review caught real defects before merge — a control fd
passed read-write, reply/cancel spoofing, a null-deref, a slot overflow, encoder-derived vectors, and
a no-op D7 gate among them.

## What is deliberately deferred (later trees, not this one)

- **Arena-payload routing — DONE for notifications** (wire v2 + slice-4/6 composition). The host
  publishes an arena lease (slice 4) to a snapshot of the kind's observers, drops the producer ref, and
  forwards the reference frame under the same BLOCK/SHED discipline as an inline fan-out (a shed
  observer's ref is released so the lease still drains; no observers reclaims immediately). The host
  never dereferences an arena offset; a co-located consumer reads in place via the lease table
  (generation + holder gated) and releases. `bus_client_publish_arena` is the producer emit. Two narrow
  pieces remain deliberately deferred, each awaiting a real consumer to validate against rather than
  built speculatively:
    - *Correlated arena patterns — DONE.* An arena request is published to the kind's server and
      delivered by reference (no server → reclaim + capability_absent); the server's arena reply is
      published back to the original requester and the correlation retired on delivery. Only the server
      may answer (a forged reply is dropped + reclaimed); a shed request retires its correlation; a
      departed peer's ref is dropped by reap. An arena cancel carries no meaningful payload, so its
      lease is reclaimed and it is dropped. The remaining gap is a real large-payload request/reply
      *consumer* (the memory recall round-trip), which is a separate memory-migration slice.
    - *Cross-thread producer allocation — DONE.* The host-private lease table is now guarded by an
      in-process mutex (bus_arena.c), so a co-located producer may allocate and fill a lease from its
      own thread while the pump thread publishes/releases/reaps and a consumer thread reads/releases in
      place. The lock covers only the table transitions; the payload byte-copy stays outside it, kept
      safe by the refcount. Proven by a ThreadSanitizer lane (scripts/run-bus-arena-tsan.sh) that runs
      all three roles concurrently over a small (recycling) arena — clean with the lock, and a verified
      data race the moment it is removed.
    - *Arena bytes in the capture stream (D10) — DONE.* The pump resolves a producer-held lease once,
      pre-routing (bus_arena_producer_bytes), and hands the span to the capture tap, which materializes
      it into the record exactly like an inline payload. Replay reads the bytes from the record blob, so
      a captured arena event replays byte-exact without the (long-gone) lease. This is the single place
      the host reads arena bytes it did not write — bounded by the span, for the governance tap only.
- **Module replay.** Slice 11 delivers observational replay; re-driving a module against recorded
  inbound events is a later tree (D10).
- **`shm_open` portability fallback.** v0 is Linux-only by D1; a fallback carries its own threat-model
  note.
- **The memory round-trip perf rows.** Marked pending in `bench/bus_baseline.json`; measurable only
  against a pre-migration baseline in the memory-migration slice (delivery step 3).

## Gates in place

- `scripts/check_bus_blast_radius.sh` (D7) — no shipping binary links the bus; wired into `make lint`,
  runs on every slice PR.
- `scripts/check_bus_d1_gate.sh` — the D1 amendment status (now ACCEPTED).
- `scripts/test_bus_conformance.sh` — C vectors, Go vectors, cross-language interop, single-host (D8).
- `scripts/check_bus_perf_gate.sh` (D4 / invariant 15) — dispatch-overhead ceiling; memory rows pending.
- `scripts/run-bus-arena-tsan.sh` — ThreadSanitizer lane for the arena lease table: producer/pump/
  consumer threads race the table through the arena API; a data race aborts non-zero.
