# Operator audit activity: unified surface residual

- **State:** PENDING. Residual scope only.

**Archived parent:** [`operator-audit-activity-surface.md`](../done/operator-audit-activity-surface.md)

## Remaining deliverables

- Define `/v1/audit/activity` as the stable grouped union of actions and provenance-bearing operational events.
- Add deterministic pagination, filters, actor/tenant isolation, and retention semantics.
- Expose the surface through the CLI with machine-readable output.
- Make degraded/missing sources explicit instead of returning an apparently complete view.
- Add authorization, ordering, cursor, and provenance tests.
