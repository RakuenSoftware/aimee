# Core substrate and module boundaries: residual work

- **State:** PENDING — residual scope only.

**Archived parent:** [`core-substrate-and-source-module-boundaries.md`](../done/core-substrate-and-source-module-boundaries.md)

## Remaining deliverables

- Complete the minimal shared core contract and remove module-specific dependencies from it.
- Split runtime roles into separately buildable/testable programs where the architecture requires it.
- Adopt the event-bus contract only after the feature-branch wire and conformance work lands on `testing`.
- Enforce dependency direction and boundary ownership in CI.
- Publish migration and compatibility gates for existing deployments.
