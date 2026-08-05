# Aimee core capability contract: cross-process residual

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`aimee-core-capability-contract.md`](../done/aimee-core-capability-contract.md)

## Delivered baseline

The required capability taxonomy, descriptors, ownership-complete source layout, stage vocabulary,
event wire, reference clients, and initial module-event adapters are implemented.

## Remaining deliverables

- Run the required Go reference modules as separate programs with no C linking or cgo dependency.
- Dispatch every module-owned stage in the canonical 23-stage MCP and ACP round trips over the bus.
- Enforce exact equality among descriptor stages, generated manifest, runtime registration, and trace.
- Record publisher, server, event kind, trust-kernel verdict, and stable input/output identity per hop.
- Prove adaptive skills and learning affect the round trip while optional modules remain absent.

## Completion evidence

The core-profile fixtures must fail on any in-process module shortcut, undeclared stage, no-op
learning/skills provider, optional link closure, or missing trust-kernel record.
