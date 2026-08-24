# Proposal: P8 — versioned, dry-runnable, rollback-able ontology packages

> **Archived proposal.** This records the implemented design; current behaviour
> is defined by the code and acceptance validation.

- **State:** done (2026-08-21) — implemented in PR #2831; see
  [acceptance validation](../../validation/evidence-lifecycle-acceptance.md).
- **Series:** [Evidence and lifecycle layer](evidence-lifecycle-layer.md), member 8 of 9.
- **Author:** JBailes
- **Date:** 2026-08-21
- **Charter roles:** Enforce, Gate-Promote, Review.
- **Audited pin:** `1d36f8c1`.

## Problem and boundary

Aimee's relation ontology is live, mutable, and unversioned. `rel_types` is a
table that a seed loader populates and that a self-extension pipeline adds
provisional rows to; `ontology_evaluations` records the promotion decisions. Each
row carries real semantics — endpoint kinds, symmetry, an inverse, cardinality
implied by `correction_behavior`, sensitivity — and those semantics decide
whether a write is accepted and how a correction behaves.

There is no way to see the ontology as an artifact, to review a proposed change
to it before it takes effect, to know what the change would do to the facts that
already exist under the old contract, or to go back. A change to
`correction_behavior` on a widely-used relation silently re-interprets every
existing edge of that type; a change to `head_kinds` can make thousands of
current facts invalid without anything reporting it.

Meanwhile the pressure to make entity and relation types *configurable* is real
and legitimate. The risk is that configurability arrives as a loosened gate. It
must not: configuration produces a **new validated contract**, never a weaker one.

Boundary: the ontology as a versioned, reviewable, migratable artifact. Not the
gate's algorithm, not the fact write path.

## §0 What already exists

| Piece | Where | Gap |
|---|---|---|
| `rel_types` (head/tail kinds, symmetry, inverse, correction behaviour, category, sensitivity, hierarchy flag, status) | `src/modules/db2/c/schema.sql` | A live table, not a versioned artifact. No history of what it looked like when a fact was written. |
| In-code seed ontology, idempotently upserted | `src/modules/db2/c/rel_types_store.c` | Ships as code, so it is versioned with the binary — but the live overlay that diverges from it is not. |
| `ontology_evaluations` (occurrence count, pending/approved/mapped/rejected, mapped_to) | `src/modules/db2/c/ontology_evolution.c` | Records decisions; no impact analysis, no operator review step, no rollback. |
| Provisional staging for novel relation types | `src/modules/db2/c/rel_types_store.c` | The extension mechanism works; the governance around it is missing. |
| `memory_relation_schema` (relation_id, subject_kind, object_kind) | `src/modules/db2/c/schema.sql` | A second endpoint-kind record, which is itself an argument for one canonical, versioned definition. |
| `entity_edges.relation_id` on semantic edges | `src/modules/db2/c/schema.sql` | Every fact already names its relation, so impact analysis is a join, not a scan. |

## Decision

### The package

An ontology package is a signed or reviewed, content-addressed artifact
containing the complete definition set:

```
ontology_package
  package_id            -- content hash
  version               -- monotonic within a lineage
  parent_version
  entity_kinds[]        -- name, description, status
  relation_types[]      -- rel_type, head_kinds, tail_kinds, is_symmetric,
                           inverse_rel_type, cardinality, correction_behavior,
                           category, sensitivity, is_hierarchy_rel, status
  provenance            -- author, review record, signature (when signed)
  created_at
```

`cardinality` is made explicit rather than left implied by
`correction_behavior`, because "single-valued" and "corrections supersede" are
two different statements that currently share one column and are the source of
the most confusing ontology behaviour.

The live `rel_types` table becomes the *materialization* of the active package
version. A `ontology_package_versions` record **new** tracks which version is
active, when it was activated, by whom, and which changeset performed the
activation.

### The six operations

1. **Export** — the active ontology as a package artifact, content-addressed and
   diffable.
2. **Import** — a package, reviewed (and signature-verified when signed), staged
   but not activated.
3. **Dry-run** — evaluate a staged package against existing data and report:
   - facts that would become invalid, by relation and by rule violated,
   - facts whose correction behaviour would change meaning,
   - relations gaining or losing symmetry or an inverse,
   - endpoint-kind narrowings and the affected edge counts,
   - provisional types the package promotes or rejects,
   - derived items (P4) whose basis would change.
4. **Migrate** — activate the package, migrating or retiring affected facts
   through a single P2 changeset with P1 events.
5. **Report** — the post-migration reconciliation: what was migrated, what was
   retired, what was left alone and why.
6. **Roll back** — reactivate the prior version, again as a changeset. Facts
   retired by the migration are restored; facts created under the new contract
   that are invalid under the old one are reported and quarantined rather than
   deleted.

Dry-run is **mandatory** before migrate, and its result is bound to the package
hash and the changeset head it was computed against. A migrate whose dry-run is
stale is refused — the same discipline P3 applies to purge.

### The invariant that cannot be traded

> Never relax the current write gate merely to make schemas configurable.

Operationally:

- A package that would make a currently-refused write succeed must state that
  widening explicitly, and the dry-run must report it as a widening, not bury it
  in a diff.
- Sensitivity may be raised by a package; lowering it is an operator-authority
  decision with its own record, because sensitivity governs what may leave the
  system.
- No package may remove the requirement that endpoint kinds be checked. A
  relation may accept more kinds; it may not accept "any" implicitly. `any` is a
  declared value with its own review consequence.
- Provisional types promoted by a package are promoted as a governance decision
  under P7's rules, not by occurrence count.

### Facts written under an older version

Every semantic edge names its relation, and every changeset names its time. A
fact written under version 3 keeps its meaning under version 3; when version 4
narrows that relation, the migration decides per fact: migrate (still valid),
retire (no longer valid, retained and reversible), or quarantine (ambiguous,
routed to P7). Nothing is deleted, and nothing is silently reinterpreted.

## Non-goals

- No change to the write gate's algorithm or to the fact write path.
- No ontology inference or LLM-authored packages. A package is authored or
  exported, then reviewed.
- No cross-tenant ontology sharing or a package registry in this proposal.
- No removal of the in-code seed. The seed remains the bootstrap; a package
  layers over it and its divergence from the seed is reportable.

## Owners and dependencies

- **Owner:** memory / db2, with governance for the review and signature path.
- **Depends on:** P1 (events), P2 (migration and rollback are changesets), P7
  (package review reuses the decision surface). P4 improves dry-run's derived
  section.
- **Depended on by:** nothing in this series.

## Threat and failure model

| Failure | Consequence | Mitigation |
|---|---|---|
| Configurability used to weaken the gate | The typed-fact guarantee is lost | Widenings reported explicitly by dry-run; sensitivity lowering is an operator decision with its own record |
| Unsigned package imported from an untrusted path | Ontology tampering | Signature verification when signed; review record required when not; import stages but never activates |
| Migration invalidates a large fact set unnoticed | Mass silent knowledge loss | Mandatory dry-run with counts; migration runs as one revertible changeset |
| Rollback after new facts were written | Facts invalid under the restored contract | Reported and quarantined, never deleted; routed to P7 |
| Two writers activate different versions concurrently | Split-brain ontology | Activation takes the changeset head; a stale activation is refused |
| Dry-run drifts from migrate | Operator authorized a different change | Dry-run bound to package hash and changeset head; staleness refused |

## Compatibility and migration

- Additive. The live `rel_types` table keeps its shape and its consumers.
- The first export creates version 1 from current state, so the pre-P8 ontology
  becomes a package without any behavioural change.
- `memory_relation_schema` is reconciled against the package definition in S2;
  if it diverges, the divergence is reported rather than silently resolved,
  because a silent resolution would itself be an unreviewed ontology change.

## Slices

| # | Scope | Done when |
|---|---|---|
| S1 | Package schema, export, version record, explicit cardinality | Exporting current state and re-importing it is a no-op with an identical hash |
| S2 | Import with staging and verification; reconcile `memory_relation_schema` | An unverified package cannot activate; any divergence is reported |
| S3 | Dry-run impact analysis over existing edges and derived items | Reported counts equal what a subsequent migration actually changes |
| S4 | Migrate as one changeset, with per-fact migrate/retire/quarantine and a reconciliation report | A narrowing migration retires exactly the facts the dry-run predicted, reversibly |
| S5 | Rollback with quarantine of newer incompatible facts; review integration with P7 | Rollback restores the prior contract and deletes nothing |

## Acceptance

```yaml acceptance
- {id: 1, tier: mechanical, check: "export then import of the active ontology is a byte-identical no-op, and the package hash is stable across repeated exports"}
- {id: 2, tier: integration, check: "a package that would widen the write gate is reported by dry-run as an explicit widening, and cannot be activated without that report being acknowledged"}
- {id: 3, tier: integration, check: "dry-run counts of facts becoming invalid equal the counts the executed migration changes"}
- {id: 4, tier: integration, check: "a migration that narrows endpoint kinds retires affected facts reversibly through one changeset and deletes none"}
- {id: 5, tier: integration, check: "rollback restores the prior version and quarantines facts written under the newer contract instead of deleting them"}
- {id: 6, tier: integration, check: "a migrate whose dry-run predates an intervening changeset is refused as stale"}
- {id: 7, tier: integration, check: "provisional relation types are promoted only through a governance decision, never by occurrence count alone"}
```

## Status and supersession

Supersedes nothing. Extends the ontology machinery recorded in
[typed-fact knowledge layer](../done/typed-fact-knowledge-layer.md) with
versioning, impact analysis and reversal, under the review path P7 provides.
