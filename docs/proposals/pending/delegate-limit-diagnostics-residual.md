# Delegate limit diagnostics: grouped dispatch and Go executor exhaustion proof

- **State:** PENDING — Go-owned grouped diagnostics and producer-exhaustion proof remain unimplemented.
- **Archived parent:**
  [`delegate-budget-must-fit-its-stage-cap.md`](../done/delegate-budget-must-fit-its-stage-cap.md).
- **Delivered prerequisite:**
  [`delegate-execution-into-the-module.md`](../done/delegate-execution-into-the-module.md).

## Lifecycle correction

PR #2634 rejected this residual because the provider/execution producer was still in C. That
dependency has since landed in `server-go/modules/delegates`, and the 2026-08-15 rejection audit
also established that a valid objective must be rewritten for its Go owner rather than rejected
because its old test plan names a legacy seam. The diagnostic objective remains unresolved.

The old requirement expected the C loop's `partial` terminal state. The Go producer deliberately
has no partial terminal state: limit exhaustion is a typed failed invocation, while caller-owned
partial/no-op policy stays in the workflow engine. This rewrite preserves the observable guarantee
without reviving obsolete C semantics.

## Verified Go baseline and remaining gaps

- `server-go/modules/delegates/executor.go` is the production producer. Its `turnMonitor` enforces
  positive `MaxTurns` from real CLI JSON events and cancels the delegate process group.
- A turn-cap failure currently returns only the string `delegate maximum turn count exceeded (N)`;
  it does not report observed turns, elapsed time, or the enclosing execution deadline as typed
  fields.
- `RegistryExecutor` types an execution deadline separately from a capacity-wait deadline, but the
  result does not carry both competing bounds in one diagnostic.
- `server-go/internal/engine.NativeRunner.delegate` wraps a single stage-deadline error in
  `DelegateLimitError`, naming the stage wall remainder, applied producer cap, and elapsed time.
- `NativeRunner.delegateGroup` applies caps and returns `DelegateGroup` results directly. A grouped
  seat stopped by the same stage deadline still lacks the shared limit diagnostic.
- Existing executor tests drive `turnMonitor` directly. They do not run a stub CLI through
  `RegistryExecutor.Execute`, cross the delegate wire, kill the process group at exhaustion, and
  assert the terminal result consumed by WFE.

## Go implementation plan

### 1. One typed termination contract at the producer

Extend the Go delegate invocation result with a bounded termination diagnostic containing:

- stable kind: `turn_cap`, `execution_deadline`, or `cancelled`;
- configured maximum turns and observed turns when the turn cap fires;
- producer execution deadline/cap and elapsed execution time;
- a safe, bounded detail string for operator context.

`server-go/modules/delegates` is the sole owner of turn counting and producer termination kinds.
Unknown or malformed termination data fails closed as a generic terminal failure; callers may not
upgrade it to `done` or `partial`. The C bus host transports the versioned result only and contains
no parallel turn-count or termination classifier.

### 2. One caller-side formatter for single and grouped dispatch

Refactor `server-go/internal/engine` so single and grouped dispatch use the same Go helper to combine
the producer diagnostic with the caller-owned stage wall bound. It returns the same stable fields
and readable message for both paths:

- stage wall budget supplied to the dispatch;
- producer execution/turn cap actually applied;
- elapsed time;
- which bound fired.

For `DelegateGroup`, capture the shared dispatch start once and decorate each failed seat
independently without changing successful seats or participant identity. A capacity refusal remains
an admission error, not execution exhaustion. Cancellation without a deadline remains cancellation,
not a fabricated wall-cap failure.

### 3. Real Go producer fixture, not a replayed result

Add a stub CLI fixture selected through a temporary Go agent registry. It emits valid Claude- and
Codex-shaped JSON events beyond a configured `MaxTurns`, then blocks so the watchdog/process-group
kill is observable. Run it through `RegistryExecutor.Execute` and the real delegate wire; do not
construct a terminal result in the test.

The fixture proves that the Go producer counts events, stops at the exact cap, reaps the process
tree, returns `failed` with `turn_cap`, and reports configured/observed turns plus elapsed time. It
must never return the retired C `partial` state.

## Acceptance

```yaml acceptance
- {id: 1, tier: mechanical, check: "cd server-go && go test ./internal/engine -run TestDelegateGroupLimitDiagnostic"}
- {id: 2, tier: integration, check: "cd server-go && go test ./modules/delegates -run TestRegistryExecutorTurnCap"}
- {id: 3, tier: integration, check: "cd server-go && go test ./modules/delegates -run TestExecutionStageTurnCapWire"}
```

- Single and grouped stage-deadline tests assert the same typed kind and fields: stage wall budget,
  applied producer cap, elapsed time, and firing bound.
- A mixed group proves only the deadline-stopped seat is decorated; successful and independently
  failed seats retain their own outcomes and participant association.
- Stub-CLI integration tests for both supported JSON families cross `RegistryExecutor.Execute` and
  the bus wire, exceed a real positive `MaxTurns`, and prove process-tree cleanup and a typed failed
  result without a live provider.
- Capacity wait, explicit cancellation, malformed producer reply, and execution deadline remain
  distinct fail-closed outcomes.
- A mechanical check keeps turn counting and producer termination classification inside
  `server-go/modules/delegates`; the legacy C host may only transport the versioned result.
- The original readable invariant remains: every limit-driven terminal event names both competing
  bounds and elapsed time, and tests fail if any of those fields disappear.

## Non-goals

- Choosing default stage, execution, or turn limits.
- Reintroducing a `partial` terminal state in the delegate producer.
- Moving caller-owned retry, work-item, participant, or partial/no-op policy into the delegate wire.
- Reopening the removed C executor or HTTP agent-service path.
