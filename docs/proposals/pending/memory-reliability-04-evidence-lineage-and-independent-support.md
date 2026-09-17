# MR-04: Evidence lineage, independent corroboration and retraction

- **State:** Proposed
- **Priority:** P1: evidence foundation
- **Owner:** Memory derivation, provenance and DB2
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-02](memory-reliability-02-authority-preserving-mutations.md)
- **Delivery:** Four implementation slices

## Problem and intended result

A fact copied into a meeting note, session summary and observation remains one underlying piece of evidence. Counting descendants as new corroboration creates self-reinforcing confidence. The same missing lineage also prevents reliable invalidation when an original source is corrected or erased.

Extend existing dependency tracking with claim-level origin families and complete versioned derivation edges. Compute independent support conservatively, and propagate retraction through every declared derivative.

## Existing integration points

Reuse `derived_memory_registry`, `derived_memory_dependencies`, `derived_rederivation_queue`, current freshness/reconciliation functions and learning-observation evidence tables in `src/modules/db2/c/schema.sql`. Ingestion/extraction, summary, observation, reviewed-procedure and embedding producers must declare their actual inputs. This is an extension of the current dependency owner.

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

Correction, supersession, revocation and deletion emit idempotent invalidation events containing input identity/version and the new generation. Mark dependent records `stale`, `unsupported` or `dependencies_unknown`; serving policy decides whether to suppress or show a warning. Authority-bearing/current assertions require usable supporting evidence.

Erasure covers derived text, indexes, embeddings, prompt caches, task projections and retained exports under their documented policy. Keep rejection/deletion intent independently of restorable content snapshots; restoration must replay the intent log before serving. Audit digests do not recreate erased payloads, but they still require access controls and retention. A process may only claim the erasure coverage it actually verifies.

## Implementation slices

1. Add versioned origin/family references and dependency edge semantics. Backfill deterministic links; mark the remainder unknown.
2. Compute scoped support projections and conservative independent counts. Detect cycles and bound traversal; incomplete traversal returns explicit partial lineage.
3. Make every producer declare inputs and join correction/revocation to invalidation/rederivation. Add a coverage inventory to prevent unregistered producers.
4. Exercise deletion and restore across derived stores; add `aimee memory evidence <id> --json` with counts, states and authorized evidence references.

## Acceptance gates

- Thirty copies and their summaries do not increase independent corroboration beyond their established origin.
- A composite derived from A+B does not become a third independent witness. Unknown sources stay unknown.
- Missing, cyclic or truncated lineage is observable and cannot silently become fully supported.
- Revoking an input suppresses release of a derivative that requires it, including cached task context.
- Cross-scope lineage queries reveal neither text nor hidden parent IDs/counts.
- Restoring an old content snapshot cannot resurrect a record whose surviving deletion intent prohibits serving.

## Rollout and rollback

Compute lineage/support in shadow before using it for ranking. Enable hard revocation and erasure propagation as correctness gates. Reverting support-based ranking must not remove tombstones, authored history or dependency invalidation.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
