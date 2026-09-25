# MR-04: Evidence lineage, independent corroboration and retraction

- **State:** In progress — acceptance audit started 2026-09-25
- **Priority:** P1: evidence foundation
- **Owner:** Go memory lineage and freshness, with existing provenance/storage owners
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-02](memory-reliability-02-authority-preserving-mutations.md)
- **Delivery:** Four implementation slices

## Problem and intended result

A fact copied into a meeting note, session summary and observation remains one underlying piece of evidence. Counting descendants as new corroboration creates self-reinforcing confidence. The same missing lineage also prevents reliable invalidation when an original source is corrected or erased.

Extend existing dependency tracking with claim-level origin families and complete versioned derivation edges. Compute independent support conservatively, and propagate retraction through every declared derivative.

## Existing integration points

Reuse `derived_memory_registry`, `derived_memory_dependencies`, `derived_rederivation_queue`, current freshness/reconciliation functions and learning-observation evidence tables in `src/modules/db2/c/schema.sql`. Ingestion/extraction, summary, observation, reviewed-procedure and embedding producers must declare their actual inputs. This is an extension of the current dependency owner.

Implement memory lineage projection, producer registration and invalidation consumption in the Go memory module through its existing storage contract. Learning and other producers retain ownership of their canonical artifacts and publish dependency/version changes over declared contracts. Do not add direct feature-module imports or copy their admission rules into memory.

## Data and independence rules

Attach host-generated origin references to immutable ingestion events. Record explicit `derived_from`, `quotes`, `republication_of`, `corrects`, `contradicts` and `related_to` semantics; only evidence-bearing relationships contribute to claim support. A related link does not automatically make two records one evidence family.

The projected claim support shape contains:

```json
{
  "support_count": 5,
  "source_family_count": 3,
  "independent_support_count": 2,
  "independence_state": "partial",
  "unknown_origin_count": 1,
  "lineage_generation": 12
}
```

These are distinct counts. `support_count` counts visible support records. Family count reports established origin groups. Independent support counts only claim-supporting groups for which the configured independence rule is satisfied. Unknown provenance does not mint a new independent vote. Return null/unknown when a meaningful independent count cannot be established; never manufacture a precise number.

Two different URLs, files, authors or hashes do not prove independence. Re-publications can share an upstream origin; two runs can share the same corrupted fixture. Record such common dependencies when known. A derived record with parents A and B records both root sets but does not provide a third corroborating vote alongside A and B. Track claim-to-evidence edges: a document containing several claims does not support every claim in every descendant.

Do not define confidence as a multiplication of uncalibrated numbers. Expose support, provenance, validity and consistency separately. Any fitted confidence model is versioned and evaluated under [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md).

## Access and retraction

Derivatives inherit the intersection of permitted audiences/purposes of their required inputs. Scopes are not a universal numeric hierarchy: disjoint audiences may yield no permissible release. A multi-source derivative cannot reveal an unauthorized parent's existence through family IDs or counts. Apply scoped projection to lineage queries.

Correction, supersession, revocation and deletion commit invalidation events containing input identity/version and the new generation through [MR-02](memory-reliability-02-authority-preserving-mutations.md)'s transactional outbox or equivalent durable change log. Apply events with durable consumer progress and idempotent replay. Mark dependent records `stale`, `unsupported` or `dependencies_unknown`; serving policy decides whether to suppress or show a warning. Authority-bearing/current assertions require usable supporting evidence. Revocation and erasure cannot be downgraded to warnings because a consumer is behind.

Record each derived owner's applied watermark and resynchronization state. Lost delivery, consumer downtime and retention gaps must be distinguishable from an empty queue. Release checks require current source-owner evidence when local progress is insufficient; otherwise block release. Erasure completion requires verified coverage from every required owner, including offline owners after recovery, rather than successful event publication alone.

Query-derived views also depend on the eligible collection they searched, including empty results. Track scoped collection changes for inserts, newly applicable evidence and newly visible records as specified in [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md). Selected-parent edges alone cannot invalidate a view when an unrelated new record supplies a constraint or contradiction.

Erasure covers derived text, indexes, embeddings, prompt caches, task projections and retained exports under their documented policy. Keep rejection/deletion intent independently of restorable content snapshots; restoration must replay the intent log before serving. Audit digests do not recreate erased payloads, but they still require access controls and retention. A process may only claim the erasure coverage it actually verifies.

## Implementation slices

1. Add versioned origin/family references and dependency edge semantics. Backfill deterministic links; mark the remainder unknown.
2. Compute scoped support projections and conservative independent counts. Detect cycles and bound traversal; incomplete traversal returns explicit partial lineage.
3. Make every producer declare inputs and collection dependencies; join correction/revocation to durable invalidation/rederivation with consumer watermarks. Add a coverage inventory to prevent unregistered producers.
4. Exercise deletion and restore across derived stores; add `aimee memory evidence <id> --json` with counts, states and authorized evidence references.

## Acceptance gates

- Thirty copies and their summaries do not increase independent corroboration beyond their established origin.
- A composite derived from A+B does not become a third independent witness. Unknown sources stay unknown.
- Missing, cyclic or truncated lineage is observable and cannot silently become fully supported.
- Revoking an input suppresses release of a derivative that requires it, including cached task context.
- Producer/consumer crashes and duplicate delivery preserve invalidation progress. An offline owner prevents a claim of complete erasure until its retained copies are verified removed.
- A newly inserted contradiction invalidates an earlier empty query-derived view without requiring a change to any previously selected parent.
- Cross-scope lineage queries reveal neither text nor hidden parent IDs/counts.
- Restoring an old content snapshot cannot resurrect a record whose surviving deletion intent prohibits serving.

## Rollout and rollback

Compute lineage/support in shadow before using it for ranking. Enable hard revocation and erasure propagation as correctness gates. Reverting support-based ranking must not remove tombstones, authored history or dependency invalidation.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)

The [linked relation input repair](../../validation/memory-linked-relation-inputs-2026-09-23.md)
binds generated relation text to every directly copied memory revision. Search,
entity edges and profiles withhold changed, expired, hidden or unobserved inputs
before limits and aggregation; rebuilding refreshes the observations. A packaged
PostgreSQL replay reproduces the previous leak. The full PostgreSQL/race suite
and exported owner pass; fresh-image validation remains pending. Transitive
closure and automatic dependent rebuilding remain open.
