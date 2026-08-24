# Proposal: P5 — a work-outcome overlay that never mutates structural truth

> **Archived proposal.** This records the implemented design; current behaviour
> is defined by the code and acceptance validation.

- **State:** done (2026-08-21) — implemented in PR #2831; see
  [acceptance validation](../../validation/evidence-lifecycle-acceptance.md).
- **Series:** [Evidence and lifecycle layer](evidence-lifecycle-layer.md), member 5 of 9.
- **Author:** JBailes
- **Date:** 2026-08-21
- **Charter roles:** Calibrate, Evaluate-Optimize, Review.
- **Audited pin:** `1d36f8c1`.

## Problem and boundary

Aimee already collects attributed retrieval outcomes and already consumes them —
for demotion, and as labels for the ranker fitter. What it does not have is an
*inspectable projection* of that experience: a way to ask "has this memory
actually helped anyone do work, and under what conditions did it fail?" and get
an answer that is separate from, and cannot contaminate, the memory's factual
standing.

The distinction is the point. A memory can be:

- authoritative but unhelpful for a particular task,
- model-derived but repeatedly useful,
- factually correct but stale relative to new code,
- contested, because different workflows produce different outcomes from it.

None of those four states is expressible today, because usefulness is consumed as
a scalar adjustment inside the demotion path rather than stored as a first-class,
queryable dimension. And the moment usefulness is stored *on* the memory row, it
starts competing with authority for the same column, which is exactly the
collapse this series exists to prevent.

Boundary: recording outcomes and projecting them into a status. No change to
assertion authority, confidence class, lifecycle state, or ranking behaviour
while the overlay is in its default read-only mode.

## §0 What already exists

| Piece | Where | Gap |
|---|---|---|
| `retrieval_event` artifacts, turn-keyed, with merged typed surfaced refs | `src/modules/db2/c/demotion.c` | The event exists; the outcome vocabulary is demotion-shaped, not work-shaped. |
| `retrieval_attribution` verdicts (`accepted`, `corrected`, `contradicted`, `rolled_back`, `irrelevant`) | `src/modules/db2/c/demotion.h` | Five verdicts about *the recall*, not about the *work*; no correction payload, no evaluator identity, no code generation at evaluation time. |
| `demotion_profile` artifacts fitted per (memory_class, scope) | `src/modules/db2/c/demotion.c` | Consumes outcomes; produces a score, not an inspectable status. |
| `agent_outcomes`, `lessons_outcome_ledger`, `lessons_outcome_citations` | `src/modules/db2/c/schema.sql` | Outcome records for agent runs and lessons; not joined to retrieved memory. |
| `feature_rows` + the ranker fitter loop | `src/modules/db2/c/schema.sql`, `src/kb/` | Learning consumes outcomes already — P5 must feed it, not fork it. |
| `files.hash`, `code_projection_generations` | `src/modules/db2/c/schema.sql` | Precise code-staleness inputs, better than a whole-file heuristic. |
| `/v1/audit/trace`, `/v1/audit/provenance` | `src/server/server_state_audit.c` | Read surfaces that already expose a turn's surfaced refs — the natural place to attach outcomes. |

## Decision

### One outcome record

Extend the existing evidence path — do not create an unrelated subsystem — with
a work-outcome record carrying the fields the current verdict lacks:

```
work_outcome
  outcome_id
  retrieval_event_id        -- joins the existing turn-keyed event
  query_or_task             -- fingerprint plus a short task label
  subject_kind              -- memory | assertion | document | code_unit | derived
  subject_id
  resulting_action          -- answer id, changeset id, or tool action reference
  authenticated_evaluator   -- verified principal; never caller-supplied
  outcome                   -- useful | dead_end | corrected | misleading | outdated
  correction_ref            -- the correcting memory/assertion, when supplied
  source_hash_at_eval
  code_generation_at_eval
  occurred_at
```

Five outcomes, closed set. They are about the *work*, not about the ranking:

| Outcome | Meaning |
|---|---|
| `useful` | The retrieved material contributed to a successful action. |
| `dead_end` | It was pursued and led nowhere. Distinct from irrelevant: it cost time. |
| `corrected` | It was used, then found wrong, and a correction was supplied. |
| `misleading` | It was plausible and wrong, and the error propagated before detection. |
| `outdated` | It was true once and no longer matches the world or the code. |

`dead_end` and `misleading` are separated deliberately: the first is a retrieval
cost, the second is a knowledge defect, and averaging them into one "bad" signal
is what makes a demotion score uninterpretable.

### The projection

A derived, materialized status per subject, recomputed from outcomes:

| Status | Rule (per scope, per subject) |
|---|---|
| `preferred` | Repeated `useful` outcomes across distinct evaluators or tasks, no recent negatives. |
| `tentative` | Too few outcomes to distinguish, or a mixed record within one workflow. |
| `contested` | Outcomes diverge systematically **by workflow or scope**, not merely by count. |
| `stale` | The subject's `source_hash` or code generation has moved since the outcomes that support it. |

`contested` is a genuine, useful state and must not be collapsed into a low
score: it means "different workflows legitimately disagree", which is a prompt
for a scoped memory or a review, not for demotion.

### The constraint that defines this proposal

> The overlay must not rewrite assertion authority or factual truth.

Enforced structurally, not by convention:

1. Outcome and projection rows live in their own tables; the writer has no code
   path that updates `entity_edges`, `memories.confidence`, any
   `confidence_class`, or any lifecycle column.
2. A negative test asserts that a full outcome workload leaves every authority,
   class and lifecycle column byte-identical.
3. Recall consumes the projection as a **separate, named, weighted signal** with
   its own default of zero. Turning the overlay on is a ranking configuration
   change, visible in P9's per-result trace as its own contribution line, never
   folded into an existing feature's value.

### Code staleness, done precisely

The useful rule to borrow is: if the supporting file hash or code generation
changed since a recommendation was learned, mark it `re-verify`. Aimee can do
this better than a whole-file heuristic, because it already records per-file
hashes and projection generations, and P4 records which inputs a derived item
actually used. So staleness is evaluated against *the specific inputs the item
was derived from*, not against the file's mtime and not against the project as a
whole.

The `stale` projection status therefore reuses P4's dependency mechanism rather
than adding a second, parallel staleness notion. There is exactly one definition
of stale in this series.

## Non-goals

- No replacement of `retrieval_attribution` or the demotion path. P5 supplies a
  richer outcome record that the existing consumers can read; the demotion scorer
  keeps working unchanged until a separate proposal retires it.
- No new fitter, objective, or ranking model. P5 produces signal; the fitter
  described in [learning-to-rank weight fitting](../done/learning-to-rank-weight-fitting.md)
  consumes it under its existing benchmark gate.
- No automatic promotion or demotion of authority from outcomes, ever.
- No user-facing rating UI. Evaluators are authenticated agents, harnesses and
  operators.

## Owners and dependencies

- **Owner:** memory / learning.
- **Depends on:** P1 (evaluator identity and events), P4 (one definition of
  stale). Usable without P2.
- **Depended on by:** P7 (review shows the outcome record), P9 (the trace shows
  the overlay's contribution).
- **Related pending work:** [kb_hybrid outcome residual](../pending/kb-hybrid-outcome-wiring-residual.md)
  (propensity logging, explicit evaluation feedback) and
  [per-query feature persistence residual](../pending/per-query-feature-persistence-residual.md)
  (grouping key). P5 must not duplicate either; it consumes their grouping key
  and contributes its outcomes to their loop.

## Threat and failure model

| Failure | Consequence | Mitigation |
|---|---|---|
| Overlay used to launder a model claim into truth | The series' central rule broken | No write path to authority columns; negative test; separate tables |
| Self-reinforcing loop: useful because surfaced, surfaced because useful | Popularity replaces correctness | Propensity weighting from the outcome residual; `preferred` requires distinct evaluators or tasks, not repeated hits |
| Positively-biased weak labels | Fitter learns nothing, or learns position bias | Five-way outcome vocabulary with explicit negatives; the fitter's existing benchmark gate remains the backstop |
| One workflow's outcomes suppress another's | Cross-tenant or cross-workflow contamination | Projection computed per scope; divergence produces `contested`, never a single global verdict |
| Evaluator identity spoofed | Fabricated usefulness | Authenticated evaluator only, resolved as P1 resolves actors |
| Outcome volume swamping the artifact store | Storage pressure | One row per (event, subject); volume measured in S4 with a proposed retention window |

## Compatibility and migration

- Additive. Existing `retrieval_attribution` rows are readable as `useful` /
  `corrected` equivalents for reporting, but are **not** rewritten and are marked
  as converted so no projection claims fidelity it does not have.
- Default state: outcomes recorded, projection computed, ranking weight zero.
  Enabling the ranking contribution is a separate, explicit configuration step
  with its own before/after evaluation.

## Slices

| # | Scope | Done when |
|---|---|---|
| S1 | Outcome record, closed vocabulary, authenticated evaluator, write API | An outcome cannot be written with a caller-supplied evaluator or an unknown outcome value |
| S2 | Projection table and recomputation, including `contested` by workflow divergence | A seeded divergent workload yields `contested`, not an averaged score |
| S3 | `stale` via P4 dependencies, keyed on recorded source hash and code generation | Rewriting one file marks only the recommendations derived from it stale |
| S4 | Read surfaces: per-subject outcome history on `/v1/audit/provenance`, CLI export, volume measurement | An operator can see every outcome for a memory with evaluator, task and correction |
| S5 | Optional ranking contribution as a separate named signal, default weight zero | Enabling it changes ranking only through its own contribution line in P9's trace |

## Acceptance

```yaml acceptance
- {id: 1, tier: mechanical, check: "a full outcome workload leaves every authority, confidence-class and lifecycle column byte-identical (negative test asserting the overlay cannot write structural truth)"}
- {id: 2, tier: mechanical, check: "outcome writes reject a caller-supplied evaluator and an outcome value outside the closed five-value set"}
- {id: 3, tier: integration, check: "systematically divergent outcomes across two workflows produce contested for the shared subject and preferred within each workflow's own scope"}
- {id: 4, tier: integration, check: "a subject whose recorded source hash or code generation has moved projects as stale using the same definition of stale as P4, with no second implementation"}
- {id: 5, tier: integration, check: "with the ranking contribution at its default weight of zero, recall ordering is identical before and after an outcome workload"}
- {id: 6, tier: integration, check: "outcomes reach the existing ranker training view through the established grouping key, without a parallel training path"}
```

## Status and supersession

Supersedes nothing. Extends
[kb_hybrid outcome wiring](../done/kb-hybrid-outcome-wiring.md) and the demotion
machinery recorded in
[graph feedback, self-audit and learning](../done/graph-feedback-self-audit-and-learning.md)
into an inspectable, source-linked projection.
