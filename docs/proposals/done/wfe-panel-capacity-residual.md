# WFE panel capacity and deadline residual

- **State:** DONE — implemented in Go and archived 2026-08-14.
- **Archived parent:**
  [`wfe-panel-cannot-seat-under-self-load.md`](../done/wfe-panel-cannot-seat-under-self-load.md).

## Delivered foundation

Group routing accounts for reported occupancy, prefers an eligible free agent, keeps old services
without occupancy reporting routable, and retries route-selected admission races as capacity
backpressure.

## Delivered residual

- The Go roundtable result now reports `panel_capacity` when eligible seats are saturated,
  `panel_capacity_deadline` when the retry window expires after capacity backpressure, and
  `panel_deadline` when delegate execution itself reaches its deadline. `panel_unreachable` is
  retained only for non-capacity/non-deadline reachability failures.
- All three new states are scheduler-owned transient pauses. Resumption creates a new execution
  version, preserving retryability and durable delegate replay rules.
- The top-level Go delegate contract carries stable capacity slugs across the module bus. The Go
  `RegistryExecutor` returns `ErrDelegateCapacity` for saturated group plans and
  `ErrDelegateCapacityDeadline` when a dispatched delegate waits for a limiter slot until its
  context expires. Both the standalone roundtable module and the WFE adapter map the latter before
  generic `context.DeadlineExceeded`.
- Capacity remains a load signal in the Go producer. Its capacity branches do not mutate provider
  health, and tests prove capacity errors remain retryable rather than becoming
  `delegate_terminal`.
- The Go producer consumes the registry's authoritative `delegate_available` projection. A
  focused test excludes an unavailable `local-gemma4` while retaining a healthy remote fallback.
- A deterministic campaign starts ten roundtables at one barrier and proves every saturated result
  is `panel_capacity`, never generic `panel_unreachable`. The campaign was run ten times (100
  overlapping roundtables total) in addition to the package and repository suites.

## Acceptance evidence

- `go test ./delegate ./modules/delegates ./modules/roundtable/... ./internal/engine`
- `go test ./modules/roundtable/panel -run TestTenOverlappingCapacityCampaignsNeverReportPanelUnreachable -count=10`
- `go test ./...`
- `go test -race ./delegate ./modules/delegates ./modules/roundtable/... ./internal/engine`
- `cd ../src && make -s lint`

The PR CI run is the independent build validation. The repeated deterministic campaign is the
load-dependent regression gate: one lucky provider-backed build is supporting evidence, not the
proof of correct classification.
