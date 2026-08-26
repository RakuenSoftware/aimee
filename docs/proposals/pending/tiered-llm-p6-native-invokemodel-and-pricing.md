# P6 residual: native InvokeModel families and pricing

- **State:** PENDING. Residual scope only.

**Archived parent:** [`tiered-llm-p6-bedrock-and-breadth.md`](../done/tiered-llm-p6-bedrock-and-breadth.md)

## Remaining deliverables

- Implement native InvokeModel request/response adapters for the supported non-Converse model families.
- Normalize streaming, tool use, usage, finish reasons, and errors into the canonical IR.
- Add authoritative, refreshable pricing metadata and unknown-price fail-safe behavior.
- Add recorded fixtures plus live smoke coverage for each family.
- Publish the support matrix and remove any family claim that lacks parity evidence.
