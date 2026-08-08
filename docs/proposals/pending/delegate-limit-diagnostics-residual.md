# Delegate limit diagnostics: grouped dispatch and live partial-result proof

- **State:** PENDING — residual of
  [`delegate-budget-must-fit-its-stage-cap.md`](../done/delegate-budget-must-fit-its-stage-cap.md),
  which reconciled the two limits and named them on the single-delegate path.

## Problem

Two independently testable gaps were left open when the parent proposal closed.

**Grouped dispatch is not annotated.** `NativeRunner.delegate` wraps a
stage-deadline failure in `DelegateLimitError`, so the parked event names the
stage wall budget, the tool-loop cap actually applied, and the elapsed time.
`NativeRunner.delegateGroup` — the panel/roundtable path — does not. A grouped
seat that dies on the stage deadline still records only
`context deadline exceeded`, which is the same unreadable event the parent
proposal set out to remove. The parent scoped itself to the implement path it had
measured, so this was deliberately deferred rather than overlooked.

**The partial-result path has no automated coverage.** The C runtime assembles a
partial result when a delegate's own tool-loop budget ends the loop: it sets
`out->abstained = 1` and an `abstain_reason` of
`"partial result after tool use: tool loop budget exhausted (…)"`
(`src/posix/agent_runtime.c`). The arithmetic that decides to stop is unit-tested
in both directions (`src/tests/test_agent_apikey.c`), and the assembly was
verified by reading the source, but nothing exercises the assembled partial
end to end — it needs a provider that can be driven to exhaust a real budget.
The parent proposal's first acceptance criterion is therefore **code-verified,
not test-verified**, and is recorded that way rather than as proven.

## Proposal

1. Annotate stage-deadline failures on the grouped dispatch path with the same
   both-limits diagnostic, so a panel seat killed by the stage cap is as
   diagnosable as an implement delegate.
2. Add a test that drives a delegate to exhaust its tool-loop budget against a
   stub provider and asserts the recorded outcome is an abstained partial whose
   reason names both limits and the elapsed time — closing the parent's first
   criterion with a test rather than a reading.

## Non-goals

**Choosing defaults for either limit.** Unchanged from the parent: how long an
implement delegate may run is the appliance operator's cost and latency decision.

## Acceptance criteria

- A grouped-dispatch seat that fails on the stage deadline records a diagnostic
  naming the stage wall budget, the applied tool-loop cap, and the elapsed time.
- A test asserts the abstained partial result produced when a tool-loop budget is
  exhausted, without requiring a live model provider.
- Both tests fail if the diagnostic drops either limit or the elapsed time.
