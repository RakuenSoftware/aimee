# Proposal: P1 — a uniform, transactional mutation ledger for every memory object

- **State:** proposed (pending — not started)
- **Series:** [Evidence and lifecycle layer](evidence-lifecycle-layer.md), member 1 of 9. Foundational.
- **Author:** JBailes
- **Date:** 2026-08-21
- **Charter roles:** Persist, Enforce, Review.
- **Audited pin:** `1d36f8c1`.

## Problem and boundary

Aimee stores the *resulting state* of a knowledge mutation well. It does not
store the *event*.

Ask "who asserted that the user prefers Postgres, from what evidence, and what
would happen if that turn were undone" and the answer has to be assembled from
five differently-shaped partial records — `memories.provenance_category` for the
class of writer, `memory_provenance` for a session-scoped free-text action note,
`entity_edges.asserted_at`/`superseded_at` for transaction time, `audit_events`
for the artifact-driven surface writes only, and the WORM chain for the subset of
actions that are governed and only when the WORM dual-write is enabled. Whole
object classes emit nothing: an entity merge, an ontology promotion, a document
re-ingest, a maintenance sweep that expires a hundred Class-C edges.

This proposal adds one uniform event ledger over every knowledge-base mutation,
emitted in the same database transaction as the mutation itself. It is the
substrate that P2 (changesets), P3 (document lifecycle) and P7 (review) all read.

Out of scope: the vault, management-plane and egress audit trails, which have
their own custody models and are not knowledge objects.

## §0 What already exists

| Piece | Where | Why it is not enough |
|---|---|---|
| `memory_provenance` (memory_id, session_id, action, details) | `src/modules/db2/c/schema.sql` | Prose memories only; free-text `action`; no actor identity, no before/after, not transactional with the write, no evidence link. |
| `memories.provenance_category`, stamped at insert and failing closed | `src/modules/db2/c/schema.sql` | Records the *class* of writer for a row, not the event or the sequence of events. |
| `memory_authority_t` on the memory CRUD seam (`memory_insert_ex`, `memory_update_content_as`, `memory_delete_as`) | `src/modules/memory/memory_core_crud.c` | The authority check exists; nothing durably records that the check happened or what it decided. |
| Semantic-edge transaction time (`asserted_at`, `superseded_at`, `suppressed`) | `entity_edges` | State, not event. Two supersessions of the same edge are indistinguishable afterwards. |
| `audit_events` (before/after JSONB snapshots, operator, verdict) | `src/modules/db2/c/schema.sql` | Structurally close to what is wanted, but bound to `artifacts.id` by foreign key — it can only describe artifact-driven writes. |
| Hash-chained WORM audit with in-transaction append | `src/modules/audit/audit_worm_chain.c`, `db2_kb_audit_append_in_txn` | The right integrity primitive; today it carries governed *actions*, is default-off, and takes free-text detail rather than a typed before/after. |
| `corpus_stage_events` | `src/modules/db2/c/schema.sql` | A good per-document precedent for exactly this shape — scoped to ingestion stages only. |
| `curator_invalidation_events` | `src/modules/db2/c/schema.sql` | Records that a source invalidated artifacts, with a count; no actor, no per-object detail, no reversal information. |

The in-flight branch described in the charter's §0.1 implements this shape for
semantic assertions. **Reconcile with it before writing a new table.**

## Decision

Add one ledger, `memory_evidence_events` **new**, written in the mutating
transaction, covering every knowledge object kind.

```sql
CREATE TABLE IF NOT EXISTS memory_evidence_events (
  event_id            TEXT PRIMARY KEY,
  changeset_id        TEXT NOT NULL DEFAULT '',   -- P2 groups events; '' until P2
  object_kind         TEXT NOT NULL,              -- assertion | memory | document |
                                                  -- document_version | entity | alias |
                                                  -- rel_type | derived | scope | link
  object_id           TEXT NOT NULL,              -- stringified pk of the object kind
  operation           TEXT NOT NULL,              -- see the operation set below
  before_ref          TEXT NOT NULL DEFAULT '',   -- typed reference to prior state
  after_ref           TEXT NOT NULL DEFAULT '',   -- typed reference to new state
  authenticated_actor TEXT NOT NULL,              -- verified principal; never from a body
  transport_identity  TEXT NOT NULL DEFAULT '',   -- cert CN / bearer subject / 'internal'
  effective_authority TEXT NOT NULL,              -- the authority the write ran under
  source_document_id  TEXT NOT NULL DEFAULT '',
  source_span         TEXT NOT NULL DEFAULT '',
  source_hash         TEXT NOT NULL DEFAULT '',
  code_generation     BIGINT NOT NULL DEFAULT 0,
  reason              TEXT NOT NULL DEFAULT '',
  occurred_at         TEXT NOT NULL,              -- when the described change took effect
  recorded_at         TEXT NOT NULL DEFAULT (to_char(CURRENT_TIMESTAMP,'YYYY-MM-DD HH24:MI:SS')),
  correlation_id      TEXT NOT NULL DEFAULT ''    -- turn_id / ingest_run_id / job id
);
```

Operation set, closed and CHECK-constrained:

```
assert     confirm    contradict   supersede   retire     restore
promote    demote     invalidate   redact      purge      derive
```

`confirm` and `contradict` are corroboration events that do not change the
object's envelope; they exist so that "this was seen again" is an event rather
than an incremented counter with no history.

### Six requirements, each with a mechanism

1. **Emitted in the mutating transaction.** The writer function takes the open
   connection. A ledger write that can fail independently of its mutation is a
   ledger that lies, so there is no asynchronous or best-effort emission path.
2. **Authenticated identity only.** `authenticated_actor` and
   `effective_authority` are resolved from the request context or from a
   compile-time internal-actor constant, never from request JSON — the rule the
   pin already enforces for `fact_authority_t`, extended to every object kind.
   `transport_identity` is recorded separately so an operator can see *how* the
   principal was proved, not only who it claims to be.
3. **Every production path, including background maintenance.** The curator
   drain, the Class-C expiry sweep, the Class-B promotion sweep, re-ingestion,
   and the vector-repair paths emit events exactly as an interactive write does.
   A sweep emits one event per object it changes, grouped by `correlation_id`.
4. **Authority, confidence and provenance stay separate.**
   `effective_authority` is not a confidence, and `source_*` is not an authority.
   The ledger has no combined "trust" column and must never grow one.
5. **Default-on.** The ledger is not behind a feature flag. The existing WORM
   dual-write stays separately gated exactly as today; the two are different
   claims — "we recorded it" and "we recorded it tamper-evidently".
6. **Purge leaves a receipt.** A `purge` event stores counts, selector, actor and
   reason, and never content, `before_ref` or `after_ref` payloads. The receipt
   survives the rows it describes.

### `before_ref` / `after_ref` are references, not copies

Both columns hold a typed reference — `"edge:918:v3"`, `"memory:4412:h=ab3f…"`,
`"docver:77"` — resolvable through the object kind's own version history, plus a
content hash where the object has one. The ledger never stores object content.
This is what allows a purge to remove content while leaving the event trail
intact, and it keeps the ledger's growth proportional to *events*, not to corpus
size.

Where an object kind has no version history to reference (prose `memories`
content today), P1's slice for that kind adds a minimal content hash to the
object row rather than copying content into the ledger.

### Bypass guard

Emission is enforced at the database, not only in C. Following the pattern the
in-flight branch proved for semantic edges, each covered object kind gets a
`BEFORE INSERT OR UPDATE OR DELETE` trigger that refuses a mutation when no
ledger event has been staged for it in the current transaction. Hand-written SQL
and future call sites are then covered by construction rather than by review
discipline.

The guard is added **per object kind, in the same slice that hooks that kind's
writers** — never ahead of them, since a guard without emitters is an outage.

## Non-goals

- Not a replacement for `audit_events` or the WORM chain. `audit_events` keeps
  its artifact-application role; the WORM chain keeps its tamper-evidence role
  and gains ledger events as a source once P1's coverage is complete.
- Not a general application event bus. This ledger describes knowledge-object
  mutations only.
- No retention or rollup policy in P1. Growth is measured in slice 5 and a policy
  is proposed from data, not guessed now.
- No read UI. P7 owns the operator surface; P1 exposes only the read primitives
  P2 and P7 need.

## Owners and dependencies

- **Owner:** memory / db2.
- **Depends on:** nothing beyond the pin. The charter's §0.1 reconciliation
  decision must be made first.
- **Depended on by:** P2 (changesets group these events), P3, P4, P5, P7.

## Threat and failure model

| Failure | Consequence | Mitigation |
|---|---|---|
| A writer forgets to emit | Silent hole in a governance claim | Per-kind DB trigger; a coverage test that unhooks each writer and asserts the mutation now fails |
| Ledger write fails, mutation succeeds | Divergent history | Same transaction; the mutation fails with it |
| Actor spoofed via payload | Authority laundering | Actor resolved from authentication only; body may lower, never raise |
| Ledger disabled to reduce write cost | Governance claim becomes false while still being made | Default-on with no disable switch; cost measured in slice 5 and addressed by retention, not by silence |
| Ledger grows unboundedly on a busy ingest | Storage exhaustion | References not content; one event per changed object; growth measured before any policy is chosen |
| Purge leaks content into the event trail | Erasure defeated | `purge` events reject non-empty before/after refs at the writer and by CHECK |

## Compatibility and migration

- Purely additive. No existing table changes shape except the per-kind content
  hash noted above.
- No backfill of historical mutations: events that were never recorded are not
  invented. The ledger's first event per object is marked `reason='ledger-start'`
  so a reader can distinguish "no history" from "no changes".
- `memory_provenance` is retained and unchanged during P1; a follow-up may
  reduce it to a view over the ledger once coverage is proved, and that is
  explicitly not part of this proposal.

## Slices

| # | Scope | Done when |
|---|---|---|
| S1 | Table, closed operation set, writer API taking an open connection, actor resolution, unit tests | The writer refuses an unauthenticated actor and refuses an unknown operation |
| S2 | Semantic assertions: hook the fact commit / retract / expiry / promotion paths, add the guard trigger | Every semantic-edge change in the test suite produces exactly one event with a resolvable before/after ref |
| S3 | Prose memories: hook `memory_insert_ex`, `memory_update_content_as`, `memory_delete_as`, `memory_reject`, add the content hash and guard | A memory edit is reconstructible from events alone; the guard rejects a raw UPDATE |
| S4 | Documents and entities: doc ingest/re-ingest, version supersession, entity register/merge/unmerge, alias bind, rel_type and ontology decisions | An entity merge and an ontology promotion each emit a typed event with actor and reason |
| S5 | Background maintenance and measurement: curator drain, expiry/promotion sweeps, re-embed and repair paths; publish event-volume and storage growth per 10k mutations | No production mutation path lacks an emitter, proved by the coverage test; growth figures published in the PR |

## Acceptance

```yaml acceptance
- {id: 1, tier: mechanical, check: "the ledger writer rejects a caller-supplied principal or authority and resolves both from the authenticated context (new unit test over the P1 writer)"}
- {id: 2, tier: mechanical, check: "an unknown operation string and a purge event carrying a non-empty before/after ref are both refused, at the writer and by CHECK constraint"}
- {id: 3, tier: integration, check: "for each covered object kind, unhooking its emitter makes the mutation fail rather than succeed silently (per-kind guard trigger test)"}
- {id: 4, tier: integration, check: "a rolled-back transaction leaves neither the mutation nor its ledger event, proved against real PostgreSQL under the AIMEE_TEST_PG mode"}
- {id: 5, tier: integration, check: "a maintenance sweep that changes N objects emits N events sharing one correlation_id, with no interactive-path special-casing"}
- {id: 6, tier: integration, check: "an end-to-end turn produces a ledger event whose correlation_id matches the turn's retrieval_event turn_id, joining recall evidence to write evidence"}
```

## Status and supersession

Supersedes nothing. Extends the audit substrate recorded in
[auditable WORM audit store](../done/auditable-worm-audit-store.md) and
[governance decision records and action audit](../done/governance-decision-records-and-action-audit.md),
and supplies the event source the pending
[operator audit activity residual](operator-audit-activity-residual.md) needs to
render a complete activity view.
