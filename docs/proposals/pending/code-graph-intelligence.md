# Proposal: Code-graph intelligence — a living, embedded, reasoning graph over code

- **State:** DRAFT — R1 (roundtable-revised), pre proposal-gate.
- **Thesis:** aimee should treat the codebase as a *living* graph that is (a) fully
  built without a manual step, (b) parsed broadly, (c) ranked by **graph structure
  AND vector similarity AND memory** in one query, and (d) able to *change what the
  agent does* — not just answer questions. Today the substrate is ~60% there but
  under-wired; this proposal closes the gaps and presses the embedding+memory edge.

## Goal

A code/knowledge graph that is **clearly the best substrate for an agent**:
broad-coverage parse, three graph layers built automatically on `workspace add`,
hybrid graph+vector+memory retrieval, incremental + cross-session, and wired into
guardrails/delegation so the graph actuates behavior. Optional webchat
visualization for humans.

## §0 What already exists (so we don't rebuild it)

- **Call graph — LIVE.** `code_calls` (caller→callee + file + line) is populated by
  the CPU structural index (`canonical_index_scan_project`, `db2/code_index.c`) at
  ingest. Backs `/v1/code/callers`, `aimee index callers|blast-radius`.
- **Typed symbol projection graph — EXISTS, not auto-built.** `code_projection_edges`
  (`db2/code_projection.c`) carries typed `source —relation→ target` edges across 7
  relations (`defines/contains/exports/routes/depends_on/calls/imports`) with
  structural-trust weights, versioned via `code_projection_generations`
  (sync→publish in `kb/kb_service_graph.c` → `db2_code_projection_sync_project`).
  **It is never run on ingest** → 0 edges in practice.
- **Entity/knowledge graph — EXISTS, sparse.** `entity_nodes`/`entity_edges` built by
  the curator LLM synthesis (`kb_curator_*`). Only fills as the curator drains.
- **Embeddings — LIVE.** `code_embeddings` + `kb_documents` chunk vectors in pgvector
  via the embed drain (`kb_curator_drain.c`). This is the differentiator we under-use.
- **Parse — hand-rolled.** `extractors.c` / `extractors_extra.c` / `extractors_new_langs.c`
  — limited language set, brittle vs real grammars.

## §1 Auto-build all three layers on ingest (highest-leverage, cheap)

Wire the projection-graph generation and an entity-graph pass into the curator drain
(`kb_curator_drain.c`) so that after `canonical_index_scan_project` populates
`code_calls`/`file_contents`, a poll also: (a) runs `db2_code_projection_sync_project`
+ publish per changed project; (b) feeds code symbols to the curator entity pass. Net:
`workspace add` materializes call graph + typed projection graph + entity graph with
no manual step. Default-on; bounded per poll.

**Idempotency (R1).** The sync is content-addressed + generation-versioned: each edge
keys on `(source, relation, target, source_hash)`, and a re-sync over an unchanged
`source_hash` is a no-op (the same skip the embed drain uses). `sync → publish` swaps
the active generation **atomically**; a failed sync **aborts** the generation (no
partial publish — the previous generation stays active). So re-running over unchanged
inputs writes nothing and yields the same published edge set — proven by a test that
syncs twice and asserts an identical edge set + a zero-work second pass.

**Dependency on §2 (R1).** P1 builds the projection graph from the **existing**
`code_calls` (already produced by the hand-rolled extractors for the languages they
cover) — it does **not** require tree-sitter. P1 therefore ships a real graph today,
bounded only by current extractor coverage; §2 (tree-sitter) merely **widens** the
inputs. To make that gap measurable instead of assumed, P1 emits a coverage metric
(edges/file, % files with ≥1 edge) so we can see exactly what §2 buys.

## §2 Tree-sitter extraction front-end (coverage)

Replace/augment the hand-rolled extractors with a **tree-sitter** front-end feeding
the *same* `code_calls` / `code_projection_edges` / symbol tables. Target ≥30
languages. Keep the existing extractor path as fallback for unsupported grammars.
This is the one true coverage gap and the largest engineering item.

## §3 Edge provenance + confidence

Surface a provenance tag on every edge — `structural` (from AST/index), `inferred`
(curator/LLM), `ambiguous` (low-confidence) — derived from the existing
structural-trust weight + edge source. Exposed in query results and the viz so
callers can filter by trust.

## §4 Graph analytics (communities, hubs, surprising links)

Computed over `code_projection_edges` + embeddings, served read-only:
- **Communities** (Louvain/Leiden over the typed graph) → module/cluster map.
- **Hubs/centrality** → most-connected symbols (refactor-risk ranking).
- **Surprising links** — pairs `(a,b)` with **high embedding similarity AND high
  graph distance**, made precise + gated (R1):
  - *similarity*: cosine(emb a, emb b) at/above the **top percentile** of the
    project's own similarity distribution (data-driven, not a hardcoded constant);
  - *distance*: shortest-path hop count over `code_projection_edges` ≥ `d_min`
    (default 4) **or** different Louvain communities;
  - **relevance gate** (guards against low-quality embeddings producing noise): both
    nodes must clear an embedding-quality floor (non-degenerate vector, enough token
    content), and each surfaced pair is confirmed by a cheap second stage — a
    shared-symbol/lexical cross-check, else a single LLM-judge call on only the top-N
    candidates — before it is shown. Precision is sampled (LLM-judge or human
    spot-check) and the feature **self-suppresses** if sampled precision drops below a
    floor. Only computable because aimee has vectors.

## §5 Hybrid graph+vector+memory retrieval (the headline)

A single ranked query that fuses three signals:
1. **graph** — N-hop neighborhood / callers / blast-radius from `code_projection_edges`;
2. **vector** — pgvector similarity over `code_embeddings`/`kb_documents`;
3. **memory** — relevant decisions/notes from the memory graph (DB2).
Returns one ranked result set ("callers of X + semantically-related code the edges
miss + the decision that explains X").

**Scoring model (R1).** The three signals have non-comparable raw scores (hop counts
vs cosine vs memory recency), so we fuse by **rank, not raw score** — Reciprocal Rank
Fusion: a candidate's fused score is `Σ_signals w_s · 1/(k + rank_s(d))` (k≈60, RRF's
standard constant). RRF needs no score normalization/calibration, is robust to a
signal being absent (the candidate simply isn't in that list), and degrades
gracefully. Per-signal weights `w_s` default equal and are config-tunable; ties break
on the structural-trust weight of the connecting edge (deterministic). Each signal
caps its own candidate list first (graph ≤ N-hop frontier, vector top-K by cosine,
memory top-M by recency·relevance), so fusion cost is bounded. Surfaced via a new
`/v1/code/context` (or an extended `/v1/code/search`) + an MCP tool the primary agent
calls before grepping.

## §6 Live + cross-session memory fusion

- **Incremental updates** on file-change/commit (hook + watch) so the graph tracks
  the working tree, not a snapshot.
- **Fuse the graph with conversation memory + the decision log** so the "why" behind
  a symbol is the *actual recorded reasoning*, not just parsed comments — queryable
  via §5. This is the thing a regenerated artifact can never hold.

## §7 Agent actuation (the graph changes behavior)

- **Blast-radius-aware edits**: before a write, surface graph-impacted files into the
  guardrail/context path (`guardrails_orchestrator.c`).
- **Graph-informed delegation**: route a delegate task with the relevant subgraph as
  context automatically.
- **Stale-edge guard**: warn when an edit touches a high-centrality/hub symbol.

**Safety constraint (R1).** Anything on the safety-critical guardrail path uses ONLY
the **deterministic structural layers** — the call graph + typed projection edges
(AST-derived, `provenance=structural`) — **never** the LLM-synthesized entity graph,
so an inference error can't mislead a safety decision. Actuation is **advisory and
fail-open**: blast-radius *surfaces context / warns*, it does not block or auto-act,
and a missing/empty graph yields **no extra restriction** (fall back to the existing
guardrails). The graph can only ADD caution, never remove an existing safety check.
Any future hard gate must be backed by structural edges + a confidence floor and stay
fail-open.

## §8 Webchat visualization (nice-to-have)

A read-only interactive graph view in the webchat UI: project/community map, click a
symbol → callers/callees/neighbors + provenance + the linked "why". Backed by a
read-only `/v1/code/graph` projection (paged). Human-facing exploration; not on the
agent's hot path.

## Phasing (each independently shippable)

- **P1 (now):** §1 auto-build + §3 provenance. Mostly wiring; makes the graph complete.
- **P2:** §2 tree-sitter + §5 hybrid retrieval (parallel) — coverage + headline.
- **P3:** §4 analytics + §8 webchat viz.
- **P4:** §6 live/memory fusion + §7 actuation — compounds the platform.

## Non-goals

- **Portable / offline / git-committed graph artifact.** Explicitly out of scope —
  aimee's value is the live server-side graph + embeddings + memory, not a file you
  carry around. We will not invest in export portability as a headline.
- A general graph DB (Neo4j/etc.) — pgvector + DB2 stay the store.

## Risks / honest limits

- Tree-sitter integration is real C/build work (vendoring grammars, ABI) — largest risk.
- Auto-building all layers raises per-ingest LLM/GPU load; must stay incremental +
  bounded per poll (reuse the embed-drain backpressure model).
- "Surprising-links" quality depends on embedding quality — mitigated by §4's
  relevance gate (quality floor + confirmation stage + sampled-precision self-suppress).
- Webchat viz scale: large graphs need server-side paging/aggregation.

## Tests

- Unit: projection-sync **idempotency** (sync twice → identical edge set + zero-work
  second pass); provenance tagging; **RRF fusion ordering** (rank blend + tie-break);
  surprising-links relevance gate (quality floor + threshold).
- Integration: `workspace add` of a sample repo → all three layers populated +
  searchable (extends the docker e2e); incremental update on file change.
- Coverage: per-language parse fixtures for the tree-sitter front-end + the P1
  edges/file coverage metric.

## Review revisions (R1)

Roundtable review (`mistral` / `mimo-2.5-pro` / `glm-5.2`; 3/3 panelists, 0 failed)
converged on four load-bearing gaps; each is now addressed in-line:

1. **Hybrid fusion had no scoring model (§5)** → Reciprocal Rank Fusion (rank-based,
   no score calibration; weighted, tie-broken by structural trust; per-signal caps).
2. **"Surprising links" under-defined / no validation (§4)** → percentile similarity +
   hop/community distance + an embedding-quality relevance gate + a confirmation stage
   + sampled-precision self-suppress.
3. **Projection-sync assumed idempotency and hid a tree-sitter dependency (§1)** →
   content-addressed, atomically published/aborted sync with a proof test; P1
   explicitly ships on existing `code_calls` (no §2 needed) and emits a coverage
   metric so the §2 gap is measured, not assumed.
4. **Actuation coupled a safety path to an LLM-augmented graph (§7)** → guardrail path
   restricted to deterministic structural edges; actuation advisory + fail-open.
