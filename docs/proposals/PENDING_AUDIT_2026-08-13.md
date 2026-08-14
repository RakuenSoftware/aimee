# Pending proposal audit update — 2026-08-13

This incremental reconciliation carries forward the exhaustive 2026-08-04 manifest and records the
delegate and WFE panel residuals changed since that snapshot by subsequent work.

## Closed residual

`delegate-limit-diagnostics-residual.md` moved from `pending/` to `rejected/`. Its partial-result
acceptance test requires driving the delegate agent loop, but the Go delegates module currently has
no provider or agent-loop producer: its plane client only forwards the cap and consumes an HTTP
terminal status. The producer remains in `src/server/server_compute.c` and
`src/server/agent_runtime.c`. A Go client test could therefore only replay a preconstructed result,
while a new C executor test would deepen the ownership violation under active removal. The
requirement is preserved on `delegate-execution-into-the-module.md`, where the producer can be
implemented and proved in Go.

## Subsequent WFE panel completion — 2026-08-14

`wfe-panel-capacity-residual.md` moved from `pending/` to `done/`. The Go delegate module,
roundtable module, and WFE scheduler now preserve capacity wait, capacity-wait deadline, and delegate
execution deadline as distinct retryable states. Deterministic coverage includes saturated pools,
admission races, health invariance, unavailable-local-backend filtering, and ten repeated runs of ten
overlapping roundtables without generic `panel_unreachable`.

The parent `wfe-panel-cannot-seat-under-self-load.md` is therefore `complete` in the dated manifest
with no remaining residual path.

The parent `delegate-budget-must-fit-its-stage-cap.md` remains `partial_archived` in the dated
manifest, with `delegate-execution-into-the-module.md` as its live residual path. All other rows are
carried forward unchanged from the prior exhaustive audit; proposals drafted after that snapshot
remain valid unlisted additions under the manifest checker's dated-snapshot contract.

## Subsequent completion

PR #2645 moved delegate execution, budget exhaustion, and partial-result production into the Go
delegates module. The successor is now archived in `done/`, so the parent manifest row is complete
and no longer carries a pending residual.

## Validation

- `python3 scripts/check-proposal-links.py`
- `python3 scripts/check-proposal-reconcile.py`
- `python3 scripts/check_pending_audit_manifest.py`
