# MR-13: Disposable task projections with explicit promotion

- **State:** Proposed
- **Priority:** P2: task continuity
- **Owner:** Task runtime and derived-memory owner
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-02](memory-reliability-02-authority-preserving-mutations.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md), [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md)
- **Delivery:** Three implementation slices

## Problem and intended result

Long-running tasks need a stable working model of constraints, touched components, decisions, hypotheses and unresolved questions. Writing that working model into canonical memory makes temporary assumptions look durable; keeping it only in accumulated prose makes it hard to invalidate or rebuild.

Introduce a task-scoped, versioned, rebuildable projection over existing evidence and execution state. It is non-authoritative by construction and cannot be returned as independent corroboration of its own inputs.

## Projection contract

Use existing task/working-state ownership and dependency registration. A projection contains task/session/principal references, revision, input versions, requirement gaps, relevant component identities, hypothesis status, source-chain references, policy version, audience intersection, creation/expiry time and an explicit non-authoritative class.

Separate verified observations from working hypotheses and planned actions. A tool result is an observation of that particular execution; a planned edit is not evidence that an edit occurred. Projection text must preserve those labels through summaries and budget transformations.

Revision creation is atomic and uses expected-revision compare-and-swap. Parallel delegates propose changes to the host; they cannot overwrite each other's working state or mint authoritative revisions. Each revision identifies the events incorporated and the unresolved merge/conflict state.

## Lifecycle

States are active, stale, blocked, expired and discarded. Rebuild after source/policy changes; block release when required inputs lose authorization. Task switching creates a new projection identity or explicitly authorized fork with inherited dependencies. A fork is not independent evidence.

Expiry removes derived working state according to cache/task retention. It does not delete input memories, erase execution receipts or reverse side effects. Retained context receipts report replay limitations if an expired projection was not retained in replayable form.

Any promotion uses the existing durable-memory proposal/admission path. Promotion identifies exact projection revision, chosen claims, source evidence, proposed scope and the actor authorized to review them. A boolean field `authoritative: false` alone is insufficient: enforce promotion restrictions at the write boundary and database privileges.

## Integration and implementation slices

1. Add typed projection schemas through existing task state and working-context channels. Register all dependencies; bind audience/time/policy and expected revision.
2. Add rebuild/invalidation and bounded model-facing projection via [MR-03](memory-reliability-03-final-payload-context-budgets.md)/[MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md). Return explicit stale/blocked state if rebuild cannot complete.
3. Add promotion preview and review through [MR-02](memory-reliability-02-authority-preserving-mutations.md), task close/expiry cleanup and concurrency/recovery tests. Preserve canonical records on discard.

## Acceptance gates

- A task hypothesis cannot appear as a current canonical fact without an admitted promotion.
- Task A's projection is not visible to task B without explicit scope-compatible transfer; different-user/project access stays denied.
- Parallel revisions either merge under declared rules or conflict; no silent lost update.
- Revoking a parent blocks dependent release even if the projection text has already been cached.
- Summarizing a projection does not upgrade its authority or add independent support.
- Expiry/discard removes only the derived task state and records resulting replay availability accurately.
- Promotion checks source versions, authority and scope at commit time and rejects stale previews.

## Rollout and rollback

Begin with explicit task projection reads; shadow generation before automatic preload. Keep TTL/work limits configurable. Rollback disables generation/serving and safely discards derived state; promoted canonical records remain governed history and are not automatically undone.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
