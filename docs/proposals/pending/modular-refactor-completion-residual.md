# Modular refactor completion and compatibility: residual work

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`large-refactor-delivery-and-compatibility.md`](../done/large-refactor-delivery-and-compatibility.md)

## Delivered baseline

The 62-slice source modularization, module inventories, ownership descriptors, dependency checks,
cleanup/disposition records, and event-bus foundation are merged.

## Remaining deliverables

- Complete the separate-process migration rather than only relocating sources into module folders.
- Make `core`, `runtime`, `control`, and `full` profiles build with Make/CMake object parity.
- Prove every optional module's full absence across code, routes, assets, metrics, jobs, and data.
- Implement and test forward database compatibility and fresh-database recovery from supported images.
- Close compatibility aliases, cleanup ledgers, rollback gates, and package migration records.

## Completion evidence

The suite is complete only when all profiles and full-minus-one fixtures pass, a supported prior
deployment upgrades without data loss, and the recovery procedure restores into a fresh database.
