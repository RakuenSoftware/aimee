# MR-12: Named served-memory views and inspectable claim cards

- **State:** Proposed
- **Priority:** P1: task-oriented access to existing memory
- **Owner:** Go memory API and views, with host/operator interfaces
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md)
- **Delivery:** Three implementation slices

## Problem and intended result

Ranked search answers “what is similar?” while an agent often needs a briefing of current state, constraints, decisions and unresolved contradictions. Operators also need to see why a claim is believed and correct the underlying record without creating a competing truth store.

Expose named served-memory views and read-only claim cards over existing canonical records. Every view uses the same eligibility, requirement, budget and receipt contracts.

## Proposed API and views

Add `aimee memory serve <view> --task <text> --json`, with authenticated scope inherited from the host. Temporal arguments are explicit and supported consistently. Implement the view operation inside the existing Go memory process and memory-data contract. Go memory clients for CLI/bus/HTTP/MCP translate to that operation; they do not implement separate SQL/ranking rules or start a new view service. Publish capabilities for Server and KB placements without routing private data into the shared store to fill an unavailable view.

| View | Required semantics |
|---|---|
| `briefing` | Active constraints, current state, relevant decisions, applicable reviewed procedures and unresolved gaps |
| `active_constraints` | Applicable authoritative constraints with priority/conflicts; never ordinary popularity selection |
| `current_state` | Current applicable assertions with version and contradiction state |
| `recent_decisions` | Relevant decisions and rationale; decision recency is not a truth rule |
| `relevant_context` | General authorized evidence, with source/coverage explanation |
| `known_failures` | Evidence-backed failures/counterexamples and their applicability |
| `reviewed_procedures` | Reviewed content plus evidence-linked applicability and outcome projection |
| `open_contradictions` | Both authorized sides, temporal coordinates and unresolved reason |
| `historical_context` | Evidence applicable to explicit valid/belief time, labeled as historical |

Ship briefing, constraints, current state and contradictions first. Add the remaining views as compositions of the same service. Return view/schema version, selected records, omissions, coverage, freshness and receipt reference; an empty/degraded view explains why.

## Claim card

A card projects claim ID/version, provenance/author/reviewer, valid/belief times, authority, lifecycle, evidence families, independent-support state, contradictions, freshness and governed correction/revalidation operations. Evidence text is expanded only after current access checks. The card digest binds its source versions and projection policy.

Do not persist editable canonical claim text in a separate card table. A cache is replaceable derived state. A “correct” action opens the [MR-02](memory-reliability-02-authority-preserving-mutations.md) transition against an expected source version; subsequent dependent cards become stale under [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md). Confidence remains an explicitly named signal with calibration status.

## Caching and serving

Cache identity binds store/owner namespace, principal/audience, scope, purpose, view/schema version, normalized task/query, requirement revision, explicit valid-at and believed-at coordinates, enabled channels and effective item/byte/token budgets. Bind model/tokenizer and policy/renderer versions when they affect selection or packing, plus selected input versions, revocation generation and the scoped collection watermark. Time mode alone cannot identify two different historical requests. Avoid including unstable diagnostics in the model-facing cache prefix.

Track query dependencies on the searched collection, including empty results. An insert, a newly visible record, or a new constraint/contradiction can change a view without modifying any selected input. Use an owner-maintained scoped collection generation advanced atomically with relevant mutations, or an equivalent query-dependency mechanism. Permission changes and scheduled temporal applicability changes also invalidate or expire affected views. A current-mode cache must expire no later than the next relevant validity or utility-horizon boundary; without a reliable boundary, recompute temporal selection on reuse.

On a cache hit, obtain current owner evidence for collection/revocation state and recheck release eligibility. A lagging replica's unchanged local watermark is insufficient; use [MR-02](memory-reliability-02-authority-preserving-mutations.md)/[MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) consumer progress or check the owner directly. Missing or incompatible dependencies trigger rebuild or explicit unavailable/partial status. Recompute coverage and issue a receipt for this invocation, even when projection bytes are reused.

The briefing packer protects constraints, preserves coherent evidence bundles and avoids duplicate procedure rendering. If contradictions cannot fit with enough explanation, return an explicit gap rather than silently presenting one side as truth.

## Implementation slices

1. Define view schemas and named requirement templates over current typed channels; implement the first four views and JSON parity.
2. Add claim-card projection, evidence expansion and expected-version correction actions using existing mutation gates.
3. Add caches bound to complete request identity, collection dependencies and temporal boundaries, then remaining views and channel-level outcome measurements; keep model text minimal and rich diagnostics separate.

## Acceptance gates

- All adapters produce equivalent authorized view contents under the same policy/input versions.
- A briefing includes required constraints/current corrections before discretionary similar episodes.
- Historical data is never relabeled current, and a contradiction view never leaks a hidden side.
- Correcting a canonical fact invalidates dependent views/cards; stale caches cannot bypass release checks.
- Inserting a constraint after a cached briefing, or a contradiction after an empty contradiction view, forces recomputation even when all prior input versions remain unchanged.
- Different task queries, views, valid/belief times and effective budgets cannot reuse an incompatible cached selection. A clock-only applicability transition refreshes current views.
- An offline or lagging invalidation consumer cannot certify a stale cache as current from its local watermark alone.
- Claim cards show unknown lineage/calibration honestly and cannot be edited into a second authoritative store.
- Final budgets and exact retained evidence agree with the view receipt.

## Rollout and rollback

Add views without changing existing endpoint names. Canary task-start briefing before expanding automatic injection. Rollback can disable automatic use while leaving explicit view access and canonical data intact.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
