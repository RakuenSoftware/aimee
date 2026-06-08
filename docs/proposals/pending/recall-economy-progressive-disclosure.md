# Proposal: Recall economy — progressive disclosure, bounded envelopes, and learned retrieval shortcuts

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter roles:** Recall, Rewrite (envelope assembly / compression),
  Extract (intent annotation), Gate-Promote (shortcut promotion),
  Calibrate / Evaluate-Optimize (per-layer transparency + token A/B).
- **Scope:** `src/server/ingress_preinject.c` (envelope assembly — the only
  behaviour change for Phases 1–2), `src/headers/config.h` +
  `src/config_fields.c` + `src/config_sections.c` (one new budget field),
  `src/server/kb_client_memory.c` (`memory.context_block` → headline-preview
  variant), `src/db2/memory_query.c` + `src/db2/schema.sql` (shortcut store +
  intent column, Phases 3–4), the deep-curator enrichment pass (intent
  extraction, Phase 4), unit + integration tests, a token A/B harness, docs. No
  new long-lived service.

## Goal

Make Aimee's context pre-injection **cheaper per turn as the knowledge base
grows, not more expensive.** Today the recall machinery is strong — vector
search, full-text search, keyword match, and graph-code fusion all run and
blend into a single ranked result. The weak point is the **last mile**: how that
result is packed into the `<aimee-context>` envelope that an external agent pays
for on every prompt. The code half of the envelope is already disciplined (short
snippets + pull-handles); the **memory half injects full bodies**, and the
envelope as a whole has **no byte budget**. As the corpus scales, the envelope
scales with it — the exact opposite of what a retrieval layer should do.

This proposal adopts four well-established information-retrieval principles and
wires them into Aimee's existing seams. None of them require new models or a new
service:

1. **Progressive disclosure** — surface a *preview* (title + headline) plus a
   stable pull-handle, and let the agent fetch full content on demand. Inject
   what is needed to *decide*, not the whole record.
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
  `confidence`, etc. **We do not need a flat "priority" field** — Aimee's signal
  set is already richer than that.
- **A preview store already exists.** `memory_summaries`
  (`src/db2/schema.sql:66`, `scope='headline'`, indexed at line 133) is exactly
  a per-record preview table. It is simply **not used by the envelope assembler.**
- **Pull-handles already exist.** The envelope already advertises
  `explore-with: ... get_context_block` and `get_context_block` already resolves
  a record to its full body (`server_mcp.c` → `kb_client_memory_context_block`).
- **A budget pattern already exists.** Session virtual-context assembly is
  byte-bounded by `virtual_context_assembly_budget` (default 4096,
  `src/config_fields.c:139`, enforced in `conversation_context.c`). The ingress
  envelope has **no equivalent.**

So this proposal is **assembly discipline plus two cheap stores**, not new
retrieval.

### A note on architecture: blend, don't gate

A naive reading of "stage retrieval cheapest-first" is to *gate* — run the cheap
layer and skip the expensive one if it answers. **Aimee deliberately blends**
all layers into one ranked result (`memory_score_parts_t`), which yields better
recall quality than gating. This proposal does **not** change that. Progressive
disclosure here is about **what we inject**, not **what we compute** — we still
run the full blend; we just inject previews of its top results instead of full
bodies. The token win is in the envelope, not in skipped compute.

## The gaps and the proposed changes

### Phase 1 — Bounded envelope (smallest, pure safety)

`ingress_preinject_build` (`src/server/ingress_preinject.c`) caps *items*
(≤6 code hits, ≤5 memory items) and *per-snippet* length (160 chars for code),
but never caps **total envelope bytes**. A growing corpus or a verbose memory
record can produce an arbitrarily large envelope.

- Add `ingress_preinject_assembly_budget` (int bytes, **default e.g. 6144**),
  mirroring `virtual_context_assembly_budget` exactly — same config plumbing
  (`config.h` / `config_fields.c` / `config_sections.c` under the existing
  ingress config object), same enforcement shape.
- In `ingress_preinject_build`, assemble highest-ranked-first and **stop when
  the budget is reached**, emitting a terminal `… (N more available via
  get_context_block)` line so truncation is visible to the agent rather than
  silent. (No silent caps — same discipline as the rest of the codebase.)

Isolated to one file plus one config field. No recall behaviour changes.

### Phase 2 — Progressive disclosure for the memory half (highest leverage)

The code half already previews. The memory half does not:
`ingress_preinject_build` calls
`kb_client_memory_context_block(query, "general", 5)` which returns the **full
text** of up to five memories straight into the envelope.

- Add a **preview variant** (e.g. `memory.context_block` gains a
  `preview=true` mode, or a sibling `memory.context_preview`) that returns, per
  hit: **title/key + the `headline`-scope row from `memory_summaries` + the
  stable memory id**, instead of full `content`. The summaries table already
  holds the headline; this is a read path, not a new write path.
- The envelope already names `get_context_block` in its `explore-with:` line, so
  the pull-handle is already advertised — the agent fetches the full body only
  for the records it actually opens.
- Where no headline summary exists yet for a record, fall back to a
  budget-trimmed first-N-chars preview (never the full body), and let the
  curator backfill the summary out of band.

This is the single change that most reduces tokens-per-turn and most de-risks
flipping `ingress_preinject_enabled` (`src/config_fields.c:42`, default-off)
to default-on.

### Phase 3 — Learned retrieval shortcuts (net-new, self-contained)

Aimee recomputes the full blend every turn; there is no exact-query cache and no
author-/agent-controlled shortcut. Recurring queries pay full price forever.

- Introduce a **retrieval-shortcut** record kind (a new `kind` value, or a small
  `retrieval_shortcuts` table keyed by normalized query) mapping a normalized
  query → an ordered set of record ids/handles, with a hit-count and last-used
  timestamp.
- The recall path consults shortcuts **before** the blend; a hit returns the
  mapped previews directly (and still respects the Phase 1 budget). A miss runs
  the normal blend.
- **Gate-Promote**: a shortcut is only written after a query recurs and its
  result is stably top-ranked across runs (cheap counter + threshold), so noise
  doesn't ossify into a cache. Shortcuts decay/evict on staleness or when the
  target record changes hash.

Self-contained behind the recall entry; if the store is empty, behaviour is
identical to today.

### Phase 4 — Intent annotation (one metadata field the model genuinely lacks)

Aimee records carry `kind`, `tier`, entities, and summaries — but nothing that
captures **intent** ("how to do X / when to use Y"). That is precisely the field
that lets a query match on purpose without an embedding round-trip, and it is
the cheapest possible signal to FTS.

- Add an optional `use_cases` / intent text field to the record schema
  (`src/db2/schema.sql`), and extend the existing generated `tsvector` so intent
  text is full-text searchable alongside `key`/`content`.
- Populate it from the **deep-curator enrichment pass that already runs an LLM
  over records** (entity/narrative/claim extraction) — this is an added Extract
  output, not a new pipeline. Backfill is incremental and non-blocking.
- Recall's lexical layer gets the intent column for free via the existing
  `tsvector @@ plainto_tsquery` path.

Touches the curator + schema, so it ships last.

### Bonus (near-free) — per-layer transparency

The envelope already carries a `confidence="…"` attribute. Extend the recall
response with `method` (which layer answered: shortcut / lexical / vector /
graph-fusion), `latency_ms`, and `truncated` so the token A/B harness can
attribute both **token tax** and **answer quality** per layer. This is the
Calibrate/Evaluate-Optimize hook that lets us prove each phase pays off before
flipping defaults.

## Phasing & ordering

| Phase | Change | Blast radius | Risk |
|------|--------|-------------|------|
| 1 | Envelope byte budget | `ingress_preinject.c` + 1 config field | trivial |
| 2 | Memory preview-then-pull | `ingress_preinject.c` + `kb_client_memory.c` read path | low (reuses summaries + existing pull-handle) |
| 3 | Learned shortcuts | recall entry + small store | medium (new store, gated) |
| 4 | Intent annotation | schema + curator Extract output | medium (schema + enrichment) |

Phases 1 and 2 together are the high-value, low-risk slice: both confined to the
envelope assembler plus the existing summaries/budget plumbing, and together
they turn pre-injection from a token liability into the asset the feature is
meant to be. 3 and 4 compound the win but are independently shippable.

## Testing & validation

- **Unit:** budget enforcement (envelope never exceeds N bytes; truncation line
  present); preview variant returns headline+id and never full body; shortcut
  store hit/miss/promote/evict; intent column flows into the FTS query.
- **Integration:** a recall round-trip where the agent opens a previewed record
  via `get_context_block` and gets the full body.
- **Token A/B harness:** measure tokens-injected-per-turn and answer correctness
  on a fixed query set, before vs after each phase, using the existing benchmark
  suite. The acceptance bar for flipping `ingress_preinject_enabled` default-on
  is *lower tokens at equal-or-better correctness.* Deploy/bench runs are
  user-gated.

## Non-goals / risks

- **Not** changing the blend to a gate — recall quality stays as-is (§0).
- **Not** adding a flat `priority` int — Aimee's score set already subsumes it.
- **Preview staleness:** progressive disclosure assumes the headline summary
  faithfully represents the body. Mitigated by tying summary freshness to record
  hash and falling back to a trimmed body preview when no summary exists.
- **Shortcut ossification:** mitigated by Gate-Promote (recurrence + stability
  threshold) and hash/staleness eviction.
- **Intent drift:** an LLM-authored intent phrasing can be wrong; it is an
  additive lexical signal, never authoritative, and never replaces content.
