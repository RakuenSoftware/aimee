# roundtable.review over the bus: seat launches starve while the review runs

Status: blocker for deleting `src/server/wfe_roundtable_proxy.c`.
Found: 2026-08-04, on CT403 running `aimee-server:rt4`.

## What works

The bus path itself is proven end to end. `server.c` dispatched
`roundtable.review` to `aimee-module-roundtable` at kind 9474 stage 2, the
module resolved the saved panel, seated its reviewer, and returned a structured
result with the correct artifact hash, run id, participant-failure taxonomy and
pause reason. Nothing about the transport, the framing, the grant or the module
handler is in question.

The delegate replay reservation was also confirmed live: two launches under one
`Idempotency-Key` returned the same `job_id` with `replayed: true`, and no
second job was created.

## What fails

The review's own seat never gets a delegate. The seat launch reaches
`POST /v1/delegate/run` and times out at the plane client's 30s request ceiling,
so the only seat is dropped and the panel parks `panel_unreachable`:

    "category": "deadline",
    "detail": "Post \"http://aimee/v1/delegate/run\": context deadline exceeded"

## Evidence that it is contention, not the request

An identical launch is instant when the server is idle and slow only while a
review is in flight:

| condition                          | elapsed |
|------------------------------------|---------|
| launch, server idle                 | 0s      |
| launch, during a running review     | 27s     |

The request shape is not the cause. Launches carrying every field a seat
carries -- `tools: true`, `via: "$random"`, `provided_target: true`,
`max_turns_cap: 24`, a 2.5 KB prompt -- all complete in 0s when no review is
running. `GET /v1/agent/list` also returns instantly during a review, so it is
not general server unresponsiveness.

The 27s figure matters: it is just under the 30s client ceiling, which is why
the review's own seat lands on the wrong side of it.

## Reading

`roundtable.review` is dispatched with `rh_dispatch_op_async` and the handler
then blocks in `obs_bus_module_call` for the whole review. The module, running
that review, calls back into this same server to launch its seats. So the review
is waiting on a callback that is contending with the review itself.

The orchestration pool is 16 threads (`SERVER_ORCHESTRATION_POOL_THREADS`), so a
single review does not exhaust it; the contention is on something narrower that
delegate launch needs and the in-flight review holds. That has not been
identified yet and should be, rather than assumed.

## What not to do

Raising the plane client's `RequestTimeout` past 30s would make this smoke test
pass. It should not be done as the fix: the wait is real, it scales with the
number of seats, and a multi-seat panel would rediscover it. It would convert a
visible failure into a slow one.

## Next

1. Identify what a delegate launch actually blocks on while a review is in
   flight. The 27s/30s proximity suggests a lock or queue released by a timeout
   rather than a capacity limit.
2. Decide whether the C review handler may block a worker for the full review
   duration at all, given the callback dependency.
3. Only then delete the proxy.

CT403 has been rolled back to `aimee-server:rt3`, which serves reviews over the
proxy and is healthy. `aimee-server:rt4` remains on the box for further
investigation. The persisted module grant there was refreshed to
`serve=9473,9474`, which is harmless under rt3.

## Related gap found on the way

`deploy/container/server-entrypoint.sh` copies a module grant only when the
target does not already exist, so that an operator can tighten a persisted
policy without the next release widening it again. The consequence is that a
module which gains a stage never gets authority for it on an existing
deployment: 403 kept `serve=9473` and the review stage was refused until the
grant was replaced by hand. Any deployment upgrading into this build would hit
the same thing silently. Worth a startup warning when a persisted grant's serve
set is a strict subset of the shipped one.
