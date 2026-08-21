# Proposal: P9 — a complete, persisted recall explanation

- **State:** proposed (pending — not started)
- **Series:** [Evidence and lifecycle layer](evidence-lifecycle-layer.md), member 9 of 9.
- **Author:** JBailes
- **Date:** 2026-08-21
- **Charter roles:** Recall, Rank-Fuse, Calibrate, Evaluate-Optimize.
- **Audited pin:** `1d36f8c1`.

## Problem and boundary

Aimee's ranking is richer than most systems it is compared against — multiple
lanes, scope enforcement before ranking, graph traversal, temporal reasoning,
relation gravity, authority classes, outcome-driven demotion. All of it is
opaque. A result arrives as an ordered list with a score, and the score is a
number nobody can decompose after the fact.

Two costs follow. The first is debugging: when a bad memory surfaces, or a good
one does not, there is no way to attribute the failure to a lane, a scope
decision, a score feature, a graph edge, or a gate — so the correction lands on
the *answer*, which teaches the system nothing about the mechanism.

The second cost is evaluation, and it is the larger one. Every outcome label
Aimee collects today (P5, the ranker fitter, demotion) attaches to a final
result. With a persisted trace, a correction can target the specific bad source,
edge, score feature, or policy. That is the difference between a label that says
"this answer was wrong" and one that says "the graph lane over-weighted a
superseded edge".

Boundary: capture and expose the decision trace for a recall. No change to what
recall returns or how it ranks.

## §0 What already exists

| Piece | Where | Gap |
|---|---|---|
| Turn-keyed `retrieval_event` with merged typed surfaced refs | `src/modules/db2/c/demotion.c` | Records *what* was surfaced; nothing about why, or what was rejected. |
| `/v1/audit/trace`, `/v1/audit/provenance` | `src/server/server_state_audit.c`, `src/server/server_http_routes.c` | The read surfaces exist and already resolve refs to sources and versions — the natural home for the trace. |
| `feature_rows` (subject, kind, feature_set_version, features JSONB) | `src/modules/db2/c/schema.sql` | Per-subject features for the fitter; not per-query, and not the applied contributions |
| Hybrid fusion across lanes | `src/kb/kb_fusion.c` | Computes exactly the decomposition this proposal wants to persist, then discards it |
| `memory_recall_shadow_deltas` (bounded top-K rank deltas, capped retention) | `src/modules/db2/c/schema.sql` | An existing precedent for bounded, retention-capped recall diagnostics — the right sizing model to copy |
| Scope query path | `src/modules/db2/c/` memory scope query | Enforces scope before ranking; the decision is not reported |

## Decision

### The trace

For each recalled result, capture and persist:

| Field | Content |
|---|---|
| Candidate source and lane | Which retrieval lane produced it (semantic, keyword, graph, temporal, shortcut) and its rank within that lane |
| Scope decision | Which scope rule admitted it, and for rejected candidates, which excluded them |
| Score contributions | Semantic, keyword, graph, temporal — each as its own value, with the weight applied |
| Traversed graph path | The node and edge sequence, when the graph lane contributed |
| Relation gravity | The gravity term and the relation that produced it |
| Authority class | The candidate's authority and confidence class as read at recall time |
| Valid-time match | Whether and how the query's time context matched the candidate's valid-time bounds |
| Source evidence | Resolvable references to the supporting sources, with their P3 document state |
| Outcome-overlay adjustment | P5's contribution as its own line, never folded into another feature |
| Staleness status | P4's status, and the input that caused it |
| Rejections | Nearby candidates that were filtered or gated, with the filter that rejected each |

The rejection list is the field most systems omit and the one with the highest
debugging value: "why was the right memory not returned" is unanswerable without
it, and it is precisely the question that gets asked.

### Persisted, bounded, and default-off for storage

- The trace is computed cheaply from data the ranker already has in hand. The
  cost is serialization and storage, not recomputation.
- **In-request**: available on demand via a request flag, returned inline. Always
  available, no storage cost.
- **Persisted**: attached to the existing turn-keyed `retrieval_event`, under a
  sampling rate and a retention cap modelled on `memory_recall_shadow_deltas`
  (bounded rows, bounded age). Default sampling is conservative and configurable;
  persistence of the top-K results plus the rejection list only.
- Traces are diagnostic data derived from user content. They carry the same scope
  and sensitivity as the material they describe, are never returned across a
  scope boundary, and are purged when their subjects are purged under P3.

### Attribution for evaluation

The payoff is a correction vocabulary richer than "wrong answer". A P5 outcome
may name the trace element at fault:

```
outcome: corrected
fault:   { kind: lane,    value: graph }       -- the graph lane surfaced it wrongly
fault:   { kind: source,  value: doc:8812 }    -- the source was bad
fault:   { kind: feature, value: temporal }    -- the temporal term was mis-weighted
fault:   { kind: policy,  value: scope_rule_7 }
```

This is what turns a label into a fix. Without it, every negative outcome lands
on the final answer, which is the least actionable place it can land.

### One definition of each reported value

P9 reports values, it does not compute them. Staleness comes from P4, outcome
adjustment from P5, document state from P3, authority from the existing class
column. If a value is unavailable because its member has not landed, the trace
reports `not-computed` for that field. A trace that silently omits a field an
operator expects is worse than one that says it does not know.

## Non-goals

- No change to ranking, fusion, scoring or ordering. A recall with tracing on and
  a recall with tracing off must return identical results in identical order.
- No new ranking features. P9 exposes what exists.
- No replacement of `feature_rows` or the fitter's training view; the trace
  complements them and feeds the pending
  [per-query feature persistence residual](per-query-feature-persistence-residual.md)
  rather than forking it.
- No end-user explanation UI. This is an operator and evaluation surface; a
  user-facing "why did you say that" view is a separate proposal.

## Owners and dependencies

- **Owner:** kb retrieval, with memory.
- **Depends on:** nothing hard — it can land at any point in the series. Fields
  from P3, P4 and P5 are reported as `not-computed` until those land.
- **Depended on by:** nothing. P5 and P7 become more useful with it.

## Threat and failure model

| Failure | Consequence | Mitigation |
|---|---|---|
| Tracing changes ranking | The instrument alters what it measures | Identical-results test with tracing on and off, over a fixed corpus and query set |
| Trace leaks content across a scope boundary | Data exposure through diagnostics | Trace inherits the scope and sensitivity of its subjects; scope test on the read surface |
| Trace survives a purge | Purged content readable from diagnostics | Traces referencing a purged subject are purged in the same P3 changeset |
| Storage growth | Retention pressure | Bounded rows and age, sampling, top-K plus rejections only; growth measured in S4 |
| Latency regression on the hot path | Recall slows for everyone | Serialization is off the critical path for the non-sampled case; a latency budget is asserted in S4 |
| Trace fields drift from the ranker | Explanations become fiction | The trace is emitted by the ranker itself from the values it applied, never reconstructed afterwards by a second implementation |

## Compatibility and migration

- Additive. No existing response shape changes; the trace is an optional field
  and an optional stored attachment.
- Recalls that predate P9 have no trace, reported as absent rather than empty.

## Slices

| # | Scope | Done when |
|---|---|---|
| S1 | In-request trace for lane, rank, scope decision and per-feature contributions | A request flag returns a decomposition whose contributions sum to the final score |
| S2 | Rejection list with the gate that rejected each nearby candidate | "Why was this not returned" is answerable for a seeded corpus |
| S3 | Graph path, relation gravity, valid-time match, authority, source evidence with document state | A graph-lane result shows its traversed path and the edges' authority |
| S4 | Persistence attached to the turn-keyed retrieval event, with sampling, retention cap, scope inheritance and a latency budget | Growth and latency figures published; identical-results test green |
| S5 | Fault attribution vocabulary wired into P5 outcomes | A negative outcome can name a lane, source, feature or policy as the fault |

## Acceptance

```yaml acceptance
- {id: 1, tier: mechanical, check: "recall with tracing enabled returns results identical in content and order to recall with tracing disabled, over a fixed corpus and query set"}
- {id: 2, tier: mechanical, check: "the reported per-feature contributions and applied weights reconstruct the final score exactly for every traced result"}
- {id: 3, tier: integration, check: "for a seeded query where a known-good memory is filtered out, the rejection list names that candidate and the specific gate that rejected it"}
- {id: 4, tier: integration, check: "a trace is never returned across a scope boundary, and traces referencing a purged subject are removed by the same P3 changeset that purges it"}
- {id: 5, tier: integration, check: "persisted traces stay within the configured row and age caps under a sustained recall workload, with published growth and latency figures"}
- {id: 6, tier: integration, check: "an outcome recorded with a fault attribution resolves to the exact trace element named, joining evaluation to mechanism"}
```

## Status and supersession

Supersedes nothing. Extends the audit-trace surface delivered by
[auditable correctness for the KB](../done/auditable-correctness-for-the-kb.md)
from "what was surfaced" to "why", and supplies the per-query decomposition the
pending [per-query feature persistence residual](per-query-feature-persistence-residual.md)
and [kb_hybrid outcome residual](kb-hybrid-outcome-wiring-residual.md) both need.
