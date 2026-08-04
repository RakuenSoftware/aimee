# A delegate's budget and its stage wall cap must be reconcilable

- **State:** PENDING — measured workflow failure; no reconciliation or diagnostic is implemented.

## Problem

Two independent limits bound an implement delegate, and nothing checks them
against each other:

- **the delegate's tool-loop budget** — `agent->timeout_ms * 4`
  (`src/posix/agent_runtime.c`), so the shipped `timeout_ms: 180000` gives
  **12 minutes** of total tool-loop time
- **the workflow's per-stage wall cap** — `autonomy.max_wall_secs`, default
  **1800s / 30 minutes**

Whichever is smaller silently truncates the work, and the resulting event says
nothing about which limit fired or what the other one was.

## Measured

Both failure directions were observed on live runs while validating the WFE.

**Delegate budget too small for the work.** An implement delegate on a real
slice made 80 successful tool calls and was then killed:

    delegate 'MiniMax-M3' attempt failed (turns=79, tool_calls=84, successful=80):
    tool loop budget exhausted (660258ms of 720000ms used)

The work was progressing normally. Twelve minutes is simply not enough for an
implement packet of that size, so the slice returned partial, looped, and made
no forward progress across repeated attempts.

**Stage cap smaller than the delegate budget.** Raising `timeout_ms` to
900000 (a 60-minute loop budget) against the default 30-minute stage cap made
every attempt die the other way:

    impl | pause | wall_cap: context deadline exceeded

The delegate was mid-work each time. No attempt could ever complete, because
the stage cap guaranteed it would be cut off before its own budget allowed it
to finish.

Neither message names both numbers, so from the event log alone it is not
possible to tell that the two limits are in conflict.

## Proposal

Make the relationship explicit rather than leaving it to two unrelated config
values that happen to be set consistently.

1. **Clamp, don't kill.** When dispatching a delegate for a stage, bound its
   tool-loop budget by the stage's *remaining* wall budget. A delegate that
   runs out of time then ends its own loop cleanly with a partial result — the
   behaviour it already has for a single call, where
   `agent_loop_per_call_timeout_ms` caps a call by the remaining loop budget.
   Applying the same principle one level up turns a hard kill into a graceful
   partial the engine can already reason about.

2. **Say which limit fired.** Whichever bound is reached, report both values
   and the elapsed time. `wall_cap: context deadline exceeded` should read as
   something a reader can act on — which limit, what it was, what the other one
   was.

3. **Refuse an unsatisfiable pairing at startup.** If the configured delegate
   budget for a write role exceeds `autonomy.max_wall_secs`, no attempt at that
   stage can ever finish. That is a configuration error and should be reported
   as one when the config is loaded, not discovered by watching attempts die.

## Deliberately not proposed

**A specific default for either value.** How long an implement delegate should
be allowed to run is a cost and latency decision that belongs to whoever runs
the appliance. The point here is that the two limits must be reconcilable and
must explain themselves — not that either number is currently wrong.

## Acceptance criteria

- A delegate whose stage budget expires returns a partial result rather than
  being killed by a deadline, and the slice records it as such.
- The event recorded when either limit is reached names both limits and the
  elapsed time.
- Loading a config whose write-role delegate budget exceeds
  `autonomy.max_wall_secs` fails with a message naming both values.
- A test covers each direction: delegate budget smaller than the stage cap, and
  stage cap smaller than the delegate budget.

## Evidence

Both quoted events are from run `wi_e51e37cf` and its predecessor
`wi_f96d4b18` on the validation appliance, July 2026.
