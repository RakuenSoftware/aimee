# Orchestration seam — delegate first-port plan (Slice 3 deliverable)

- **State:** PENDING — the "clear first-port plan" that Slice 3 of
  `response-orchestration-stages.md` is required to produce. The seam itself
  (`gw_orchestration_seam.{h,c}` + `test_gw_orchestration_seam.c`) lands in Slice 3;
  the actual delegate port described here is a **sequenced follow-up**, not Slice 3.
- **Author:** JBailes
- **Date:** 2026-07-17

## What Slice 3 delivered

A distinct orchestration-hook seam, deliberately NOT a request/response stage transform:

- `gw_turn_snapshot_t` — immutable, borrowed view of the turn (turn id, session id,
  read-only request + response-or-NULL). Hooks observe; they do not rewrite it.
- `gw_turn_capabilities_t` — two narrow handles, `spawn_delegate(role, brief)` and
  `dispatch_workflow(lane, payload)`. The only way a hook effects change.
- `gw_orch_result_t` — tagged verb `CONTINUE / COMPLETE / SUSPEND / FAIL`, **fail-OPEN**
  (a bad hook never blocks a turn; the wire site logs FAIL). `SUSPEND` carries a
  caller-owned continuation string.
- `gw_orchestration_registry_build` / `gw_orchestration_run` — same up-front catalog
  validation as `gw_response_registry` (dup/empty-name/NULL-fn/overflow → −1), but the
  runner is fail-open rather than fail-closed.

Proven through the runner in `test_gw_orchestration_seam.c`, using a delegate-spawn hook as
the worked example.

## Why delegates are the first port

Workflows already have a request-side advisory seam (`gw_stage_router` /
`router_advise.c`, wired at `anthropic_http.c:289` and `openai_chat.c:986`). Delegate
spawning has **no** seam — it is called imperatively:

- `delegate_spawn_ondemand(compute_ctx_t*)` — `server/server_delegate_ondemand.c:92`
  (thread pool + in-flight ceiling; the real spawn primitive).
- Callers: `server/server_compute.c:410`, `server/server_coord_dispatcher.c:55`,
  `server/server_skill_jobs.c:48`.
- The turn-time gate today is the imperative predicate `gateway_prevent_subagents_enabled()`
  (checked inline in `anthropic_http.c:529`, `openai_chat.c:1248`), not a registered module.

So delegates are the genuinely-new orchestration path and the honest first port.

## Follow-up port plan (NOT Slice 3)

1. **Adapter: a `spawn_delegate` capability backed by `delegate_spawn_ondemand`.** A small
   translation from `(role, brief)` + the turn snapshot to a `compute_ctx_t` the existing
   spawner accepts, respecting the in-flight ceiling (a refused spawn → handle returns <0,
   the hook returns `CONTINUE`, never `FAIL`-blocks the turn). No change to the spawn
   primitive itself.
2. **A `"delegates"` orchestration hook** that encodes the current `prevent_subagents` /
   on-demand decision and calls `caps->spawn_delegate`. Enable/disable via the config-store
   (per the roundtable ruling that the config-store — not env — is canonical for
   enablement); pick the toggle name in the config-surface slice, mirroring how Slice 2 left
   `AIMEE_STAGE_GOVERNANCE`'s durable surface to that slice.
3. **One wire site** builds the hook catalog + capabilities and calls `gw_orchestration_run`
   at the turn point where `delegate_spawn_ondemand` is decided today, replacing the inline
   imperative call. Prove enabled-spawns / disabled-does-not through the runner, the same
   shape as the memory and governance toggle tests.

## Open (for the port's own slice, not now)

- **Continuation / SUSPEND consumer.** No turn suspends-and-resumes yet; `SUSPEND` is defined
  in the contract (roundtable-ruled) but has no live producer. Resumption ownership,
  idempotency, and cancellation are that slice's scope, per the parent proposal.
- **Workflows as the second port.** Once delegates prove the seam, fold the existing
  `gw_stage_router` advisory into a `dispatch_workflow`-backed orchestration hook so both
  live behind one registry.
