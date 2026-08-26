# Per-query ranking feature persistence

- **State:** PENDING. Residual scope only.

**Archived parent:** [`per-query-grouping-key-for-ranking.md`](../done/per-query-grouping-key-for-ranking.md)

## Remaining deliverables

- Add a durable query/retrieval grouping key to ranking feature rows.
- Backfill or explicitly quarantine legacy ungrouped rows.
- Teach fitting, diagnostics, and retention to operate on grouped query sets.
- Eliminate `missing_grouping_key` for newly admitted data and make violations observable.
- Test concurrent identical queries, retries, and cross-tenant isolation.
