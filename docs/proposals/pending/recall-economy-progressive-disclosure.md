# Proposal: Recall economy — progressive disclosure, bounded envelopes, and learned retrieval shortcuts

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter roles:** Recall, Rewrite (envelope assembly / compression),
  Extract (intent annotation), Gate-Promote (shortcut promotion),
  Calibrate / Evaluate-Optimize (per-layer transparency + token A/B).
- **Scope:** `src/server/ingress_preinject.c` (envelope assembly), existing
  config plumbing (`src/headers/config.h`, `src/config.c`,
  `src/config_fields.c`, `src/config_sections.c`), `src/server/kb_client_memory.c`
  + `src/kb/kb_service_memory.c` + `src/db2/kb_service_backend_memory.c`
  (preview/read contract for `memory.context_block` or sibling RPC), `server_mcp.c`
  + MCP tool metadata if pull-handles become id-addressable, `src/db2/memory_query.c`
  + `src/db2/schema.sql` (shortcut store + intent column, Phases 3–4), the
  curator enrichment pass (intent extraction, Phase 4), unit + integration tests,
  a token A/B harness, docs. No new long-lived service.

## Goal

Make Aimee's context pre-injection **cheaper per turn as the knowledge base
grows, not more expensive.** Today the recall machinery is strong — vector
search, full-text search, keyword match, and graph-code fusion all run and
blend into a ranked result. The weak point is the **last mile**: how that result
is packed into the `<aimee-context>` envelope that an external agent pays for on
every prompt. The code half of the envelope is already disciplined (short
snippets + follow-up tools). The memory half currently injects the rendered
`memory.context_block` string, which is internally capped by memory assembly but
still too large and too opaque for a per-turn ingress envelope. The ingress
envelope also has **no outer byte budget** that includes code snippets, memory,
wrapper, and footer together.

This proposal adopts four well-established information-retrieval principles and
wires them into Aimee's existing seams. None of them require new models or a new
service:

1. **Progressive disclosure** — surface a *preview* (title + headline) plus an
   id-addressable pull-handle, and let the agent fetch full content on demand.
   Inject what is needed to *decide*, not the whole record.
2. **Bounded assembly** — give the injected envelope an explicit byte budget, so
   a larger corpus produces a *better-ranked* envelope of the *same* size, never
   a bigger one.
3. **Learned shortcuts** — let recurring query→record mappings be cached and
   promoted so repeated lookups short-circuit the full blend.
4. **Intent annotation** — store an "what is this for / when to use it" phrasing
   on records and index it, so a query can match on *intent* without an
   embedding round-trip.

This is the retrieval-economy sibling of the context pre-injection work
(`docs/proposals/pending/context-preinjection-ingress.md`): that proposal makes
Aimee *pre-inject the right things*; this one makes the act of pre-injecting
*pay for itself at scale*.

## §0 What already exists (so we don't rebuild it)

The expensive, hard parts are done. Confirmed in the tree:

- **Staged retrieval ingredients are all present.** Vector search over pgvector
  (`src/db2/pgvec_transport.c`, `1.0 - (embedding <=> :qvec)` on an HNSW index);
  full-text search via generated `tsvector` columns —
  `memories_fts_tsv`, `memory_chunks_fts_tsv`, `code_fts_tsv`
  (`src/db2/schema.sql:219-269`) with `plainto_tsquery` / `ts_rank` /
  `ts_headline` already used in `src/db2/code_index.c`; keyword/ILIKE matching
  (`src/db2/memory_query.c`); and graph-code fusion
  (`src/memory_graph_fusion.c`, permanently on, #2721).
- **Rich per-result scoring** is already computed and explainable —
  `memory_score_parts_t` (`src/headers/memory.h`) carries `lexical`,
  `semantic`, `graph_score`, `code_proximity`, `salience`, `surprise`,
  `confidence`, `intent`, etc. **We do not need a flat "priority" field** —
  Aimee's signal set is already richer than that. Phase 4 makes the existing
  intent scoring surface durable and searchable; it does not invent a new
  ranking concept.
- **A preview store already exists.** `memory_summaries`
  (`src/db2/schema.sql:66`, `scope='headline'`, indexed at line 133) is exactly
  a per-record preview table. It is simply **not used by the envelope assembler.**
- **Follow-up retrieval already exists, but not yet id-addressable.** The
  envelope already advertises `explore-with: ... get_context_block`, and
  `get_context_block` resolves a query to a full rendered context block
  (`server_mcp.c` → `kb_client_memory_context_block`). It does **not** currently
  accept `memory_id` or a `memory:123` handle, so Phase 2 must add that contract
  (or expose an MCP `memory_get` equivalent) before previews can be true
  pull-handles.
- **A budget pattern already exists.** Session virtual-context assembly is
  byte-bounded by `virtual_context_assembly_budget` (default 4096,
  `src/config_fields.c:139`, enforced in `conversation_context.c`). The ingress
  envelope has **no equivalent**. Separately, `memory_assemble_context()` already
  has internal caps (`MAX_CONTEXT_TOTAL`, per-section budgets, optional
  `memory.context_budget` token mode), so the missing layer is an outer
  ingress-envelope budget, not the first budget anywhere in memory assembly.

So this proposal is **assembly discipline plus a preview/read contract and two
cheap stores**, not new retrieval.

### A note on architecture: blend, don't gate

A naive reading of "stage retrieval cheapest-first" is to *gate* — run the cheap
layer and skip the expensive one if it answers. **Aimee deliberately blends**
all layers into one ranked result (`memory_score_parts_t`), which yields better
recall quality than gating. This proposal does **not** change that. Progressive
disclosure here is about **what we inject**, not **what we compute** — we still
run the full blend; we just inject previews of its top results instead of full
bodies. The token win is in the envelope, not in skipped compute.

## Implementation contract for Phases 1–2

The high-value slice should be concrete before any default-on discussion:

- `ingress_preinject_assembly_budget` counts final UTF-8 bytes for the complete
  `<aimee-context>` envelope: opening tag, code section, memory previews, footer,
  truncation line, and closing tag.
- Assembly never splits a candidate mid-record. If the next candidate does not
  fit, it is skipped or the section stops, and truncation is explicit.
- Reserve footer space up front so `explore-with` and truncation metadata survive
  tiny budgets.
- Memory previews should be structured internally even if rendered as text:
  `id`, `handle`, `key`, `kind`, `tier`, `headline`, `score` when available,
  `updated_at`, `truncated`.
- A preview handle must be openable. Either extend `get_context_block` with
  `memory_id` / `handle`, or expose a sibling MCP tool backed by
  `memory.get`. Do not emit ids that the agent cannot resolve.
- Missing headline summaries fall back to a first-N-character preview, never the
  full body, and are marked `headline_missing=true` so the curator can backfill.
- Prompt-injection and sensitivity handling are part of the contract: preview
  text is escaped/sanitized for the envelope format, respects memory sensitivity
  and workspace scope, and is labelled as retrieved context rather than
  instructions.

## The gaps and the proposed changes

### Phase 1 — Bounded envelope (smallest, pure safety)

`ingress_preinject_build` (`src/server/ingress_preinject.c`) caps *items*
(≤6 code hits, ≤5 memory items) and *per-snippet* length (160 chars for code),
but never caps **total envelope bytes**. The memory block it calls is internally
capped, but the final ingress envelope can still exceed a desirable per-turn
budget once code snippets, memory, wrapper, and footer are combined.

- Add `ingress_preinject_assembly_budget` (int bytes, **default e.g. 6144**),
  mirroring `virtual_context_assembly_budget` exactly — same config plumbing
  (`config.h` / `config.c` / `config_fields.c` / `config_sections.c`), placed
  either next to the existing top-level `ingress_preinject_enabled` flag or under
  a new `ingress.preinject` section if that config surface is introduced.
- In `ingress_preinject_build`, assemble under the byte budget and **stop when
  the next complete candidate will not fit**, emitting a terminal `... (N more
  available via get_context_block)` line so truncation is visible to the agent
  rather than silent. (No silent caps — same discipline as the rest of the
  codebase.)
- Do not claim cross-source "highest ranked first" until code and memory return
  comparable scores. The first implementation can use fixed source bands
  (wrapper/footer reserve, code reserve, memory-preview reserve), then move to
  score-normalized packing when structured scores are exposed.

Isolated to the ingress assembler plus config plumbing. No recall behaviour
changes.

### Phase 2 — Progressive disclosure for the memory half (highest leverage)

The code half already previews. The memory half does not:
`ingress_preinject_build` calls
`kb_client_memory_context_block(query, "general", 5)` which returns the
rendered full context block straight into the envelope.

- Add a **preview variant** (e.g. `memory.context_block` gains a `preview=true`
  mode, or a sibling `memory.context_preview`) that returns, per hit:
  **key/title + the `headline`-scope row from `memory_summaries` + stable id +
  openable handle**, instead of full `content`. The summaries table already
  holds the headline; this is a read path, not a new write path.
- Extend the pull path so the handle actually opens the selected record. Today
  `get_context_block` accepts `query`, `block_type`, and `limit`; it does not
  accept `memory_id`. Phase 2 should either add `memory_id`/`handle` to that MCP
  tool and RPC, or add a sibling `memory_get`/`get_memory` MCP tool that returns
  the full body for `memory:123`.
- Where no headline summary exists yet for a record, fall back to a
  budget-trimmed first-N-chars preview (never the full body), and let the
  curator backfill the summary out of band.

This is the single change that most reduces tokens-per-turn and most de-risks
flipping `ingress_preinject_enabled` (`src/config_fields.c:42`, default-off)
to default-on.

### Phase 3 — Learned retrieval shortcuts (net-new, self-contained)

Aimee recomputes the full blend every turn; there is no exact-query cache and no
author-/agent-controlled shortcut. Recurring queries pay full price forever.
This phase must be treated as a retrieval optimization, not a Phase 1/2
precondition.

- Introduce a **retrieval-shortcut** record kind (a new `kind` value, or a small
  `retrieval_shortcuts` table keyed by normalized query) mapping a normalized
  query → an ordered set of record ids/handles, with a hit-count and last-used
  timestamp.
- Start in **shadow mode**: consult shortcuts before the blend, but still run the
  normal blend and record whether the shortcut would have agreed. Only after the
  shortcut agrees across a threshold should it become eligible to answer without
  the full blend.
- A promoted shortcut hit returns mapped previews directly (and still respects
  the Phase 1 budget). A miss, low-confidence hit, stale target, or scope
  mismatch runs the normal blend.
- **Gate-Promote**: a shortcut is only written after a query recurs and its
  result is stably top-ranked across runs (cheap counter + threshold), so noise
  doesn't ossify into a cache. Shortcuts decay/evict on staleness or when the
  target record version changes. Current `memories` rows have `updated_at` and
  artifact-backed rows have `artifact_hash`; if content-hash invalidation is
  required for all records, add an explicit digest/version column.

Self-contained behind the recall entry; if the store is empty, behaviour is
identical to today.

### Phase 4 — Intent annotation (make the existing intent signal durable)

Aimee's scoring struct already has an `intent` slot, and records carry `kind`,
`tier`, entities, and summaries. What is missing is a durable record field that
captures **use intent** ("how to do X / when to use Y") and feeds the lexical
index. That field lets a query match on purpose without an embedding round-trip,
and it is one of the cheapest useful signals to add to FTS.

- Add an optional `use_cases` / intent text field to the record schema
  (`src/db2/schema.sql`), and extend the existing generated `tsvector` so intent
  text is full-text searchable alongside `key`/`content`.
- Populate it from the **deep-curator enrichment pass that already runs an LLM
  over records** (entity/narrative/claim extraction) — this is an added Extract
  output, not a new pipeline. Backfill is incremental and non-blocking.
- Recall's lexical layer gets the intent column for free via the existing
  `tsvector @@ plainto_tsquery` path.
- Keep intent additive. It can boost lexical recall, but it must not replace
  content, citations, scope checks, or contradiction handling.

Touches the curator + schema, so it ships last.

### Bonus (near-free) — per-layer transparency

The envelope already carries a `confidence="…"` attribute. Extend the preview
or recall response with `method` (shortcut / lexical / vector / graph-fusion /
hybrid), `latency_ms`, `truncated`, `budget_bytes`, `used_bytes`,
`omitted_count`, and `headline_missing_count` so the token A/B harness can
attribute both **token tax** and **answer quality** per layer. This is the
Calibrate/Evaluate-Optimize hook that lets us prove each phase pays off before
flipping defaults.

## Phasing & ordering

| Phase | Change | Blast radius | Risk |
|------|--------|-------------|------|
| 1 | Envelope byte budget | `ingress_preinject.c` + config field | low |
| 2 | Memory preview-then-pull | ingress + kb client/service + MCP pull contract | low/medium |
| 3 | Learned shortcuts | recall entry + small store + shadow validation | medium |
| 4 | Intent annotation | schema + curator Extract output | medium (schema + enrichment) |

Phases 1 and 2 together are the high-value slice: Phase 1 is pure envelope
safety, and Phase 2 adds the preview/read contract needed for true progressive
disclosure. Together they turn pre-injection from a token liability into the
asset the feature is meant to be. 3 and 4 compound the win but are independently
shippable.

## Testing & validation

- **Unit:** budget enforcement (envelope never exceeds N bytes including wrapper
  and footer; truncation line present; no candidate is split); tiny-budget,
  exact-fit, UTF-8, no-code, no-memory, and KB-unavailable cases; preview variant
  returns headline+id+openable handle and never full body; fallback preview marks
  missing headlines; shortcut store hit/miss/promote/evict/stale-target; intent
  column flows into the FTS query.
- **Integration:** a recall round-trip where the agent opens a previewed record
  via the advertised handle and gets the full body; sensitivity/workspace-scoped
  memories do not leak into unrelated envelopes.
- **Token A/B harness:** measure tokens-injected-per-turn and answer correctness
  on a fixed query set, before vs after each phase, using the existing benchmark
  suite. The acceptance bar for flipping `ingress_preinject_enabled` default-on
  is *lower p95 injected bytes at equal-or-better correctness*, with no increase
  in false omissions where the answer required a full body that the preview did
  not cause the agent to open. Deploy/bench runs are user-gated.

## Non-goals / risks

- **Not** changing the blend to a gate — recall quality stays as-is (§0).
- **Not** adding a flat `priority` int — Aimee's score set already subsumes it.
- **Preview staleness:** progressive disclosure assumes the headline summary
  faithfully represents the body. Mitigated by tying summary freshness to record
  version/hash and falling back to a trimmed body preview when no summary exists.
- **Shortcut ossification:** mitigated by Gate-Promote (recurrence + stability
  threshold), shadow-mode validation, scope checks, and version/staleness
  eviction.
- **Intent drift:** an LLM-authored intent phrasing can be wrong; it is an
  additive lexical signal, never authoritative, and never replaces content.
- **Prompt injection / sensitive memory leakage:** previews are retrieved data,
  not instructions. Escape envelope markup, label retrieved text clearly, and
  apply existing sensitivity/workspace filters before any preview enters the
  high-authority ingress prompt.
