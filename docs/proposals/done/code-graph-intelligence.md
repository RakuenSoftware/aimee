# Proposal: Code-graph intelligence: a living, embedded, reasoning graph over code

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** done
- **Completed:** 2026-06-28
- **Moved from:** `docs/proposals/pending/code-graph-intelligence.md`
- **Summary:** **every section §0.5–§8 shipped to the `testing` branch**. The §5
  code+graph+vector+memory hybrid, §7 graph-informed delegation + blast-radius advisory,
  the §4 surprising-links route with its LLM-judge relevance gate + precision
  self-suppress, the §8 `/v1/code/graph` backend route + the webchat Graph view, the §6
  live updates (memory-fusion leg + default-branch change-detection gate +
  post-merge/post-checkout reindex hooks), and the **§2 tree-sitter front-end** with
  grammars for **all 16 hand-rolled supported languages** + call-edge extraction (opt-in
  `AIMEE_TREESITTER` build, fall-through to the hand-rolled extractors). Two **optional
  follow-ups** were deliberately deferred at close-out (roundtable-ratified) and are
  recorded as future-work / known limitations below: tree-sitter grammars **beyond** the
  16-class parity set, and an always-on §6 fanotify/inotify watch. The latter is a
  **documented freshness limitation** (non-git / out-of-band trees stay only as fresh as
  the last manual `aimee index scan`), not merely a nice-to-have. See the "Implementation
  status" and "Deferred" sections.
- **Thesis:** aimee should treat the codebase as a *living* graph that is (a) fully
  built without a manual step, (b) parsed broadly, (c) ranked by **graph structure
  AND vector similarity AND memory** in one query, and (d) able to *change what the
  agent does*, not just answer questions. Today the substrate is ~60% there but
  under-wired; this proposal closes the gaps and presses the embedding+memory edge.

## Goal

A code/knowledge graph that is **clearly the best substrate for an agent**:
broad-coverage parse, three graph layers built automatically on `workspace add`,
hybrid graph+vector+memory retrieval, incremental + cross-session, and wired into
guardrails/delegation so the graph actuates behavior. Optional webchat
visualization for humans.

## §0 What already exists (so we don't rebuild it)

- **Call graph, LIVE.** `code_calls` (caller→callee + file + line) is populated by
  the CPU structural index (`canonical_index_scan_project`, `db2/code_index.c`) at
  ingest. Backs `/v1/code/callers`, `aimee index callers|blast-radius`.
- **Typed symbol projection graph, EXISTS, not auto-built.** `code_projection_edges`
  (`db2/code_projection.c`) carries typed `source —relation→ target` edges across 7
  relations (`defines/contains/exports/routes/depends_on/calls/imports`) with
  structural-trust weights, versioned via `code_projection_generations`
  (sync→publish in `kb/kb_service_graph.c` → `db2_code_projection_sync_project`).
  **It is never run on ingest** → 0 edges in practice.
- **Entity/knowledge graph, EXISTS, sparse.** `entity_nodes`/`entity_edges` built by
  the curator LLM synthesis (`kb_curator_*`). Only fills as the curator drains.
- **Embeddings, LIVE.** `code_embeddings` + `kb_documents` chunk vectors in pgvector
  via the embed drain (`kb_curator_drain.c`). This is the differentiator we under-use.
- **Parse (hand-rolled.** `extractors.c` / `extractors_extra.c` / `extractors_new_langs.c`)
limited language set, brittle vs real grammars.

## §0.5 Source of truth: the git default branch

**Principle.** Code indexing (the graph **and** the embeddings) is sourced from
the repository's **git default branch** (e.g. `origin/main`), not the user's
working tree, current checkout, or a feature branch they are mid-edit on. The kb's
code view must be **stable and canonical**: a developer's WIP, an uncommitted edit,
or a throwaway branch must never thrash the graph/embeddings or pollute what other
sessions retrieve as "the code."

**Resolution order** (`code_collect.c`, `git_resolve_default_ref`):
1. `git symbolic-ref --short refs/remotes/origin/HEAD`: the remote's advertised
   default (kept in `origin/<branch>` form end-to-end, no prefix mixing).
2. If unset (common when the remote was added after clone), repair **once** with
   `git remote set-head origin -a` and retry. Don't surface the transient state.
3. First existing of `origin/main`, `origin/master`, `main`, `master`.
4. **No fall-through to the current HEAD or working tree.** A git repo with no
   resolvable default branch is **skipped** with a diagnostic, silently indexing
   the checkout would re-introduce the exact WIP-thrash this eliminates.

**Non-git dirs** fall back to the working-tree walk unconditionally. An explicit
opt-in `AIMEE_CODE_INDEX_SOURCE=worktree` indexes the working tree for a user who
*wants* their WIP indexed (documented as team-unsafe, since it re-introduces
thrash); `default`/`auto` (the default) honor the chain above.

**Read mechanism.** One `git ls-tree -r -z <ref>` enumerates `(oid, path)` pairs;
one `git cat-file --batch` (fed the wanted oids from a temp file, so our side only
reads, no bidirectional-pipe deadlock) streams content in request order. Content
is paired to path **by sequence**, so a newline in a path can't misattribute a
blob. The same extension/skip-dir/size/binary filters as the worktree walk apply.

**Code-vs-edit-tool split (invariant).** Graph + embedding **indexing** reads the
default branch. **Edit-time** tools that ask "what does *this in-progress change*
touch": blast-radius (§7), and the sweep proposer when proposing from
staged/unstaged changes. Read the **working tree** via their own reader and
deliberately do **not** route through the indexing collector. Conflating the two
would make a blast-radius query against the default-branch graph silently miss the
very edit the user is asking about. The choice is documented per tool, never
inferred. (Today only `kb_client_index` and `server_sweep` use the canonical
collector; blast-radius already has its own working-tree reader, so the split
holds structurally.)

**Idempotency interaction.** §1's content fingerprint (md5 over file `(path,hash)`)
is unaffected. It already captures "did the default-branch content change." A
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
partial publish. The previous generation stays active). So re-running over unchanged
inputs writes nothing and yields the same published edge set, proven by a test that
syncs twice and asserts an identical edge set + a zero-work second pass.

**Dependency on §2 (R1).** P1 builds the projection graph from the **existing**
`code_calls` (already produced by the hand-rolled extractors for the languages they
cover). It does **not** require tree-sitter. P1 therefore ships a real graph today,
bounded only by current extractor coverage; §2 (tree-sitter) merely **widens** the
inputs. To make that gap measurable instead of assumed, P1 emits a coverage metric
(edges/file, % files with ≥1 edge) so we can see exactly what §2 buys.

## §2 Tree-sitter extraction front-end (coverage)

Replace/augment the hand-rolled extractors with a **tree-sitter** front-end feeding
the *same* `code_calls` / `code_projection_edges` / symbol tables. Target ≥30
languages. Keep the existing extractor path as fallback for unsupported grammars.
This is the one true coverage gap and the largest engineering item.

**Status, front-end + full supported-language set shipped (opt-in).**
`code_treesitter.c` parses a file with the tree-sitter grammar for its extension and
emits the same `definition_t` symbols (functions and types) the hand-rolled extractor
does; `extract_definitions` prefers it where a grammar is compiled in and **falls back**
otherwise, so the **default build is unchanged**. The runtime + grammars are large
generated parsers, so they are **fetched at build time** (`scripts/fetch-treesitter.sh`,
pinned commits, gitignored) and compiled only in the opt-in `AIMEE_TREESITTER` build
(external scanners linked where a grammar needs one); `code_treesitter.c` is a stub
without it. Ships grammars covering **all 16 hand-rolled-supported language classes**.
C/C++, C#, Python, Go, JavaScript, TypeScript, Rust, Java, Ruby, PHP, Lua, Bash, Swift,
Kotlin, Dart, CSS, as **17 tree-sitter grammars** (C and C++, which the hand-rolled
extractor handles as one `LANG_C` class, get separate `tree_sitter_c`/`tree_sitter_cpp`
grammars), each with a per-language `classify_*` mapping its definition node types to
`function`/`type`. The walk descends through organizational wrappers (namespaces,
`export`/decorator wrappers) and the member bodies of types, so **nested members
(class/impl/trait methods) are surfaced** across every OO language, while it never
descends a function body (so locals stay out). `unit-test-code-treesitter` parses real
source in every language and asserts the extracted defs (top-level + nested). It also
extracts **call edges** (`code_treesitter_calls` → `call_ref_t` caller→callee, wired into
`extract_calls`): a caller-tracking walk attributes every call to its enclosing function
and resolves the callee to the last identifier of the callee expression (`obj.m()` → `m`,
`a::b()` → `b`); Bash/CSS defer to the hand-rolled path. Adding a grammar is mechanical,
vendor its `parser.c`(+`scanner.c`), register its `TSLanguage` + extensions in
`ts_language_for_ext`, and add a `classify_*`. Remaining increment: more languages.

## §3 Edge provenance + confidence

Surface a provenance tag on every edge, `structural` (from AST/index), `inferred`
(curator/LLM), `ambiguous` (low-confidence), derived from the existing
structural-trust weight + edge source. Exposed in query results and the viz so
callers can filter by trust.

## §4 Graph analytics (communities, hubs, surprising links)

Computed over `code_projection_edges` + embeddings, served read-only:
- **Communities** (Louvain/Leiden over the typed graph) → module/cluster map.
- **Hubs/centrality** → most-connected symbols (refactor-risk ranking).
  **Status, shipped.** `GET /v1/code/graph/hubs?project&max_results` ranks a
  project's symbols by **degree centrality** over the visible projection graph:
  `db2_code_projection_list_edges` reads the published generation's edges, the pure
  `kb_graph_hubs` (`src/kb/kb_graph_analytics.c`) tallies in/out/weighted degree and
  returns the top N (deterministic: degree desc → weighted-degree desc → node asc),
  and the route surfaces `edge_count` + a `truncated` flag (the analytics cap is
  10k edges; beyond it the ranking is over a deterministic source/target-ordered
  prefix). Unit tested (`unit-test-kb-graph-analytics`: aggregation, tie-break
  determinism under input permutation, self-loops, truncation, empty endpoints) +
  shim route test (`test_code_graph_hubs_*`). **Agent-callable** via the MCP `index`
  family (`index({command:"hubs", project})`) through `kb_client_code_graph_hubs`.
  Weighted/betweenness/PageRank centrality and community detection are follow-ups.
- **Surprising links**: pairs `(a,b)` with **high embedding similarity AND high
  graph distance**, made precise + gated (R1):
  - *similarity*: cosine(emb a, emb b) at/above the **top percentile** of the
    project's own similarity distribution (data-driven, not a hardcoded constant);
  - *distance*: shortest-path hop count over `code_projection_edges` ≥ `d_min`
    (default 4) **or** different Louvain communities;
  - **relevance gate** (guards against low-quality embeddings producing noise): both
    nodes must clear an embedding-quality floor (non-degenerate vector, enough token
    content), and each surfaced pair is confirmed by a cheap second stage. A
    shared-symbol/lexical cross-check, else a single LLM-judge call on only the top-N
    candidates, before it is shown. Precision is sampled (LLM-judge or human
    spot-check) and the feature **self-suppresses** if sampled precision drops below a
    floor. Only computable because aimee has vectors.

  **Status, distance + selection core shipped.** Two pure analytics primitives
  back the precise definition above: `kb_graph_shortest_hops` (undirected BFS over
  `code_projection_edges`, capped at `KB_GRAPH_BFS_MAX_NODES`, returns hop count /
  0 / -1-for-disconnected) and `kb_graph_surprising` (a **data-driven** cosine floor
  at the requested similarity percentile of the supplied pair set, then keeps pairs
  whose hop distance ≥ `d_min` **or** which are disconnected, in deterministic order)
  in `src/kb/kb_graph_analytics.c`, unit-tested in `unit-test-kb-graph-analytics`.

  **Status, route shipped.** `GET /v1/code/graph/surprising?project&max_results&k&
  d_min&percentile&min_cosine` gathers candidate pairs from `code_embeddings` via a
  project-scoped, anchor-bounded **lateral self-kNN** (`pgvec_code_similar_pairs`,
  riding the HNSW cosine index), whose `node_key` is byte-identical to the projection
  file-node key (`db2_entity_node_key_file`) so pairs map to graph nodes with no
  remapping, then runs the pure core above. The hop distance is computed over the
  graph **with the project containment super-hub excluded**, otherwise every
  same-project file pair is 2 hops (file←project→file) and the signal is meaningless.
  A genuine vector-store outage is a 503 (distinct from an empty 200); every link
  carries `hops` (-1 = disconnected) + a `disconnected` bool. The SQL was validated
  against real pgvector/halfvec.

  **Status, relevance gate shipped.** With `judge=true` the top candidates go through
  the §4 confirmation: a cheap shared-symbol cross-check plus ONE batched **Tier-B
  LLM-judge** call (`kb_surprising_judge`, via the curator's `kb_curator_llm_run`,
reuses the configured `kb_curator_tier_b_*` provider, no new endpoint) that confirms
  genuine parallel/duplicated logic vs coincidental similarity. Each judged link gains
  `shared_symbols` and (when the model returns a verdict) `confirmed` + `reason`;
  verdicts for pairs not actually sent are rejected so the model can't fabricate a
  confirmation. Bounded to the top 12 links; opt-in (no LLM configured → unconfirmed
  structural candidates). The remaining **precision self-suppress** (sample judged
  precision, auto-disable below a floor) is a monitoring layer left as a follow-up.

## §5 Hybrid graph+vector+memory retrieval (the headline)

A single ranked query that fuses three signals:
1. **graph**: N-hop neighborhood / callers / blast-radius from `code_projection_edges`;
2. **vector**: pgvector similarity over `code_embeddings`/`kb_documents`;
3. **memory**: relevant decisions/notes from the memory graph (DB2).
Returns one ranked result set ("callers of X + semantically-related code the edges
miss + the decision that explains X").

**Scoring model (R1).** The three signals have non-comparable raw scores (hop counts
vs cosine vs memory recency), so we fuse by **rank, not raw score**. Reciprocal Rank
Fusion: a candidate's fused score is `Σ_signals w_s · 1/(k + rank_s(d))` (k≈60, RRF's
standard constant). RRF needs no score normalization/calibration, tolerates a
signal being absent (the candidate simply isn't in that list), and degrades
gracefully. Per-signal weights `w_s` default equal and are config-tunable; ties break
on the structural-trust weight of the connecting edge (deterministic). Each signal
caps its own candidate list first (graph ≤ N-hop frontier, vector top-K by cosine,
memory top-M by recency·relevance), so fusion cost is bounded. Surfaced via a new
`/v1/code/context` (or an extended `/v1/code/search`) + an MCP tool the primary agent
calls before grepping.

**Status, fusion core shipped.** The RRF scoring model is implemented as a pure,
self-contained module `src/kb/kb_rrf.c` (`kb_rrf_fuse`): it takes N ranked signal
lists + per-signal weights + the RRF constant `k`, and returns one ranking by
`Σ_s w_s/(k+rank_s)`, tolerating absent signals, with deterministic tie-breaks
(structural-trust desc, then id) and a cross-signal-consensus boost. Fully unit
tested (`unit-test-kb-rrf`): exact rank-blend math, weighting, consensus-beats-single,
absent-signal robustness, determinism under input-order permutation, truncation.

**Status, route shipped.** `GET /v1/code/hybrid?query&symbol&project` fuses two
signals in **file-path** space through `kb_rrf_fuse` so consensus is meaningful, a
file that is both textually relevant to `query` AND structurally connected to
`symbol` (calls it) ranks highest: `code` (lexical `canonical_index_code_search`) +
`graph` (`canonical_index_find_callers`, marked structural). Memory recall
(`db2_memory_find_facts_like`) returns as a separate typed `why` array, the recorded
reasoning behind the code, context rather than a fused file row (matching the §5
example "+ the decision that explains X"). Each result is labeled with its
contributing `signals`, enriched (snippet / caller), and carries `signal_hits` +
`structural_weight`. Shim-tested end-to-end in `unit-test-kb-http-routes`
(`test_code_hybrid_*`: both legs fuse + label + enrich, memory why, no-symbol path,
missing-query 400). **Agent-callable** via the MCP `index` family, `index({command:
"hybrid", query, symbol?, project?})` — wired through `kb_client_code_hybrid`
(verbatim JSON forward). **Per-signal weights are config-tunable** (shipped):
`kb.code_hybrid.{weight_code,weight_graph,rrf_k}` (defaults `1.0/1.0/60` preserve the
prior behavior; `weight ≤ 0` disables a leg) flow from `legacy_config_read` into
`kb_rrf_fuse`, so an operator can re-balance lexical-vs-structural relevance without a
rebuild. **Remaining:** the **vector** leg (`pgvec_code_search`, needs the query
embedder, integration/deploy-tier) as a third fused signal.

## §6 Live + cross-session memory fusion

- **Incremental updates** on default-branch movement (post-merge / fetch hook +
  watch) so the graph tracks new commits on the canonical branch (§0.5), not a
  stale snapshot, and not the working tree.

  **Status, change-detection gate shipped.** The `/v1/code/scan` route now no-ops
  the expensive git re-walk when the default branch hasn't moved: `git_resolve_default_sha`
  (the default ref's tree SHA, §0.5 chain) is compared via the pure
  `code_default_branch_changed` against the last-indexed SHA stored in
  `kb_runtime_state`; unchanged + non-`force` → `{"skipped":true}` (`unit-test-code-collect`
  exercises the SHA-tracks-commits + gate logic against real git repos).

  **Status, post-merge + post-checkout hooks shipped.** `code_index_install_branch_hook`
  writes marker-guarded `post-merge` **and** `post-checkout` git hooks (mirroring
  `verify_install_git_hook`; won't clobber a foreign hook, O_NOFOLLOW) that background
  `aimee index scan <project> <root>`, so a pull/merge **or branch switch** advancing the
  default branch re-indexes the graph, and the SHA gate above makes that a cheap no-op
  when nothing moved (the post-checkout hook reindexes only on a branch switch, `$3==1`,
  not a per-file checkout). Two entry points: the server-side `/v1/code/scan
  {install_hook:true}` (so `workspace add` can enable live reindex), and the client-side
  `aimee index watch <name> <root>`. A local command, the correct path when the repo
  lives on the client (a remote server can't write the client's `.git/hooks`). Best-effort,
  never failing the scan. Tested against real git repos (`test_install_branch_hook`
  asserts both hooks + the branch-switch gate) + the route (`test_code_scan_installs_hook`).
  §6 live is now end-to-end: **detect** (SHA gate) → **fire** (hook) → **rebuild** (§1
  drain). A fanotify/inotify watch for non-git or always-on freshness remains an optional
  follow-up.
- **Fuse the graph with conversation memory + the decision log** so the "why" behind
  a symbol is the *actual recorded reasoning*, not just parsed comments, queryable
  via §5. This is the thing a regenerated artifact can never hold.

  **Status, fusion half shipped.** `/v1/code/hybrid` now fuses a 4th ranked
  **`memory`** signal: symbol-anchored, it walks the symbol's incident curator-built
  knowledge-graph edges and resolves each neighbor entity to a `file_path`, so files
  the recorded reasoning associates with the symbol rank alongside the code/graph/
  vector legs (not just the post-fusion `why`). Config-tunable `code_hybrid_weight_memory`;
  empty (no-op) until the curator has synthesized an entity graph
  (`handle_get_code_hybrid` memory leg, `test_code_hybrid_memory_leg`). The **live
  half** (incremental reindex on default-branch movement) remains open.

## §7 Agent actuation (the graph changes behavior)

- **Blast-radius-aware edits**: before a write, surface graph-impacted files into the
  guardrail/context path (`guardrails_orchestrator.c`).
  **Status, shipped.** `pre_tool_check` now emits a structural blast-radius
  **ADVISORY** before an `Edit`/`Write`/`MultiEdit`: it lists the dependent files
  that the edited file structurally affects (from the KB sidecar's
  `/v1/code/blast-radius`, i.e. the call graph + typed projection edges, never the
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
  context automatically. *(Follow-up, not yet wired.)*
- **Stale-edge guard**: warn when an edit touches a high-centrality/hub symbol.
  *(Shipped as the hub note within the blast-radius advisory above.)*

**Safety constraint (R1).** Anything on the safety-critical guardrail path uses ONLY
the **deterministic structural layers**. The call graph + typed projection edges
(AST-derived, `provenance=structural`), **never** the LLM-synthesized entity graph,
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

**Status, backend route shipped.** `GET /v1/code/graph?project&node&max_results`
returns a node's incident projection edges, each with `neighbor`, `relation`,
`direction` (`out` = node→neighbor, `in` = neighbor→node, `self` = recursive edge),
`structural_weight`, and the §3 `provenance` tag, plus `match_count` (total incident,
pre-cap) and a `truncated` flag that fires when **either** the page cap (`max_results`,
1–200) **or** the projection scan window (`HUBS_MAX_EDGES`) bounds the result. Reuses
`db2_code_projection_list_edges` + `kb_graph_edge_provenance` (`handle_get_code_graph`
in `src/kb/http/kb_http_code.c`); read-only, off the agent hot path. Shim route tests
(`test_code_graph_node_*`: out/in neighbors, page-cap truncation, self-loop).

**Status, frontend shipped.** A read-only **Graph** page in the webchat SPA
(`frontend/src/pages/Graph.tsx`): for the active session's project it ranks the hubs,
click one to expand its callers/callees/neighbors (direction + relation + §3
provenance), drill into any neighbor, and surface the surprising links (with optional
LLM confirm). It is an adjacency explorer (no heavy graph lib). Backed by webchat Go
proxies `/api/graph/{hubs,surprising,neighbors}` (`webchat/graph.go`,
`webchat/graph_test.go`) that forward aimee-server's `index_graph_*` MCP tools (the
per-node route was exposed as `index({command:"neighbors"})` / `index_graph_node` so
all three are reachable from the frontend over the trusted UDS hop).

## Phasing (each independently shippable)

- **P1 (now):** §1 auto-build + §3 provenance. Mostly wiring; makes the graph complete.
- **P2:** §2 tree-sitter + §5 hybrid retrieval (parallel). Coverage + headline.
- **P3:** §4 analytics + §8 webchat viz.
- **P4:** §6 live/memory fusion + §7 actuation. Compounds the platform.

## Implementation status (as of this revision)

**Shipped to `testing`:**
- **§0.5** default-branch sourcing (`code_collect.c`, `unit-test-code-collect`).
- **§1** auto-build of the projection graph on the curator drain, content-addressed +
  idempotent (`kb_graph_build_project_if_changed`, `unit-test-kb-graph`).
- **§2** tree-sitter extraction front-end (`code_treesitter.c`) covering **all 16
  hand-rolled-supported language classes** (C/C++, C#, Python, Go, JavaScript, TypeScript,
  Rust, Java, Ruby, PHP, Lua, Bash, Swift, Kotlin, Dart, CSS) as 17 grammars. C and C++,
  one `LANG_C` class in the hand-rolled extractor, get separate grammars, feeding the same
  `definition_t` symbols as the hand-rolled extractors with fall-through; descends through
  organizational wrappers + type member bodies so **nested members are surfaced**, and
  extracts **call edges** (`code_treesitter_calls` → `extract_calls`). Opt-in
  `AIMEE_TREESITTER` build (runtime + grammars fetched, not committed,
`scripts/fetch-treesitter.sh`), so the default build is unchanged; covered by
  `unit-test-code-treesitter` (parses real source per language, asserts top-level + nested
  defs) and a dedicated opt-in `treesitter` CI lane.
- **§3** edge provenance (`structural`/`inferred`/`ambiguous`) surfaced in `graph.explain`.
- **§4** hub/degree-centrality analytics, `GET /v1/code/graph/hubs`
  (`kb_graph_analytics.c`, `unit-test-kb-graph-analytics`), **agent-callable** via
  `index({command:"hubs"})`; plus the **surprising-links route** `GET
  /v1/code/graph/surprising`, an anchor-bounded pgvector self-kNN
  (`pgvec_code_similar_pairs`) feeding the pure `kb_graph_surprising`
  (BFS distance + data-driven percentile), over the coupling graph **with the project
  containment hub excluded** (`handle_get_code_graph_surprising`,
  `test_code_graph_surprising_*`), with an opt-in **LLM-judge relevance gate**
  (`judge=true` → shared-symbol cross-check + one batched Tier-B judge,
  `kb_surprising_judge`, `unit-test-kb-surprising-judge`) and a **precision
  self-suppress**: the judge samples the structural generator's precision
  (`confirmed`/`judged`, rolling per-project in `kb_runtime_state`), and an unjudged
  request returns no candidates once that precision falls below the configurable
  `code_surprising_precision_floor` (`kb_surprising_precision_suppress`).
- **§5+§6** RRF fusion core (`kb_rrf.c`, `unit-test-kb-rrf`) + the `GET /v1/code/hybrid`
  route fusing `code` + `graph` + **`vector`** (embedding similarity over
  `code_embeddings` via `pgvec_code_search_paths`, gated on a dim-matched embedder,
  graceful-degrading) + **`memory`** (the §6 cross-session knowledge-graph leg,
  symbol-anchored over curator entity edges) in file-path space with a typed memory
  `why`, **agent-callable** via `index({command:"hybrid"})`, with **config-tunable
  per-signal weights** (`kb.code_hybrid.*`). The vector SQL was validated against real
  pgvector/halfvec.
- **§6 live**: `/v1/code/scan` skips the git re-walk when the default-branch tree SHA
  is unchanged (`git_resolve_default_sha` + `code_default_branch_changed`, stored in
  `kb_runtime_state`; worktree-opt-in aware), and an opt-in `install_hook:true`
  installs a `post-merge` reindex hook (`code_index_install_branch_hook`) so pulls keep
  the graph fresh (`unit-test-code-collect`, `test_code_scan_*`).
- **§7** structural blast-radius **advisory** on the guardrail edit path + **graph-informed
  delegation** (a delegate's prompt is prefixed with the callers/dependencies of the
  files its task references). Both advisory, fail-open, structural-only, opt-in
  (`guardrails_blast_radius.c`, `delegate_inject_graph_context`,
  `unit-test-guardrails-blast-radius`, `test_delegate_dispatch_reliability`); folds in
  the stale-edge hub note.
- **§8** read-only node-projection route, `GET /v1/code/graph?project&node&max_results`
  returns a node's incident edges (relation / direction incl. `self` / structural weight /
  §3 provenance) with `match_count` + a page-or-scan `truncated` flag
  (`handle_get_code_graph`, `test_code_graph_node_*`), **plus the webchat Graph view**
  (`frontend/src/pages/Graph.tsx`) that ranks hubs, drills neighbors, and shows
  surprising links, via webchat Go proxies `/api/graph/*` (`webchat/graph.go`,
  `webchat/graph_test.go`) over the `index_graph_*` MCP tools.

**Deferred future-work, optional, ratified at close-out (every shipped section's
front-end is complete).** A close-out roundtable (6 panelists, 0 failed, not degraded)
converged on closing the
proposal now and carrying the two items below as explicit future-work, each with the scope
recorded so it is a self-contained task, not a re-discovered design:

- **§2 grammars beyond the 16-class supported set.** The tree-sitter front-end, all
  hand-rolled-parity grammars (17 grammars covering the 16 hand-rolled language classes.
C and C++ split), call-edge extraction, nested-member descent, and the opt-in
  `treesitter` CI lane all ship. The proposal's aspirational ≥30-language target is **not**
  met and **not** claimed shipped, only the 16-class hand-rolled parity set is covered.
  Extending toward it is mechanical and carries **no parity regression** (the extra
  languages have no hand-rolled coverage today), so it is a build-tier / binary-size
  decision per deploy, not a gap. Per-language add recipe: (1) vendor its `parser.c`
  (+ `scanner.c`) via `scripts/fetch-treesitter.sh`; (2) register its `TSLanguage` +
  extensions in `ts_language_for_ext`; (3) add a `classify_*` (probe the grammar's node
  types first. Don't guess); (4) extend the per-language `unit-test-code-treesitter`
  fixtures + the supported-languages list. *Hardening note (panel):* add a CI assertion
  that enumerates every supported language, loads its grammar, and smoke-parses a fixture,
  failing the build on any missing/mismatched grammar, so a dropped grammar surfaces
  loudly rather than silently skipping previously-parsed code.

- **§6 always-on filesystem watch. A known freshness limitation, not just a nice-to-have.**
  The change-detection SHA gate, the post-merge + post-checkout reindex hooks, and the
  memory-fusion leg all ship, so the **git** path is fully covered: a pull/merge/branch-
  switch advancing the default branch re-indexes (cheaply no-op'd when nothing moved).
  **Limitation:** there is **no always-on fanotify/inotify watch**, so a **non-git tree**,
  or a git tree mutated **out-of-band** without a merge/checkout event, is only as fresh as
  the last manual rebuild, and §7 blast-radius advisories + graph-informed delegation that
  read graph state inherit that staleness for such trees. The manual refresh path exists
  today (`aimee index scan <project> <root>`, or the `/v1/code/scan` route); the gap is
  *automatic* freshness without a git event. Deferred because this dev/build host
  **SIGTERMs long-running watch daemons**, so a watcher cannot be end-to-end validated here,
an operational blocker, not a correctness one. Revisit on a deployment target that
  supports persistent watchers; until then, treat non-git / out-of-band freshness as
  manual-refresh-only.

- **Post-merge AppSec pass (recommended, separate from this close-out).** This proposal
  closing does not substitute for a code-level security review of the landed `testing`
  surface. A dedicated AppSec pass over §3 provenance, §4 LLM-judge (untrusted-output-
  driven; runs opt-in + advisory with deterministic precision self-suppress + bounded
  candidate count), §5 RRF legs, §6 reindex hooks (marker-guarded, SHA-gated, opt-in;
  `shquote`-hardened against ref-name injection), §7 advisory delegation, and §8 the web
  route (bounded `max_results`, UDS-trusted) is recommended before promotion beyond
  `testing`.

## Non-goals

- **Portable / offline / git-committed graph artifact.** Explicitly out of scope,
aimee's value is the live server-side graph + embeddings + memory, not a file you
  carry around. We will not invest in export portability as a headline.
- A general graph DB (Neo4j/etc.), pgvector + DB2 stay the store.

## Risks / honest limits

- Tree-sitter integration is real C/build work (vendoring grammars, ABI), largest risk.
- Auto-building all layers raises per-ingest LLM/GPU load; must stay incremental +
  bounded per poll (reuse the embed-drain backpressure model).
- "Surprising-links" quality depends on embedding quality, mitigated by §4's
  relevance gate (quality floor + confirmation stage + sampled-precision self-suppress).
- Webchat viz scale: large graphs need server-side paging/aggregation.

## Tests

- Unit: projection-sync **idempotency** (sync twice → identical edge set + zero-work
  second pass); provenance tagging; **RRF fusion ordering** (rank blend + tie-break),
shipped as `unit-test-kb-rrf`; surprising-links relevance gate (quality floor +
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

## Review revisions (R2): source of truth

A second roundtable (`mistral` / `mimo-2.5-pro` / `glm-5.2`; 3/3, 0 failed) on the
indexing source converged on §0.5: index the **git default branch**, not the
working tree. Load-bearing outcomes, all now in §0.5:

1. **Drop the current-HEAD fallback**: it re-introduced WIP-thrash; an unresolvable
   default branch now **skips** with a diagnostic instead.
2. **Repair `origin/HEAD`** with `git remote set-head origin -a` before descending
   the fallback chain (it is unset on a long tail of real checkouts).
3. **Read via `ls-tree` + `cat-file --batch`** (one fork each), pairing content to
   path by sequence (NUL-safe), not per-file `git show` (N forks).
4. **Code-vs-edit-tool split is a first-class invariant**: indexing reads the
   default branch; blast-radius / staged-change tools read the working tree.

Implemented in `code_collect.c` with `unit-test-code-collect` covering all cases.
