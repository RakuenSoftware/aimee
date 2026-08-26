# Capability-thresholded delegate routing: residual work

- **State:** PENDING. Residual scope only.

**Archived parent:** [`capability-thresholded-delegate-routing.md`](../done/capability-thresholded-delegate-routing.md)

## Shipped baseline

Catalog-backed provider/model identity, models.dev enrichment, pricing metadata, provider-general routing, scope and health inputs, and retirement of the old auto-escalation path are present. See the archived parent proposal for the original design and evidence.

## Remaining deliverables

1. Define and persist a competence axis independent of price/size.
2. Make the learning bandit capability-aware and use context-band cost in selection.
3. Scope health to the concrete provider registration rather than only the provider family.
4. Redesign delegate roles around the capability contract.
5. Add live-provider and full-CI validation for threshold behavior and fallback.

## Acceptance

Tests must prove threshold rejection, registration-scoped health isolation, cost-aware selection, and deterministic fallback when no candidate clears the threshold.
