# Dynamic tool egress: authenticated registration identity

- **State:** PENDING. Residual scope only.

**Archived parent:** [`dynamic-tool-egress-classification.md`](../done/dynamic-tool-egress-classification.md)

## Remaining deliverables

- Bind every dynamically registered tool to an authenticated server/installation identity.
- Derive egress policy from that identity and declared capability, not a mutable name prefix.
- Remove the own-server name-prefix exemption.
- Reject identity changes, collisions, and unauthenticated registrations closed.
- Audit registration identity, resolved egress class, and the policy revision used at invocation.

## Acceptance

Tests cover forged prefixes, reconnects, identity rotation, collisions, and unchanged host-CLI behavior.
