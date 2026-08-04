# Descriptor-authoritative builds: residual work

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`module-runtime-source-ownership-and-build-residual.md`](../done/module-runtime-source-ownership-and-build-residual.md)

## Delivered baseline

All current module descriptors pass schema and dependency validation, declare complete ownership, and
leave one explicitly tracked legacy-root exception.

## Remaining deliverables

- Generate Make and CMake membership and dependency edges from the descriptor graph.
- Validate visibility, cycles, ownership, and platform variants from that same authority.
- Reduce the legacy-root exception to zero or a separately approved minimum.
- Prove clean, incremental, disabled-module, and packaging parity across both build systems.
- Reject hand-maintained build membership that diverges from a descriptor.

## Completion evidence

A fixture module must be addable or removable by descriptor changes alone, with identical object and
package contents under Make and CMake and a mechanical failure for an undeclared source.
