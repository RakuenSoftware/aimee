# Proposal: Push, not poll: make an idle aimee do nothing

- **State:** PENDING. Investigation complete; design for review. No code in this
  PR.
- **Author:** JBailes
- **Date:** 2026-10-30
- **Trigger:** the 2026-10-30 outage. CT100's 72G root filled, `store-db` could
  not start, and every governed tool call was refused because its audit evidence
  could not be committed. 34G of the disk was the audit WORM; almost all of its
  recent growth is the system polling itself while idle.

## Summary

The ouroboros ring is a push transport. It has subscriptions, notifications,
block/shed delivery policies and typed `overflow` on loss. aimee uses none of
that. In `src/modules/process-contracts.json` no client subscribes to any kind,
and exactly one client publishes (memory-postgres, kind 3000). Every interaction
is request/reply, so every component that needs to know "has anything changed?"
asks on a timer. Every one of those asks crosses a `ledger` stage, so it writes
two permanent WORM rows.

The same pattern repeats one layer down. The ring's own idle waits are sleeps,
not wakeups: the host pump naps 200µs–5ms, Go module runtimes nap 1–10ms (1ms
for the economizer), and the C module runtime sleeps 1ms on every pass. Spec
decision D5 (an `eventfd` doorbell) is not implemented anywhere.

An idle server therefore:

- makes about **25 ring calls a second** (about 91k an hour);
- writes about **4.5M audit rows a day**, about **1.3–1.5 GB a day**, more than
  99.9% of which record nothing happening;
- burns about **20% of one core** across 23 processes;
- fills each 256 MB bus-capture file within **2–3 minutes** of starting, after
  which capture is off for the rest of the run.

## Incident timeline (2026-10-30, UTC)

| Time | Event |
|---|---|
| through 07:59 | Idle-rate polling, hour after hour (see the table below). |
| 08:xx | CT100 root reaches 100%. Audit rows per hour fall from about 3,600 core config polls to 554, then 17 in hour 09. |
| ~10:00 | `store-db` in a restart loop: `mkdir …/overlay2/…/merged: no space left on device`. `aimee-server` unhealthy. |
| ~10:10 | Every agent tool call refused: "required governed-action audit evidence could not be committed". `aimee server status` still reported `aimee-server: ok`. |
| ~10:20 | CT100 rootfs grown online 72G → 136G (`pct resize 100 rootfs +64G`); `store-db` restarted; server healthy. |

Disk use at the time: `/var/lib/docker/volumes/aimee_aimee-server-home` 39G, of
which `audit/` 34G:

- `worm-recovery-20261014.db`: 20.4G, the live WORM log, 59.6M rows at about
  342 B per row.
- `worm-restored.db` and `worm-restored-20261014.db`: the same 15.7G inode
  (link count 2), read-only, from the Oct 14 recovery. The original is also kept
  as `/mnt/pve/backup/aimee-audit-recovery-20261017/worm-live.original.db.zst`.
  The recorded sha256 matches the manifest, but the restored copy itself has not
  been re-hashed.
- Bus capture files: 16 × about 256 MB = 4.0G in the volume root, never reaped.

## Measurements

**Source:** the live WORM, last 2.6M rows (2026-10-29 19:08 → 2026-10-30
10:44), read with SQLite read-only. The idle baseline is hours 03–07, when no
agent was active.

### Idle call rate by caller and stage

| Caller (principal) | Stage | Calls/hour idle | Period | Origin |
|---|---|---|---|---|
| wfe (64) | aimee-lifecycle | ~57,600 | 16 calls per scheduler pass | `server-go/internal/engine/scheduler.go:144` (`Run`), `:158` (`fill`) |
| wfe (64) | config-store | 26,634 | 8 calls per pass, plus 2 every 5s | the same `fill()` (policy, concurrency, per-workflow sources) and the trigger scan, `server-go/cmd/aimee-server/main.go:289` |
| core (0) | config-store `version` | 3,582 | 1s | `src/server/server.c:2256` main loop → `config_reload_if_changed()` |
| memory-postgres (73) | config-store | 1,440 | 5s | `server-go/modules/memory/personal_vectors.go:88`: endpoint read each pass |
| memory-egress (70) | egress-http | 1,440 | 5s | the same loop: embedder serving-ID probe each pass |
| core (0) | aimee-sessions | 240 | 15s | readiness sampler, `src/server/server_ready.c:91` → `db1_store_probe()` |
| core (0) | aimee-delegation, aimee-agent-work | 120 each | 30s | `src/server/server_delegate_monitor.c` sweep |

Total: about 91k calls an hour, or 182k WORM rows an hour.

### WORM growth by day

| Days | Rows/day |
|---|---|
| Oct 14–21 | 3.5M–5.0M |
| Oct 22–26 | 0–2.5M (server down or wfe idle-stopped) |
| Oct 27–29 | 4.25M–4.75M |

20.4G built up in 16 days. At this rate the 61G freed on Oct 30 lasts about six
weeks.

### What is not polling

- **aimee-pki and aimee-identity** (about 1 call per 3.3s while active, about 8
  an hour idle) track agent activity at roughly 10 calls per agent tool event.
  They are demand-driven: one mTLS presentation write plus identity checks per
  HTTPS connection. This is amplification rather than polling, and is covered
  under Phase 4.
- **memory-data, git-credential-resolve and guardrail-state** (1,860-byte
  requests) follow hook traffic.
- **workspace-runner and runner-io** follow runner activity, with no idle
  baseline.

### Idle CPU

Ten seconds of `/proc/<pid>/stat` deltas in `aimee-aimee-server-1` while idle
came to about 205 ticks at 100 Hz, or **about 20% of one core**:

| Process | Ticks |
|---|---|
| config module | 28 (serving wfe) |
| economizer | 27 (1ms ring poll) |
| aimee-server | 21 |
| memory | 18 |
| aimee-wfe | 15 |
| postgres module | 15 |
| aimee module | 13 |
| 16 others | 3–7 each |

## Inventory of time-driven loops

Each loop is classified as one of:

- **P:** polls for state another component owns. Should become a subscription.
- **T:** a genuine timer for a deadline or backoff that this component owns.
  Keep it, but arm it for the next due time instead of ticking.
- **H:** a liveness or health heartbeat. Mostly redundant with ring heartbeats.
- **W:** a wait-for-ready loop at startup. Bounded; fine.

| Loop | Class | Notes |
|---|---|---|
| wfe scheduler `fill()` every 1s | P + T | Resume, abandon, orphan reconcile, one resume query per transient pause reason (8+ in `schedulerTransientPauses`), fan-in parents, `WorkItems`, and three config reads, all every second. `Notify()` exists but is only called for wfe-internal events. |
| wfe trigger scan, 5s | P | Re-reads trigger definitions and `trigger.scan_interval_secs` from config every 5s so that edits take effect. |
| wfe budget heartbeat, 30s while running | T | Lease extension for a running item. Legitimate while work is active, and zero while idle. |
| core main loop, 1s: `config_reload_if_changed()` | P | The comment still describes watching for out-of-band file edits, but config is now a module. The module knows exactly when it commits. |
| core `admission_ensure_configured()`, per turn | P | One `version` call per agent turn (`src/server/agent_runtime.c:133`). |
| core readiness sampler, 15s | H | Probes DB1 with a sessions read. The ring already tracks every module's liveness with heartbeats and pidfd, and the host knows. |
| core delegate monitor, 30s | P + T | Stale-job sweep and admission reap. The ring, the store or the delegate's own heartbeat should report staleness; a deadline timer covers the rest. |
| core trigger scheduler, 30s | T | Cron is a genuine timer. Mostly a no-op under the Go wfe (`go_wfe`). |
| core embedder probe, 2s | H | `src/server/embedder_probe.c:95`: HTTP probe of the embedder. |
| memory personal vectors, 5s | P + T | Has a `wake` channel already. The 5s tick covers retry backoff (`next_attempt_at`) and endpoint changes, and re-probes the embedder identity every pass. |
| memory shared index (KB placement), 5s | P | Same pattern on the KB. Inactive here (no KB). |
| postgres session reaper, 15s | T | Local idle reap, no ring calls. Fine as a timer. |
| `aimee mcp-serve` tools watch, 15s per session | P | Each MCP session polls the server for `mcp.tools_list` generation over HTTPS, a new connection each time. None were running on the workstation during the measurement, so this stream does not appear in the table above. |
| ring host pump (`obs_bus.c` `consumer_main`) | transport | Naps 200µs → 5ms when idle. D5's `eventfd` is not implemented. |
| ring Go module runtime | transport | `go/ring/module_runtime.go`: idle 1ms → 10ms backoff, client heartbeat every 5s. The economizer pins it to 1ms (`RequestAdmissionPollInterval`). |
| ring C module runtime and module client | transport | `c/src/module_runtime.c:506`: an unconditional 1ms sleep on every pass, even after an event. `c/src/module_client.c:129,221` likewise. |
| ring accept loop | transport | `c/src/runtime.c:228`: non-blocking accept polled every 10ms. |

## Why this happened

1. **Ported, not redesigned.** The scheduler's shape (a 1s tick, a dozen cheap
   `UPDATE … WHERE` sweeps) is right for an in-process SQL store. When the store
   moved behind the ring, each sweep became a module call and the loop was left
   alone. The cost per sweep went from microseconds to two fsynced WORM rows.
   This is an inference from the code's shape; it has not been checked against
   history.
2. **There is no change-notification contract.** No store module publishes "this
   changed". Without that, polling is the only correct way for a consumer to
   learn about changes, so every consumer polls.
3. **Durability is per stage, not per operation.** All 90 `ledger` stages record
   every call. A read-only `WorkItems` list or a config `version` check costs the
   same as a state transition, so the audit log faithfully records the polling.
4. **The transport has no doorbell.** Without D5, every ring participant trades
   latency for idle CPU. The economizer chose 1ms. Push at the application layer
   would still leave the transport polling underneath.
5. **Nothing measured the idle cost.** No test or CI gate asserts what an idle
   server does, and no alert covered the audit volume's free space. Readiness
   checks `audit_capacity` (512 MB floor), yet status reported `ok` during the
   outage. That disagreement is unexplained and needs its own investigation.

## Target design

The invariant: **an idle aimee makes zero ring calls, writes zero audit rows,
and every process sleeps in the kernel.** Work is pushed to whoever must act,
and genuine time-based work is a timer armed for its due time.

### 1. Change notifications from the stores that own the state

- **Lifecycle store.** After each committed transition, it publishes a
  `work-item-changed` notification. The payload is a work-item id, a state
  generation, and the earliest pending `resume_at` if any. The store is the only
  component that knows when a transition commits. This covers new items, state
  changes, pause/resume, child completion (fan-in) and terminal states.
- **Config module.** After each committed mutation, it publishes
  `config-changed {version}`. It is the only writer; the CLI, the UI and
  `config_save` all go through it.
- **Delegation and agent-work stores.** They publish job state changes, so the
  delegate monitor reacts to transitions instead of sweeping.
- **Kinds.** These are notification kinds declared in `process-contracts.json`
  under each component's `publish`, and in subscriber grants (`subscribe=`). Use
  the block policy, so a slow consumer backpressures the producer and nothing is
  lost. Payloads carry ids and generations, never state, so each consumer reads
  what it needs.

### 2. Consumers subscribe and keep state in memory

- **wfe.** It subscribes to `work-item-changed` and `config-changed`. It
  dispatches on events. Fan-in resume and orphan cleanup run in response to the
  transitions that cause them.
- **Config clients (C and Go).** They cache the snapshot and refresh on
  `config-changed`. This removes the core 1s `version` poll, the per-turn
  `version` call, wfe's 8 reads per pass, the trigger scan's reads and memory's
  endpoint read.
- **Memory indexer.** It already has a `wake` channel. Writes wake it, and
  `config-changed` wakes it when the endpoint changes. The embedder serving
  identity is cached until the endpoint or a failure invalidates it.
- **The tools generation for `mcp-serve`.** It moves to the existing server push
  channel (SSE), the same way presence is delivered, instead of a 15s HTTPS poll.

### 3. Timers for time, armed for the next deadline

- **wfe.** It keeps one timer for the minimum `resume_at` across paused items,
  learned from notifications and from the reconcile. It replaces the
  per-reason resume queries and the wall-cap checks.
- **Delegate staleness.** A per-job deadline timer, re-armed by the job's
  heartbeat.
- **Cron.** It stays a timer, armed for the next fire time instead of a 30s
  tick.

### 4. Reconcile only when something may have been missed

A consumer runs one full reconcile in each of these cases:

- when it attaches to the ring;
- when it receives an `overflow` for a subscribed kind;
- when it reattaches after an epoch change.

Nothing else is time-based. The ring guarantees that a consumer either receives
an event or is told it missed one, so a periodic "safety sweep" is not needed.

### 5. Health from the ring, not from probes

The host already tracks heartbeats, pidfd liveness and served kinds.
Readiness should read that state and drop the 15s DB1 sessions probe and the 2s
embedder probe. The embedder becomes stale when an egress call to it fails,
which is demand-driven.

### 6. A doorbell in the transport (ouroboros-ring)

- **Implement D5.** The host waits on an `eventfd` that a publishing client
  signals, with a bounded timeout only for heartbeat reaping.
- **Add the same for consumers.** Each slot gets an `eventfd` (or futex word)
  that the host signals when it routes into that slot's inbound ring. The Go and
  C module runtimes block on it.
- **Remove the 1ms sleeps.** The unconditional 1ms sleep in the C module
  runtime goes. The economizer's 1ms override becomes unnecessary.
- **Accept loop.** It blocks on `poll(2)` instead of a 10ms non-blocking loop.
- **Heartbeats.** These remain, at their existing intervals (seconds). They are
  the only periodic activity left at idle.

This belongs in the ouroboros-ring repo as a contract change, with conformance
vectors for C, Go and Rust.

## Audit and capture consequences

- **Idle WORM growth goes to about zero.** Recorded rows are the transitions
  themselves, plus one notification per transition rather than 86,400 polls a
  day.
- **Per-operation durability is not proposed here.** It would change the audit
  contract: 90 stages are `ledger`, and some read-only operations sit on ledger
  stages. Removing the polls removes the volume without that change. Whether
  read-only operations should be `capture` instead of `ledger` is a separate
  decision for the owner.
- **Capture becomes useful again.** At about 1.6 MB/s from startup, the 256 MB
  per-session cap is hit within minutes. Without the polling, a capture file
  covers real activity. Old capture files need a retention rule; 16 are
  currently kept and never reaped.

## Phases

Each phase is its own PR.

1. **Measure and gate.** Add an idle-cost test. Bring a fresh server up on .252,
   let it settle, then assert zero ring calls and zero WORM rows over 60s idle,
   with an allowance only for heartbeats. Wire it into CI as a regression gate,
   so it records the current failure. Add a disk-free alert on the audit volume.
   It is actionable, unlike low RAM. Also investigate why `aimee server status`
   reported `ok` during the outage.
2. **Config push.** The config module publishes `config-changed`. The C
   `config_client` and Go `config.Client` subscribe and cache. Remove the core 1s
   `version` poll, the per-turn `version` call, and the wfe and memory config
   reads. This spans aimee-module-config (the module repo) and aimee (clients
   and contract).
3. **Lifecycle push and the wfe scheduler.** The lifecycle store publishes
   `work-item-changed`. The wfe scheduler becomes event-driven, with one resume
   timer and reconciles only on attach or overflow. The trigger scan reacts to
   `config-changed`.
4. **Core sweeps and probes.** The delegate monitor becomes event-driven with
   deadline timers. Readiness reads ring state. The embedder probe becomes
   demand-driven. The memory indexer caches the serving identity. `mcp-serve`
   tools-generation goes over SSE. mTLS presentation writes are debounced per
   certificate, and connections are reused, to cut the roughly 10 PKI and
   identity calls per tool event.
5. **Ring doorbell** (ouroboros-ring). D5 for the host, per-slot wakeups for
   consumers, blocking accept, and runtime and SDK updates, followed by an aimee
   pin bump. The idle-CPU gate is tightened to "no process wakes faster than its
   heartbeat".

Phases 2 and 3 remove more than 99% of the audit volume. Phase 5 removes the
idle CPU.

## Verification

- **The idle gate from phase 1:** zero ring calls and zero WORM rows over 60s
  idle on a fresh .252 environment, run before and after each phase.
- **Latency:** dispatch latency from enqueue to start must be no worse than
  today's (under 1s on poll) for every event-driven path. Measured in the e2e
  suite.
- **Loss:** with the shed policy forced in a test, a consumer that misses a
  notification receives `overflow`, reconciles, and converges to the same
  state.
- **CPU:** idle CPU on the server container is measured with the same
  `/proc/<pid>/stat` method. The target is under 1% of a core after phase 5.

## Decisions for the owner

1. **The duplicate restored WORM copy (15.7G).** Delete `worm-restored.db` and
   `worm-restored-20261014.db` after re-hashing against the backup's recorded
   sha256? This cannot be undone. Not done.
2. **Durability of read-only operations on ledger stages.** Leave as is (this
   proposal makes it moot for polling), or classify per operation?
3. **Capture retention.** How many capture sessions to keep.
