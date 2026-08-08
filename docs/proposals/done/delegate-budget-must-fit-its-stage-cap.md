# A delegate's budget and its stage wall cap must be reconcilable

- **State:** DONE (2026-08-08) — the two limits are reconciled, and whichever one
  stops a delegate now names both of them and the elapsed time. Clause 3 shipped
  in an amended form; see [Amendment](#amendment-2026-08-08). Residual:
  [`delegate-limit-diagnostics-residual.md`](../pending/delegate-limit-diagnostics-residual.md).

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

## Delivered

Reconciled against `testing` on 2026-08-08 and **archived** here; its
independently testable remainder returned to `pending/` as
[`delegate-limit-diagnostics-residual.md`](../pending/delegate-limit-diagnostics-residual.md).
Two of the three clauses were already on `testing` when this was picked up; the
proposal's own status line above was stale and is corrected here rather than
rewritten.

| Clause | State | Evidence |
|---|---|---|
| 1. Clamp, don't kill | Already shipped | `applyDelegateDeadlineCap` (`server-go/internal/engine/native_runner.go`) bounds the tool-loop cap by the stage's remaining wall minus a verifier reserve, and refuses a write dispatch that cannot get one viable call. The C loop ends on `agent_loop_per_call_timeout_ms` returning -1 and assembles a partial result — `out->abstained = 1`, `abstain_reason = "partial result after tool use: …"` (`src/posix/agent_runtime.c`). |
| 2. Say which limit fired | Completed here | The delegate-budget direction already named both limits and elapsed from C (`tool loop budget exhausted (elapsed=… effective=… configured=… stage_remaining_cap=…)`). The stage-deadline direction reported only `wall_cap: context deadline exceeded`; `DelegateLimitError` now adds `stage_wall_remaining=… delegate_tool_loop_cap=… elapsed=…`, which reaches the parked event through the existing `detail` path unchanged. |
| 3. Refuse an unsatisfiable pairing | Shipped, amended | `autonomy.max_wall_secs` below the write-role floor is rejected when set, naming the value and the floor (`server-go/internal/config/store.go`). See the amendment below for why the literal form of this clause could not be implemented. |
| 4. A test per direction | Completed here | `TestStageDeadlineDiagnosticNamesBothLimitsAndElapsed` and `TestDelegateBudgetSmallerThanStageCapKeepsItsOwnDiagnostic`, plus `TestDelegateLimitErrorNamesWriteStageMagnitudes`, `TestWriteRoleWallFloorMatchesConfigBound`, `TestWallCapBelowWriteRoleFloorIsRejectedNamingBothValues`, `TestDefaultWallCapRemainsAcceptable`. The clamp arithmetic is pinned in both directions in `src/tests/test_agent_apikey.c`. |

## Amendment (2026-08-08)

Clause 3 asked that loading a config whose **write-role delegate budget exceeds
`autonomy.max_wall_secs`** be refused. That criterion cannot be implemented as
written, for two independent reasons found while reconciling it:

1. **Clause 1 removed the failure it describes.** Once the delegate's tool-loop
   budget is clamped to the stage's remaining wall, a *larger* configured budget
   is no longer unsatisfiable — it is simply reduced at dispatch. The condition
   the clause names is no longer the condition that prevents progress.
2. **It would refuse the shipped defaults.** A reasoning agent's default budget
   is `AGENT_REASONING_TIMEOUT_MS` (600000ms) × 4 = **2400s**, against a default
   `autonomy.max_wall_secs` of **1800s**. The literal rule therefore rejects a
   stock install at startup — a worse failure than the one being fixed.

What genuinely cannot make progress after clause 1 is the **opposite** pairing: a
wall cap too small for a write delegate to obtain one viable call. Below
`delegateWriteVerifyReserve` + `delegateWriteMinRunBudget` (5m + 1m = **360s**)
every implement attempt refuses before starting, and `max_wall_secs` accepted
values as low as 30s. That is the unsatisfiable configuration, and it is what is
now rejected — preserving the clause's intent ("reported as a configuration error
when the config is loaded, not discovered by watching attempts die") while
dropping a premise that clause 1 had already invalidated.

The floor constant is duplicated across the `config` and `engine` packages
because `engine` already imports `config`; `TestWriteRoleWallFloorMatchesConfigBound`
fails if the two drift apart.

## Not closed here

The stage-deadline annotation covers the single-delegate dispatch path, which is
the implement path this proposal measured. The grouped/panel dispatch path and a
live end-to-end proof of the partial-result behaviour are carried in
[`delegate-limit-diagnostics-residual.md`](../pending/delegate-limit-diagnostics-residual.md).
