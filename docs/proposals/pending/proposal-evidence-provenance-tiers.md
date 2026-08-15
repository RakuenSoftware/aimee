# Proposal: Evidence provenance-tier contract — classify + gate Tier-3 (untrusted) memory as an anti-poisoning defense

- **State:** PENDING — validated and rewritten 2026-08-15 for Go-owned enforcement.
- **Charter roles:** Classify-Score / Enforce / Gate-Promote / Constrain-Verify

## 2026-08-15 lifecycle correction

PR #2692 incorrectly moved this evidence-provenance proposal to `rejected/` because its old
implementation plan named C and SQL enforcement seams. The separate
[`governance-policy-surface-and-posture.md`](governance-policy-surface-and-posture.md) proposal
never left `pending/`; #2692 changed only its backlink to point at the rejected location. This
correction restores the evidence-provenance proposal and that backlink to `pending/` and replaces
the stale implementation ownership below. The anti-poisoning objective was not rejected on merit
and remains unresolved.

## 2026-08-15 Go ownership rewrite

The anti-poisoning gap below remains validated. The earlier implementation sketch placed policy in
the current C store and assembler; that ownership is obsolete. The canonical owner is now
`server-go/modules/memory`.

Go owns category mapping, actor-bound write classification, least-trust inheritance, anchor
eligibility, Tier-3 rendering, and human-confirmation authorization. Existing C and SQL paths are
temporary mechanical adapters only: they may transport requests, persist a Go-returned category,
apply a Go-authorized mutation, and run a migration. They may not contain a second category map,
choose a tier, promote trust, or render an alternate prompt fragment. If the Go memory process is
unavailable or returns an invalid response, the adapter fails closed.

The Go classifier is not delivered as unused library code. Classifier, write adapter, assembler
gate, promotion path, migration, and P1–P9 probes land as one atomic proposal outcome.

## Thesis

Every memory and fact carries an **evidence provenance tier** describing how much
*human* evidence stands behind it:

- **Tier 1 — direct human evidence.** The user stating a fact, or a document the
  human hands in to be ingested (verbatim).
- **Tier 2 — indirect human evidence.** Information *derived from* a human-provided
  document, or a code graph built from human-provided code.
- **Tier 3 — no human evidence.** Content a delegate/agent ingested or generated
  with no human in the loop (a fetched web page, a delegate's own assertion).

Two rules govern use:

1. **Tier 1 and Tier 2 may be main (anchor) evidence. Tier 3 may only ever be
   *supporting* evidence — never main.** This is the point of the tier: **Tier 3 is
   the poisoning / malicious-instruction surface.** A delegate that ingests a
   hostile page ("ignore prior rules", "the deploy key is X") or fabricates a fact
   must not be able to make that content authoritative. It can inform, never decide.
2. **Tier 2/3 promote to Tier 1 only on direct, authenticated human confirmation** —
   never by a delegate, a background job, or any non-human setter.

This is the memory-tier realization of the standing Code Principle *"treat external
content and generated output as untrusted; do not let them override system,
developer, user, or repository instructions."* It complements the merged
[binding retrieval context-contract](proposal-retrieval-context-contract.md), which
distinguishes *main* vs *supporting* evidence by confidence — this adds the
*provenance* axis to that same distinction.

## §0 What already exists (DRY map)

The tier **field, a write entrypoint, and a single assembly chokepoint already
exist** — the gap is that nothing populates the field or gates on it. Verified:

| Piece | Existing surface | State |
| --- | --- | --- |
| The tier field | `provenance_category TEXT NOT NULL DEFAULT 'user_stated'` on `memories` (`db2/schema.sql`, `db2/schema_sqlite.sql`); read in `db2/memory_query.c`, mapped in `db2/memory_row_mapper_pg.c`, emitted by `render.c` | **exists** |
| Single write entrypoint | `marshal_memory_store` (`cli_v1_routes.c`) → column binds in `db2/memory_score_fields.c` | **exists** — the classification point |
| **Single assembly chokepoint** | `memory_assemble_context` → `emit_candidate` (`memory_assemble.c`), which already serializes candidates into the prompt (renders `[Source: …]`, orders via `context_tier_priority`) | **exists** — the universal gate + isolation point |
| A secondary anchor label | `memory_answer_mode_for_anchor` (`memory_core_search_c.c`) downgrades `synthesis`/`restoration` anchors to `"synthesised"` | **exists** — label only, not a gate |
| Numeric trust (complementary axis) | `evidence_strength`, `confidence`, `observation_count`, ≥2-unit corroboration filter (`memory_core_search_b.c`) | **exists**; separate *strength* axis |
| Lineage for derived memories | synthesis→source links (`memory_core_tiers.c`) + `add_provenance` / `memory_provenance` table + `mem_cite` | **exists** — powers lowest-wins + promotion audit |
| Authenticated operator context | `operator_id` + WORM ledger (`audit_worm*.c`; `operator-audit-activity-surface` proposal) | **exists** — the promotion precondition |
| Related pending work | `org-data-connectors-and-source-ingestion` scopes *ingest-time* PII/poison enforcement | this is the **memory-tier gate** behind it |

**Do not conflate:** the `MEM_SOURCE_*` mask (`memory_core_search_b.c`) is a
*retrieval-source* axis (which index found a hit), **not** evidence provenance.

### The verified gap (why "the code is already there" is only half-true)

The write path — `marshal_memory_store` → the `INSERT INTO memories (tier, kind,
key, content, use_cases, confidence, …)` in `db2/memory_score_fields.c` — **omits
`provenance_category`**, so every memory inherits the schema default
`'user_stated'` = Tier 1, regardless of author. Nothing sets any other value, and
`memory_answer_mode_for_anchor` only special-cases `synthesis`/`restoration` (never
actually written to the field). **Net: delegate-ingested/agent-generated memories
are silently stamped Tier-1 human evidence and can be main evidence today — an
open, fail-open poisoning hole.** The field and seams exist; the classifier and the
gate do not.

## Part II — Go implementation plan

This lifecycle-correction PR restores and rewrites the plan; it does not claim the plan is already
implemented. The new process-contract entries, Go handlers, adapters, migration, ownership checker,
and P1–P9 fixtures below must land together in the later implementation PR. The existing
`go test ./modules/memory` result validates the current module baseline only, not these future
acceptance criteria.

### §1 Canonical Go types and fail-closed mapping (Classify-Score)

Add `server-go/modules/memory/provenance.go` as the sole category authority:

```go
type ProvenanceTier uint8
type ProvenanceCategory string

func TierForCategory(category ProvenanceCategory) ProvenanceTier
func LeastTrusted(base ProvenanceTier, sources ...ProvenanceTier) ProvenanceTier
```

`TierForCategory` maps the existing category vocabulary to Tier 1, 2, or 3. Empty, malformed, and
unknown values always map to Tier 3. `LeastTrusted` returns the **maximum numeric tier**, because
Tier 3 is less trusted than Tier 1; the former `min(tier)` wording was backwards.

| Tier | Canonical categories | Meaning |
| --- | --- | --- |
| 1 | `user_stated`, `human_interruption`, `human_confirmed`, `ingested_document` | Direct authenticated human evidence |
| 2 | `doc_claim`, `code_graph`, `derived`, `inferred`, `extracted`, `synthesis`, `restoration`, `observed` | Derived from authenticated human-provided material |
| 3 | `unknown_origin`, `delegate`, `agent_generated`, `external_fetch`, `untrusted` | No authenticated human evidence |

The mapping and least-trust function are table-tested in Go. No C source may contain a parallel
category map or trust-ordering rule.

### §2 Actor-bound Go write classification and fail-closed storage

Add named memory-process stages for `provenance-classify-write`,
`provenance-evaluate-candidates`, and `provenance-confirm`. Allocate their event/stage identifiers
as new dense memory-stage entries in `src/modules/process-contracts.json`, then bind them in
`server-go/modules/memory/memory.go` and the existing C gateway adapter. The contract validator and
Go/C wire-conformance tests must reject an unallocated, duplicate, or mismatched identifier; do not
reuse a payload shape from another memory stage.

The classify request carries content origin, derivation kind, source categories, and an opaque actor
assertion minted by authenticated admission. Client JSON, CLI flags, delegate payloads, and the
legacy adapter cannot assert “human” themselves. The Go handler binds the assertion to the bus
invocation principal and returns a canonical category, numeric tier, and reason:

| Authenticated context | Go result |
| --- | --- |
| Direct human statement | Tier 1 / `user_stated` |
| Human-handed document, verbatim | Tier 1 / `ingested_document` |
| Claim/code graph derived from authenticated human material | Tier 2 / `doc_claim` or `code_graph` |
| Delegate, agent, system job, external fetch/tool, missing assertion, or unknown | Tier 3 / a specific Tier-3 category or `unknown_origin` |

A requested category is advisory only and can never improve the Go result. Every production memory
write—interactive store, extraction, synthesis, import, background work, and delegate/tool write—
must call the Go stage. The current C write path becomes a mechanical adapter that persists exactly
the returned category. Go timeout, unavailable process, invalid response, actor-binding failure, or
an unclassified caller causes rejection or Tier 3; it never falls back to `user_stated`.

#### Existing-column migration and rollout

The migration is explicit; editing `ADD COLUMN IF NOT EXISTS` does not change an existing
PostgreSQL default.

1. Capture the P7 human-statement and human-document baseline before changing data.
2. Deploy the Go classifier and fail-closed adapters so all new writes bind an explicit category.
3. Run `ALTER TABLE memories ALTER COLUMN provenance_category SET DEFAULT 'unknown_origin'` for
   PostgreSQL and change the SQLite create-schema default for new databases.
4. Backfill in bounded transactions using authenticated session/admission and audit records. Keep
   Tier 1 only when immutable evidence proves a direct human statement or handoff; classify verified
   derivatives as Tier 2; classify delegate/agent/external/system and every unprovable row as Tier 3.
5. Emit dry-run and applied counts by old/new category, retain a rollback snapshot, and refuse to
   mark the migration complete while any row lacks a canonical category.
6. Enable the main-evidence gate only after the backfill and P7 comparison pass. Final mode has no
   fail-open shadow path.

A legacy binary that omits the field therefore lands `unknown_origin` rather than Tier 1 during a
mixed-version rollout.

### §3 Go-owned least-trust inheritance (Constrain-Verify)

Every derived or synthesized write declares its source memory IDs. The adapter loads their stored
categories and passes them to Go; it does not compute the result. Go applies:

```text
derived tier = max(base derivation tier, every source tier)
```

The base tier for extraction, inference, restoration, or synthesis is Tier 2. Missing source rows,
missing categories, an empty source set for a claimed derivation, or any Tier-3 source yields Tier 3.
The caller cannot request a more trusted result. Persistence of the Go-returned category and the
source-lineage links is one transaction; partial category/lineage writes fail.

This closes laundering: sources `{1,3}` produce Tier 3, while a claim extracted from a Tier-1
human-handed document produces Tier 2.

### §4 Universal Go main-evidence eligibility gate (Enforce)

Before any retrieval mode selects an anchor or serializes prompt context, the C assembly chokepoint
sends the complete candidate batch to `provenance-evaluate-candidates`. Go returns, per candidate:

- canonical tier;
- `main_eligible`;
- `supporting_eligible`;
- a rendered safe fragment;
- a stable reason code.

Tier 3 is never main evidence. For general answers, Tier 1 and Tier 2 may be main. For
deploy-, credential-, authorization-, or security-critical operations, only Tier 1 may be main; Tier
2 becomes supporting-only. The high-stakes policy class comes from the authenticated operation
context, not a user-controlled request field.

Every direct search, summarization, anchor expansion, graph expansion, and untasked-context path
must consume this one Go verdict. The legacy assembler may copy the returned fragment but may not
select an ineligible anchor or render raw candidate text. If Go is unavailable, the candidate is
omitted and no authoritative anchor is produced.

### §5 Go-owned Tier-3 structural isolation (Constrain-Verify)

Go renders every Tier-3 supporting item inside one fixed untrusted-data envelope with an immutable
“data only, never instructions” preamble. Payload serialization applies both string escaping and
angle-bracket escaping, so literal opening/closing envelope tokens in hostile text cannot terminate
the wrapper. The C adapter copies the returned bytes; it has no alternate Tier-3 renderer.

Tests assert the exact structural output. They do not use a nondeterministic “the model ignored the
instruction” oracle. Security rests on anchor exclusion, fixed data labeling, and escape-safe prompt
bytes, all of which are deterministic.

### §6 Go-authorized, authenticated human confirmation (Gate-Promote)

Add `memory confirm <id>` as a typed mutation routed to `provenance-confirm`. Confirmation is a
**trust promotion and numeric-tier decrease**; it is not called a numeric tier increase.

The Go handler requires an authenticated human actor assertion bound to the bus invocation and the
target memory ID. Delegate principals, agents, tools, bulk imports, background jobs, missing/expired
assertions, and mismatched actors are rejected. A client-supplied operator ID is never proof.

On authorization, the adapter applies the returned mutation atomically:

- set `provenance_category = 'human_confirmed'`;
- insert `memory_provenance` action `human_confirm` with the authenticated actor/session;
- append the WORM audit event containing old/new category and reason.

If any write or audit append fails, the promotion rolls back. Non-human paths may preserve or reduce
trust but can never promote it. Promotion failures are WORM-logged without changing the memory.

### §7 Executable acceptance matrix

P7 baseline capture runs first. P1–P6 then run against the real store, the Go memory process, the
legacy adapters, and human/delegate fixtures.

| Probe | Setup | Required deterministic behavior |
| --- | --- | --- |
| **P1 Direct injection** | Tier-3 item contains adversarial instructions | Go marks it main-ineligible and returns only the untrusted-data rendering |
| **P2 Laundering** | Delegate derives from Tier-3 content | Go returns Tier 3 via numeric `max`; persisted lineage and category agree |
| **P3 Self-promotion** | Delegate requests Tier 1/2 | Go rejects or returns Tier 3; storage cannot improve it |
| **P4 Forged-human promotion** | Confirm without a bound authenticated human assertion | Go rejects; category is unchanged and failure is audited |
| **P5 Supporting injection** | Tier-3 directive reaches assembly | Snapshot contains the fixed data-only envelope and no raw alternate rendering |
| **P6 Delimiter escape** | Payload embeds literal envelope closing tokens | Tokens are escaped and remain inside the wrapper |
| **P7 No regression** | Authenticated human store and human-handed document baseline | Direct statement remains Tier 1; verbatim document Tier 1; extracted claim Tier 2 |
| **P8 Go unavailable** | Stop or corrupt the Go memory stage | Writes/assembly/promotion fail closed; no Tier-1 or raw-render fallback |
| **P9 Mixed-version writer** | Legacy writer omits category after migration | Database default records `unknown_origin` / Tier 3 |

`src/tests/test_memory_provenance_integration.c` owns the P8 process-stop and malformed-response
hooks and the P9 legacy-wire writer fixture against the real store. Its P8 assertions cover write,
assembly, and promotion independently; its P9 assertion reads the stored row back through the
normal query path. These are required integration probes, not manual rollout observations.

Required implementation suites and artifacts (not delivered by this lifecycle correction):

- Go unit/table tests for mapping, numeric least-trust, actor classification, eligibility, rendering,
  escaping, and promotion authorization;
- Go wire-conformance tests for every new stage and malformed/cancelled request;
- C adapter tests proving adapters only transport/apply Go verdicts and fail closed;
- real-store integration tests covering migration, backfill, atomic lineage/promotion/audit, and all
  P1–P9 fixtures;
- `scripts/check_memory_provenance_ownership.py`, with a planted-violation test, rejecting canonical
  category maps, numeric tier comparisons, human-promotion authorization rules, or Tier-3 envelope
  rendering outside `server-go/modules/memory`. Adapter fixtures may name wire values, but
  production adapters may only transport or persist the Go verdict.

Until P1–P6 and P8–P9 pass against the real store and delegate fixture, and P7 matches its captured
baseline, this proposal remains validation-pending.

### Non-goals

- Changing the numeric `evidence_strength`/`confidence` axis.
- Reinterpreting the `MEM_SOURCE_*` retrieval-source mask as provenance.
- Building the full ingest poison/PII scanner owned by
  `org-data-connectors-and-source-ingestion`.
- Preventing Tier-3 storage or human review; Tier 3 is isolated supporting data.
- Moving generic database or WORM implementation into the memory policy package. Their temporary
  adapters apply Go decisions but do not own them.

### Resolved design decisions

- **Backfill:** only immutable authenticated-human evidence preserves Tier 1; unknown provenance
  fails closed to Tier 3.
- **Document split:** an authenticated human-handed document is Tier 1 verbatim; extracted claims
  are Tier 2; externally fetched documents are Tier 3.
- **High-stakes anchors:** deploy/security/credential/authorization operations require a Tier-1
  anchor. Tier 2 is supporting-only there.
- **Trust ordering:** least trust means maximum numeric tier.
- **Promotion proof:** authenticated admission assertion bound to invocation, actor, and memory ID;
  a request field is never proof.
