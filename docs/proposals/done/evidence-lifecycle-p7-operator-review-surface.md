# Proposal: P7: one operator review surface over every pending knowledge decision

> **Archived proposal.** This records the implemented design; current behaviour
> is defined by the code and acceptance validation.

- **State:** done (2026-08-21). Implemented in PR #2831; see
  [acceptance validation](../../validation/evidence-lifecycle-acceptance.md).
- **Series:** [Evidence and lifecycle layer](evidence-lifecycle-layer.md), member 7 of 9.
- **Author:** JBailes
- **Date:** 2026-08-21
- **Charter roles:** Review, Gate-Promote, Enforce.
- **Audited pin:** `1d36f8c1`.

## Problem and boundary

Aimee already produces the decisions an operator needs to make. It produces them
in six unrelated places, in six shapes, with no shared decision vocabulary and no
shared evidence rendering: `memory_conflicts`, `contradiction_log`,
`curiosity_items`, `epistemic_directives`, `learning_proposals`,
`entity_name_conflicts`, `ontology_evaluations` awaiting a verdict, and
`docs.review_needed`. An operator who wants to know "what needs my judgement
today" has to visit each of them and assemble the context by hand.

Worse, the decisions that *are* available are under-informed. Approving a
proposed change today does not show what evidence stands behind it, what the
current value is, whose authority each side carries, what depends on it, or what
would leave recall if the change were applied. So the decision is either made
blind or not made at all, and the queues grow.

The second problem is what "promotion" means. Today, several paths promote
content on the basis of repeated model agreement crossing a threshold. Repeated
agreement among model writes is not evidence of correctness; it is evidence of
consistency, including consistent error. Promotion must be an explicit governance
decision.

Boundary: a read surface and a decision API over existing queues. P7 creates no
new detection mechanism and no new queue.

## §0 What already exists

| Queue | Where | What it lacks for a decision |
|---|---|---|
| `memory_conflicts` (a, b, resolved, resolution) | `src/modules/db2/c/schema.sql` | Evidence, authority comparison, blast radius |
| `contradiction_log` | `src/modules/db2/c/schema.sql` | Same |
| `curiosity_items` (gap, importance, novelty, routing score) | `src/modules/db2/c/schema.sql` | A decision vocabulary: it is a question queue with no answer surface |
| `epistemic_directives` (open questions with cause and evidence) | `src/modules/db2/c/schema.sql` | Resolution is a memory id, with no operator action path |
| `learning_proposals` (sink, state, action_json, corroboration_count) | `src/modules/db2/c/schema.sql` | The closest to a review item; `corroboration_count` is exactly the repeated-agreement promotion this proposal argues against |
| `entity_name_conflicts` (open/resolved/failed) | `src/modules/db2/c/entity_registry.c` | Operator adjudication path |
| `ontology_evaluations` (pending → approved/mapped/rejected) | `src/modules/db2/c/ontology_evolution.c` | Decided by the model; no operator review step |
| `docs.review_needed`, `review_reason` | `src/modules/db2/c/kb_docs.c` | Renders as a list, with no consequence preview |
| `memory_promotion_approvals` (memory_id, target_tier, approver, note) | `src/modules/db2/c/schema.sql` | The right shape for a governance record: used by one path only |
| `frontend/src/console/pages/TypedFacts.tsx` | frontend | An operator console exists to host this |

## Decision

### One review item

A read-only projection, **not** a new queue table, over every source above:

```
review_item
  item_id              -- stable, derived from (source_queue, source_id)
  source_queue         -- conflict | contradiction | curiosity | directive |
                          learning_proposal | entity_conflict | ontology | document |
                          fact_candidate
  subject_kind, subject_id
  proposed_change      -- envelope-level, in P2's diff shape
  current_value        -- the incumbent, in the same shape
  authority_comparison -- proposer vs incumbent, both authenticated identities
  evidence             -- complete list from P1 events and evidence links
  source_spans         -- resolvable, with document state from P3
  valid_time_effect    -- what the change does to valid-time bounds
  dependent_memories   -- P4 reverse dependencies
  recall_blast_radius  -- what enters and leaves recall if applied
  epistemic_kind       -- P6; decides which decisions are offered
  outcome_summary      -- P5 projection for the subject
  suggested_operation
  priority
```

Two properties make this a decision surface rather than a report: every field is
resolved *before* the operator is asked, and any field that cannot be computed is
returned as an explicit `not-computed` marker rather than as an empty value. An
empty evidence list and an uncomputed evidence list must never look alike.

### The decision set

```
accept   correct   retire   restore   promote   invalidate_source   purge   request_evidence
```

- `accept`: apply the proposed change as a P2 changeset.
- `correct`: apply a different value the operator supplies, at operator authority.
- `retire` / `restore`, lifecycle transitions, both reversible.
- `promote`: an explicit governance decision, below.
- `invalidate_source` / `purge`. P3 document operations, with P3's mandatory
  preview and, for purge, its preview token.
- `request_evidence`: defer, and record *what* would settle it. The queue item
  stays open with a stated evidential requirement instead of being silently
  re-queued forever.

Every decision is authority-checked, runs as one P2 changeset, emits P1 events,
and is therefore revertible by the same mechanism as any other change. There is
no operator action in P7 that escapes the ledger.

Which decisions are offered depends on `epistemic_kind` (P6): an `episode` offers
annotation rather than `correct`; an `instruction` offers revocation rather than
`retire`; a `hypothesis` offers resolution.

### Promotion is a governance decision

> Promotion means an explicit governance decision, not higher confidence after
> repeated model agreement.

Concretely:

1. No path promotes content to a higher authority class on a corroboration count.
   Counts may *raise priority in this queue*; they may not change authority.
2. `promote` records the deciding principal, the evidence set as it stood, and
   the resulting authority, extending the `memory_promotion_approvals` shape to
   every promotable object kind.
3. Promotion of a `mental_model` into an assertion is possible only here, and
   only with the derivation and its inputs shown (P4).
4. Existing count-based promotion paths, including the Class-B durability sweep
   and `learning_proposals` corroboration, are audited in S1 and either
   reclassified as *priority* signals or explicitly retained with a written
   justification in the PR. Silence about them is not an acceptable outcome.

### Presentation

One list, one detail view, one decision bar, ordered by a priority that combines
blast radius, authority conflict, and age. The queues keep their own storage and
their own producers; P7 is a projection with a decision API. If a queue's rows
stop being produced, its items simply stop appearing. The surface has no
independent state to go stale.

## Non-goals

- No new detection, conflict-finding, or curiosity generation.
- No new queue table and no migration of existing queues into one store.
- No auto-decision, auto-accept, or bulk-apply-by-filter in this proposal. Bulk
  operations over a *previewed* selection are a later slice, after the decision
  path has operated in production.
- No end-user-facing review. This is an operator surface.

## Owners and dependencies

- **Owner:** governance / console, with memory.
- **Depends on:** P1 (evidence, actor), P2 (decisions are changesets), P3
  (document decisions and preview), P4 (dependents), P5 (outcome summary), P6
  (which decisions are offered). P7 is deliberately last among the foundations
  because a review surface built before them shows an operator a decision they
  cannot evaluate.
- **Depended on by:** P8 (ontology package review reuses this surface).

## Threat and failure model

| Failure | Consequence | Mitigation |
|---|---|---|
| Operator approves blind | Rubber-stamped governance | Every decision requires resolved evidence and blast radius; uncomputed fields are explicit and block destructive decisions |
| Model agreement laundered as promotion | Authority inflation | Counts affect priority only; promotion records a deciding principal |
| Decision surface itself unaudited | The reviewer escapes review | Every decision is a changeset with P1 events and is revertible |
| Queue fatigue | Real items buried under noise | Priority by blast radius and authority conflict; `request_evidence` removes an item from the active list with a stated condition rather than deferring it invisibly |
| Stale item applied after the world moved | Wrong change applied confidently | Items carry the changeset head they were computed against; a decision on a stale item is refused and recomputed |
| Privilege escalation through the console | Operator actions taken by a lesser principal | Authority derived from authentication exactly as in P1; route ACL for every decision endpoint |

## Compatibility and migration

- Purely additive read surface. Existing queue consumers and their endpoints
  continue to work unchanged.
- No queue is migrated or emptied. An item decided through P7 is marked resolved
  in its own source queue by that queue's existing mechanism.

## Slices

| # | Scope | Done when |
|---|---|---|
| S1 | The projection over three queues (conflicts, fact candidates, learning proposals); audit of every count-based promotion path | The audit's findings are recorded in the PR, and no count-based path changes authority |
| S2 | Decision API for `accept`, `correct`, `retire`, `restore`, each as a changeset with authority checks | Each decision is revertible through P2 and appears in the ledger |
| S3 | Evidence, source spans, dependents and blast radius resolved into the item, with explicit `not-computed` markers | A destructive decision is refused while any required field is uncomputed |
| S4 | `promote` with a recorded governance decision; `invalidate_source` and `purge` with P3's preview and token | Promotion records the principal and evidence set; purge cannot be reached without a fresh preview |
| S5 | Remaining queues, `request_evidence` with stated conditions, priority ordering, console view | One list answers "what needs my judgement", ordered by consequence |

## Acceptance

```yaml acceptance
- {id: 1, tier: mechanical, check: "no production path raises an authority class on a corroboration count; counts affect queue priority only (audit test over the promotion paths enumerated in S1)"}
- {id: 2, tier: integration, check: "every P7 decision produces a changeset and ledger events, and is revertible by the same mechanism as any other change"}
- {id: 3, tier: integration, check: "a destructive decision is refused while evidence, dependents or blast radius is not-computed, and the marker is distinguishable from an empty result"}
- {id: 4, tier: integration, check: "promote records the deciding principal, the evidence set as it stood, and the resulting authority, for each promotable object kind"}
- {id: 5, tier: integration, check: "a decision submitted against a stale item is refused and the item is recomputed rather than applied"}
- {id: 6, tier: integration, check: "the offered decision set differs by epistemic kind: an episode offers annotation, an instruction offers revocation, a hypothesis offers resolution"}
- {id: 7, tier: integration, check: "every decision endpoint enforces operator authority derived from authentication, verified by a route ACL test"}
```

## Status and supersession

Supersedes nothing. Consolidates the review surfaces assumed by
[graph feedback, self-audit and learning](../done/graph-feedback-self-audit-and-learning.md),
[memory auto-population phase 4](../pending/memory-auto-population-phase4.md) and
[governance decision records and action audit](../done/governance-decision-records-and-action-audit.md)
into one operator decision path, and provides the surface the pending
[operator audit activity residual](../pending/operator-audit-activity-residual.md) reads from.
