# Retrieval result cache: a query-aware schema

- **State:** DONE — delivered scope archived 2026-07-26.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

*Filed as a precondition record for
[surface-neutral-retrieval-substrate.md](surface-neutral-retrieval-substrate.md).
Classification: **enhancement**.*

> ## SUPERSEDED — the key argument was right for an extractor that no longer exists
>
> This record argued that a retrieval cache must key on `(document, query,
> budget)` because "a key without the query is a page cache, not a result
> cache." That reasoning is correct **for a chunk-and-rank extractor**, where the
> selection policy is frozen at write time.
>
> Extraction is no longer that. It is a deterministic pure function over the
> stripped text, re-run on every read, so `(query, budget)` are REAPPLIED at read
> time rather than baked into the stored value. The cache supplies the document
> and never stores a policy decision, which makes the query correctly absent from
> the key.
>
> **Implemented** as `db1/web_page_cache.{c,h}`: stripped page text keyed by
> canonical URL. Consequences, all in the same direction — any query against a
> previously-fetched page hits rather than only a repeat of the same query;
> changing the extractor invalidates nothing, so there is no policy version to
> bump; and the key has no budget dimension.
>
> The record is kept rather than deleted because the reasoning was sound under
> its own premise, and the premise changing is the interesting part.
>
> **Not foreclosed:** a span-level cache keyed by `(url, query, budget)` on top
> of the page layer remains possible if repeated identical queries ever justify
> it. The page layer makes that additive rather than a redesign.

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
  features, computed_at` (`src/modules/db2/c/schema.sql:584-593`), with primary key
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
