# Proposal: Graph feedback and coverage expansion — self-audit, snapshot diffing, retrieval that learns, and broader language coverage

- **State:** PENDING — net-new design, not yet started. Builds on the existing
  code-graph substrate (`code_projection_edges`, `entity_nodes/edges`, `code_embeddings`,
  the memory graph in DB2, the MCP `index` family, `/v1/code/graph/*`) and the
  central agent-memory interception layer. Does **not** re-propose anything in
  `code-graph-intelligence.md` (§3 provenance, §4 communities/hubs/surprising
  links, §5 RRF hybrid retrieval, §6 live updates, §7 actuation) — it adds the
  *feedback* loops that sit on top of that substrate.
- **Author:** JBailes
- **Date:** 2026-06-29
- **Charter roles:** Reason (self-audit + gap surfacing), Persist (durable
  outcome ledger + lessons artifact), Calibrate (time-decayed corroboration,
  determinism), Review (snapshot diff as a PR signal).

## Thesis

aimee already turns the codebase into a living, queryable graph with provenance,
analytics, and hybrid retrieval. Three things are still missing, and all three
are *feedback* — the graph informs the agent, but nothing flows back:

1. **The graph never audits itself.** It can answer "what calls X?" but it never
   says "these 14 inferred edges have never been confirmed," "this symbol is
   orphaned," or "this module is incohesive — it should probably be split." The
   richest signal in a confidence-tagged graph is *what it is unsure about*, and
   today that signal is computed implicitly and thrown away.

2. **Nothing compares two versions of the graph.** Every retrieval is against
   *now*. But the most valuable question at review time is *what changed in the
   structure* — did this branch introduce a circular import, a new cross-module
   coupling, a removed caller, an orphaned symbol? That is a pure graph-diff and
   we don't compute it.

3. **Retrieval never learns from its own outcomes.** When an agent retrieves
   context, acts on it, and the result is good (or a dead end, or the user
   corrects it), that outcome is lost. The next session re-derives the same dead
   ends and re-trusts the same misleading sources. A graph that records which of
   its own answers *worked* can rank its sources by earned trust, not just by
   structure.

This proposal adds those three loops. The first two are cheap, deterministic
analytics over the existing graph. The third is the headline: a durable,
LLM-free **retrieval-outcome learning loop** that makes the agent measurably
better at finding the right context the longer it works in a repo.

It then adds a fourth, independent workstream: **ingestion-coverage expansion
(§6)** — the graph is only as good as what feeds it, and there are whole classes
of input that aimee silently does not extract today. After review this is
**descoped to its parity-blocking core**: only **Tier-1 language grammars** ship
in this proposal (cheap, deterministic, on the existing tree-sitter substrate).
The heavier strands — reference extractors for SQL/HCL/config, visual-media
ingest (with an evaluation gate), and local audio/video transcription — are each
**split into their own follow-on proposal** so they get a real design pass and
don't dilute review of the feedback loops.

## Goal

Make the graph a *closed loop*: it tells the agent what to verify (self-audit),
it tells review what structurally changed (snapshot diff), and it remembers which
of its own answers earned trust (retrieval learning) — so context quality
compounds across sessions instead of resetting.

## §0 What already exists (so we don't rebuild it)

- **Confidence/provenance tags per edge** — `structural` / `inferred` /
  `ambiguous`, derived from structural-trust weight + edge source
  (`code-graph-intelligence` §3). This proposal *consumes* those tags; it does
  not redefine them.
- **Hubs / centrality** — `GET /v1/code/graph/hubs`, `kb_graph_hubs`. Degree
  ranking exists; §1 below adds the *other* structural-health signals
  (orphans, cycles, cohesion) that hubs alone don't surface.
- **Communities** — Louvain/Leiden over the typed graph (§4, partly shipped).
  §1 reuses community membership for cohesion scoring; §3 reuses it for
  source grouping.
- **Memory graph (DB2)** — `entity_nodes/edges`, `db2_memory_find_facts_like`,
  and the central agent-memory interception hooks. §3's lessons artifact is a
  new *consumer* and *producer* of memory, not a new store.
- **MCP `index` family + `/v1/code/*` routes** — the surfaces these three loops
  plug into. No new transport.

## §1 Graph self-audit — surface what the graph is unsure about

A read-only analytic pass over the published projection graph that emits a
ranked list of **structural-health findings** and **verification questions**.
Pure, deterministic, no LLM. Each finding has a `type`, the nodes/edges it
concerns, and a one-line `why`.

Finding types (all computable today from `code_projection_edges` + confidence
tags + community membership):

- **Unverified inferred edges.** Symbols carrying ≥ k `inferred`/`ambiguous`
  edges that have never been confirmed by an outcome signal (see §3). These are
  the model's guesses about the architecture — the highest-value thing to ask a
  human or a delegate to confirm. *"Are the 5 inferred relationships involving
  `auth_resolve` actually correct?"*
- **Orphans / weakly-connected symbols.** Real definitions with degree ≤ 1
  (excluding file-hub and container nodes). Candidate dead code, a missing edge
  the extractor dropped, or an undocumented entry point. *"What connects
  `legacy_token_path` to the rest of the system?"* Implemented as a `bottom` /
  `bottom-excluding-hubs` **mode on the existing `/v1/code/graph/hubs` route**,
  not a sibling endpoint — one ranking, two views, so the top and bottom of the
  degree distribution can't disagree.
- **Import / dependency cycles.** File-level circular dependencies, found by
  collapsing symbol nodes to their source file, orienting `imports`/`depends_on`
  edges, then **Tarjan SCC decomposition followed by a bounded DFS per SCC**
  (per-SCC cap, e.g. 100 cycles) — not naive Johnson enumeration, which explodes
  on dense diamonds. Truncation is reported in the response, never silent. A
  concrete refactor signal the current hub/centrality list does not produce.
- **Bridge symbols.** High edge-betweenness nodes that are *not* file hubs —
  the cross-cutting concerns that connect otherwise-separate modules. *"Why does
  `config_load` connect the auth module to the curator?"* **Exact Brandes below
  a node/edge threshold; deterministic-seed sampling above it**, marked
  `approximate:true`, so the audit stays cheap on large graphs.
- **Low-cohesion modules.** Communities scored by **conductance / modularity
  contribution** (not raw `actual/max-possible` density, which returns 1.0 for a
  3-node community and misreads sparse-but-valid dependency structure), gated at
  community size **≥ 8**, reporting the metric value and threshold with each
  finding — split candidates. *"Should `kb` be split into smaller, more focused
  modules?"*

**Why this matters for aimee.** §4 already ranks the *most-connected* symbols;
self-audit ranks the *least-trustworthy and least-explained* parts of the graph.
Those are exactly the inputs a roundtable review, a sweep proposer, or an
autonomous-dev planner should be fed first. Wire it as:
`GET /v1/code/graph/audit?project` + `index({command:"audit", project})`,
returning the ranked findings. The autonomous-dev loop can consume "unverified
inferred edges" and "cycles" directly as work items.

**Honesty gate.** Findings are bounded and deterministic (stable sort, total-order
tie-breaks). When there is no signal — no ambiguous edges, no orphans, cohesive
communities — the route says so explicitly rather than inventing noise. A
"clean" audit is a real, reportable result.

## §2 Graph snapshot diff — what the structure did, as a review signal

Given two graph generations of the same project (the projection layer is already
versioned via `code_projection_generations`), compute a structural diff:

- **Nodes** added / removed (new symbols, deleted symbols).
- **Edges** added / removed, grouped by relation and by confidence tag.
- **Newly introduced cycles** (a §1 cycle present in `new` but not `old`).
- **New cross-community edges** (a coupling that now bridges two modules that
  were previously separate) and **newly orphaned** symbols.

Edge identity is relation-typed and direction-aware; **node identity** must be
stable and generation-independent for the diff to be well-defined — keyed on
`project + normalized-relative-path + language + symbol-kind + qualified-name`,
with a source-span hash only as a last-resort fallback. A same-symbol move
across files is reported as a **rename** (matched by qualified-name + kind), not
a delete+add, and key collisions are disambiguated by span. The summary is a
one-line `"N new edges, M removed, 1 new import cycle, 1 rename"`.

**Source change vs. our own code change.** A diff must not present a *parser*
change as a *structural* change. Each changed edge carries a `diff_kind`
(`source-induced` / `extractor-induced` / `projector-induced`), and the route
**refuses to compare two generations whose extractor version differs** unless the
caller passes `force=true` and accepts an explicit warning header. The route
takes explicit `from_gen`/`to_gen` or named aliases (`from=default_latest`,
`to=working_tree`) and returns HTTP 409 with the available generation list when
either is missing. Diffs are persisted keyed by `(from_gen, to_gen,
pipeline_version)` so the PR-review path reads, not recomputes.

**Why this matters for aimee.** This is the single most useful thing the graph
can contribute to **PR review** and the **agent-directed review** path: instead
of re-reading the diff line-by-line, the reviewer is handed *"this branch added
a circular dependency between `a.c` and `b.c`, introduced a new edge from the
auth module into the curator, and orphaned `old_handler`."* It is a pure
function of two generations we already persist — no new extraction. Surface it
as `GET /v1/code/graph/diff?project&from_gen&to_gen` and feed it into the
review roundtable brief. The default-branch indexing model (§0.5 of the
code-graph proposal) means `from` = last indexed default-branch generation and
`to` = the working-tree generation for a branch under review.

**Determinism is a hard requirement here.** A diff is only useful if it shows
*real* change. Two analytics traps must be closed or the diff is pure noise:

- **Stable community IDs across runs (two-pass, deterministic).** Pass 1: claim
  each new community for its best-overlap predecessor by `|intersection|`, ties
  broken by `lex(min(member_id of intersection))` — no raw "greedy" that
  permutes on ties. Pass 2: unclaimed new communities get fresh IDs in
  `min-member-id` lex order. So "community 3" means the same module run-to-run,
  with no phantom churn. The cross-community-edge diff reports **both**
  `old_partition_crossing` and `new_partition_crossing` per edge, so a genuinely
  new coupling is distinguished from a community boundary that merely moved.
- **Total-order tie-breaks** in every ranked output (hubs, audit, communities),
  so equal-scored items don't permute between runs and read as change.

This determinism work benefits §1 and §4 too; it is the reproducibility
substrate for any persisted/diffed graph artifact.

## §3 Retrieval that learns from its own outcomes (the headline)

Today a retrieval is fire-and-forget. This loop closes it.

### The signal

When an agent answers from the graph (a hybrid query, a callers lookup, a
memory recall), it can record an **outcome** for that answer. The record has an
explicit schema:

```
outcome_record {
  session_id, turn_id, project_id, branch/generation,
  cited_node_ids[],            # generation-qualified node identities (§2)
  answer_outcome,              # useful | dead_end | corrected  (answer-level)
  per_citation_disposition[],  # optional per-node: useful | stale | unused
  correction_text,             # if corrected
  actor: { id, source },       # user | reviewer | agent  (provenance, see below)
  confirmed,                   # bool — gates durable negative trust
  ts
}
```

**Attribution is two-level**, because a correct answer can cite a stale source
and a wrong answer can cite the right one. Answer-level `useful`/`dead_end`/
`corrected` is always recorded; `per_citation_disposition` is recorded when the
agent can attribute (it carries an explicit citation list at answer time).
Absent per-citation signal, credit is split equally across cited nodes — never a
blanket penalty on every source of a failed answer. Auto-capture of `useful`
fires when the agent **cites a source node again within N turns** of the answer
(`AIMEE_TRUST_AUTO_USEFUL_TURNS`, default 3) — a concrete, non-circular proxy,
not "the agent acted on it."

**Storage is isolated from the memory graph.** Outcome records live in a
**dedicated outcome ledger** (a new table + a `db2_lessons_*` API family), *not*
in `entity_nodes/edges`. They must be excluded from `db2_memory_find_facts_like`
and from the memory-fact decay/prune schedule — otherwise outcomes leak into
normal recall and get pruned out from under the learning loop. The central
agent-memory interception layer is the capture point (it sits between the agent
and its memory); it writes the ledger, it does not co-mingle with facts.

### The aggregation (deterministic, no LLM)

A periodic reflection pass folds the outcome records into a **trust score per
source node**. Crucially it *scores*, not *counts*:

- each citation contributes a **signed, time-decayed** value — `useful`
  positive, `dead_end`/`corrected` negative — with **asymmetric half-lives**:
  `dead_end` decays on a short half-life (`AIMEE_TRUST_HALF_LIFE_DAYS`, default
  30) while `corrected` factual fixes are *sticky*
  (`AIMEE_TRUST_CORRECTION_HALF_LIFE_DAYS`, default 180) so a real correction
  doesn't silently expire back to "trusted";
- a node is promoted to **preferred** only once **corroborated** by ≥ N
  *distinct* answers (`AIMEE_TRUST_CORROBORATION_THRESHOLD`, default 2) — one
  lucky hit can't mint a trusted source;
- a node with both positive and negative history is **contested**, and
  **recency decides** the current verdict;
- the ledger is **immutable and append-only**: a record is never deleted when
  its node disappears (rename/delete/extractor-ID churn) — it is keyed by
  generation-qualified node identity and retained for the audit trail. Missing
  *current* nodes are excluded from active re-ranking but their corrections and
  dead-ends survive in the lessons ledger (documented retention window).

The output is a compact, ranked **lessons artifact** grouped by community label:

- **Preferred sources** — corroborated, reliably useful (rank these up in §5's
  RRF memory leg).
- **Tentative** — useful once, not yet corroborated.
- **Contested** — mixed signal; recency-resolved.
- **Known dead ends** — questions/sources that led nowhere; *don't re-derive
  them*.
- **Corrections** — answers the user fixed, and the correct answer.

The whole pass is deterministic: stable sort, byte-stable output for a given
input and a given `now`. No LLM, no per-release re-billing.

### How it actuates

Two consumers, both already in the architecture:

1. **Session preamble.** Load the lessons artifact (or the relevant community
   slice) at session start, the same way central agent-memory injects context.
   **Lessons are scoped** — by `project_id`, by tenant/user boundary, and by
   branch/generation applicability — so a dead end learned in one repo, branch,
   or user's session never bleeds into an unrelated one. Shared *team* lessons
   require an explicit approval gate before they cross a user boundary.
2. **Retrieval re-ranking.** Feed per-node trust into §5's RRF fusion **as a
   tie-break only** (v1) — RRF's rank-distance blend is the primary signal and
   trust nudges within a tie, so earned trust never overrides structural truth.
   (A trust-as-weight variant is explicitly deferred; it changes RRF's
   invariant and needs its own calibration.) Known dead-end nodes are demoted;
   corrected answers carry their correction inline.

**Correction authority.** A `corrected` record only becomes durable trusted
material under a provenance/authority model: every record carries `actor.source`
(`user` / `reviewer` / `agent`) and `confirmed`. An autonomous **agent cannot
mint durable negative trust unilaterally** — agent-sourced corrections stay
`confirmed=false` and influence nothing until a user or reviewer confirms them.
Only confirmed corrections reach the session preamble or re-ranking.

**Closing the §1 loop.** §1's "unverified inferred edge" findings carry stable
finding IDs. When such a finding is confirmed or refuted (by a human, a reviewer,
or a delegate), that verdict is written to the same outcome ledger keyed by the
finding ID — which both updates the edge's ambiguity and feeds the lessons
artifact. This is the wire that makes §1 (surfacing what's unverified) and §3
(recording what was verified) a single closed loop rather than two open halves.

**Why this matters for aimee.** This is the difference between a memory that
*stores* facts and a memory that *learns which facts pay off*. It directly
feeds the autonomous-dev loop (don't re-explore known dead ends), the roundtable
(prefer corroborated sources), and recall quality generally. It is also cheap
and safe: deterministic, append-only, LLM-free, and it degrades to today's
behavior when there are no outcome records yet.

## §4 Provenance sanitization as an injection boundary (hardening)

Every field that flows from corpus content into an agent's context — symbol
labels, `source_file`, `source_location`, community names, memory-fact text —
is attacker-influenceable: a malicious file path or docstring can carry ANSI
escapes, fabricated log lines, or prompt-injection markup. Any graph/memory
field that is rendered into an agent prompt should pass through a **single
sanitizer** at the render boundary (strip control/escape sequences, neutralize
injection markup, bound length) before concatenation.

This is the same concern the central agent-memory GA gate raised
(*prompt-injection provenance*), made concrete for the graph surfaces: §1's
findings, §2's diff, and §3's lessons all render untrusted-derived strings into
the agent, so each must go through the boundary. Cheap, centralizable, and it
closes a real hole as soon as the graph ingests anything beyond first-party code.

**Contract (not a wishlist).** This **extends the existing sanitizer** from the
central agent-memory GA work rather than introducing a parallel mechanism — the
implementing PR must name that sanitizer's file and add to its kinds/rules, and a
"sanitizer owners and reuse" note records the extension. The interface is
`sanitize_for_prompt(field, source_kind)` where:

- `source_kind` is an explicit enum — `symbol_label`, `file_path`,
  `source_location`, `community_name`, `memory_fact`, `lesson_text`,
  `correction_text`, `image_caption`, `transcript`, `markdown_doc`.
- **Per-kind rules:** strip ANSI/C0/C1 control sequences from every kind; bound
  length per kind; for path/label kinds reject newline and shell/log-line
  markers; for the Markdown/HTML/RST and model-derived kinds (`image_caption`,
  `transcript`, `lesson_text`, `correction_text`) neutralize injection markup
  (fenced blocks, fake tool/role tags, embedded prompt directives).
- **Attack corpus** ships with the PR: ANSI escapes in a file path, Markdown
  injection in a docstring, fabricated `[graphify]`/log lines, embedded prompt
  tags (`<system>`, fake tool-call markers) in labels and captions.
- **Call-site audit** document: every render path in §1/§2/§3/§6 enumerated and
  proven to route through the boundary; CI guard so a new render call-site can't
  bypass it.

## §5 Cache-correctness refinements (hardening, optional)

The §1 content fingerprint already detects "did the default-branch content
change." Two refinements make the cache *correct under aimee's own evolution*,
not just under content change:

- **Version-namespace the structural cache.** A structural/AST extraction result
  is the output of aimee's *own extractor code* — a parser fix in a new release
  must invalidate stale structural cache entries (namespace by extractor
  version; sweep other versions on first use).
- **Version the semantic cache by its full contract — but never re-bill.** The
  earlier framing that LLM-derived entries "depend only on file content" is
  wrong: extraction schema, prompt, model family/version, and embedding-model
  version all change the output and can silently serve stale results. Namespace
  semantic entries by `(content_hash, extractor_contract_version,
  prompt/schema_version, model_family/version, embedding_model_version)`.
  Crucially, **preserve** prior-namespace entries rather than deleting them, so a
  contract bump never re-bills *unchanged* files that were already extracted
  under the old contract — it just stops *serving* them under a changed contract.
  The split is: invalidate-and-recompute the cheap deterministic layer; namespace
  (don't discard) the expensive layer.
- **Frontmatter-insensitive doc hashing.** Hash the *body* of a Markdown/MDX
  file plus an **explicit allowlist of ignorable frontmatter keys** (`status`,
  `reviewed`, `tags`, `date`) — the bytes skipped are exactly the YAML block
  between the leading `---` delimiters, nothing else. Title and doc-type markers
  remain hash-significant (opt-in to ignore title). So a metadata-only edit
  doesn't re-bill extraction, but a content edit always does.

(Portable relative source paths re-anchored on checkout are already aimee's
model per the default-branch indexing design; noted only for completeness.)

## §6 Ingestion-coverage expansion — more languages (in scope); media (split out)

Every loop above operates on what the graph *contains*, and the graph is
structurally blind to large parts of a real repo. The review was clear that this
bundles architecturally distinct projects with different risk profiles and
evaluation methods, and that "parity" overpromises given the long-tail tiers.
So this section is **descoped**: only **§6a Tier 1 language grammars** ships
inside *this* proposal (cheap, deterministic, plugs straight into the existing
tree-sitter front-end). Everything else is **named here but split into its own
follow-on proposal** so it gets a real design pass and doesn't dilute review of
the feedback loops:

- **Tier 2 reference extractors** (SQL / HCL / structured config) — own proposal.
- **Visual-media ingest** (images, with an evaluation gate) — own proposal.
- **Audio/video transcription** (local STT) — own proposal.
- **Spreadsheet structural extraction** — folds into the Tier-2 proposal.

### §6a Programming-language parity

aimee extracts definitions + call edges for **17 languages** through its
tree-sitter front-end (C, C++, C#, Python, Go, JS/TS, Rust, Java, Ruby, PHP,
Lua, Bash, Swift, Kotlin, Dart, CSS), with hand-rolled fallbacks for the same
set. Files outside that set are still *stored as text* (the canonical scan is a
binary-denylist, so it keeps `.md`/`.json`/`.yaml`/etc.) but they produce **zero
graph structure** — no symbols, no call edges, so they are invisible to hubs,
communities, blast-radius, the self-audit (§1), and the diff (§2).

The substrate to fix this already exists: `code_treesitter.c` is a grammar
table + per-language classifier, and adding a language is "vendor the grammar,
register it in the extension→grammar map, add a definitions/calls classifier."
The richer extractor we are learning from carries grammars for a substantially
wider set. Proposed parity targets, **tiered by leverage** so each tier ships
independently:

- **Tier 1 — common languages in real polyglot repos, currently dark:**
  **Scala**, **Groovy** (and `.gradle` build files), **Elixir**,
  **Objective-C / Objective-C++** (`.m`/`.mm` — already allowlisted in one walk
  but with no extractor), **PowerShell**, **Kotlin Script** edge cases. These are
  the genuinely parity-blocking set: common languages that leave a real hole in a
  mixed codebase today, and each is a true defs+calls grammar on the existing
  substrate.

The remaining tiers are **named here but deferred to the follow-on proposals**
above, because they are *not* "just add a language":

- **Tier 2 — infra/data-as-code is references-only, not defs/calls.** SQL has no
  call graph in the code-graph sense; HCL/Terraform has module semantics;
  JSON/YAML config has keys, not symbols. Extracting these as if they were code
  produces misleading structure. The Tier-2 proposal must introduce **new
  relation types** (`references_table`, `references_column`,
  `references_resource`, `references_module`) and a real design pass — it does
  **not** claim these files are "extracted like the 17 code languages."
- **Tier 3 — scientific/hardware/systems** (Julia, Zig, Fortran,
  Verilog/SystemVerilog) and **Tier 4 — web SFCs** (Vue/Svelte/Astro) and
  **Tier 5 long-tail** (Pascal/Delphi, Apex, Razor/XAML, CUDA/Metal): genuine
  grammar adds, but lower-leverage; batch them after Tier 1 lands and proves the
  per-grammar PR shape.

Each Tier-1 grammar is a self-contained PR (grammar + classifier + a fixture per
language + a parse/extract test), gated behind the existing `AIMEE_TREESITTER`
build flag, falling through to text-only storage when a grammar is absent — so
nothing regresses if a grammar fails to build. **Build matrix:** add one CI entry
per grammar with a documented supported-platforms list, and an `AIMEE_GRAMMARS`
selector so a platform only compiles grammars known to build there; an *expected*
grammar that fails to build fails the build loudly rather than silently
degrading to a test/local split.

### §6b Visual and audio media — net-new, **split into follow-on proposals**

aimee's document ingest is already **broad for office formats** — PDF
(structured, with page geometry), Markdown/text, and a pandoc path covering
DOCX, PPTX, XLSX, ODT, EPUB, HTML, RTF. The gaps are the **non-text modalities**
(images, audio, video sit in the binary-denylist and are dropped). These are
genuinely net-new and have different failure modes, quality metrics, and cost
profiles, so per the review **each becomes its own proposal** — captured here as
direction, not specified for implementation in this one:

- **Visual-media ingest (own proposal, with an evaluation gate).** Route a raster
  image through a **vision model** to produce a text description + extracted
  entities/relationships, then ingest as a document. Biggest modality gap —
  architecture/ER diagrams, UI mockups, whiteboard photos are design artifacts
  the graph can't see. **Promotion gate (required before default-off → on):** a
  labeled fixture set of ≥ 50 diagrams with expected entities/relationships,
  shipped with the PR; promote only on precision ≥ 0.7 and recall ≥ 0.5 — vague,
  high-cardinality, unattributable caption nodes are worse than no nodes. Reuses
  the curator's LLM backend; no new provider.
  - **SVG is *not* a raster image and must not hit the vision path.** SVG is
    active XML-like content (scripts, external refs, embeddable prompt payloads).
    Parse it as text/XML with external-resource loading disabled and no
    rasterization, size-limit it, and route it through the §4 sanitizer — never
    through the generic vision pipeline.
- **Audio/video transcription (own proposal).** **Transcribe locally** — name the
  model and size budget (e.g. whisper.cpp small/medium, ≤ 300 MB) and the
  supported platforms (linux-x86_64, darwin-arm64). *Local* matters: media never
  leaves the box. Failure mode is explicit: if the model can't load, **skip the
  file with a structured log — never partially ingest**. **Data-flow policy must
  be documented:** the transcript is local, but it then flows to the normal
  embedding/synth backends; an `AIMEE_LOCAL_ONLY_MEDIA` flag must force
  end-to-end local processing for users who need it.
- **Spreadsheet structural extraction (folds into the Tier-2 proposal, §6a).**
  This is *new extraction work*, not a pandoc refinement: emit graph nodes for
  sheets, named tables, and column headers (with `contains` edges) via the same
  grammar-table + classifier pattern, shipped with a fixture workbook.

**Boundaries that apply to all media work whenever it ships.** Opt-in and
flag-gated like structured PDF ingest (`kb_pdf_ingest_enabled`) — never on by
default, never blocking; every model-derived field (caption, transcript) is
untrusted input that carries provenance and passes the §4 sanitization boundary
before it reaches an agent.

## Phasing (each independently shippable)

- **P0 — Prompt-injection boundary (§4), prerequisite.** The sanitizer is *not*
  late hardening — §1, §2, and §3 all render untrusted corpus-derived strings
  (symbol labels, paths, community names, fact text, lessons, corrections) into
  agent prompts, so none of them may merge until the boundary exists in front of
  them. Ship `sanitize_for_prompt(field, source_kind)` (extending the existing
  central agent-memory GA sanitizer — see §4) with its attack corpus and a
  call-site audit *first*. Every new render call-site added by P1–P3 must audit
  to this boundary in its own PR.
- **P1 — Self-audit (§1).** Pure analytics over existing data. Add cycles
  (Tarjan SCC + bounded per-SCC DFS), orphans (a `bottom` mode on the existing
  hubs route, not a fork), cohesion (conductance/modularity-contribution gated
  at community size ≥ 8), bridges (exact Brandes below a threshold, deterministic
  sampling above with `approximate:true`), and unverified-inferred findings; one
  route + one MCP command. Highest leverage, lowest risk.
- **P2 — Determinism + snapshot diff (§2).** Stable node identity + the two-pass
  community remap + total-order tie-breaks, then the generation diff route
  (extractor-version-guarded). Feeds review.
- **P3 — Retrieval learning (§3).** Outcome capture at the memory-interception
  boundary into a **dedicated outcome ledger** (not `entity_nodes/edges`), the
  deterministic reflection pass, the scoped lessons artifact, and the
  session-preamble + RRF-tie-break consumers. The headline; closes the loop with
  §1 by recording verification outcomes against finding IDs.
- **P4 — Cache refinements (§5).** Version-namespacing and frontmatter-aware
  hashing; fold into the relevant surfaces as they ship.
- **P5 — Coverage expansion (§6a Tier 1 only here).** One PR per Tier-1 language
  grammar. The remaining strands (Tier-2 reference extractors, visual-media
  ingest with its evaluation gate, audio/video transcription) are **split into
  their own follow-on proposals** per the review — see §6.

## Non-goals

- Not re-proposing provenance tags, communities, hubs, surprising links, RRF
  hybrid retrieval, live updates, or actuation — those are `code-graph-
  intelligence`'s scope and are partly/fully shipped.
- Not a new graph store, transport, or visualization. These are analytics +
  feedback loops over the existing substrate.
- Not an LLM-driven scoring layer. §1 and §3's aggregation are deterministic on
  purpose; the only optional LLM touch is reusing the existing judge for
  borderline confirmations, never for the core scores.
- §6 is not a new graph model, a new ingest transport, or a new LLM provider. It
  reuses the tree-sitter front-end, the `/v1/ingest` route, and the curator's
  configured backends. It does not re-implement the office-format ingest aimee
  already has — it adds the languages and the non-text modalities that are
  missing.

## Risks / honest limits

- **Outcome capture needs a real signal.** §3 only pays off once agents actually
  record outcomes. Mitigation: auto-capture `useful` on answers the agent
  proceeds to act on, `corrected` when the user overrides — so the common cases
  need no extra step.
- **Self-audit can be noisy on a young graph.** Sparse projection edges → many
  false orphans. Mitigation: the honesty gate (report "insufficient signal")
  and gating findings behind the inferred/ambiguous confidence tags.
- **Cycle enumeration cost** on dense graphs — bounded by a max cycle length and
  a result cap, logged when truncated (no silent caps).
- **Determinism is load-bearing for §2** and easy to regress; it needs explicit
  permutation-invariance tests (rank outputs identical under input reordering).
- **§6 grammar build fragility** — some grammars ship source-only and need a C
  toolchain; a grammar that fails to build must fall through to text-only
  storage, never break the build. Vision/transcription add a model dependency
  and real cost, so both are opt-in, flag-gated, and off by default — the parity
  claim is "can ingest," not "ingests everything by default."
- **§6 modality trust** — image captions and transcripts are untrusted,
  model-generated text; they must carry provenance and pass §4 sanitization
  before reaching an agent, exactly like any other corpus-derived field.

## Tests

- §1: orphan/cycle/cohesion/bridge detection on fixtures; honesty gate on a clean
  graph; tie-break determinism under input permutation.
- §2: node/edge add-remove diff exactness; new-cycle detection; community-ID
  stability across re-index (remap correctness); empty-diff on identical
  generations.
- §3: signed time-decay math, half-life, corroboration threshold (one hit ≠
  preferred), contested-by-recency, dropped-missing-node, byte-stable output for
  fixed `now`; round-trip of an outcome record into a re-ranked retrieval.
- §4: sanitizer strips ANSI/control/injection markup from every rendered field.
- §5: version-namespaced structural cache invalidates on version bump while the
  semantic cache survives; frontmatter-only doc edit is a cache hit.
- §6a: one parse+extract fixture per newly added language (definitions + call
  edges asserted); fall-through to text-only storage when the grammar is absent;
  no regression on the existing 17 languages.
- §6b: (carried by the follow-on proposals) vision ingest behind its flag with
  the ≥ 50-diagram precision/recall promotion gate; SVG routed through the
  text/XML + §4 path, never the vision pipeline; transcript ingest from a fixture
  media file with the load-failure-skips contract; xlsx structural nodes
  (sheet/table/column) via the Tier-2 extractor; every model-derived field passes
  the §4 sanitizer.

## Review revisions (R1)

Roundtable review (review mode, 2 rounds) returned 14 blocking findings plus
suggestions; the surviving panel (2 of 4 panelists; the run was `degraded` on an
upstream provider stall, not a content failure) was thorough. Dispositions —
all **accepted**; the body sections above were revised to match:

- **§4 promoted to P0 prerequisite** (was P4). The sanitizer now has a real
  contract — `sanitize_for_prompt(field, source_kind)` with a `source_kind` enum,
  per-kind rules, a shipped attack corpus, and a call-site audit + CI guard — and
  explicitly **extends the existing central agent-memory GA sanitizer** rather
  than forking a parallel one. *(items 1, 2, 26)*
- **§3 outcome model made concrete:** explicit record schema; two-level
  (answer + per-citation) attribution with equal-split fallback and a
  non-circular auto-`useful` proxy; a **dedicated outcome ledger** isolated from
  `entity_nodes/edges` and `db2_memory_find_facts_like`; **immutable/append-only**
  retention so corrections/dead-ends survive node deletion; **correction
  authority** model (agents can't mint durable negative trust unconfirmed);
  **per-project/tenant/branch lesson scoping**; trust enters RRF **as a tie-break
  only** (weight variant deferred); **asymmetric decay** (sticky corrections).
  *(items 3, 4, 8, 9, 11, 15, 18, 19, 33)*
- **§2 made well-defined:** stable generation-independent **node identity** +
  rename detection; **two-pass deterministic community remap** with explicit
  tie-breaks; `diff_kind` + **extractor-version guard** (refuse cross-version
  compare without `force`); old/new partition-crossing both reported; explicit
  route generation semantics + persisted diffs. *(items 5, 6, 7, 21, 31, 35)*
- **§1 algorithms specified:** Tarjan SCC + bounded per-SCC DFS for cycles;
  exact-Brandes-below-threshold / sampled-above for bridges; **conductance/
  modularity-contribution** (not naive density) gated at size ≥ 8 for cohesion;
  orphans as a **`bottom` mode on the existing hubs route**, not a fork; §1↔§3
  loop **closed** by wiring finding IDs into the outcome ledger. *(items 14, 16,
  17, 20, 25)*
- **§5 corrected:** the semantic cache is **namespaced by its full contract**
  (schema/prompt/model/embedding versions), preserved-not-discarded to avoid
  re-billing; frontmatter hashing has an explicit ignorable-key allowlist.
  *(items 29, 30)*
- **§6 descoped + split:** renamed "parity"→"expansion"; only **Tier-1 language
  grammars** remain in scope here; **Tier-2 is references-only** with new
  relation types and its own proposal; **visual-media** (with a ≥ 50-fixture
  precision/recall gate) and **audio/video transcription** (named model + size +
  load-failure-skips + `AIMEE_LOCAL_ONLY_MEDIA` data-flow policy) each become
  their own proposal; **SVG treated as text/XML**, never the vision path; XLSX
  structure folded into Tier-2; per-grammar CI build matrix + `AIMEE_GRAMMARS`
  selector. *(items 10, 12, 13, 22, 23, 24, 27, 28, 32, 34)*
