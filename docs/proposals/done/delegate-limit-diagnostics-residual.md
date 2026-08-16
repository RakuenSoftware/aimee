# Delegate limit diagnostics: grouped dispatch and Go executor exhaustion proof

- **State:** DONE — Go-owned grouped diagnostics and real producer-exhaustion proof delivered
  2026-08-16.
- **Archived parent:**
  [`delegate-budget-must-fit-its-stage-cap.md`](../done/delegate-budget-must-fit-its-stage-cap.md).
- **Delivered prerequisite:**
  [`delegate-execution-into-the-module.md`](../done/delegate-execution-into-the-module.md).

## Lifecycle correction

PR #2634 rejected this residual because the provider/execution producer was still in C. That
dependency has since landed in `server-go/modules/delegates`, and the 2026-08-15 rejection audit
also established that a valid objective must be rewritten for its Go owner rather than rejected
because its old test plan names a legacy seam. This Go implementation resolves that diagnostic
objective.

The old requirement expected the C loop's `partial` terminal state. The Go producer deliberately
has no partial terminal state: limit exhaustion is a typed failed invocation, while caller-owned
partial/no-op policy stays in the workflow engine. This rewrite preserves the observable guarantee
without reviving obsolete C semantics.

## Delivered Go implementation

- `server-go/modules/delegates` now returns a validated, bounded termination diagnostic for
  `turn_cap`, `execution_deadline`, and `cancelled` outcomes. Turn-cap results carry configured and
  observed turns; all producer terminations carry the applied execution cap and elapsed time.
- `server-go/delegate` preserves the typed diagnostic across the versioned JSON result, rejects
  malformed or success-attached termination data, and keeps capacity, cancellation, turn-cap, and
  execution-deadline errors distinct.
- `server-go/internal/engine` uses one formatter for single and grouped calls. It combines the
  producer diagnostic with the caller-owned stage wall bound without changing successful seats,
  independent failures, or participant identity.
- Real Claude- and Codex-shaped stub CLIs exceed `MaxTurns` through `RegistryExecutor.Execute`, cross
  the module wire, and block until the watchdog kills and reaps their process trees.
- The producer output buffer no longer embeds `bytes.Buffer`, preventing `os/exec` from bypassing
  its mutex through a promoted `ReadFrom` while stdout and stderr drain concurrently.

### 1. One typed termination contract at the producer

The Go delegate invocation result carries a bounded termination diagnostic containing:

- stable kind: `turn_cap`, `execution_deadline`, or `cancelled`;
- configured maximum turns and observed turns when the turn cap fires;
- producer execution deadline/cap and elapsed execution time;
- a safe, bounded detail string for operator context.

`server-go/modules/delegates` is the sole owner of turn counting and producer termination kinds.
Unknown or malformed termination data fails closed as a generic terminal failure; callers may not
upgrade it to `done` or `partial`. The C bus host transports the versioned result only and contains
no parallel turn-count or termination classifier.

### 2. One caller-side formatter for single and grouped dispatch

Single and grouped dispatch use the same Go helper to combine
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

The stub CLI fixture is selected through a temporary Go agent registry. It emits valid Claude- and
Codex-shaped JSON events beyond a configured `MaxTurns`, then blocks so the watchdog/process-group
kill is observable. Run it through `RegistryExecutor.Execute` and the real delegate wire; do not
construct a terminal result in the test.

The fixture proves that the Go producer counts events, stops at the exact cap, reaps the process
tree, returns `failed` with `turn_cap`, and reports configured/observed turns plus elapsed time. It
must never return the retired C `partial` state.

## Acceptance

Completed on 2026-08-16; all three executable checks below pass, together with the complete
`server-go` suite, focused `go vet`, and race-enabled delegate/engine tests.

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
