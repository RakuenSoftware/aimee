# WFE panel capacity and deadline residual

- **State:** PENDING — residual scope only.
- **Archived parent:**
  [`wfe-panel-cannot-seat-under-self-load.md`](../done/wfe-panel-cannot-seat-under-self-load.md).

## Delivered foundation

Group routing accounts for reported occupancy, prefers an eligible free agent, keeps old services
without occupancy reporting routable, and retries route-selected admission races as capacity
backpressure.

## Remaining work

- Give an all-saturated eligible pool a typed waiting/capacity state that remains distinct from
  provider failure while preserving retryability.
- Report deadline expiry while waiting for capacity distinctly from delegate execution timeout.
- Prove concurrency-limit responses never change provider health classification.
- Exclude an unreachable local backend using authoritative health while retaining healthy fallbacks.
- Run a repeated live concurrency campaign rather than treating one green build as evidence.

## Acceptance

Deterministic tests cover the saturated pool, concurrent admission race, capacity wait deadline,
health invariance, and unreachable backend. At least ten build runs with overlapping roundtables have
no generic `panel_unreachable`; any unfillable panel names capacity and deadline state precisely.

