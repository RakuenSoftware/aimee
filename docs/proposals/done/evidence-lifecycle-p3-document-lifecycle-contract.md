# Proposal: P3: an explicit document lifecycle contract with blast-radius preview

> **Archived proposal.** This records the implemented design; current behaviour
> is defined by the code and acceptance validation.

- **State:** done (2026-08-21). Implemented in PR #2831; see
  [acceptance validation](../../validation/evidence-lifecycle-acceptance.md).
- **Series:** [Evidence and lifecycle layer](evidence-lifecycle-layer.md), member 3 of 9. Foundational.
- **Author:** JBailes
- **Date:** 2026-08-21
- **Charter roles:** Persist, Enforce, Review.
- **Audited pin:** `1d36f8c1`.

## Problem and boundary

Documents are the load-bearing evidence under a large share of Aimee's knowledge,
and they are the object class with the weakest lifecycle contract.

At the pin, `docs.state` is a free-text column defaulting to `'staged'`, with
`'staged'` the only value any query filters on. There is no distinction between
"this source is wrong, stop believing it", "this source was replaced by a newer
version", and "this content must be physically removed": the three operations a
document actually needs, with three different consequences for derived
knowledge and two different reversibility properties.

The consequence is not theoretical. A user who says "that spec was wrong, remove
it" today has no operation that reliably stops the facts extracted from it from
being recalled, and no way to see what would break before doing it. A user
subject to a deletion obligation has no operation that removes content while
retaining the fact that a deletion occurred.

Boundary: the lifecycle of `docs` / `document_versions` and its consequences for
knowledge derived from them. The staleness *mechanics* for derived items are P4;
this proposal specifies the contract P4 implements against.

## §0 What already exists

| Piece | Where | Gap |
|---|---|---|
| `docs.state` (default `'staged'`), `review_needed`, `review_reason` | `src/modules/db2/c/schema.sql` | Free text; only `'staged'` is queried (`src/modules/db2/c/kb_docs.c`); no closed state machine. |
| `document_versions` with `is_current`, `superseded_at`, `version_no` | `src/modules/db2/c/schema.sql` | Version supersession already exists: the `retire` half is mostly built and simply unnamed. |
| `docs` UNIQUE on (content_hash, converter, converter_version, scope) | `src/modules/db2/c/schema.sql` | Content-addressed identity, so re-ingest of identical content is already idempotent. |
| `document_sections`, `kb_doc_regions`, `kb_table_cells`, `artifact_citations` | `src/modules/db2/c/schema.sql` | Span-level source links exist: the raw material for a blast-radius preview. |
| `curator_invalidation_events` (source_kind, source_id, artifacts_stale) | `src/modules/db2/c/schema.sql` | Records that a source invalidated N artifacts; no per-object detail, no preview, no reversal. |
| `corpus_processing_jobs`, `corpus_stage_events` | `src/modules/db2/c/corpus_jobs.c` | Stage machine for ingestion, not for post-ingestion lifecycle. |
| `kb_documents.generation` | `src/modules/db2/c/schema.sql` | Generation scoping for project chunks; a precedent for bulk supersession. |

## Decision

### The four states

`docs.state` becomes a closed enum with a CHECK constraint.

| State | Meaning | In recall | Available for derivation | Source retained | Reversible |
|---|---|---|---|---|---|
| `active` | Believed and usable | yes | yes | yes | n/a |
| `invalidated` | Judged wrong or untrusted | no | no | yes | yes |
| `retired` | Superseded by a newer version | no | no | yes | yes |
| `purged` | Content physically removed | no | no | **no** | **no** |

`staged` is retained as a pre-`active` ingestion state and is unchanged.

### Operation consequences

| Operation | Source retained | Derived facts current | Reversible |
|---|---|---|---|
| Invalidate | yes | no | yes |
| Retire | yes | no | yes |
| Re-ingest | old version retained | recomputed | yes |
| Purge | no | no | no |

Every one of these runs as a P2 changeset and emits P1 events. Purge's changeset
is created with `reversible=0` and an `irreversible_why`.

### Blast-radius preview: required before invalidate and purge

`document.preview_lifecycle(doc_id, operation)` returns, before anything changes:

- **Facts supported only by this document**: the set that would lose all
  supporting evidence.
- **Facts with other surviving evidence**: the set that stays supported, counted
  separately so an operator can see the difference between "this removes
  knowledge" and "this removes a citation".
- **Derived items**: summaries, patterns, preference projections, mental models
  produced from it (P4's dependency edges; before P4 lands, reported as
  `derived: not-computed`, never as an empty list).
- **Citations that will stop resolving**: `artifact_citations` and section
  references pointing into it.
- **Code and entity links that will become unsupported.**
- **Recall impact**: the count of currently-recallable items that would leave
  recall, so the operator sees the size of the behavioural change, not only the
  size of the data change.

The preview is a read-only, side-effect-free call and is exposed to the operator
review surface (P7) as the mandatory step before either destructive operation.

### The rule that matters most

> Do not silently delete derived facts.

When a document leaves `active`, facts derived from it are **not deleted**. Each
is marked `unsupported` (no surviving supporting evidence) or `stale` (evidence
changed but some remains), and policy (not the lifecycle operation) decides
whether that removes them from recall. The default policy is:

- `unsupported` + assertion authority no higher than model → excluded from recall,
  retained in storage, listed in the review queue.
- `unsupported` + user or operator authority → **retained in recall, flagged for
  re-verification**. A user's own statement does not stop being theirs because a
  document that happened to corroborate it was withdrawn.
- `stale` → down-ranked and flagged, never removed.

This asymmetry is deliberate and is the concrete expression of the series' rule
that source evidence and assertion authority are different things.

### Re-ingest

Re-ingesting a changed document creates a new `document_versions` row, marks the
prior version `retired`, recomputes derived knowledge against the new version,
and records both the retirement and the recomputation in one changeset. Facts
that the new version still supports keep their identity and history rather than
being deleted and re-asserted, otherwise every re-ingest would reset the
corroboration and outcome history of everything the document touches.

### Purge

Purge removes `docs.normalized_text`, section content, region and cell content,
and any embedding derived from that content. It retains:

- the `docs` row shell with `state='purged'` and no content,
- a content-free receipt: selector, counts by object kind, actor, authority,
  reason, timestamp, and the changeset id,
- the P1 event trail, whose `before_ref`/`after_ref` are references and hashes
  rather than content, and which therefore survives purge intact by design.

Purge is operator-authority only, requires a preview token from a preview issued
within the same session, and refuses to run if the preview is stale, the same
confirm-before-destroy discipline the repository already applies to its
irreversible vault operations.

## Non-goals

- No retention scheduler, legal-hold model, or automatic expiry. Purge is always
  an explicit authorized action.
- No change to ingestion, chunking, converters, or the corpus stage machine.
- No cascade delete of derived rows. P3 marks; policy decides recall.
- No cross-tenant purge. Scope is honoured exactly as recall honours it.

## Owners and dependencies

- **Owner:** kb / corpus, with db2.
- **Depends on:** P1 (events), P2 (changesets). P4 completes the derived half of
  the preview; P3 ships with `derived: not-computed` until then and must not
  report an empty list in its place.
- **Depended on by:** P4, P7.

## Threat and failure model

| Failure | Consequence | Mitigation |
|---|---|---|
| Purge used to destroy an audit trail | Governance defeated | Events store references and hashes, never content; receipt survives; purge cannot delete events |
| Invalidate silently drops user-authored facts | User's own knowledge lost to a document decision | User/operator-authority facts stay in recall, flagged for re-verification |
| Preview computed, document changes, purge proceeds | Operator authorized the wrong blast radius | Preview token bound to document version and changeset head; stale token refused |
| Re-ingest resets corroboration and outcome history | Learning signal destroyed on every document update | Facts still supported keep identity; only evidence links are recomputed |
| `purged` row still served from a cache or vector store | Content survives deletion | Purge enumerates and clears derived embeddings in the same changeset; residual stores named explicitly in the receipt |
| State machine bypassed by a direct UPDATE | Silent state corruption | CHECK constraint plus P1's per-kind guard trigger on `docs` |

## Compatibility and migration

- Existing rows migrate to their most conservative reading: `staged` stays
  `staged`; everything else becomes `active`.
- Recall paths gain a state filter. Because every migrated row is `active` or
  `staged`, recall behaviour is unchanged at migration time. The filter is
  inert until an operator uses a new state.
- `curator_invalidation_events` continues to be written during P3 and becomes a
  view over the ledger in a follow-up, not in this proposal.

## Slices

| # | Scope | Done when |
|---|---|---|
| S1 | Closed state enum, CHECK, migration, recall state filter | Recall returns nothing from a non-`active` document; behaviour unchanged for migrated rows |
| S2 | `invalidate` and `restore` as changesets, with the unsupported/stale marking rules | Invalidate removes model-authority derived facts from recall and retains user-authority ones with a flag; restore is exact |
| S3 | Blast-radius preview over facts, citations, entity and code links | Preview counts match what the subsequent operation actually changes, asserted in one test |
| S4 | `retire` unified with `document_versions` supersession; re-ingest keeps fact identity | Re-ingest of a changed document retires v1, recomputes evidence, and preserves corroboration history |
| S5 | `purge`: content removal, embedding cleanup, content-free receipt, preview token, operator gate | Purge leaves no content in any store named by the receipt, and the event trail still reconstructs the document's history |

## Acceptance

```yaml acceptance
- {id: 1, tier: mechanical, check: "docs.state is CHECK-constrained to staged/active/invalidated/retired/purged and a direct UPDATE to an unlisted value fails"}
- {id: 2, tier: integration, check: "invalidating a document deletes zero derived facts; every affected fact is marked unsupported or stale and appears in the review queue"}
- {id: 3, tier: integration, check: "a user-authored fact whose only corroborating document is invalidated stays in recall and is flagged for re-verification"}
- {id: 4, tier: integration, check: "the blast-radius preview's counts equal the counts the executed operation actually changes, for both invalidate and purge"}
- {id: 5, tier: integration, check: "purge removes content from docs, sections, regions, cells and derived embeddings, and the content-free receipt plus the P1 event trail still reconstruct the document's lifecycle"}
- {id: 6, tier: integration, check: "a purge attempted with a preview token issued before an intervening change is refused as stale"}
- {id: 7, tier: integration, check: "re-ingesting a changed document retires the prior version and preserves the identity and corroboration history of facts the new version still supports"}
```

## Status and supersession

Supersedes nothing. Extends
[structured PDF ingestion and evidence layer](../done/structured-pdf-ingestion-and-evidence-layer.md)
and [ingest restoration and recall contract](../done/ingest-restoration-and-recall-contract.md)
with the post-ingestion half of a document's life.
