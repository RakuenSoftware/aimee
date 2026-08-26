# KB hybrid outcome wiring: residual work

- **State:** PENDING. Residual scope only.

**Archived parent:** [`kb-hybrid-outcome-wiring.md`](../done/kb-hybrid-outcome-wiring.md)

## Shipped baseline

B1/B2 outcome plumbing, the pairwise objective, IPW weight consumption, per-document overlap, and ranker capture are implemented.

## Remaining deliverables

- Record selection propensities at decision time and bind them to retrieval events.
- Add an explicit evaluation-feedback ingestion path with idempotency and provenance.
- Validate learning uplift, bias controls, and rollback against representative live traffic.
- Publish health diagnostics for missing or invalid propensities and feedback lag.
