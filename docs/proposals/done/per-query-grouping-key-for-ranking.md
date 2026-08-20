# Per-query grouping key for supervised ranking

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE — delivered scope archived 2026-07-26.

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. Remaining work is tracked in
> [`per-query-feature-persistence-residual.md`](../pending/per-query-feature-persistence-residual.md).

*Filed as a precondition record for
[surface-neutral-retrieval-substrate.md](surface-neutral-retrieval-substrate.md).
Classification: **enhancement, blocking learned ranking on every surface**.*

## Problem

Supervised ranking needs training rows of the form
`(query, candidate, features, outcome)`. The feature store cannot express them.

`src/kb/kb_ranker_fit.c:258-275` states this in-tree, as runtime diagnostics
rather than commentary:

- `missing_grouping_key` — "`feature_rows` has no retrieval_event_id/query
  column (PK is subject_id,subject_kind,feature_set_version; per-candidate
  upsert), so per-(query,candidate) training rows do not exist."
- `subject_space_mismatch` — "ranker features are written on
  `feature_rows.subject_kind='kb_document'` (the kb_hybrid code-search path);
  retrieval outcomes are attributed to 'memory' row ids on the memory-recall
  surface — disjoint id spaces."

Two independent defects. The first is structural: the primary key
`(subject_id, subject_kind, feature_set_version)` with a per-candidate upsert
means a candidate has exactly one feature row across all queries — the second
query overwrites the first's features. The second is a naming problem: outcomes
and features are keyed in different id spaces, so the join is empty.

## Consequences to be honest about

This blocks learned ranking on **every** surface, not just any new one. It is
not a limitation introduced by adding consumers; it is a pre-existing gap that
adding consumers makes visible. Anything describing the ranking stack as
"calibrated and reusable" is overstating what the storage can currently support.

What remains achievable without this: offline tuning of a small number of scalar
parameters against a labeled fixture corpus — benchmark-driven hyperparameter
search, which needs no per-query persistence because the corpus supplies the
grouping.

## Direction

1. Add a grouping dimension — a retrieval-event id, or an explicit query hash —
   to the feature store, and include it in the primary key so features become
   per-`(query, candidate)` rather than per-candidate.
2. Unify the subject id space, or record the surface alongside the subject so
   features and outcomes join on a compound key rather than a bare row id. This
   is the same problem an opaque, surface-qualified subject key solves.
3. Decide retention: per-query feature rows grow with query volume, unlike the
   current per-candidate upsert which is bounded by corpus size. This is the
   main cost of the change and needs a policy, not a default.

## Acceptance

- Two different queries surfacing the same candidate produce two distinct
  feature rows.
- Retrieval outcomes join to feature rows on a non-empty key for at least one
  surface end to end.
- A retention policy is specified and enforced.
- `kb_ranker_fit.c`'s `missing_grouping_key` and `subject_space_mismatch`
  diagnostics no longer fire on that surface.
