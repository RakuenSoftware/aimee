# Core process separation: residual work

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`core-substrate-and-source-module-boundaries-residual.md`](../done/core-substrate-and-source-module-boundaries-residual.md)

## Delivered baseline

The canonical descriptor catalog, complete source ownership, dependency validation, shared-memory
event-bus wire, C host/client, Go client, and conformance fixtures are present on `testing`.

## Remaining deliverables

- Reduce the communication core to its declared substrate without feature-module dependencies.
- Build and test architecture-required runtime roles as separate programs connected only by the bus.
- Prove that memory-first migration stays within the recorded crossing-performance budget.
- Publish and exercise compatibility, upgrade, rollback, and deployment gates for existing installs.
- Reject undeclared in-process cross-module paths mechanically.

## Completion evidence

Completion requires separate-program build artifacts and an end-to-end trace showing every migrated
module hop crossing the authorized bus, plus upgrade and rollback fixtures from the supported prior
deployment shape.
