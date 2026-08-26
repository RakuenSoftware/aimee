# P2b residual: KB forwarding and true streaming

- **State:** PENDING. Residual scope only.

**Archived parent:** [`tiered-llm-p2-kb-egress-authority.md`](../done/tiered-llm-p2-kb-egress-authority.md)

## Remaining deliverables

- Forward admitted server requests to KB with actor, organization, request, and policy identity intact.
- Stream provider output end to end without whole-response buffering.
- Propagate cancellation, deadlines, backpressure, errors, and audit correlation across every hop.
- Prove no direct-egress bypass and no cross-tenant identity loss.
- Add disconnect, slow-consumer, retry, and partial-stream integration tests.
