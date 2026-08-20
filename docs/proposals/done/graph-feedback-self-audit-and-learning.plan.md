# Implementation plan: Graph feedback and coverage expansion

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Companion to `graph-feedback-self-audit-and-learning.md`. Slice-by-slice build
order, each slice an independently-shippable PR that is roundtable-reviewed
before merge. Base branch: `origin/testing`. Worktree: `../aimee-graphfb`.

## Gap analysis — what actually exists vs. what the proposal assumes

Surveyed the codebase (2026-07-03). The proposal's §0 "what already exists" is
**partly inaccurate**; two load-bearing assumptions are false and change the
slice plan:

| Proposal §0 claim | Reality in the code | Impact |
|---|---|---|
| Hubs / centrality — `kb_graph_hubs`, `/v1/code/graph/hubs` | ✅ `src/kb/kb_graph_analytics.{c,h}`, route in `kb_http_code.c`, MCP `index{command:hubs}` | reuse as-is; add `bottom` mode |
| Surprising links (code-graph §4) | ✅ `kb_graph_surprising` + judge suppress | not touched here |
| Confidence/provenance tags per edge | ✅ `kb_graph_edge_provenance(edge_origin, structural_weight)` in `kb_service_graph.c` already derives `structural`/`inferred`/`ambiguous` (e.g. `curator`+weight-0 → `inferred`, `session`+weight-0 → `ambiguous`); `entity_edges.structural_weight`/`edge_origin`/`review_class` back it | consume this fn as-is for §1 unverified-inferred |
| Projection generations (versioned) | ✅ `code_projection_generations` + lifecycle (`code_projection.{c,h}`) | diff reads these; but `db2_code_projection_list_edges` only reads the **visible** generation — §2 needs arbitrary-gen listing |
| **Communities — Louvain/Leiden, "partly shipped"** | ❌ **NOT shipped.** No `community_id` column, no Louvain/Leiden anywhere. | **New foundation slice required.** §1 cohesion, §2 community-ID stability, §3 lessons-grouping all depend on it. |
| **§4 "existing central agent-memory GA sanitizer" to extend** | ❌ **Does not exist.** Prompt-injection provenance was named a *future GA gate* in `central-agent-memory-interception.md`, never built. No `sanitize_for_prompt`, no render-boundary sanitizer. | **P0 must CREATE the sanitizer**, not extend one. The proposal's "name that sanitizer's file / add to its kinds" contract is re-cast as "create the module + own it." |
| Tree-sitter front-end, 17 languages | ✅ `src/code_treesitter.c` clean grammar table (`ts_language_for_ext`, `ts_lang_t`) | §6a adds grammars the documented way |
| MCP `index` family + `/v1/code/*` routes | ✅ `mcp_tools_extended.c`, `kb_http_code.c` — only `hubs`/`surprising` today | `audit`/`diff` are net-new commands + routes |

**Net:** the substrate is real, but **community detection and the sanitizer are
missing prerequisites** the proposal assumed present. Both become explicit
early slices. Everything else (self-audit findings, diff, outcome ledger,
reflection, cache namespacing, Tier-1 grammars) is genuinely net-new.

## Slice order (each = one roundtable-reviewed PR)

Dependency-ordered. P0 sanitizer first because §1/§2/§3 all render
corpus-derived strings into agent prompts and must not merge in front of the
boundary. Community detection second because three later slices depend on it.

### S0 — P0: prompt-injection sanitizer (prerequisite)
- **Pre-audit (R1):** ship a short note in the PR naming the existing string
  primitives found (`strip_llm_private_scaffold` in `util.c`,
  `server/tool_schema_sanitizer.c`, `shell_escape`) and confirming none is a
  render-boundary prompt sanitizer — so S0 is purely the boundary, not a
  string-utils grab-bag.
- New module `src/kb/prompt_sanitizer.{c,h}` (owned here, since none exists).
- **Status-returning API (R1)**, not void/string-copy:
  `sanitize_status_t sanitize_for_prompt(const char *field, sanitize_kind_t kind, char *out, size_t out_len, size_t *out_written)`
  returning `SANITIZE_OK` / `SANITIZE_TRUNCATED` / `SANITIZE_REJECTED` + a
  reason code, so a security-sensitive renderer can fail closed per field.
- **Two-layer design (R1):**
  - *Strict validators* for structured fields (`file_path`, `symbol_label`,
    `source_location`, `community_name`) — reject (not silently rewrite) on
    newline / control / shell-log markers.
  - *Render-context escaping* for free text (`memory_fact`, `lesson_text`,
    `correction_text`, `image_caption`, `transcript`, `markdown_doc`) —
    neutralize injection markup, bound length, never reject.
- `sanitize_kind_t` enum covers all ten kinds above. Every kind strips
  ANSI/C0/C1; each carries an explicit length bound.
- **Enumerated markers (R1)** documented in the PR + covered by a *categorized*
  attack corpus: `<system>`/`<|im_start|>`/`<|im_end|>`, fake tool-call markers,
  `### Instruction:`, backtick-fenced role mimicry, ANSI role tags, fabricated
  `[graphify]`/log lines, embedded prompt directives.
- Ships: `src/tests/test_prompt_sanitizer.c` + fixtures (categorized by attack
  type), a call-site audit doc (`docs/SANITIZER_CALL_SITES.md`), and a **CI
  guard built around a small set of sanctioned renderer helpers** (a render
  call must pass a sanitized value) — grep is only a backstop.
- **No behavioural surface yet** — the render call-sites arrive in S1/S5/S6.

### S-community — foundation: deterministic community detection
- New `kb_graph_communities()` in `kb_graph_analytics.{c,h}`. **Algorithm pinned
  (R1):** single, fully-specified deterministic method — **no "fallback."**
  Normalization spec documented in the header: edges treated **undirected**,
  parallel edges aggregated by summed structural weight, self-loops dropped,
  edge weight = `structural_weight`, modularity with resolution γ=1.0, fixed
  iteration cap, convergence = no-move-pass, total-order tie-break on every
  move decision (candidate community by `min-member-id` lex; final community id
  = `min-member-id` lex). Pure, in-memory.
- Persist community membership **keyed by projection generation-id for every
  generated projection (R1)** — not only the visible one — so S2 can diff
  arbitrary generations. New table `code_projection_communities(gen_id,
  node_id, community_id)`; retention follows the generation cleanup schedule.
- Two-pass cross-generation stable-ID remap is deferred to S2 (needs a prior
  generation to map against); S-community lands the single-generation
  deterministic partition, whose ids are **generation-local** by contract.
- Tests: partition determinism under input permutation; known-modular fixture.
- Parallelizable with S0 (produces IDs, renders nothing) — but sequenced after
  it so S0 review surfaces community-name rendering concerns first.

### S1 — P1: graph self-audit
- New `GET /v1/code/graph/audit?project` + MCP `index{command:"audit"}`.
- Finding types, each a pure function over the projection edge array:
  - **cycles**: collapse to file nodes, Tarjan SCC + bounded per-SCC DFS (cap
    100/SCC, truncation reported).
  - **orphans**: `bottom`/`bottom-excluding-hubs` **mode on the hubs route**
    (not a fork) — degree ≤ 1 real defs.
  - **bridges**: exact Brandes below a node/edge threshold, deterministic-seed
    sampling above with `approximate:true`.
  - **low-cohesion**: conductance / modularity-contribution per community (uses
    S-community), gated at size ≥ 8, reports metric + threshold. **Community IDs
    here are generation-local/provisional (R1)** until S2's two-pass remap;
    responses mark them `community_id_scope:"generation-local"`.
  - **unverified-inferred**: symbols with ≥ k inferred/ambiguous edges never
    confirmed. **Ships explicitly labeled `signal:"no-confirmation-yet"` (R1)**,
    excluded from the default audit-summary roll-up, expected volume documented
    in the PR — because the confirmation signal only arrives in S3c. Each
    finding carries a **stable finding ID** for the §1↔§3 loop.
- **Staleness visibility (R1):** the audit response echoes the
  `extractor_version` / `pipeline_version` of the generation it analyzed, so a
  consumer can detect a result computed over a stale cache before S4 lands.
- Honesty gate: explicit "insufficient signal" / "clean" result.
- Every rendered string routes through S0 `sanitize_for_prompt`.

### S2 — P2: determinism substrate + snapshot diff
- Determinism: stable generation-independent **node identity** (`project +
  norm-rel-path + language + symbol-kind + qualified-name`, span-hash fallback);
  total-order tie-breaks audited across hubs/audit/communities; **two-pass
  community remap** (claim-by-overlap then fresh-ID lex order).
- Extend `code_projection` read API to list edges of an **arbitrary** generation
  (not just visible) + carry confidence tag.
- New `GET /v1/code/graph/diff?project&from_gen&to_gen` (+ named aliases
  `default_latest`/`working_tree`): node/edge add-remove, rename detection,
  new-cycle, new cross-community edge (both old/new partition-crossing),
  newly-orphaned. `diff_kind` per edge; **refuse cross-extractor-version compare
  without `force`**; HTTP 409 + generation list when a gen is missing.
- Persist diffs keyed by `(from_gen, to_gen, pipeline_version)`.
- Permutation-invariance tests are the acceptance gate.

### S3 — P3: retrieval learning (the headline; split into 4 slices, R1)
- **S3a-schema**: ledger table + immutability (append-only, no-UPDATE/no-DELETE
  trigger) + **enforced isolation** — the record schema exactly as §3. Isolation
  is *enforced, not commented (R1)*: the ledger lives in its own table whose
  name carries a `lessons_` prefix that `db2_memory_find_facts_like` and the
  decay/prune sweep explicitly skip, **plus a CI grep guard** that fails if a
  `lessons_*` table is joined into memory-fact recall or the prune schedule.
  This slice is schema-only — no capture, no consumers.
- **S3a-api**: the capture call-site. **Names the cite-emit hook (R1)** — the
  point in the central agent-memory interception boundary where a citation is
  observed (file:line identified during S3a-schema; if absent, S3a-api builds
  it). Auto-`useful` proxy = cite-again-within-N-turns, with a test proving the
  counter increments on a real citation.
- **S3b reflection + lessons**: deterministic trust scoring — signed
  time-decay, asymmetric half-lives (`dead_end` 30d / `corrected` 180d),
  corroboration ≥ N distinct, contested-by-recency; byte-stable output for a
  fixed `now`. Lessons artifact grouped by community label (uses S-community),
  scoped by project/tenant/branch.
- **S3c actuation**: session-preamble injection (scoped, team-lessons gated) +
  RRF **tie-break-only** hook + §1↔§3 finding-ID loop (verdict on a §1 finding
  writes to the ledger). **Correction-authority actor model named explicitly
  (R1):** `actor.source ∈ {user, reviewer, agent}` where `user` = the human on
  the session, `reviewer` = a caller carrying the reviewer capability already
  used by the agent-directed-review path, `agent` = autonomous; `confirmed` is
  a ledger-row flag settable to true **only** by a `user`/`reviewer` actor. If
  that capability check has no existing enforcement point, S3c builds it; agent
  corrections stay `confirmed=false` and inert until then.

### S4 — P4: cache-correctness refinements
- **Explicit version-marker scheme (R1):** cache rows carry a `namespace_version`
  column (not a key-prefix hack); a cross-version read test proves a v1-written
  entry is still *readable* after v2 lands and that a v1-keyed lookup never hits
  a v2 entry.
- Version-namespace the structural/AST cache (extractor version); sweep other
  versions on first use.
- Namespace the semantic cache by full contract `(content_hash,
  extractor_contract_version, prompt/schema_version, model_family/version,
  embedding_model_version)` — **preserve** old-namespace entries (never re-bill).
- Frontmatter-insensitive doc hashing: hash body + allowlisted ignorable keys
  (`status`, `reviewed`, `tags`, `date`) only.
- **Ordering note (R1):** S4 depends on S2's node-identity/version substrate;
  the earlier slices (S-community/S1/S2) surface staleness via the echoed
  `extractor_version` (see S1) rather than blocking on S4, so S4 stays late but
  its dependency on S2 is documented.

### S5 — P5: Tier-1 language grammars (one PR each)
- Order by leverage / build simplicity: **Scala**, **Groovy** (+`.gradle`),
  **Elixir**, **Objective-C/++** (`.m`/`.mm`), **PowerShell**, then Kotlin-Script
  edge cases.
- Per grammar: vendor grammar (`scripts/fetch-treesitter.sh` + Makefile),
  register in `ts_language_for_ext` + `ts_lang_t`, defs/calls classifier,
  one fixture + parse/extract test, CI matrix entry + `AIMEE_GRAMMARS` selector.
- Fall through to text-only storage when a grammar is absent; expected-grammar
  build failure fails loudly.
- **Grammar-drop policy for already-indexed repos (R1):** if a
  previously-available grammar becomes unavailable, prior AST nodes are left
  untouched and new files of that extension index as text-only (option (b)) —
  no destructive re-index, no startup refusal; a structured log records the
  degrade. A version bump that intends to *replace* an extractor's output is an
  S4 namespace concern, not a silent drop.

### S-closeout
- Roundtable-verify all in-scope sections shipped; move proposal to
  `docs/proposals/done/` with close-out notes (deferred: §6b media + Tier 2–5,
  which the proposal already split into follow-on proposals).

## Out of scope (already split by the proposal)
- §6b visual-media, audio/video transcription, Tier-2 reference extractors,
  Tier 3–5 grammars → each its own follow-on proposal.

## Testing
- Unit: pure analytics tested standalone (ctest) — this is where determinism /
  permutation-invariance / trust-math live.
- Integration: a fresh CT on `root@192.168.1.253` with an indexed fixture repo
  to exercise the `/v1/code/graph/audit` + `/diff` routes and the outcome-ledger
  round-trip end-to-end (unit tests can't cover the DB2 + HTTP path).
- Every slice PR passes the repo CI gates (lint, docs-gen, schema-sync, no
  co-author trailers) per `aimee-pr-ci-gates` memory.

## Roundtable protocol
- Plan approved by roundtable before S0.
- Each slice: roundtable-review the **code** (not just design) before PR merge,
  per the `always-roundtable-review-before-pr` standing rule. `--rounds 1`.

## R1 — roundtable plan-review dispositions (2026-07-03)

Roundtable review (`--mode review --rounds 1`, 6/7 panelists, not degraded, 49
items). **No blocking rejections** — the plan and both gap re-scopings (community
detection not shipped; sanitizer must be created not extended) were accepted.
Suggestions folded into the slices above:

- **S0 sanitizer:** status-returning API (`ok/truncated/rejected` + reason);
  two-layer split (strict validators for structured fields vs. render-escaping
  for free text); CI guard around sanctioned renderer helpers, grep as backstop;
  enumerated attack markers + categorized corpus; pre-audit of existing
  primitives. *(accepted)*
- **S-community:** algorithm pinned (no "fallback"); full normalization spec in
  the header; membership persisted **per generation-id**, not visible-only, so
  S2 diff can compare arbitrary generations. *(accepted)*
- **S1:** low-cohesion community IDs marked generation-local/provisional until
  S2; unverified-inferred ships labeled `no-confirmation-yet`, out of the
  default roll-up, expected volume documented; audit echoes `extractor_version`
  for staleness visibility. *(accepted)*
- **S3:** split into S3a-schema / S3a-api / S3b / S3c; isolation *enforced*
  (name-prefix skip + CI guard) not commented; cite-emit hook named in S3a-api;
  correction-authority actor model made explicit in S3c. *(accepted)*
- **S4:** explicit `namespace_version` column + cross-version read test;
  dependency on S2 documented; kept late (staleness surfaced via S1's echoed
  version rather than blocking earlier slices). *(accepted)*
- **S5:** grammar-drop policy pinned to option (b) — leave prior AST, index new
  files text-only, structured-log the degrade. *(accepted)*
- **Slice order:** S0 and S-community are parallelizable but sequenced so S0
  review lands first; the stale-cache concern (move S4 earlier) is addressed by
  staleness-visibility echoes rather than reordering, keeping the critical path
  short. *(accepted with rationale)*
