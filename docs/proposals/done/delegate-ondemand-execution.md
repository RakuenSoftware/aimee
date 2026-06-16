# Proposal: on-demand delegate execution (retire the CPU-pool gate)

- **State:** landed — merged to `testing` via PR #333. The server-only on-demand
  delegate execution (per-thread spawn + `delegate_max_inflight` backstop, all
  three call sites converted, the CPU-pool gate retired) is in tree
  (`src/server/server_delegate_ondemand.c`); session-delegate and aimee-kb worker
  pools remain the noted fast-follows. (Originally implemented on
  `feat/delegate-io-pool`; self-reviewed — the roundtable infra was itself wedged
  by the bug this fixes.)
- **Author:** JBailes
- **Date:** 2026-06-15
- **Scope:** server only (this PR). Session-delegate pools and aimee-kb worker
  pools are noted as fast-follows.

## Problem

Background (sessionless) delegates — roundtable reviews, `aimee delegate
--background`, coord-task and skill-review jobs — ran on the global **compute
pool** (`CONFIG_DEFAULT_BACKGROUND_THREADS = 2`) and were further gated by the
2-wide **compute budget** (`server_compute_budget_acquire` blocks on a
semaphore sized to `compute_threads`). A delegate holds a whole worker thread
for the *entire* provider call, which is ≈100% network wait — near-zero CPU and
RAM. So just **two** concurrent (or slow, or hung) delegates saturated the pool
and every further delegate sat `pending` forever. This is the wedge observed in
the field: workers blocked in provider TLS reads while jobs never started.

These caps are **relics**. They date from when the server embedded and indexed
*inline* (genuinely CPU-bound work that warranted a core-sized pool). That work
has since moved to the **embedder container** and **aimee-kb**; every remaining
submit to the server compute pool is `delegate_worker` or a tool call — I/O
coordination, not CPU. Gating I/O concurrency on a CPU-sized pool is the wrong
resource model.

## What stays (not a relic)

The **per-model/provider concurrency limiter** (`concurrency_acquire_*`). It
enforces each provider's rate/parallel limits and prevents hammering an
endpoint. After this change it becomes the *only* throttle on delegate
concurrency — which is exactly its purpose (the budget comment already said so:
"Model/provider concurrency limits decide how many delegates may run in
parallel").

## Design: on-demand threads + high safety ceiling

Each sessionless delegate runs on **its own detached thread**, spawned on
demand and reaped on completion (`delegate_spawn_ondemand` in
`server_compute.c`). No fixed pool; no compute-budget gate. Throttling is the
per-model limiter. A configurable **backstop ceiling** (`delegate_max_inflight`,
default **512**, env `AIMEE_DELEGATE_MAX_INFLIGHT`) prevents a pathological
fan-out from exhausting fds/memory — "effectively unlimited" for any real
workload, but not literally unbounded.

Mechanics:
- An atomic-by-mutex in-flight counter; spawn rejects (`-1`, job marked failed)
  only at the ceiling — unreachable under the per-model limiter in normal use.
- Detached threads carry the same 32 MB stack as compute-pool workers (the
  agent loop has deep frames).
- The compute **budget is bypassed** exactly as session delegates already do:
  a positive `compute_executor_threads` grant makes `compute_ctx_begin_budget`
  take its non-blocking path (verified: `compute_ctx_release_budget` then never
  touches the global budget). This is the *second* 2-wide gate, also removed for
  delegates.
- **Shutdown:** `delegate_ondemand_drain(5000)` gives in-flight delegates a
  bounded window. On-demand delegates bypass the budget (never touch
  `compute_budget_mutex`) and own no socket (no `conns_mutex`); a straggler past
  the window only touches DB1 + its own ctx, so teardown is safe.
- **Observability:** `aimee workers` no longer shows delegates as pool slots
  (they aren't pooled), so a `delegates_inflight` gauge is added to the workers
  response. `compute_pool_set_job` is a safe no-op off a pool thread.

## Call sites converted

All three sites that submitted `delegate_worker` to the global pool:
`server_compute.c` (sessionless `delegate_dispatch`),
`server_coord_dispatcher.c`, `server_skill_jobs.c`.

## Out of scope (fast-follows)

- **Session-delegate pools** (`server_session_pool`, 4 threads, *shared* with
  chat-stream + tool workers). Lower severity (per-session, already budget-free)
  and entangled with chat/tool isolation — a separate, careful change.
- **aimee-kb worker pools** — same "embedder is containerized → remaining work
  is I/O" thesis applies; validated + changed in its own PR.
- The complementary delegate **timeout** fix (separate PR) ensures a single hung
  call also self-terminates; with on-demand execution a hung call no longer
  wedges *others*, so the two are independent and additive.

## Testing

- `aimee_resolve_delegate_max_inflight` unit-tested (default / configured / env
  override / garbage-env) in `test_config.c`.
- Full `aimee-server` + `aimee-kb` link clean under `-Werror`.
- Regression green: compute-pool, config, agent, coord-jobs, server-compute,
  server-jobs-aux, delegate-monitor, cmd-delegate.
