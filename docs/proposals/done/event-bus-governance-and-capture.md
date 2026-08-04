# Proposal: govern and capture the module event bus as one uniform seam

- **State:** DONE — archived 2026-08-04 as partially implemented. Capture/replay and multiple audit,
  guardrail, memory, tool, vault, and sandbox sinks landed; structural all-event capture,
  synchronous action authorization, and attestation integration continue in
  [`event-bus-enforcement-and-attestation-residual.md`](../pending/event-bus-enforcement-and-attestation-residual.md).
- **Historical state:** DRAFT — 2026-07-23; awaiting roundtable review. A later-drafted consuming child; it does
  not inherit any prior approval.
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries-residual.md)
- **Owns:** how governance and audit observe, record, and enforce over the core-owned module event
  bus — the single tap through which every inter-module message is captured, action-class events are
  authorized before delivery, and the stream is fed to the durable ledger and the attestation bundle.
- **Consumes (does not modify):** the shared-memory event bus, the governance/audit tap, and
  invariants 13/17/18 from the modularization suite; the **in-flight** attestable-enforcement work
  ([`governance-attestable-enforcement.md`](governance-attestable-enforcement.md)) — its hash-chained
  WORM ledger, `policy_rev`, `uncovered_enforcers` inventory, and `aimee audit attest` bundle.
- **Implementation dependencies:** the modularization suite's event bus and `execution-policy`/`audit`
  contracts; the attestable-enforcement deltas A1–A5 landing in code.
- **Date:** 2026-07-23

## Why a separate proposal

The attestable-enforcement proposal is **mid-implementation**. It should not be amended to carry a
capability that depends on the not-yet-built module event bus. This proposal is the home for that
addition: it consumes the enforcement ledger and attestation surface that work delivers and wires the
future bus into them, without changing that proposal's contracts, sequencing, or acceptance. If review
finds this proposal needs to change an attestable-enforcement contract, that is a signal to raise it
with that work's owner, not to edit it here.

## Thesis

Attestable enforcement routes each enforcer into the chain one wiring at a time, and its
`uncovered_enforcers` list exists precisely because capture is a per-site effort that can silently
miss a site. The modularization suite removes that problem at the root: once the communication core is
C and every module is a separate program on a core-owned shared-memory event bus, **every
inter-module message is a typed event on that single bus**, and no module-to-module path exists
outside it (suite invariants 12–13). That makes the bus the one place to govern and log the entire
cross-module message stream — capture and enforcement stop being scattered and become a property of
the substrate.

## Decision

The core-owned governance/audit **tap** on the module event bus is the uniform capture and
enforcement seam. Every inter-module event passes through it; the tap records, and for action-class
events authorizes, each event using the enforcement contracts the attestable-enforcement work owns.

- **Capture completeness is structural, not inventoried.** The tap offers every event to `audit`;
  the enforcer sinks the attestable-enforcement A2 inventory names (gateway policy, memory
  interception, integrity gate, native gate, vault, trigger/forge, guard) become event kinds on the
  bus rather than bespoke side logs. An enforcer that acts without publishing a bus event cannot act
  at all, so `uncovered_enforcers` collapses from "sites we remembered to wire" to "declared event
  kind never chained" — a mechanical descriptor check, not a manual sweep.
- **Enforcement is a bus concern, within budget.** An action-class event carries a synchronous
  pre-delivery verdict through `execution-policy` before the bus delivers it; the verdict and its
  `policy_rev` ride the same event record. Non-action, high-frequency events (for example `memory`
  recall) are observed and recorded, not synchronously gated, so completeness stays inside the bus
  performance budget (suite invariant 15) — the same asynchronous, batched recording the
  attestable-enforcement *Risks* note already assumes for the WORM hot path.
- **One record shape.** Each governed bus event chains a row carrying the event kind, the publishing
  and serving module, the actor/principal, the verdict, and `policy_rev`, so `aimee audit attest`
  reports over a uniform stream instead of reconciling several enforcer formats.
- **Sequencing.** This lands after both the event bus and attestable-enforcement A1–A5 exist. It does
  not gate or alter them; until the bus exists, that work's explicit per-site wiring remains the
  capture mechanism, and this proposal is how that wiring stops being manual once the module boundary
  is real.

## Trust boundary (unchanged)

The tap runs inside the audited service's own process (core), so it does not change the host-compromise
guarantee. The out-of-process sealer and off-host anchor from attestable-enforcement A4 remain the
actual trust anchor; this proposal improves capture completeness and uniformity, not resistance to a
compromised host. The tap is trust-kernel infrastructure, not a feature module, and is the single
full-stream observer permitted by suite invariant 18 — modules still see only their authorized slice.

## Non-goals

- Changing the WORM ledger, `policy_rev`, attestation bundle, or any attestable-enforcement contract;
  this proposal consumes them.
- Weakening the host-compromise/trust-anchor guarantee or replacing the A4 anchor pair.
- Governing cross-service (Runtime↔Control Plane) traffic, which travels the network transport and is
  already covered by its own audit; this proposal owns the intra-service module bus.
- Owning organizational governance policy authoring/distribution (optional `governance`) or the
  bus/admission/routing mechanics themselves (the modularization suite).

## Binding checks

```yaml acceptance
- {id: 1, tier: integration, check: "scripts/test_bus_governance_tap.sh --every-inter-module-event-offered-to-tap --no-module-to-module-path-outside-bus --tap-is-sole-full-stream-observer --tap-in-core-process"}
- {id: 2, tier: mechanical, check: "scripts/check_capture_completeness.sh --enforcer-sinks-become-event-kinds gateway-policy,memory-intercept,integrity-gate,native-gate,vault,trigger-forge,guard --uncovered-enforcers-is-declared-kind-never-chained --forbid-enforcer-action-without-bus-event --mechanical-descriptor-check"}
- {id: 3, tier: integration, check: "scripts/test_bus_enforcement.sh --action-class-synchronous-execution-policy-verdict-before-delivery --verdict-and-policy-rev-in-event-record --non-action-observed-recorded-not-gated --within-bus-performance-budget"}
- {id: 4, tier: integration, check: "scripts/test_bus_audit_record.sh --one-row-shape event-kind,publishing-module,serving-module,actor,verdict,policy-rev --feeds-existing-worm-chain --aimee-audit-attest-reports-uniform-stream --consumes-not-modifies-attestable-enforcement"}
- {id: 5, tier: mechanical, check: "scripts/check_proposal_ordering.sh --after event-bus --after governance-attestable-enforcement-a1-a5 --does-not-gate-or-modify docs/proposals/pending/governance-attestable-enforcement.md --trust-anchor-remains-a4-out-of-process"}
```

## Review status

Freshly drafted 2026-07-23. Not reviewed. It adds a consuming governance surface over the module bus
and modifies no parent or in-flight contract; if review finds it does, it must be re-scoped rather
than amending the mid-implementation attestable-enforcement proposal here.
