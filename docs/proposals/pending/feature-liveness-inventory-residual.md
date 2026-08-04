# Suite-wide feature-liveness inventory residual

- **State:** PENDING — residual scope only.
- **Archived parent:**
  [`feature-liveness-and-background-curator-removal.md`](../done/feature-liveness-and-background-curator-removal.md).

## Delivered foundation

The background skill curator is deleted, its touch-set disposition is authoritative, and
`check_background_skill_curator_absence.py` runs in the default lint lane. Foreground and
operator-triggered skill behavior remains intact.

## Remaining work

- Inventory every production feature cluster across source, registrations, config, routes, data,
  jobs, metrics, documentation, and tests.
- Exclude self-registration, self-scheduling, config exposure, and test-only edges from liveness
  evidence.
- Assign exactly one disposition to every cluster: retain/move, consolidate, replace-then-delete,
  delete, or time-bounded unknown.
- Record the supported journey or non-self production consumer for retained clusters, and touch set,
  compatibility impact, rollback owner, reviewer, and expiry data where the disposition requires it.
- Add an executable completeness/expiry check to the protected lane.

## Acceptance

The committed ledger covers every mechanically discovered cluster with no unclassified or expired
entry, and a fresh discovery run cannot add a cluster without failing until its disposition is
reviewed.

