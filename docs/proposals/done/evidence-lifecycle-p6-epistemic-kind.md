# Proposal: P6: epistemic kind as a dimension separate from authority and tier

> **Archived proposal.** This records the implemented design; current behaviour
> is defined by the code and acceptance validation.

- **State:** done (2026-08-21). Implemented in PR #2831; see
  [acceptance validation](../../validation/evidence-lifecycle-acceptance.md).
- **Series:** [Evidence and lifecycle layer](evidence-lifecycle-layer.md), member 6 of 9.
- **Author:** JBailes
- **Date:** 2026-08-21
- **Charter roles:** Persist, Reason, Enforce.
- **Audited pin:** `1d36f8c1`.

## Problem and boundary

Aimee currently answers "what kind of thing is this memory?" with three columns
that each mean something different and are each used as a proxy for the others:
`memories.tier` (a storage/promotion level), `memories.kind` (a loose label
defaulting to `'fact'`), and `confidence_class` (a provenance-keyed authority
class). None of them says what *epistemic* kind of claim the row makes, and that
is the property that should decide whether the row can be contradicted, whether
it has valid time, whether it expires, and whether correcting it even makes sense.

The concrete failure: "The deploy failed after migration X" and "Migration X is
unsafe" are treated the same way because both mention the same event and arrive
through the same path. The first is an **episode**, an observation of something
that happened, which is immutable except for redaction and can never be "wrong"
in the way a claim can. The second is a **mental model**. A derived explanatory
synthesis, which absolutely can be wrong, should carry its derivation, and should
go stale when its basis moves. Applying the same correction and expiry rules to
both means either episodes get incorrectly retracted, or models never do.

Boundary: adding one orthogonal dimension and making the rules that already exist
key off it. This proposal does not replace Aimee's authority classes, and it does
not change what any existing rule does for `world_fact`.

## §0 What already exists

| Piece | Where | Gap |
|---|---|---|
| `confidence_class` A/B/C, provenance-keyed | `src/modules/db2/c/fact_lifecycle.h` | Authority, correctly. Frequently *read* as if it also said what kind of claim the row makes. |
| `memories.kind` (default `'fact'`), `cognified_memory_kind` | `src/modules/db2/c/schema.sql` | Loose labels with no closed set and no rule attached. |
| `memories.tier` L0/L1/…, `kind_lifecycle` promote/demote/expire thresholds | `src/modules/db2/c/schema.sql` | Lifecycle policy keyed on `kind`: the right mechanism attached to the wrong dimension. |
| `memory_units.memory_kind` (`episodic`), `is_episode_card`, `memory_episodes` | `src/modules/db2/c/schema.sql` | Episodes exist as a *storage* concept; nothing keys correction rules off them. |
| `epistemic_directives`, `prospective_memories`, `rules`, `collab_rules` | `src/modules/db2/c/schema.sql` | Instruction-like and policy-like content already lives in separate tables: evidence that the distinction is real and currently expressed by table choice rather than by a dimension. |
| `rel_types.correction_behavior` (`supersede`, `hard_delete`, `immutable`) | `src/modules/db2/c/schema.sql` | Correction policy per relation; no per-claim-kind policy. |
| `memories.provenance_category` | `src/modules/db2/c/schema.sql` | Who wrote it, not what kind of claim it is. |

## Decision

Add `epistemic_kind`, a closed, CHECK-constrained dimension on assertions and
memories, defaulting to `world_fact`.

| Kind | What it is | Contradictable | Valid time | Correction rule |
|---|---|---|---|---|
| `world_fact` | A claim about the world | yes | yes | supersede per relation policy (today's behaviour, unchanged) |
| `episode` | An observation that an event occurred | **no** | occurrence time only | immutable except redaction; a disputed episode is annotated, never retracted |
| `experience` | An episode plus its outcome | no (the episode); yes (the outcome) | occurrence time | outcome corrigible via P5; episode part immutable |
| `mental_model` | A derived explanatory synthesis | yes | inherited from inputs | goes stale via P4; never promotable to fact without an explicit governance decision |
| `preference` | A person- or group-scoped disposition | yes | yes | supersede, and **only** by the scope owner or higher authority |
| `instruction` | Requested behaviour | not applicable | until revoked | revoked, not contradicted; being "false" is a category error |
| `policy` | Operator-approved normative behaviour | not applicable | until revoked | changed only through governance authority |
| `hypothesis` | Explicitly unresolved speculation | yes | yes | resolves to `world_fact` or is retired; never silently promoted |

### The dimensions stay independent

```
epistemic kind × authority × confidence × scope × lifecycle state × tier
```

Six independent axes. Concretely, and each of these is a legal combination that
the system must represent without contradiction:

- a **user-authored** `hypothesis` with **low** confidence in **project** scope,
high authority, explicitly unresolved;
- a **model-derived** `policy` **candidate** awaiting operator promotion,
policy-kind content that carries no policy force until promoted;
- an **operator-authored** `preference` scoped to one team, maximal authority,
  strictly bounded scope;
- an `episode` at **L0** tier with **A**-class authority, immutable, cheap, and
  authoritative all at once.

No column may be derived from another. In particular: `epistemic_kind` must never
be inferred from `provenance_category`, and authority must never be inferred from
`epistemic_kind`. A `policy` row written by a model is a *proposed* policy with
model authority. That is precisely the combination P7 exists to adjudicate, and
collapsing it would let a model write policy by choosing a label.

### What changes behaviourally

Only three things, each small and each testable:

1. **Correction routing.** `db2_fact_retract` and the memory correction path
   consult `epistemic_kind` *before* the relation's `correction_behavior`. An
   `episode` refuses retraction and offers annotation; an `instruction` routes to
   revocation; everything else behaves exactly as today.
2. **Expiry.** `kind_lifecycle` thresholds are keyed on `epistemic_kind` rather
   than on the free-text `kind`, with migrated rows keeping today's effective
   thresholds so no row's expiry behaviour changes at migration.
3. **Recall labelling.** P9's trace reports the kind, so an answer can say "this
   is a derived model, not an observation": which is the user-visible payoff and
   the reason the distinction is worth storing at all.

Ranking is untouched. `epistemic_kind` is not a score input in this proposal.

## Non-goals

- No replacement of `confidence_class`, `provenance_category`, `tier` or
  `lifecycle_state`. All four remain, with unchanged meanings.
- No automatic classification of existing rows by content inspection. Migration
  is conservative and explicit.
- No new storage tier or promotion path.
- No LLM-based kind inference at write time in this proposal. Producers declare
  the kind; a later proposal may add assisted classification with a review gate.

## Owners and dependencies

- **Owner:** memory / db2.
- **Depends on:** nothing hard. Reaches full value with P4 (mental-model
  staleness) and P7 (promotion as a governance decision).
- **Depended on by:** P7 (decisions differ by kind), P9 (kind is a reported field).

## Threat and failure model

| Failure | Consequence | Mitigation |
|---|---|---|
| A model labels its own output `policy` or `preference` | Behaviour change by self-labelling | Kind carries no authority; a `policy` with model authority has no policy force until an operator promotes it (P7) |
| Kind inferred from provenance | The two dimensions silently collapse | Explicit test that the same content written by user and by model yields identical kind and different authority |
| `episode` used to make claims unfalsifiable | Wrong content becomes uncorrectable | Episodes are observations of *occurrence*; a disputed episode is annotated and can be invalidated as evidence, and the annotation is visible wherever the episode is |
| Migration mislabels history | Retroactive rule changes on existing rows | Everything migrates to `world_fact`, the current behaviour; nothing is reclassified by guesswork |
| Kind proliferation | The dimension stops meaning anything | Closed set with a CHECK; adding a kind requires a proposal amendment and a stated rule for all five columns of the table above |

## Compatibility and migration

- Additive column, default `world_fact`, CHECK-constrained.
- All existing rows migrate to `world_fact`, so every current correction and
  expiry rule applies exactly as it does today.
- `memories.kind` and `cognified_memory_kind` are retained. S4 decides whether
  `kind` becomes a display label over `epistemic_kind` or is deprecated, and
  records that decision rather than leaving two overlapping labels indefinitely.

## Slices

| # | Scope | Done when |
|---|---|---|
| S1 | Column, CHECK, migration, producer-declared kind on the assertion and memory write paths | Every new write declares a kind; every legacy row reads `world_fact` |
| S2 | Correction routing by kind: episode immutability with annotation, instruction revocation | Retracting an episode is refused with an annotation offer; retracting a `world_fact` is unchanged |
| S3 | `kind_lifecycle` keyed on `epistemic_kind`, thresholds migrated to preserve current behaviour | No row's expiry timing changes at migration, proved by a before/after sweep comparison |
| S4 | `hypothesis` resolution path and `mental_model` non-promotability; reconcile `memories.kind` | A mental model cannot become an assertion by any path except an explicit P7 promotion |
| S5 | Kind exposed on recall results and audit surfaces | An answer can distinguish an observation from a derived synthesis |

## Acceptance

```yaml acceptance
- {id: 1, tier: mechanical, check: "epistemic_kind is CHECK-constrained to the eight listed values and defaults to world_fact; an unlisted value is refused"}
- {id: 2, tier: mechanical, check: "identical content written by a user and by a model yields the same epistemic_kind and different authority, proving the dimensions are independent"}
- {id: 3, tier: integration, check: "retracting an episode is refused and offers annotation; retracting a world_fact behaves exactly as at the pin"}
- {id: 4, tier: integration, check: "a policy-kind row written under model authority carries no policy force until an explicit operator promotion"}
- {id: 5, tier: integration, check: "after migration, an expiry sweep produces the identical set of expirations as the pre-migration sweep over the same corpus"}
- {id: 6, tier: integration, check: "a mental_model cannot be converted into an assertion row by any write path other than an explicit governance promotion"}
```

## Status and supersession

Supersedes nothing. Refines the claim taxonomy assumed by
[typed-fact knowledge layer](../done/typed-fact-knowledge-layer.md) and by the
pending [evidence provenance tiers](../pending/proposal-evidence-provenance-tiers.md), which
governs the orthogonal question of how much *human* evidence stands behind a row.
