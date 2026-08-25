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

## Context authority

Canonical request blocks carry metadata independently from their wire content:
origin, authority, trust, sensitivity, model visibility, and an optional
knowledge revision. This keeps retrieved or tool-produced evidence distinct
from task instructions and platform policy even when a provider ultimately
renders both as text.

Authority promotion is explicit and conservative. Model, tool, retrieval, and
memory output cannot become a task instruction or policy. User content can be a
task instruction but cannot become platform policy. Code-owned guidance and
freshness notices are platform instructions; recalled material remains
unverified evidence in its own block.

## Knowledge freshness

Knowledge is versioned by a domain and scope identifier. Live curator
invalidations advance both the affected scope and the aggregate knowledge
epoch. Each session records the aggregate epoch it last observed. When a later
turn observes a newer epoch, the request gains a typed instruction to
re-retrieve affected facts rather than silently relying on an earlier answer.

The in-process epoch table is a bounded cache, not the source of truth. The
durable curator feed remains authoritative across daemon restarts. An unseen
session or scope is reported as `unknown`, never incorrectly asserted current.

## Effect contracts

Every tool dispatch can be represented as a mechanical effect proposal: tool
identity, effect class, target digest, normalized-arguments digest, mode, and
lifecycle state. Target and argument content are hashed at the boundary and are
never retained by the contract or emitted in events.

Shadow mode observes `proposed`, `validated` or `mismatch`, `executing`, and a
terminal outcome without changing authorization. It provides a safe measurement
period for contract coverage and drift before enforcement is enabled. Existing
role, execution-policy, workflow, and guardrail decisions remain authoritative.

Reversible enforcement is deliberately narrower than the write-tool category.
It applies only where the dispatcher has a stable target and a mechanical
postcondition: file creation/replacement, string or anchored file edits, and
symbol edits with an explicit path. A contract mismatch or absent target refuses
execution. A nominally successful mutation is not reported successful until a
read-back check passes; failed verification reports an uncertain local state and
requires inspection before retry. Other writes remain in shadow mode.
