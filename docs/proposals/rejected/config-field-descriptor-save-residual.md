# Config descriptor table: generic save residual

- **State:** REJECTED — archived 2026-08-14.

**Archived parent:** [`config-field-descriptor-table.md`](../done/config-field-descriptor-table.md)

## Decision

The residual is rejected under the current implementation policy: pending proposal work must be
implemented in Go or moved to `rejected/`. This work would replace C-owned `config_save` writers
and therefore cannot honestly be implemented in Go without creating a second configuration owner
or crossing the existing config-module boundary.

This is a scope/ownership rejection, not a claim that descriptor-driven persistence is unsound.
The existing descriptor-driven defaults, schema validation, and parse path remain unchanged.

## Rejected deliverables

- Generate flat-field persistence from the descriptor table.
- Preserve intentional omission/default semantics and secret handling.
- Remove duplicated per-field writers only after byte/semantic compatibility tests pass.
- Add a checker that every eligible flat field has parse, schema, default, and save coverage.

Nested and custom config sections remain outside this rejected residual.
