# Core substrate and module boundaries: residual work

- **State:** DONE — archived 2026-08-04 after the descriptor/source-ownership and event-bus
  portions landed. The separately buildable process boundary and compatibility closeout remain in
  [`core-process-separation-residual.md`](../pending/core-process-separation-residual.md).

**Archived parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)

## Archive rationale

The module descriptor catalog, ownership-complete source moves, dependency validation, bus wire,
reference clients, and cross-language conformance foundation now exist. This residual is therefore
partly delivered and cannot remain an honest all-pending unit. The undelivered process-isolation and
rollout work has been split into the narrower residual above.

## Remaining deliverables

- Complete the minimal shared core contract and remove module-specific dependencies from it.
- Split runtime roles into separately buildable/testable programs where the architecture requires it.
- Adopt the event-bus contract only after the feature-branch wire and conformance work lands on `testing`.
- Enforce dependency direction and boundary ownership in CI.
- Publish migration and compatibility gates for existing deployments.
