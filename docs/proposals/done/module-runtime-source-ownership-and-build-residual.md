# Module runtime ownership and build: residual work

- **State:** DONE — archived 2026-08-04 after descriptor completeness, source ownership, and graph
  validation landed. Authoritative build generation and packaging parity remain in
  [`descriptor-authoritative-build-residual.md`](../pending/descriptor-authoritative-build-residual.md).

**Archived parent:** [`module-runtime-source-ownership-and-build.md`](module-runtime-source-ownership-and-build.md)

## Archive rationale

All current module descriptors declare complete ownership and the repository validates descriptor
schema, dependency structure, and the remaining legacy-root exception. The descriptors do not yet
drive both build systems or prove clean/incremental/disabled/package parity, so that distinct work is
carried by the residual above.

## Remaining deliverables

- Make module descriptors the authoritative build/dependency graph.
- Generate build membership and validate cycles, visibility, and platform variants.
- Complete event-contract ownership after the event bus lands on `testing`.
- Reduce the documented legacy-root exception to zero or an explicitly approved minimum.
- Add clean, incremental, disabled-module, and packaging tests.
