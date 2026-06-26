# Proposal: Code-graph intelligence — a living, embedded, reasoning graph over code

- **State:** IN PROGRESS — §0.5/§1/§3/§4/§5/§7 implemented (on the `testing` branch),
  including the §5 vector leg (code+graph+vector hybrid); remainder (§2 tree-sitter,
  §4 surprising-links, §6 live fusion, §8 webchat viz, §7 graph-informed delegation)
  still open, as build-/integration-/frontend-tier work. See the "Implementation
  status" section below.
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

## §0.5 Source of truth: the git default branch

**Principle.** Code indexing — the graph **and** the embeddings — is sourced from
the repository's **git default branch** (e.g. `origin/main`), not the user's
working tree, current checkout, or a feature branch they are mid-edit on. The kb's
code view must be **stable and canonical**: a developer's WIP, an uncommitted edit,
or a throwaway branch must never thrash the graph/embeddings or pollute what other
sessions retrieve as "the code."

**Resolution order** (`code_collect.c`, `git_resolve_default_ref`):
1. `git symbolic-ref --short refs/remotes/origin/HEAD` — the remote's advertised
   default (kept in `origin/<branch>` form end-to-end, no prefix mixing).
2. If unset (common when the remote was added after clone), repair **once** with
   `git remote set-head origin -a` and retry — don't surface the transient state.
3. First existing of `origin/main`, `origin/master`, `main`, `master`.
4. **No fall-through to the current HEAD or working tree.** A git repo with no
   resolvable default branch is **skipped** with a diagnostic — silently indexing
   the checkout would re-introduce the exact WIP-thrash this eliminates.

**Non-git dirs** fall back to the working-tree walk unconditionally. An explicit
opt-in `AIMEE_CODE_INDEX_SOURCE=worktree` indexes the working tree for a user who
*wants* their WIP indexed (documented as team-unsafe, since it re-introduces
thrash); `default`/`auto` (the default) honor the chain above.

**Read mechanism.** One `git ls-tree -r -z <ref>` enumerates `(oid, path)` pairs;
one `git cat-file --batch` (fed the wanted oids from a temp file, so our side only
reads — no bidirectional-pipe deadlock) streams content in request order. Content
is paired to path **by sequence**, so a newline in a path can't misattribute a
blob. The same extension/skip-dir/size/binary filters as the worktree walk apply.

**Code-vs-edit-tool split (invariant).** Graph + embedding **indexing** reads the
default branch. **Edit-time** tools that ask "what does *this in-progress change*
touch" — blast-radius (§7), and the sweep proposer when proposing from
staged/unstaged changes — read the **working tree** via their own reader and
deliberately do **not** route through the indexing collector. Conflating the two
would make a blast-radius query against the default-branch graph silently miss the
very edit the user is asking about. The choice is documented per tool, never
inferred. (Today only `kb_client_index` and `server_sweep` use the canonical
collector; blast-radius already has its own working-tree reader, so the split
holds structurally.)

**Idempotency interaction.** §1's content fingerprint (md5 over file `(path,hash)`)
is unaffected — it already captures "did the default-branch content change." A
future optimization can use the branch tree SHA (`git rev-parse <ref>^{tree}`) as a
cheaper outer skip-gate, with the per-file `(path,hash)` set still driving
upsert/delete on a change. Submodule content drift is out of scope unless opted in.

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
  **Status — shipped.** `GET /v1/code/graph/hubs?project&max_results` ranks a
  project's symbols by **degree centrality** over the visible projection graph:
  `db2_code_projection_list_edges` reads the published generation's edges, the pure
  `kb_graph_hubs` (`src/kb/kb_graph_analytics.c`) tallies in/out/weighted degree and
  returns the top N (deterministic: degree desc → weighted-degree desc → node asc),
  and the route surfaces `edge_count` + a `truncated` flag (the analytics cap is
  10k edges; beyond it the ranking is over a deterministic source/target-ordered
  prefix). Unit tested (`unit-test-kb-graph-analytics`: aggregation, tie-break
  determinism under input permutation, self-loops, truncation, empty endpoints) +
  shim route test (`test_code_graph_hubs_*`). **Agent-callable** via the MCP `index`
  family — `index({command:"hubs", project})` — through `kb_client_code_graph_hubs`.
  Weighted/betweenness/PageRank centrality and community detection are follow-ups.
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

**Status — fusion core shipped.** The RRF scoring model is implemented as a pure,
self-contained module `src/kb/kb_rrf.c` (`kb_rrf_fuse`): it takes N ranked signal
lists + per-signal weights + the RRF constant `k`, and returns one ranking by
`Σ_s w_s/(k+rank_s)`, robust to absent signals, with deterministic tie-breaks
(structural-trust desc, then id) and a cross-signal-consensus boost. Fully unit
tested (`unit-test-kb-rrf`): exact rank-blend math, weighting, consensus-beats-single,
absent-signal robustness, determinism under input-order permutation, truncation.

**Status — route shipped.** `GET /v1/code/hybrid?query&symbol&project` fuses two
signals in **file-path** space through `kb_rrf_fuse` so consensus is meaningful — a
file that is both textually relevant to `query` AND structurally connected to
`symbol` (calls it) ranks highest: `code` (lexical `canonical_index_code_search`) +
`graph` (`canonical_index_find_callers`, marked structural). Memory recall
(`db2_memory_find_facts_like`) returns as a separate typed `why` array — the recorded
reasoning behind the code, context rather than a fused file row (matching the §5
example "+ the decision that explains X"). Each result is labeled with its
contributing `signals`, enriched (snippet / caller), and carries `signal_hits` +
`structural_weight`. Shim-tested end-to-end in `unit-test-kb-http-routes`
(`test_code_hybrid_*`: both legs fuse + label + enrich, memory why, no-symbol path,
missing-query 400). **Agent-callable** via the MCP `index` family — `index({command:
"hybrid", query, symbol?, project?})` — wired through `kb_client_code_hybrid`
(verbatim JSON forward). **Per-signal weights are config-tunable** (shipped):
`kb.code_hybrid.{weight_code,weight_graph,rrf_k}` (defaults `1.0/1.0/60` preserve the
prior behavior; `weight ≤ 0` disables a leg) flow from `config_load` into
`kb_rrf_fuse`, so an operator can re-balance lexical-vs-structural relevance without a
rebuild. **Remaining:** the **vector** leg (`pgvec_code_search`, needs the query
embedder — integration/deploy-tier) as a third fused signal.

## §6 Live + cross-session memory fusion

- **Incremental updates** on default-branch movement (post-merge / fetch hook +
  watch) so the graph tracks new commits on the canonical branch (§0.5), not a
  stale snapshot — and not the working tree.
- **Fuse the graph with conversation memory + the decision log** so the "why" behind
  a symbol is the *actual recorded reasoning*, not just parsed comments — queryable
  via §5. This is the thing a regenerated artifact can never hold.

## §7 Agent actuation (the graph changes behavior)

- **Blast-radius-aware edits**: before a write, surface graph-impacted files into the
  guardrail/context path (`guardrails_orchestrator.c`).
  **Status — shipped.** `pre_tool_check` now emits a structural blast-radius
  **ADVISORY** before an `Edit`/`Write`/`MultiEdit`: it lists the dependent files
  that the edited file structurally affects (from the KB sidecar's
  `/v1/code/blast-radius`, i.e. the call graph + typed projection edges — never the
  LLM entity graph), and appends a high-centrality **hub** note at/above the
  refactor-risk threshold (≥5 dependents), folding the stale-edge guard below into
  the same surface. Advisory + **fail-open**: gated behind
  `guardrails.blast_radius.advisory_enabled` (default **off**, opt-in); never
  blocks; any miss (flag off, no indexed project owns the path, sidecar error, no
  dependents) leaves the existing guardrail decision untouched, and it never
  clobbers a higher-priority guardrail message. The decision is a pure, testable
  core (`blast_radius_advisory_format` in `src/guardrails_blast_radius.c`) over an
  already-fetched `blast_radius_t`, with the sidecar I/O resolver kept thin and
  shared with `classify_path`'s existing severity check (no duplicate fetch path).
  Unit tested hermetically (`unit-test-guardrails-blast-radius`: listing, ellipsis
  cap, singular/plural, hub note, truncation-safety, project resolution + path
  boundary, fail-open on no-match/sidecar-error, flag gate, message precedence).
- **Graph-informed delegation**: route a delegate task with the relevant subgraph as
  context automatically. *(Follow-up — not yet wired.)*
- **Stale-edge guard**: warn when an edit touches a high-centrality/hub symbol.
  *(Shipped as the hub note within the blast-radius advisory above.)*

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

## Implementation status (as of this revision)

**Shipped to `testing`:**
- **§0.5** default-branch sourcing (`code_collect.c`, `unit-test-code-collect`).
- **§1** auto-build of the projection graph on the curator drain, content-addressed +
  idempotent (`kb_graph_build_project_if_changed`, `unit-test-kb-graph`).
- **§3** edge provenance (`structural`/`inferred`/`ambiguous`) surfaced in `graph.explain`.
- **§4** hub/degree-centrality analytics — `GET /v1/code/graph/hubs`
  (`kb_graph_analytics.c`, `unit-test-kb-graph-analytics`), **agent-callable** via
  `index({command:"hubs"})`.
- **§5** RRF fusion core (`kb_rrf.c`, `unit-test-kb-rrf`) + the `GET /v1/code/hybrid`
  route fusing `code` + `graph` in file-path space with a typed memory `why`,
  **agent-callable** via `index({command:"hybrid"})`, with **config-tunable per-signal
  weights** (`kb.code_hybrid.*`), and the **vector leg** — embedding similarity over
  `code_embeddings` as a third RRF signal (`pgvec_code_search_paths`), gated on a
  dim-matched embedder and graceful-degrading. So /v1/code/hybrid now fuses
  code+graph+vector. The SQL was validated against real pgvector/halfvec.
- **§7** structural blast-radius **advisory** on the guardrail edit path — advisory,
  fail-open, structural-only, opt-in (`guardrails_blast_radius.c`,
  `unit-test-guardrails-blast-radius`); folds in the stale-edge hub note.

**Deferred — needs the embedder / a build-tier dependency / a deployed corpus / a frontend
(not completable in the agent host):**
- **§2 tree-sitter** front-end — large build-tier work (vendor ~30 grammars + ABI).
- **§4 surprising-links** — needs embeddings + the percentile/LLM-judge confirmation stage.
- **§6 live/memory fusion** — post-merge/fetch incremental hook + watch.
- **§8 webchat visualization** — frontend, read-only `/v1/code/graph` paged projection.
- **§7 graph-informed delegation** — route a delegate task with the relevant subgraph
  (follow-up to the shipped blast-radius advisory).

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
  second pass); provenance tagging; **RRF fusion ordering** (rank blend + tie-break)
  — shipped as `unit-test-kb-rrf`; surprising-links relevance gate (quality floor +
  threshold).
- Integration: `workspace add` of a sample repo → all three layers populated +
  searchable (extends the docker e2e); incremental update on file change.
- Source selection (`unit-test-code-collect`): the canonical collector indexes the
  git **default branch** (not feature-branch/working-tree WIP); resolves & repairs
  `origin/HEAD`; **skips** a git repo with no default branch; falls back to the
  working tree for non-git dirs and the `AIMEE_CODE_INDEX_SOURCE=worktree` opt-in.
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

## Review revisions (R2) — source of truth

A second roundtable (`mistral` / `mimo-2.5-pro` / `glm-5.2`; 3/3, 0 failed) on the
indexing source converged on §0.5: index the **git default branch**, not the
working tree. Load-bearing outcomes, all now in §0.5:

1. **Drop the current-HEAD fallback** — it re-introduced WIP-thrash; an unresolvable
   default branch now **skips** with a diagnostic instead.
2. **Repair `origin/HEAD`** with `git remote set-head origin -a` before descending
   the fallback chain (it is unset on a long tail of real checkouts).
3. **Read via `ls-tree` + `cat-file --batch`** (one fork each), pairing content to
   path by sequence (NUL-safe), not per-file `git show` (N forks).
4. **Code-vs-edit-tool split is a first-class invariant** — indexing reads the
   default branch; blast-radius / staged-change tools read the working tree.

Implemented in `code_collect.c` with `unit-test-code-collect` covering all cases.
