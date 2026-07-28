# Versioned knowledge sources and external representation integration

Status: implementation in progress

## Scope and repository boundary

This public proposal covers Aimee's runtime responsibilities for versioned evidence and an external
learned-representation provider. The provider is an opaque, independently versioned component.
Its model architecture, tokenizer construction, datasets, curation, training objectives, teacher
supervision, evaluation corpus, thresholds, checkpoints, and operating procedures are private model
artifacts and are intentionally outside this repository.

Aimee owns immutable evidence, source monitoring, typed structure, access control, provenance,
freshness, deterministic authority enforcement, and atomic index publication. The external provider
may supply compatible representations and relevance signals. Public integration records only the
metadata required to validate an artifact: provider/model identity, checkpoint and tokenizer hashes,
representation schema, output compatibility, health, and generation identity.

## Runtime invariants

1. Complete original source bytes are primary evidence and remain content-addressed and immutable.
2. Chunks and extracted code units are secondary evidence. Each names one exact original version and
   carries source anchors. Chunks are never concatenated to reconstruct the original.
3. Representations, graph edges, extracted claims, summaries, memories, judgments, and syntheses are
   tertiary evidence. Every tertiary object retains lineage through its secondary parent to one
   exact primary version. Lower-authority evidence never overrules higher-authority evidence.
4. Repository identity is independent of checkout path. Ref, commit, tree, snapshot, and index
   generation are distinct identities.
5. A default branch is a moving selector, not a storage namespace. Non-default branches coexist.
6. Committed snapshots are immutable. WIP/worktree evidence is explicitly labeled and cannot replace
   committed evidence.
7. A generation becomes queryable only through atomic publication after every required source,
   structural, representation, and index validation succeeds.
8. Exact lexical retrieval remains independent of learned signals.
9. Staleness is observable. Stale evidence is never silently described as current.
10. In a Git workspace, an omitted project means the attached checkout's exact ref and commit; it
    never means an unbounded union of projects or branches. Detached HEAD fails closed.
11. Deleted/merged refs and superseded document renditions leave current retrieval immediately.
    Immutable originals remain available only under the applicable historical-retention policy.

## Evidence authority and lineage

Authority is ordinal and cannot be compensated for by similarity or confidence:

| Tier | Evidence | Runtime rule |
| --- | --- | --- |
| Primary | Exact original bytes at a revision or commit | Immutable final citation target |
| Secondary | Anchored chunk, code unit, page region, cell, or rendition | Must name one primary version and carry anchors |
| Tertiary | Representation, graph edge, claim, summary, memory, judgment, or synthesis | Must name its secondary parent and exact primary lineage |

Primary evidence establishes what the source contains. Secondary evidence locates and parses it.
Tertiary evidence helps retrieve or interpret it. Before presentation, evidence escalates to the
primary version and exact anchors. Contradictory primary evidence invalidates lower-tier claims;
contradictory secondary evidence suppresses derived interpretation until reconciled. Multiple
tertiary descendants of the same source span count as one provenance root.

`kb_evidence_lineage` is the cross-surface ledger for subject, tier, exact original, secondary parent,
anchors, derivation identity, hashes, and generation. Specialized indexes retain their efficient
layout, but no derived output is publishable without complete lineage.

## Document identity

- `kb_original_documents` stores logical source identity.
- `kb_original_versions` stores immutable hash, byte length, media type, blob reference, capture
  time, and source revision where available.
- `kb_document_heads` selects the current published version of each logical document.
- `kb_document_version_lifecycle` records publication and supersession without mutating old primary
  evidence.
- `kb_documents` stores derived chunks tied to one exact original version and rendition/parser.

The content-addressed blob store retains originals. Re-ingest can replace derived indexes without
discarding the prior original. Only a successful, non-empty derivation advances the document head.
Source revision or ETag participates where trustworthy; full content hashing remains final identity.

## Repository and branch identity

- `kb_source_repositories` stores checkout-independent repository identity and configured default
  ref.
- `kb_source_refs` stores a moving ref, observed head/tree, active snapshot, publication state, and
  active/merged/deleted lifecycle.
- `kb_source_snapshots` stores immutable commit/tree or explicitly labeled WIP identity.
- `kb_index_generations` stages and publishes one snapshot's index while recording compatible
  provider-artifact metadata and validation results.

Every indexed path, symbol, edge, memory fence, and external representation is scoped to a snapshot
and generation. Cross-branch queries are explicit unions. A single-branch query cannot leak another
snapshot's candidates.

## Current-checkout routing

The attached checkout is resolved locally using Git's symbolic ref plus exact commit and tree. Aimee
then resolves repository/ref/commit to one physical published generation. A stale request receives a
conflict response; the client publishes the newly observed generation once and retries. It never
substitutes the default branch. An actively checked-out branch remains valid even when its tip is
reachable from the default branch.

This contract applies to symbol lookup, lexical and hybrid code search, callers, structure, blast
radius, graph navigation, code-span reads, source-derived memory, question answering, recall, agent
context, and delegated worktree context. General user/language memory remains shared. Source-derived
memory carries an exact repository/ref/commit/generation fence and is invisible without matching
source context; pruning leaves the fence as a tombstone so derived memory cannot become global.

The committed generation is authoritative for its snapshot. Current worktree bytes and source
packets are a higher-freshness primary overlay for reads and edits; uncommitted bytes are never
mislabeled as part of the committed generation.

## Continuous refresh

Events reduce latency and reconciliation provides correctness:

- `post-checkout`, `post-commit`, and `post-merge` hooks asynchronously publish the exact attached
  checkout;
- synchronous stale-generation repair is the backstop when a hook is absent or incomplete;
- filesystem and remote-source events enqueue content checks;
- periodic ref/content reconciliation catches missed events, watcher overflow, clock skew, and
  downtime;
- each queue is idempotent by source identity plus observed revision;
- retries resume staging work and never publish partial generations.

Health exposes observed and published revision, lag, last check, last success, failure reason, and
freshness objective. Retrieval returns generation and freshness metadata and fails closed when policy
does not allow stale evidence.

## Incremental publication

1. Observe and preserve a new ref or document revision.
2. Diff content identities against the prior published snapshot.
3. Reuse safe parse/index/provider outputs for identical content and compatible artifact hashes.
4. Process added or changed units and tombstone removed units in staging.
5. Rebuild deterministic structural relationships for the affected closure.
6. Request compatible external representations for changed or context-invalidated subjects.
7. Validate source coverage, lineage, hashes, snapshot isolation, freshness, and representation
   compatibility.
8. Atomically supersede the old generation and advance the ref/document selector.

A failed generation leaves the previous published generation intact.

## Finite retention

A merged or deleted ref is retired and detached from ordinary retrieval immediately. Its generations
receive a configurable grace deadline. A bounded worker claims eligible generations, purges their
relational and external projections behind generation fences, and either finalizes or releases the
claim for crash-safe retry. Superseded generations no longer referenced by active selectors are also
retention candidates, preventing update-heavy branches from growing forever.

Document supersession follows the same selector/evidence split. Older original bytes remain primary
historical evidence; derived chunks, vectors, and tertiary projections leave current retrieval after
a newer version publishes.

## External representation ABI

The runtime representation record contains only integration metadata:

- subject kind and ID, access scope, repository/snapshot/generation where applicable;
- original version and anchors where applicable;
- provider/model/checkpoint/tokenizer/profile and representation-schema identity;
- content and context hashes;
- opaque compatible dense, sparse, latent, or score payloads supported by the installed provider;
- authority, valid time, observed time, freshness, provenance, and publication state.

Existing specialized indexes remain the fallback during migration. New provider outputs are
shadow-written first; production activation requires public runtime safety gates plus the provider's
private acceptance process. A learned score can reorder only within the candidates allowed by access,
source, freshness, and evidence-authority policy. Deterministic authority enforcement runs last.

## Rollout

1. Implemented: immutable originals, document heads, source snapshot/ref/generation metadata,
   generation-staged branch scans, current-checkout routing, source-fenced memory, ref retirement,
   bounded generation pruning, and PDF supersession.
2. Implemented: current-generation fencing across lexical, structural graph, vector, and memory
   retrieval while preserving explicit cross-project/history queries.
3. Next: define and test the versioned external representation ABI without importing private model
   construction or training dependencies into Aimee.
4. Shadow-write provider outputs and validate source reuse, context invalidation, isolation,
   provenance, and rollback.
5. Gate provider-assisted retrieval against the existing deterministic fallback.
6. Atomically promote a complete provider/index generation while retaining the previous compatible
   pair for rollback.
