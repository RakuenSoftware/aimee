# Config descriptor table: generic save residual

- **State:** PENDING — residual scope only.

**Archived parent:** [`config-field-descriptor-table.md`](../done/config-field-descriptor-table.md)

## Remaining deliverables

- Generate flat-field persistence from the descriptor table.
- Preserve intentional omission/default semantics and secret handling.
- Remove duplicated per-field writers only after byte/semantic compatibility tests pass.
- Add a checker that every eligible flat field has parse, schema, default, and save coverage.

Nested and custom config sections remain explicitly outside this residual unless separately proposed.
