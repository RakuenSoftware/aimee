# Proposal: Evidence and lifecycle layer — series charter

- **State:** proposed (pending — not started). Umbrella charter for nine member
  proposals, `evidence-lifecycle-p1` … `p9`.
- **Author:** JBailes
- **Date:** 2026-08-21
- **Charter roles:** Persist, Review, Enforce, Calibrate, Gate-Promote.
- **Audited pin:** `1d36f8c1` (`testing`). Every "already exists" claim below was
  read at that commit in this tree, not inferred from filenames.

## Problem and boundary

The next priority for Aimee's knowledge base is not another retrieval algorithm.
It is a complete evidence and lifecycle layer around **every** memory mutation,
so that a governance claim about the knowledge base is answerable from stored
state rather than asserted from architecture.

At the pin, Aimee's *epistemics* are strong and its *bookkeeping* is uneven.
Authority is derived from authentication and can no longer be nominated by a
caller; a functional relation cannot be silently overwritten by a lower class;
semantic edges carry valid time, transaction time, tombstones, and a typed
relation gate. But there is no uniform answer to "what changed, who changed it,
on what evidence, and what would undo it" — the assertion layer records the
*resulting state*, not the *event that produced it*, and prose memories,
documents, entities, the ontology, and derived artifacts each keep a different
partial history or none at all.

This series adds that layer. Its boundary is the knowledge base: `entity_edges`
semantic assertions, the `memories` family, `docs`/`kb_documents`, the entity
registry, `rel_types`, and derived artifacts. It does not touch the vault,
management plane, or egress subsystems, each of which already has its own
audited custody model.

## The central design rule

> Structural truth, assertion authority, source evidence and experiential
> usefulness are four different things. Store them separately, link them
> explicitly, and derive current behavior from all four.

Every member proposal is checked against this rule. A design that collapses two
of the four dimensions into one column is rejected on that ground alone, even
when it is simpler and even when the collapse is currently lossless.

## §0 What already exists — do not rebuild

Verified at the pin. The right column is the substrate; members extend it and
are forbidden from re-implementing it.

| Capability | Where it lives at the pin |
|---|---|
| Write authority derived from authentication, never from the request body | `src/modules/db2/c/fact_lifecycle.h` (`fact_authority_t`, `fact_authority_from_provenance`) |
| Provenance-keyed confidence classes A/B/C, with A reserved to a direct user assertion | `src/modules/db2/c/fact_lifecycle.c`, `fact_class_for` |
| Rank-bounded functional correction: an outranked write is dropped, not stored alongside | `src/modules/db2/c/rel_types_store.c` commit path |
| Per-memory provenance category, stamped at insert and failing closed | `memories.provenance_category` in `src/modules/db2/c/schema.sql` |
| Typed semantic-relation gate, seed ontology, provisional/novel handling | `src/modules/memory/memory_fact_gate.c`, `src/modules/db2/c/rel_types_store.c` |
| Self-extending ontology promotion decisions | `src/modules/db2/c/ontology_evolution.c`, `ontology_evaluations` |
| Valid time and transaction time on semantic edges, plus retained tombstones | `entity_edges.valid_from/valid_until/asserted_at/superseded_at/suppressed` |
| Declarative retraction honouring each relation's correction behaviour | `db2_fact_retract` in `src/modules/db2/c/fact_lifecycle.c` |
| Class-C expiry and Class-B durability maintenance modes | `db2_fact_expire_speculative`, `db2_fact_promote_durable` |
| Source-linked document fragments with spans | `kb_doc_regions`, `kb_table_cells`, `document_sections`, `artifact_citations` |
| Document versions, current-version flag, converter identity | `document_versions`, `docs` |
| Corpus stage journal per document | `corpus_stage_events`, `src/modules/db2/c/corpus_jobs.c` |
| Reversible entity merges and alias canonicalization | `entity_registry`, `entity_aliases`, `entity_merges` |
| Code projection generations and per-file source hashes | `code_project_generations`, `code_projection_generations`, `files.hash` |
| Scope enforcement applied before ranking | `src/modules/db2/c/schema.sql` scope tables, memory scope query path |
| Hybrid retrieval and attributed outcome-based demotion | `src/kb/kb_fusion.c`, `src/modules/db2/c/demotion.c` |
| Turn-keyed retrieval events and a provenance read surface | `db2_demotion_retrieval_event_write_turn`, `/v1/audit/trace`, `/v1/audit/provenance` |
| Hash-chained WORM audit ledger with an in-transaction append | `src/modules/audit/audit_worm_chain.c`, `src/modules/db2/c/kb_audit_worm.c` |
| Operator-facing typed-fact console | `frontend/src/console/pages/TypedFacts.tsx` |

Decision records that own parts of the above and must not be contradicted:
[typed-fact knowledge layer](../done/typed-fact-knowledge-layer.md),
[auditable correctness for the KB](../done/auditable-correctness-for-the-kb.md),
[auditable WORM audit store](../done/auditable-worm-audit-store.md),
[graph feedback, self-audit and learning](../done/graph-feedback-self-audit-and-learning.md),
[governance decision records and action audit](../done/governance-decision-records-and-action-audit.md),
[structured PDF ingestion and evidence layer](../done/structured-pdf-ingestion-and-evidence-layer.md),
[kb_hybrid outcome wiring](../done/kb-hybrid-outcome-wiring.md),
[memory db1/db2 architecture](../done/memory-db1-db2-architecture.md).

### §0.1 Prior art in flight — reconcile, do not duplicate

An unmerged branch (`agent/fact-authority-lifecycle`, commit `1a842a3d`, cut
from an older `testing`) implements a large part of P1 and P2 **for semantic
assertions only**: a mandatory mutation seam with an actor rank enum, graph
commits with structured diffs, an evidence-mention table, an erasure receipt,
review actions, and a database trigger refusing any semantic mutation not owned
by an open commit. None of it is present at the pin, and its tree layout
predates the `src/modules/db2/c` relocation.

P1 and P2 must therefore choose one of two paths, in the open, before any new
table is created:

1. **Adopt and generalize** — land that branch's assertion-scoped seam first,
   then widen it to the remaining object kinds. Preferred if the branch merges.
2. **Design forward** — build the general ledger directly, treating the branch
   as a reviewed reference implementation of the assertion slice.

What is not acceptable is a second, parallel ledger that leaves two sources of
truth for the same event.

## §1 The nine members

| # | Proposal | Scope in one line |
|---|---|---|
| P1 | [Uniform mutation ledger](evidence-lifecycle-p1-uniform-mutation-ledger.md) | One transactional evidence-event ledger over every knowledge object kind. |
| P2 | [Knowledge changesets](evidence-lifecycle-p2-knowledge-changesets.md) | Group writes into commits with show / diff / preview-revert / compensating revert. |
| P3 | [Document lifecycle contract](evidence-lifecycle-p3-document-lifecycle-contract.md) | `active` / `invalidated` / `retired` / `purged`, with a blast-radius preview. |
| P4 | [Derived-memory dependencies](evidence-lifecycle-p4-derived-memory-dependencies.md) | Derived knowledge names its inputs and goes stale when they move. |
| P5 | [Work-outcome overlay](evidence-lifecycle-p5-work-outcome-overlay.md) | Experiential usefulness as a projection that cannot rewrite authority. |
| P6 | [Epistemic kind](evidence-lifecycle-p6-epistemic-kind.md) | An epistemic dimension orthogonal to authority, confidence, scope, state and tier. |
| P7 | [Operator review surface](evidence-lifecycle-p7-operator-review-surface.md) | One decision surface over the existing queues. |
| P8 | [Versioned ontology packages](evidence-lifecycle-p8-versioned-ontology-packages.md) | Export, review, dry-run, migrate and roll back the ontology as a contract. |
| P9 | [Recall explanation](evidence-lifecycle-p9-recall-explanation.md) | A persisted per-result trace of lane, scope, contributions, gates and staleness. |

## §2 Ordering and dependencies

```
P1 ──► P2 ──► P3 ──► P4 ──► P7
        │       │      │
        │       └──────┴────► P5
        └────────────────────► P8
P9  (independent; strictly richer once P4 and P5 land)
```

P1, P2 and P3 are foundational. Without them, P5 and P7 create more state
without making that state governable — the exact failure this series exists to
prevent. P9 has no hard dependency and may be pulled forward if retrieval
debugging becomes the pressing need.

Each member is independently landable and independently revertible. No member
may merge with a later member's table present but unwritten.

## §3 Series-wide non-goals

- No new retrieval algorithm, ranking model, or fusion mode.
- No relaxation of the write gate, authority comparison, or scope enforcement.
  Configuration produces a *new validated contract*; it never removes one.
- No parallel audit table disconnected from the mutation it describes. An
  optional or bypassable ledger cannot support a governance claim, so a member
  that ships one has failed rather than partially succeeded.
- No collapsing of authority, confidence, provenance and usefulness into a
  single stored score. Fusing them is a read-time decision.
- No member changes recall behaviour while in its default state.

## §4 Series-wide threat and failure model

| Threat | Series answer |
|---|---|
| Caller-supplied identity used as authority | Members derive the actor from authentication, extending the pin's existing rule to every new write path. A body may lower authority, never raise it. |
| A mutation path that skips the ledger | Members extend a database-level guard to each object kind they cover. A ledger that background maintenance can bypass is treated as absent, not as partial. |
| Ledger disabled in production while the claim is retained | P1's ledger is default-on and non-optional; the WORM dual-write stays separately gated, as it is today. |
| Purge used to destroy the audit trail | Purge removes protected content and leaves a content-free receipt naming counts and selector, never content. |
| Revert used to rewrite history | Every revert is a compensating changeset. Original events are immutable and are never deleted or edited. |
| Derived prose outliving its factual basis | P4 makes staleness a computed consequence of input movement rather than a periodic guess. |
| An outcome overlay laundering a model claim into truth | P5's projection is structurally unable to write authority or lifecycle columns. |
| Evidence tables becoming a second copy of the corpus | Evidence rows store ids, spans and hashes — never content. P3's purge depends on this. |

## §5 Compatibility and migration

- Every member is additive at the schema level and default-off at the behaviour
  level, with the single exception of P1's ledger writes, which are default-on
  from the slice that introduces them.
- Existing rows migrate as their most conservative reading: unknown provenance
  is model authority, unknown epistemic kind is `world_fact`, unknown document
  state is `active`, absent dependencies mean "not derived".
- No member rewrites history so an older row looks as though it had been written
  under the new contract. Backfilled rows are marked as backfilled and are
  excluded from any claim that requires end-to-end provenance.

## §6 Acceptance

```yaml acceptance
- {id: 1, tier: integration, check: "P1: every production knowledge mutation path emits a ledger event in the same transaction as the mutation, proved by a coverage test that fails when one path is unhooked"}
- {id: 2, tier: integration, check: "P2: reverting a changeset creates a compensating changeset and leaves the original changeset and its events byte-identical"}
- {id: 3, tier: integration, check: "P3: invalidating a document returns a blast-radius preview and deletes no derived fact; every affected derived item is marked unsupported or stale"}
- {id: 4, tier: integration, check: "P5: an outcome-overlay write cannot alter any authority, confidence-class, or lifecycle column, proved by a negative test"}
- {id: 5, tier: mechanical, check: "make -C src proposal-links-check"}
- {id: 6, tier: mechanical, check: "python3 scripts/check-proposal-reconcile.py"}
```

## §7 Status and supersession

This charter supersedes nothing. It coordinates work downstream of
[typed-fact knowledge layer](../done/typed-fact-knowledge-layer.md) and adjacent
to the pending
[evidence provenance tiers](proposal-evidence-provenance-tiers.md),
[operator audit activity residual](operator-audit-activity-residual.md),
[kb_hybrid outcome residual](kb-hybrid-outcome-wiring-residual.md) and
[per-query feature persistence residual](per-query-feature-persistence-residual.md)
proposals, each cited by the member that touches its surface.
