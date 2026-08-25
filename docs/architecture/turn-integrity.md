# Turn integrity core

The turn-integrity core provides protocol-neutral, caller-owned contracts for
the lifecycle of an Aimee turn. It owns bounded schemas, state transitions, and
an optional observation hook. It does not own authorization, persistence,
provider transport, or audit storage.

The server installs a bridge from the observation hook to the durable event bus.
Other binaries can use the same state machine without linking server or audit
dependencies.

## Invariants

- A turn identity exists before work is dispatched.
- State transitions are monotonic and terminal states cannot be reopened.
- Configuration, toolset, routing, policy, and context snapshots bind once.
- Events carry bounded metadata only; prompt, tool argument, result, and model
  response content are excluded.
- Mechanical policy remains authoritative. Observability is not an
  authorization boundary.
