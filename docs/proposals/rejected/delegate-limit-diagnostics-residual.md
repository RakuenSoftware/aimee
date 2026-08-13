# Delegate limit diagnostics: grouped dispatch and live partial-result proof

- **State:** REJECTED — the requested implementation belongs to the not-yet-ported Go delegate
  executor, not the legacy C runtime.
- **Archived parent:**
  [`delegate-budget-must-fit-its-stage-cap.md`](../done/delegate-budget-must-fit-its-stage-cap.md).
- **Superseded by:**
  [`delegate-execution-into-the-module.md`](../pending/delegate-execution-into-the-module.md).

## Rejection rationale

The residual combined two gaps at different ownership boundaries:

1. Grouped deadline annotation can be implemented in the Go delegates plane.
2. The requested end-to-end proof must drive the agent loop against a stub provider until its
   configured tool-loop budget is exhausted and then observe the assembled partial result.

There is no Go producer for that result today:

- `server-go/modules/delegates/delegates.go` dispatches policy and planning stages; its
  `StageInvoke` path only canonicalizes a role into a fixed-size response and explicitly does not
  invoke a delegate.
- `server-go/modules/delegates/plane/client.go` is an HTTP consumer. It forwards
  `tool_loop_timeout_ms_cap`, polls `/v1/delegate/status`, and accepts an already-produced
  `job_status: partial`; it defines no provider or agent-loop interface for a stub to drive.
- `src/server/server_compute.c` creates the tool-loop deadline and
  `src/server/agent_runtime.c` applies it around the actual agent execution and fallback paths.

Consequently, a Go HTTP stub can only replay a preconstructed terminal status and prove client-side
formatting. There is no Go producer branch for repeated stub-provider turns to exercise. Creating
that branch just to satisfy this residual would be the delegate-executor port itself, not a bounded
diagnostic fix. Adding another C executor test would instead deepen precisely the ownership
violation the active module-port proposal is removing.

This residual is therefore rejected rather than partially implemented or falsely marked done. Its
required behavior becomes acceptance evidence for the Go executor port: when execution moves into
`server-go/modules/delegates`, grouped and single dispatch must share the limit diagnostic and the
Go agent-loop test must drive a stub provider through actual budget exhaustion. The rejection is
therefore about current ownership and testability, not about dropping the behavior.

## Preserved requirement

The successor is not complete until deterministic Go tests prove:

- every grouped seat stopped by the stage deadline names the stage wall budget, applied tool-loop
  cap, and elapsed time; and
- a stub provider can force the Go agent loop to exhaust that cap and yield an abstained partial
  whose reason contains both limits and elapsed time.
