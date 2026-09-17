# MR-12: Named served-memory views and inspectable claim cards

- **State:** Proposed
- **Priority:** P1: task-oriented access to existing memory
- **Owner:** Memory API, context assembly and operator interface
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md)
- **Delivery:** Three implementation slices

## Problem and intended result

Ranked search answers “what is similar?” while an agent often needs a briefing of current state, constraints, decisions and unresolved contradictions. Operators also need to see why a claim is believed and correct the underlying record without creating a competing truth store.

Expose named served-memory views and read-only claim cards over existing canonical records. Every view uses the same eligibility, requirement, budget and receipt contracts.

## Proposed API and views

Add `aimee memory serve <view> --task <text> --json`, with authenticated scope inherited from the host. Temporal arguments are explicit and supported consistently. The internal bus/HTTP/MCP adapters call one view service; they do not implement separate SQL/ranking rules.

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

Cache keys include principal/audience, scope, purpose, time mode, input versions, policy/renderer version and revocation generation. Avoid including unstable diagnostics in the model-facing cache prefix. A cache hit still checks whether its release context is current. Missing or incompatible dependencies trigger rebuild or explicit unavailable/partial status.

The briefing packer protects constraints, preserves coherent evidence bundles and avoids duplicate procedure rendering. If contradictions cannot fit with enough explanation, return an explicit gap rather than silently presenting one side as truth.

## Implementation slices

1. Define view schemas and named requirement templates over current typed channels; implement the first four views and JSON parity.
2. Add claim-card projection, evidence expansion and expected-version correction actions using existing mutation gates.
3. Add scoped caches, remaining views and channel-level outcome measurements; keep model text minimal and rich diagnostics separate.

## Acceptance gates

- All adapters produce equivalent authorized view contents under the same policy/input versions.
- A briefing includes required constraints/current corrections before discretionary similar episodes.
- Historical data is never relabeled current, and a contradiction view never leaks a hidden side.
- Correcting a canonical fact invalidates dependent views/cards; stale caches cannot bypass release checks.
- Claim cards show unknown lineage/calibration honestly and cannot be edited into a second authoritative store.
- Final budgets and exact retained evidence agree with the view receipt.

## Rollout and rollback

Add views without changing existing endpoint names. Canary task-start briefing before expanding automatic injection. Rollback can disable automatic use while leaving explicit view access and canonical data intact.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
