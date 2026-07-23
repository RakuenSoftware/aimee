# Retrieval result cache: a query-aware schema

*Filed as a precondition record for
[surface-neutral-retrieval-substrate.md](surface-neutral-retrieval-substrate.md).
Classification: **enhancement**.*

## Problem

There is no retrieval cache anywhere in the tree — `semcache`,
`semantic_cache`, and `retrieval_cache` all return zero matches. `tool_web_read`
therefore refetches a page on every call, including when the same page is read
repeatedly within one investigation.

## Why the obvious key is wrong

An earlier draft proposed keying a cache on
`(target_surface, subject_id, feature_set_version)` and claimed cross-surface
collision-freedom "by construction". Both halves were wrong, and the reasons
constrain any real design:

- **`feature_rows` has no `target_surface` column.** Its columns are
  `subject_id, subject_kind, scope_kind, scope_id, feature_set_version,
  features, computed_at` (`src/db2/schema.sql:584-593`), with primary key
  `(subject_id, subject_kind, feature_set_version)`. Surface lives on the
  artifact and audit-event tables, not here. Reusing `feature_rows` for caching
  would silently drop the surface dimension.
- **A key without the query is a page cache, not a result cache.** Retrieval
  output is a function of `(document, query, budget, ranking policy)`. Keying on
  the document alone would return spans selected for a different question.

## What a correct key needs

At minimum: the surface; the normalized document identity; the normalized query;
the selection budget; and a policy version covering the chunker, the legs, and
the selection rule, so that changing any of them invalidates rather than serves
stale output. Authorization scope matters too — a cached result must not cross a
principal boundary that the live path would have enforced.

## Design questions to settle before implementing

- Its own table, or a generalization of an existing one? Given the surface and
  query dimensions are both absent from `feature_rows`, a dedicated table is the
  likely answer.
- Freshness. Static per-surface TTLs are the simple starting point; inferred
  volatility is explicitly deferred until staleness failures are observed.
- Invalidation on policy change — a version column compared on read, rather than
  a migration on every tuning change.
- Interaction with egress policy: a cache hit must not become a way to serve
  content that current policy would now refuse to fetch. Re-validating the
  destination on hit is cheap.
- Whether entries are ever shared across principals, and if so under what
  equivalence.

## Acceptance

- A repeated identical query against an unchanged document serves from cache.
- The same document under a different query does **not** serve the first
  query's spans.
- Changing chunker, leg, or selection policy invalidates prior entries.
- No cached entry crosses a surface or principal boundary.
- A cache hit is re-validated against current egress policy.
