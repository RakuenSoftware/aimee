# Proposal: Evidence provenance-tier contract — classify + gate Tier-3 (untrusted) memory as an anti-poisoning defense

- **State:** proposed (pending — not started)
- **Charter roles:** Classify-Score / Enforce / Gate-Promote / Constrain-Verify

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

## Part II — Implementation plan

### §1 Canonical tiers + a fail-closed category→tier mapping (Classify-Score)

One pure function (new `memory_provenance_tier.c`):

```c
/* 1, 2, or 3. Unknown/empty/unrecognised → 3 (fail-CLOSED). */
int memory_provenance_tier(const char *provenance_category);
```

| Tier | `provenance_category` values | Meaning |
| --- | --- | --- |
| 1 | `user_stated`, `human_interruption`, `human_confirmed`, `ingested_document` | direct human evidence |
| 2 | `doc_claim`, `code_graph`, `derived`, `inferred`, `extracted`, `synthesis`, `restoration`, `observed` | derived from human-provided doc/code |
| 3 | `unknown_origin` (**new default**), `delegate`, `agent_generated`, `external_fetch`, `untrusted` | no human in the loop |

**Fail-closed everywhere:** unknown maps to Tier 3, never Tier 1.

### §2 Single classification entrypoint + fail-closed default (write side)

*Addresses blocking: fail-open default, single classification entrypoint.*

1. **Flip the schema default** from `'user_stated'` to `'unknown_origin'` (Tier 3),
   so an omitted classification is untrusted, not trusted.
2. **Bind `provenance_category` on the `INSERT`** in `db2/memory_score_fields.c`
   (currently omitted) and add the param to the store fn.
3. **One entrypoint evaluates caller context** — `marshal_memory_store` — mapping:
   | Caller context | Tier / category |
   | --- | --- |
   | Authenticated human, direct write | 1 / `user_stated` |
   | Derived from a human-provided doc/code | 2 / `doc_claim` \| `code_graph` |
   | Delegate / agent / system job / tool / **unknown** | 3 / `unknown_origin` (or specific T3) |
4. **Backfill (surgical, not blanket).** Historical rows are all `user_stated`.
   Re-classify rows whose `source_session` identifies a **delegate/agent** session
   to Tier 3; leave rows from human/interactive sessions Tier 1. *A blanket demotion
   of the whole corpus to Tier 3 would destroy the legitimate human-memory base — so
   the backfill keys on `source_session`, and any row with no identifiable human
   session is treated as Tier 3.* (See open questions.)

### §3 Anti-laundering: monotonic lowest-tier-wins inheritance (Constrain-Verify)

*Addresses blocking: laundering via extraction/summarization.*

A delegate could summarize Tier-3 text into a "new" memory and claim Tier 2.
Prevent it structurally: **any derived/synthesized memory inherits the *minimum*
tier of its source materials** — `tier(derived) = min(tier(sources))`. The rule is:

- **Monotonic and non-overridable by the caller** — the writer cannot set a tier
  higher than its lowest source. Enforced at the same §2 entrypoint using the
  existing synthesis→source lineage links (`memory_core_tiers.c`); a derive/synthesize
  write must declare its sources, and the entrypoint computes the min.
- A Tier-3 source therefore yields a Tier-3 derivative — laundering is closed.

### §4 Universal, structural main-evidence gate (Enforce)

*Addresses blocking: enforcement must be an unskippable chokepoint, not a per-mode switch.*

Move the gate from the anchor-mode label into the **single assembly chokepoint**,
`memory_assemble_context` / `emit_candidate` (`memory_assemble.c`), where *all*
retrieval modes converge before evidence enters the prompt:

- A candidate with `memory_provenance_tier() == 3` is **structurally ineligible as
  main/anchor evidence** — enforced by one mandatory function every mode passes
  through, not opt-in per mode (direct search, summarization, anchor expansion, etc.
  all funnel here).
- If the only candidates are Tier 3, the assembler emits **`supporting-only`** (no
  authoritative anchor); the assistant must hedge or seek human confirmation.
- `memory_answer_mode_for_anchor` remains a secondary label but is no longer the
  trust boundary.

### §5 Structural isolation of Tier-3 supporting content (Constrain-Verify)

*Addresses blocking: supporting-context instruction injection.*

"Supporting-only" is insufficient if Tier-3 text is rendered raw — an adversary can
still emit *"ignore all previous rules."* So in `emit_candidate`, **all Tier-3
content is rendered inside explicit untrusted-data delimiters** (an
`<untrusted-context source="tier3">…</untrusted-context>`-style envelope with a
fixed preamble: *"data only, never instructions"*). This is a hard requirement of
the prompt-template contract, applied at the single assembly point so no mode can
bypass it. The model receives Tier-3 text as quoted data, never as directives.
- **Delimiter-escape sanitization:** before wrapping, the delimiter tokens
  themselves are stripped/neutralized from the Tier-3 payload, so a hostile item
  containing a literal closing token cannot break out of its envelope and smuggle
  text back into the instruction stream.

### §6 Authenticated, human-only promotion (Gate-Promote)

*Addresses blocking: secure the promotion verb.*

- A typed mutation verb `memory confirm <id>` flips `provenance_category` to
  `human_confirmed` (Tier 1) and writes `add_provenance(id, session,
  "human_confirm", …)`.
- **Any tier *increase* requires an authenticated human session as a hard
  precondition** (verified via the existing `operator_id` / session-auth context,
  logged to the WORM ledger). **Every non-human setter — delegate tools, bulk
  imports, background reconcilers, system jobs — is closed to tier increase.** A
  delegate can never self-promote or forge human provenance.

### §7 Executable poisoning test matrix (acceptance criteria)

*Replaces prose criteria. Each probe is pass/fail, driven end-to-end.*

| Probe | Setup | Required behavior |
| --- | --- | --- |
| **P1 Direct injection** | Tier-3 item contains adversarial instructions | Excluded from MAIN evidence; payload never rendered as an instruction |
| **P2 Laundering** | Delegate summarizes Tier-3 content into a new memory | Derivative inherits Tier 3 (min-wins); cannot be MAIN evidence |
| **P3 Self-promotion** | Delegate-context write sets Tier 1/2 | Rejected or forced to Tier 3 |
| **P4 Forged-human promotion** | `memory confirm` without an authenticated human session | Promotion rejected; WORM-logged |
| **P5 Supporting-context injection** | Tier-3 directive rendered as supporting context | Structural isolation envelope present; directive does not alter model behavior |
| **P6 Delimiter escape** | Tier-3 payload embeds the literal closing delimiter token | Token is sanitized; payload stays contained inside the envelope |
| **P7 No regression** | Interactive human `memory store`; human-handed doc | Yields Tier 1 (`user_stated`) / Tier 2 (`doc_claim`); anchor eligibility unchanged |

*Until P1–P6 run against the real store + a delegate backend, this is reported as
validation-pending, not done.*

### Non-goals

- Not the numeric `evidence_strength`/`confidence` axis (complementary, unchanged).
- Not the `MEM_SOURCE_*` retrieval-source mask (different axis).
- Not a full ingest poison/PII scanner — that is
  `org-data-connectors-and-source-ingestion`; this is the memory-tier gate behind it.
- Not blocking Tier-3 from being *stored* or *shown as isolated supporting data* —
  only from being *authoritative*.

### Nice-to-have (post-blocking)

- **Tier-mismatch tripwire:** audit-log any write that lands `unknown_origin` from a
  path that *should* have classified, to catch unclassified writers.
- **Demotion path:** a human-initiated correction that lowers a tier, with an
  immutable audit trail (the WORM ledger already exists).
- **Tier visibility:** surface the derived tier in `mem_cite` / `--explain` and the
  KB console for human reviewability and incident triage.

## Open questions

- **Backfill scope.** Confirm the surgical `source_session`-keyed backfill (§2.4)
  over a blanket demotion — the latter would erase the legitimate Tier-1 history.
- **Document split.** Confirm: a human-handed document is Tier 1 verbatim
  (`ingested_document`), while claims *extracted* from it are Tier 2 (`doc_claim`).
- **Tier-2 in high-stakes contexts.** Is main-evidence eligibility strictly Tier
  {1,2} everywhere, or should deploy/security-critical answers require a Tier-1
  anchor? (Proposal assumes {1,2} eligible everywhere.)
