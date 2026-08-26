# MCP adapter: general bus routing residual

- **State:** PENDING. Residual scope only.

**Archived parent:** [`mcp-adapter-optional-module.md`](../done/mcp-adapter-optional-module.md)

## Remaining deliverables

- Host MCP invocation on the general event bus with correlation, cancellation, deadline, and bounded payload semantics.
- Preserve authenticated tool identity, collision handling, audit records, and egress policy across the bus boundary.
- Define retry/idempotency behavior and disconnect recovery.
- Prove parity with the existing server/KB federation and invocation path before switching defaults.
