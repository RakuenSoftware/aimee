# Event-bus enforcement and attestation: residual work

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`event-bus-governance-and-capture.md`](../done/event-bus-governance-and-capture.md)

## Delivered baseline

The bus has capture/replay support and concrete audit, guardrail, memory, tool, vault, and sandbox
sinks. Module-call adapters expose an initial governed-event path.

## Remaining deliverables

- Make the core tap the structural observer for every inter-module event and forbid bypass paths.
- Require a synchronous execution-policy verdict before delivery of every action-class event.
- Chain the uniform event shape, verdict, actor/principal, and `policy_rev` into the durable ledger.
- Convert all remaining enforcer sites into declared event kinds with mechanical completeness checks.
- Feed the uniform bus stream into the existing attestation bundle without weakening its trust anchor.

## Completion evidence

Fixtures must fail when an inter-module or enforcer action bypasses the tap, while proving non-action
capture stays within the bus budget and an attestation covers the resulting uniform records.
