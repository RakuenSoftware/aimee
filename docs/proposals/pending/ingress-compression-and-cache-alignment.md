# Proposal: Envelope compression, cache-prefix alignment, reversible rehydration, and failure-mined corrections

- **State:** reviewed — design-ready (2026-06-16). The design-roundtable blockers
  (below) are resolved in **§6.5 Design-review resolutions**; implementation may
  proceed per the §7 phasing.
- **Design roundtable (2026-06-16):** found 9 blocking / 12 major — **all resolved
  in §6.5.** Key blockers were: the `X-Aimee-Compress` override using thread-local
  state (unsafe in the threaded server); the lossy-fold transform enum not
  distinguishing recoverability; rehydrate conflating byte-exact replay with
  durable re-read; durable resolvers taking model-supplied path/line args
  (prompt-injection / path-traversal); and unfalsifiable validation gates ("task
  class" undefined, no independent drift oracle). §6.5 closes each (request-scoped
  context, lossiness-classed enum, a `recovery_mode` response schema, workspace-root
  realpath validation, and concrete benchmark/oracle protocols). The panel's
  three-unit split (P0+P1a → P2 behind the MCP tools+deployment-shape → P3–P5
  behind cost-accounting) is reflected in §7 phasing.
- **Author:** JBailes
- **Date:** 2026-06-11 (consolidated after eight PR-#181 review rounds; all 35
  findings verified in-tree and folded into the body)
- **Charter roles:** Rewrite (envelope compression / cache placement),
  Recall (recovery resolver / rehydration handle), Extract / Gate-Promote (failure-mined
  corrections), Calibrate / Evaluate-Optimize (token + accuracy A/B).
- **Scope:** `src/server/ingress_preinject.c` (a structured envelope IR + the
  compression hook over it), `src/server/openai_chat.c` (cache-prefix placement
  at the live Codex/OpenAI seam), `src/server/server_http.c` +
  `api/openapi-server-v1.yaml` + generated `src/server/openapi_server_data.h`
  (compression request override if exposed),
  `src/server/anthropic_http.c` (a *separate opt-in phase* — see §2.3 / §7),
  `src/server/server_mcp.c` +
  `src/server/server_mcp_call_table.inc` + `src/mcp_tools.c` (rehydration tool,
  any missing durable resolver tools such as `memory_get` / code-span read,
  discovery metadata + goldens), config plumbing (`src/headers/config.h`,
  `src/config.c`, `src/config_fields.c`, `src/config_sections.c`,
  `src/config_save.c`), the learning machinery (`src/db2/learning.c/.h`,
  `src/kb/kb_learning_synth.c`, `src/config_learning.c`, the
  `interaction_event_embeddings` table, `src/kb/kb_mining.c`) for failure
  mining, `src/payload_rewrite.c` + `src/headers/payload_rewrite.h` +
  `src/headers/token_tracker.h` for cache-prefix integration including the
  `agent_execute_messages()` Responses wire path,
  workspace-provider / runner surfaces (`src/headers/workspace_provider.h`,
  `src/server/workspace_provider_detached.c`, `src/server/server_runner_endpoints.inc`)
  if code-span recovery reads source, `bench/ingress_token_bench.py` +
  `benchmarks/learning/learning_replay.py`
  for the accuracy/token A/B, config surface tests (`src/tests/test_config.c`,
  `src/tests/test_config_surface.c`, `src/tests/test_cmd_config.c`), generated
  config docs (`docs/gen/configuration.md` via `scripts/gen-reference-docs.py`),
  unit + integration tests, docs. No new long-lived service; the ML prose
  compressor is explicitly out of scope (§5).

## Design at a glance

For readers not following the review history (the "Review findings (resolved)"
map below records how eight rounds hardened the contract), the design is four
default-off levers added to the pre-injection path, mined from headroom:

1. **Compress** envelope content over a typed IR — code folder first (§1).
2. **Place** volatile per-turn context *after* the provider cache prefix (§2).
3. **Recover** folded detail through a *callable* resolver — `code_span_get` /
   `memory_get` for durable data, `rehydrate` for ephemeral (§3).
4. **Learn** from failed sessions via the existing learning-signal pipeline (§4).

Phasing is P0→P5 (§7). Two hard gates dominate (§6): a lossy fold ships only where
its resolver is *provably reachable* on that ingress, and a default flip requires a
**net** token win (resident savings minus recovery round-trips) on Aimee's own
corpora — not merely a resident-token reduction.

## Provenance

The four ideas below are lifted — by design, not code — from
[`chopratejas/headroom`](https://github.com/chopratejas/headroom), an
Apache-2.0 context-compression layer for agents (ContentRouter → specialized
compressors → CacheAligner → reversible-compression/CCR, plus a `headroom learn`
failure miner). Headroom *as a product* overlaps Aimee's identity (it sells
cross-agent memory and an ingress proxy, both of which Aimee already is), so this
proposal does **not** adopt headroom. It mines four mechanisms headroom has that
Aimee's injection path does **not**, and reimplements them on Aimee's existing C
seams. None require headroom's Python/Rust runtime or its `Kompress-base` model.

## Relationship to existing proposals

This proposal sits between three siblings and must not duplicate them:

- `docs/proposals/pending/recall-economy-progressive-disclosure.md` owns
  **bounded envelope assembly**, **progressive disclosure (preview + pull-handle)**,
  **learned shortcuts**, and **intent annotation** — *which records, at what size,
  in what shape* enter the envelope. This proposal's envelope IR (§1) and
  recovery resolver contract (§3) are designed to **be** that proposal's
  preview/read contract where it has one, not a parallel scheme.
- `docs/proposals/done/ingress-cost-accounting-and-optimizations.md` owns
  **provider usage extraction, USD pricing, the ledger, `/v1/usage/*`, generic
  cached-token reporting, and the `cache_control` *marking* mechanism**
  (`ingress_cache_marking_enabled` / `ingress_cache_min_chars`). This proposal
  does **not** re-own any of that; §2 owns only the *byte-placement invariant*
  that decides what is safe to mark, and consumes that proposal's ledger fields
  for its benchmarks. The premise conflict between the two is reconciled in §2.1.
- `docs/proposals/done/context-preinjection-ingress.md` shipped the envelope
  itself (`ingress_preinject_build()`, confidence steering, the attention guard).
  Symbol spans exist on `find_symbol` / structure reads, but **not** on today's
  `code_search_hit_t` returned to the ingress path; §1 now treats span-enriched
  code entries as a required shape upgrade, not a field that already exists.

## Review findings (resolved)

Eight review rounds against the live tree hardened this contract; all 35 findings
are resolved in the body sections (§1–§8) and were re-verified in-tree — the seven
load-bearing code claims especially: the five OpenAI/Responses injection sites in
`openai_chat.c`, the server-dispatch `index.structure` route dropping `line_end`
(the KB HTTP route emits it), the absent `memory_get`/`code_span_get` MCP tools,
the `payload_rewrite` prefix subsystem, the `db2/learning.*` signal→proposal
pipeline, `interaction_event_embeddings` + the `kb_mining.c` recurrence job, and
the `agent_execute_messages()` Responses path. The findings are grouped by theme
below; each `#N` is the stable reference used by the "(resolves review #N)" tags
in the body.

- **Envelope IR & compression (§1).** #1 the envelope is one opaque string, not a
  typed IR a router can dispatch over → §1.1. #9 `code_search_hit_t` carries no
  symbol spans, and #33 the server-dispatch `index.structure` route drops
  `line_end` (the KB HTTP route emits it) → §1.2. #2 folds are lossy, not
  information-preserving, and #23 a durable re-read returns *current* content with
  a drift signal, not byte-exact pre-fold bytes → §1.1/§1.3. #35 split
  byte-equivalent (P1a, zero-dependency) from lossy (P1b) folding → §1.2/§7. #10
  the `X-Aimee-Compress` escape needs real HTTP plumbing and #29 OpenAPI edits
  must regenerate the embedded server spec → §1.4. #34 net-saving attribution
  needs per-entry compression/resolver telemetry, not aggregate usage → §1.1/§6.
- **Cache-prefix placement (§2).** #3/#12 the marking mechanism, ledger, and
  `ingress_cache_marking_enabled` belong to the cost-accounting proposal; this one
  owns only the placement invariant → §2.1 (the envelope-volatile vs
  "stable-anchor" premise mismatch is settled empirically → §2.2). #4 Anthropic
  injection is not a free stateless-proxy-preserving change → §2.3/P5. #5 cache
  controls are provider-specific — capability table, no-op default → §2.4. #11
  reuse the existing `payload_rewrite` prefix subsystem, and #19 the Responses
  path (`agent_execute_messages()`) carries no session state for prefix telemetry
  → §2.5. #13 `ingress_preinject_build()` is called at five OpenAI sites, and #30
  inline Responses tool interception is a full continuation state machine → §2.6.
- **Recovery resolvers & rehydration (§3).** #6 handles need scope, capability-
  binding, version invalidation, topology, and log-hygiene rules → §3.2. #18/#27
  the durable pull-handle is recall-economy's `memory_get` (a backend, not yet an
  MCP tool), not a generic `fetch` → §3.1. #26/#22 durable code recovery needs a
  callable `code_span_get`/range read (which does not exist — `find_symbol`
  returns locations only), and the first lossy phase gates on *that* reachability,
  not on `rehydrate` → §3.1/§3.4. #16/#21/#25 resolver reachability is a
  per-ingress precondition proven from the installed MCP config, distinct from the
  context-preinjection *hooks* → §3.4. #20/#32 injected tools and a server-side
  `code_span_get` must respect route auth and the workspace-provider boundary →
  §3.5. #24/#28 durable folds never need the handle store; only ephemeral folds do
  (P2 vs P2e) → §3.1/§7.
- **Failure mining (§4).** #7 route corrections through the existing
  `db2/learning.*` signal→proposal→review pipeline, not a direct memory write →
  §4.1. #14 the `kb_mining.c` recurrence job already promotes the same
  `interaction_event_embeddings` clusters → refactor it or use a disjoint dedup
  key → §4.4.
- **Config surface (cross-cutting).** #17 every new flag
  (`ingress_compress_enabled`, `ingress_rehydrate_*`, the failure-mining gate)
  needs full load/save, CLI, generated docs, and config-surface tests, or it is a
  dead switch → §1.4/§3.5/§4.5.
- **Validation (§6).** #8/#15 the harnesses are not pre-existing —
  `learning_replay.py` needs the live prediction wire and `ingress_token_bench.py`
  needs per-stage, cache, and correctness arms → §6 (in scope). #31 the
  default-flip gate is **net** token economics (resident savings − recovery
  round-trips, by task class), not resident reduction alone → §6.

## Goal

Cut the per-turn token cost *and* the per-turn dollar cost of pre-injection
without losing task fidelity, and close the loop so the records we inject get
better when a session goes wrong. Four levers:

1. **Structural compression of envelope content** — fold the code block (and,
   later, typed tool-result previews) over a *typed envelope IR* before it enters
   `<aimee-context>`, so more signal fits under the same budget and the attention
   guard fights less filler. The resident form is treated as intentionally lossy
   only when the applicable recovery resolver is reachable (§3).
2. **Cache-prefix placement** — keep genuinely volatile, per-turn envelope content
   *after* the provider's cacheable prefix so it does not invalidate cached
   history. This owns only the *placement invariant*; the `cache_control` marking
   mechanism and ledger belong to the cost-accounting proposal.
3. **Recovery resolvers + a rehydration tool** — durable folds reopen through
   their durable resolver (`memory_get`, code-span/range read); ephemeral folds
   keep the uncompressed original locally behind a capability-bound, TTL-scoped
   handle and expose `rehydrate` so the model recovers full fidelity only when
   it needs it.
4. **Failure-mined corrections** — a pass that turns a badly-ended session into a
   *learning signal* routed through the existing proposal/review machinery, not a
   direct memory write.

---

## §1 Structural compression over a typed envelope IR

### §1.1 P0 dependency — the envelope IR (resolves review #1)

Today `ingress_preinject_build()` renders one opaque string from three sources
(`ingress_preinject_format_code_block()`, the rendered `memory.context_block`,
and the audit context file). A ContentRouter-style compressor has nothing typed
to dispatch over, and folding the *already-rendered* string risks re-parsing
prose as code.

So §1 is gated on a **P0 envelope IR**: before rendering, `ingress_preinject_build()`
assembles a list of typed entries, and the final string is produced from that
list. Each entry carries, minimally:

- `source_kind` — `code_hit` | `memory_block` | `audit` | (future) `tool_result`;
- `record_id` / `handle` — id-addressable, shared with §3 and recall-economy's
  preview/read contract;
- `sensitivity` / `scope` — session, workspace, tenant (drives §3.2 and redaction);
- `preview` — the rendered text that goes resident in the envelope;
- `original_ref` — how to recover the original (§3). Recovery guarantees differ by
  resolver. For **durable** sources (code spans, memory records) this is a
  **re-read pointer** (project/file/line-range, or record id + version) re-fetched
  on demand — cheaper than copying bytes into the store every turn. A re-read
  returns the **current** durable content plus a version/hash **drift signal**; it
  equals the pre-fold bytes only when the source has not changed (the agent may
  have edited it mid-session), so it is *not* an unconditional byte-exact replay.
  For **ephemeral** sources (tool output that will not survive the turn) it is the
  **stored bytes**, which *are* byte-exact. Copying every code-hit body into the
  store on the per-turn hot path is explicitly avoided (see the latency risk in §8);
- `budget` — bytes/tokens this entry is allowed, from recall-economy's bounded
  assembler when present;
- `transform` — which fold was applied, so the metadata is visible to the model and
  the test suite. **§6.5 B2 normatively expands this enum** to one value per
  lossiness class: `none` · `code_whitespace_collapse` · `code_comment_strip` ·
  `code_signature_span` · `json_fold` (the original `code_fold` is split by
  recoverability).
- `metrics` — entry id, original/resident byte and token estimates, transform,
  resolver kind, resolver invoked/not invoked, recovered bytes/tokens, provider
  round-trips, and latency. Aggregate provider usage cannot attribute net savings
  to a specific fold, so this per-entry telemetry is part of the IR contract.

Where recall-economy has already landed its preview/read contract, this IR
**is** that contract extended with `original_ref` + `transform`; it is not a
second scheme.

### §1.2 First phase folds the code block only (resolves review #1, correction)

Correction from review: JSON tool-output entries are **not** in today's envelope,
so there is no reliable JSON target yet. The first phase therefore ships exactly
one compressor:

- **Code folder** (headroom's `CodeCompressor`, AST-aware in spirit; a
  conservative line/brace folder in C to start), in two tiers by loss (resolves
  review #35):
  - **byte-equivalent (P1a):** collapse blank-line runs and trailing whitespace
    on `code_hit` entries. Provably lossless, so per §3.4(b) it needs **no**
    recovery resolver and ships first as the low-risk default-flip candidate.
  - **lossy (P1b):** strip comment bodies and, after span enrichment, prefer
    signature + relevant span over whole blocks. This tier is what pulls in a
    callable resolver, workspace authority, `line_end`, reachability proof, and
    the per-entry telemetry tail.

The **JSON folder** (headroom's `SmartCrusher`) is deferred to when typed
`tool_result` previews exist in the IR (memory/tool-result previews from
recall-economy, or a future tool-output envelope). It is specified here only so
the IR's `transform` enum reserves room for it.

Span-aware folding has a real data dependency: P0/P1 must either extend
`/v1/code/search` and `code_search_hit_t` with `line`/`line_end`/`kind`, or do a
bounded secondary `index_structure`/`find_symbol` lookup for selected files. When
spans are absent, the folder falls back to path + snippet folding and must not
pretend it has a byte-exact body span. If the enrichment path uses the server
NDJSON/RPC `index.structure` handler, that route must also propagate `line_end`;
today the KB HTTP endpoint emits it and the client parses it, but
`handle_index_structure()` serializes only `line`.

### §1.3 The resident form is intentionally lossy (resolves review #2)

The first revision called folding "information-preserving for the consumer."
That is too strong: stripping comments or collapsing a block to signature+span
removes detail that some tasks need. The corrected contract:

- the folded **resident** form is treated as **lossy/summarized** unless a fold is
  proven byte-equivalent (e.g. pure trailing-whitespace trim);
- every lossy fold carries a visible `rehydrate` handle and a `transform` tag, so
  the model can see what was summarized and decide to open it;
- the byte-exact original (§3) bounds the loss **only if** the handle is
  available, authorized, and discoverable in that ingress — it does not make the
  resident text itself lossless;
- the default-on flip is **blocked** until the accuracy A/B (§6) includes tasks
  where the omitted detail is required *and* confirms the model rehydrates when
  it needs it.

Because **byte-equivalent** folds (P1a — blank-line/whitespace collapse) lose
nothing, they are exempt from the resolver requirement (§3.4(b)), the
rehydrate-handle pairing, and the forced-rehydration accuracy gate; they need only
the resident-token and net-economics measurement. Only the **lossy** tier (P1b)
is blocked on resolver reachability and the accuracy A/B.

### §1.4 Compression gate and request override (resolves review #10)

Gated by a new config bool `ingress_compress_enabled` (default **off**), with the
same level of config surface as existing fields: `src/headers/config.h`,
`src/config.c`, `src/config_fields.c`, `src/config_sections.c`,
`src/config_save.c`, `aimee config get/set/list` behavior, generated docs, and
config-surface tests. The implementation must explicitly decide whether this is
a top-level ingress field beside `ingress_preinject_enabled` or part of a nested
section; the proposal does not assume that a prose-only flag exists.

If a per-call escape is exposed, it requires actual HTTP plumbing:

- parse `X-Aimee-Compress: 0` in `server_http.c` alongside `X-Aimee-Preinject`;
- carry it via the **per-request context struct** into `ingress_preinject_build()`
  (scoped + reset per request — never an unscoped thread-local; see §6.5 B1);
- cover buffered chat/completions, buffered Responses, streaming chat/completions,
  and streaming Responses tests;
- document it in the OpenAPI surface if it is part of the public `/v1` contract,
  then regenerate `src/server/openapi_server_data.h` from
  `api/openapi-server-v1.yaml` so `/v1/openapi.yaml` and `/v1/openapi.json`
  serve the updated header contract.

---

## §2 Cache-prefix placement at the ingress seams

### §2.1 Ownership split with the cost-accounting proposal (resolves review #3)

The cost-accounting proposal owns the **mechanism**: provider usage extraction,
USD pricing, the ledger, `/v1/usage/*`, generic cached-token reporting, and the
`cache_control` *marking* of the envelope (`ingress_cache_marking_enabled` /
`ingress_cache_min_chars`). This proposal does **not** re-own any of it and adds
**no duplicate user-facing cache-marking flag**. Placement may still need an
internal/shadow gate, or may ride the existing `cache_aware_rewrite_enabled`
prefix subsystem (§2.5), because placement telemetry and cache marking are
different rollout risks.

This proposal owns one thing the other does not: the **placement invariant** —
ensuring that whatever is marked cacheable is actually byte-stable turn-to-turn,
and that genuinely volatile content is not placed ahead of the cached prefix.
Marking and placement are complementary: marking a *volatile* block cacheable
creates a cache *write* every turn (cost, few reads); the invariant is what keeps
that from happening.

### §2.2 Reconciling the premise (resolves review #3, premise mismatch)

The two proposals disagree on a fact: cost-accounting calls the envelope "large
and constant across a session — the ideal prompt-cache anchor"; this proposal
calls it volatile. `ingress_preinject_build(query, …)` rebuilds the envelope from
the **current turn's query** and the hook fires on every `UserPromptSubmit`, so
its *content* is per-turn volatile. The merged classification:

- **Stable, cacheable prefix:** system / persona / prior history — these do not
  change turn-to-turn within a session.
- **Volatile, after-the-boundary:** `<aimee-context>` — rebuilt per turn.

This is settled **empirically, not by assertion**: the bench (§6) reports
realized `cache_read` vs `cache_creation` tokens for the envelope under both
placements. If, in practice, the envelope turns out stable enough on many turns
(similar queries) that marking it nets positive, the data — not either
proposal's prose — decides. The two proposals must publish the same classification
once measured.

### §2.3 Anthropic is a separate opt-in phase (resolves review #4)

`anthropic_http.c` is a deliberate stateless wire-format proxy: it does not run
Aimee's agent loop, memory, persona, or toolset, because Claude Code owns its own
context and tools. The earlier claim that injection can happen "without
abandoning" that contract "because the proxy still forwards" is **withdrawn** —
it is not precise. Injecting into the Anthropic path requires, at minimum:

- extracting a query from Anthropic messages without mutating client-owned tool
  state;
- calling `ingress_preinject_build()` from the Anthropic path;
- preserving Anthropic `system` block **arrays** and their `cache_control`
  metadata rather than flattening through `anthropic_system_to_text()`;
- a per-request + config gate separate from the OpenAI/Codex seam;
- proving Claude Code will actually call the advertised Aimee MCP tools when the
  envelope tells it to `rehydrate` or use a durable pull-handle such as
  `memory_get`.

Therefore Anthropic injection is **P5**, its own opt-in phase behind its own gate,
with tests for direct Anthropic-provider passthrough *and* non-Anthropic delegate
translation. The live seam this proposal targets first is Codex/OpenAI
(`openai_chat.c`), which already injects.

### §2.4 Provider-specific cache controls (resolves review #5)

There is no single "latest point" rule. Anthropic exposes explicit `cache_control`
blocks; OpenAI prompt caching is automatic and prefix/min-token dependent; many
OpenAI-compatible delegates expose no usable control. The contract:

- a per-provider **capability table** consulted before any request mutation;
- **no-op** for providers without explicit cache controls (placement still keeps
  volatile content last, but nothing is marked);
- a test that **serializes the exact provider request body** and asserts that any
  **client-supplied** cache controls are preserved untouched;
- bench output (§6) that separates **resident-token** reduction from **realized**
  provider cache reads/writes.

### §2.5 Reuse the existing prefix-state subsystem (resolves review #11)

Aimee already has prompt-cache-aware prefix machinery:
`payload_rewrite_prefix_hash()`, `payload_rewrite_track_request()`,
DB1 `payload_rewrite_state`, `cache_aware_rewrite_enabled`, and the
`payload_rewrite_status` MCP tool. P3 must extend that subsystem rather than
create a second prefix hash / state table:

- define the stable prefix hash in terms of the provider-facing bytes actually
  sent after placement;
- include `<aimee-context>` only when the experiment intentionally classifies it
  as stable;
- record placement shadow decisions in the existing DB1 state or a clearly named
  sibling column/table;
- consume normalized usage from `token_tracker.h` / the cost-accounting ledger
  rather than parsing provider usage twice.
- wire the Responses/Codex `agent_execute_messages()` path into the same state:
  pass a real session id through the retry/context path, call the prefix tracking
  hook on the provider-facing body, and cover both buffered and streaming
  Responses. Today that path forwards tool calls to the client and is a separate
  request-shaping path from `agent_execute()`, so it will otherwise miss
  session-scoped prefix telemetry.

### §2.6 Cover every OpenAI/Responses injection path (resolves review #13)

`openai_chat.c` calls `ingress_preinject_build()` at **five** sites spanning the
legacy chat/completions, buffered Responses continuation, streaming
chat/completions, and streaming Responses variants. P3 is not complete until all
of them share the same placement rule,
request override behavior, continuation behavior, and body-serialization tests.
The Responses continuation path is especially important because it prepends the
stored transcript before building the envelope; the prefix classifier must not
accidentally treat a growing transcript as cache-stable. The `agent_execute_messages()`
path is also where Codex supplies its own `tools` array and receives tool calls
back. Any inline resolver-tool interception there is a **new Responses
state-machine**, not a small request-shaping tweak: Aimee must consume the
synthetic function call itself, emit the corresponding `function_call_output` to
the provider, continue the provider turn, preserve call ids, and avoid leaking
the synthetic tool call to the client in both buffered and SSE modes.

---

## §3 Recovery resolvers + a rehydration tool

### §3.1 Approach — headroom's CCR, unified with recall-economy's pull-handle

Give every lossy fold a callable recovery resolver, but store byte-exact
pre-fold bytes behind a handle only for ephemeral entries that cannot be re-read.
**Do not** invent a second handle scheme:
recall-economy introduces id-addressable pull-handles at the MCP tool layer and
its concrete durable-memory surface is `memory_get` over the existing
`memory.get` / `kb_client_memory_get` backend. This proposal aligns to that
surface rather than creating a generic `fetch` verb:

- `memory_get` — recall-economy: durable memory preview → full memory record.
- code-span/range read — durable code preview → full source span. This is **not**
  currently an Aimee MCP tool: `find_symbol` returns path/line metadata, while
  `read_file` lives in the delegate/agent toolset. P1/P2 must either add a real
  Aimee MCP resolver such as `code_span_get` (path/project + line range, with
  workspace authorization), or prove that the target client has a native
  filesystem read tool for the same workspace before allowing lossy code folds.
  A server-side `code_span_get` must read through the active workspace-provider
  boundary (`shared`, `detached`, or `mirror`) rather than directly opening the
  indexed path on the server host.
- `rehydrate` — this proposal: folded ephemeral/compressed resident form →
  byte-exact original for data that cannot be re-read cheaply or safely.

If the projects later choose a generic `fetch` verb, it must be adopted in
recall-economy too; this proposal should not fork that contract. Because
`memory_get` is not an MCP tool in the current tree, this proposal either waits
for recall-economy to land that wrapper or includes it in scope. The MCP surface
touched is `server_mcp.c` **and** `server_mcp_call_table.inc`, with discovery
metadata in `src/mcp_tools.c` and the tool goldens updated (regen via
`DUMP_TOOLS=1`).

### §3.2 Handle safety contract (resolves review #6)

An in-process TTL LRU is the first storage primitive, but handles are an access
path to potentially sensitive originals, so:

- **Scope:** every handle is bound to session, workspace, and the source record's
  sensitivity/tenant; a request may only rehydrate handles minted for its own
  scope.
- **Unguessable / capability-bound:** handles are capability tokens, not short
  sequential ids — possessing the envelope is the only way to learn a handle.
- **Version invalidation:** a handle carries the source record's version/hash;
  rehydrate fails closed if the underlying record changed.
- **Topology:** the store is in-process and **best-effort across restarts** — a
  handle may expire on restart or under LRU pressure. This is documented, not
  hidden.
- **Log hygiene:** handles and originals are never written to logs where they
  would expose sensitive content.
- **Recoverable failure:** on expired/unauthorized/unknown handle, the tool
  returns clear error text steering the model to the durable resolver
  (`memory_get`, code/file-span read, or rerun recall) instead — never a silent
  empty result.
- **Lifetime config:** the store's TTL and size are config-driven, but the
  implementation must choose and fully plumb the names/section:
  `ingress_rehydrate_ttl_s` / `ingress_rehydrate_max_entries` as top-level
  ingress fields, or a nested `ingress.rehydrate.*` equivalent. Either way the
  per-turn memory cost is bounded, tunable, documented, and default-off with §1.

### §3.3 Payoff

Lets §1 fold **aggressively** (optimize resident density, not "safe to lose"),
because the detail is deferred behind a tool call the model makes iff it needs it
— but only to the extent §3.4's reachability precondition holds in that ingress.

### §3.4 Tool reachability is a per-ingress precondition (resolves review #16)

`rehydrate` is only useful where the external agent can call it, and that differs
by ingress:

- **Claude Code hook path:** expected to work only when the installed Claude
  config has Aimee's MCP server registered. Note this is a **separate installer
  action** from the context-preinjection *hooks* the done proposal shipped — hooks
  inject context but do not make `memory_get`/`rehydrate` callable. P2 must prove
  reachability from the actual integration installer/config and an end-to-end
  hook-session fixture that asserts the **MCP-tool registration**, not just the
  hook wiring; the done context-preinjection proposal is not enough proof by
  itself.
- **OpenAI/Codex wire ingress:** callable only if the client has Aimee's MCP
  configured, or if the ingress injects the tool definition into the request and
  intercepts the resulting tool call inline (the `function_call` loop Codex
  already drives). Injecting envelope text that *names* the tool is not enough.
- **Anthropic:** gated on §2.3/P5.

Contract: per target ingress, P2 must **(a)** prove **the fold's resolver** is
reachable end-to-end — a callable code-span/range read for a code fold,
`memory_get` for a memory fold, `rehydrate` for an ephemeral fold (§3.1) — or
**(b)** restrict that ingress to non-lossy folds (byte-equivalent only) and
previews rich enough to stand alone. A **lossy** fold is permitted only where its
resolver's reachability is proven. The first phase's code folder is therefore
gated on **actual code-span-read reachability**, not on `rehydrate`, which need
not exist yet. For Aimee MCP clients, `find_symbol` alone is insufficient because
it returns locations rather than contents; either add a `code_span_get`-style MCP
tool or prove a client-native `read_file`/range-read tool is available and scoped
to the same workspace. This is the concrete gate that turns §1.3's "lossy is
safe" from an assumption into a tested precondition.

**Store topology.** The handle store is in-process and assumes the mint (envelope
build) and the `rehydrate` call land in the **same server process** — true for
Aimee's single-process threaded server and the single-container Docker deploy, so
the cross-thread mint→rehydrate path works with a mutex. A **multi-replica /
load-balanced** deployment can route `rehydrate` to a replica that never minted
the handle; there it must fall back to the durable resolver / rerun recall or a
clearly named shared store. This is documented as a known limitation, not
silently broken.

### §3.5 Config, tool metadata, and injected-tool safety (resolves review #17/#18/#20)

The rehydration phase is not complete until the surrounding surfaces are
complete:

- config fields for TTL, LRU cap, and enablement are load/save round-tripped,
  exposed through `aimee config`, generated into `docs/gen/configuration.md`, and
  covered by config-surface tests;
- the durable pull path matches recall-economy's `memory_get`; if recall-economy
  has not landed that MCP wrapper, this proposal must either depend on it or add
  the wrapper itself over the existing `memory.get` / `kb_client_memory_get`
  backend;
- durable code recovery is backed by a callable resolver (`code_span_get` or a
  proven client-native range read), not merely `find_symbol` metadata. If Aimee
  owns the resolver, it uses the workspace provider / detached-runner channel and
  carries the active workspace identity in the handle or request;
- MCP discovery metadata and generated tool goldens include the new
  `rehydrate` surface, and the handle shape is visible enough for the model to
  know which resolver applies;
- any inline OpenAI/Codex tool-definition injection is capability-gated and
  tested against the same route authorization, workspace/session scope,
  read-only bearer behavior, and remote TCP restrictions as the existing
  HTTP/MCP surface;
- inline OpenAI/Codex interception owns the full Responses continuation state:
  synthetic resolver calls are consumed by Aimee, converted into
  `function_call_output`, re-submitted to the provider, and hidden from the client
  unless the client explicitly opted into seeing internal resolver events.

---

## §4 Failure-mined corrections — as learning signals, not memory writes

### §4.1 Route through the existing learning machinery (resolves review #7)

The repo already has a learning pipeline: signals → proposals → review/accept →
promotion to a sink. The public surface is in `src/db2/learning.c/.h`
(`db2_learning_signal_insert`, `db2_learning_proposal_insert`,
`db2_learning_proposal_find_pending`, `db2_learning_proposal_list`, accept/reject
states), synthesis in `src/kb/kb_learning_synth.c`, CLI in `src/cmd_learning.c`.

§4 therefore does **not** write a correction directly to durable memory. It:

1. **Mines** a badly-ended session into evidence;
2. emits a **`db2_learning_signal_insert`** with that evidence and a
   `db2_learning_proposal_insert` against the appropriate sink (memory / rule /
   artifact) with `target_key`;
3. lets the **existing review/accept path** (and Gate-Promote) decide whether it
   becomes a durable record.

This reuses dedup (`..._find_pending`), corroboration bumping, and TTL the
pipeline already has, instead of a parallel writer.

### §4.2 Grounded signal sources (resolves review #7, sourcing)

- The primary observable is the **`interaction_event_embeddings`** table, whose
  `failure_mode` / `event_type` / `cluster_key` columns already capture
  failure-shaped events server-side. Mining clusters of `failure_mode` is the
  reliable signal.
- The **attention guard's per-session log is a Claude-Code-hook artifact**, not a
  general server-side outcome log — it is usable only for the Claude Code ingress
  and must not be assumed present elsewhere.
- **"Abandoned / retried turn" is not a reliable cross-ingress observable** today;
  it is dropped from the v1 detector unless/until a server-side turn-outcome
  signal exists.
- **Explicit user corrections** overlap the existing implicit
  repeated-correction detector; §4 **reuses that path** rather than adding a
  parallel one.

Gated default-off, but not by a prose-only flag. Because this work rides the
existing `kb_mining.c` recurrence path, the preferred gate is
`kb_mining_enabled` plus a narrower per-job failure-learning gate such as
`kb_mining_failure_learning_enabled`. If the implementation keeps the separate
name `curator_failure_mining_enabled`, it must add the full config plumbing,
generated docs, CLI visibility, and tests listed in §4.5.

### §4.3 Why a signal, not a file

Headroom's `headroom learn` writes corrections to a flat `CLAUDE.md`/`AGENTS.md`
— per-clone, unranked, unreviewed. Routing through Aimee's learning pipeline
yields a record that is cross-agent, ranked, deduped, contradiction-checked, and
**human/Gate-Promote-reviewed before** it becomes durable. This is the place
Aimee's design is strictly better than headroom's.

### §4.4 Integrate with existing KB mining (resolves review #14)

Failure clusters in `interaction_event_embeddings` are already consumed by
`src/kb/kb_mining.c`; the recurrence job currently writes `workflow_pattern`
artifacts directly through `db2_artifact_write()`. §4 must either:

- refactor that recurrence job to emit `learning_signals` / `learning_proposals`
  first, then let review/Gate-Promote commit the artifact; or
- create a distinct failure-mining job with a non-overlapping event filter and
  dedup key so the same cluster does not produce both a direct artifact and a
  learning proposal.

The preferred path is the first one, because it reuses the existing
high-watermark mining job and removes the parallel-promotion behavior.

### §4.5 Config surface for failure mining (resolves review #17)

Failure mining must not introduce a dead switch. The accepted implementation
must choose one of these two config shapes:

- extend the existing KB mining surface (`kb_mining_enabled` plus a narrower
  failure-learning job toggle), which matches the code that already owns
  `interaction_event_embeddings` recurrence; or
- add `curator_failure_mining_enabled` as a real field with load/save, CLI,
  generated docs, config-surface tests, and a clear reason it should not live
  under KB mining.

Either way, the default is off until the validation gates pass.

---

## §5 What is explicitly out of scope

- **The ML prose compressor (`Kompress-base`).** It needs HuggingFace/GPU weight
  and a second model deploy — operationally heavy given the single-embedder
  history (`single-embedder-pivot`). The structural code folder (§1) captures the
  near-term win with none of that cost. Revisit only if A/B shows prose is the
  residual.
- **JSON folding in the first phase** (deferred until typed tool-result previews
  exist in the IR — §1.2).
- **Anthropic ingress injection in the first phases** (P5, separate opt-in — §2.3).
- **Adopting headroom as a sidecar/proxy.** A per-turn network hop fights Aimee's
  stateless-proxy latency story; reimplementing the structural folder in C is the
  cleaner path.
- **Cross-agent memory store / `SharedContext`.** Aimee's is more mature.
- **The cache-marking mechanism, ledger, pricing, and `/v1/usage/*`** — owned by
  the cost-accounting proposal (§2.1).

---

## §6 Validation (resolves review #8)

The gate for every default-on flip, per the flag-rollout-readiness bar. The
harness upgrades below are **in-scope work**, not pre-existing validation.

**Dependencies.** `benchmarks/learning/learning_replay.py` exists on
`origin/testing`, but it is a schema/metric runner: without `--predictions` it
does not exercise the live detector, and its own docstring still calls out the
missing `aimee learning replay --fixtures …` prediction emitter. This proposal
depends on that live prediction wire plus compression-specific fixtures.
`bench/ingress_token_bench.py` today is only a preinject on/off **token** bench —
it has no per-stage resident tokens, no provider cache reads/writes, no
compression-on/off arm, and no correctness outcomes. Adding those is part of
this proposal.

**Acceptance gates:**

- **Resident reduction** — bytes/tokens by IR section and `transform`, compression
  on vs off.
- **Net token economics (resolves review #31)** — beyond resident reduction, the
  bench reports the **recovery rate** (fraction of lossy folds the agent re-opens
  via a resolver) and the **net** per-session token delta = resident tokens saved
  − recovery round-trip tokens (the resolver tool call + the recovered span),
  broken out **by task class**. The §1 default flip requires net positive; a task
  class where recovery is frequent enough to erase the saving must keep folds
  non-lossy there or not flip. Latency from recovery round-trips counts against the
  same gate. The attribution comes from §1.1 per-entry compression/resolver
  telemetry, not only aggregate provider usage.
- **Realized cache** — provider `cache_read` / `cache_creation` tokens, read from
  the cost-accounting proposal's **ledger fields** (not re-derived), under both
  placements (§2.2).
- **Answer correctness under forced rehydration** — accuracy A/B
  (`learning_replay.py`) that **includes tasks where the folded-out detail is
  required**, asserting the model rehydrates when it needs to. Aimee's analog of
  headroom's GSM8K/SQuAD/BFCL claim — **verified on Aimee's own corpora; not
  trusted transitively.**
- **Handle safety** — handle expiry, unauthorized-scope rehydration, and
  version-mismatch all return the §3.2 recoverable error, proven by test.
- **Tool reachability and authorization** — Claude Code reachability proven from
  installed config, OpenAI/Codex tool-definition injection or interception denied
  when route auth / workspace / session / remote TCP policy does not allow it,
  and lossy folds disabled on ingresses that fail the reachability fixture.
  The fixture must distinguish `find_symbol` location discovery from actual
  content recovery: code folds need a callable `code_span_get`/range-read or
  proven client-native `read_file`, and memory folds need a callable `memory_get`
  MCP wrapper if memory handles are emitted. For server-owned `code_span_get`,
  tests cover shared, detached, and mirror workspace-provider behavior or the
  implementation explicitly restricts the resolver to supported provider kinds.
- **Prompt-injection / sensitivity fixtures** — for both compressed previews and
  rehydrated originals (a folded preview must not become an injection vector, and
  a rehydrate must not cross scope).
- **Prefix-stability invariant** (§2) and **recovery round-trip** (§3) as hard
  unit-suite invariants — split by resolver: an **ephemeral stored-bytes**
  rehydrate is byte-exact, while a **durable re-read pointer** returns current
  content and must surface a correct drift flag when the source changed between
  fold and read (the test mutates the source and asserts the flag, not byte
  equality).
- **Responses/Codex prefix telemetry** — `agent_execute_messages()` records the
  provider-facing prefix through the existing `payload_rewrite` subsystem with a
  real session id for both buffered and streaming Responses.
- **Inline Responses interception** — if synthetic resolver tools are injected,
  tests cover buffered and SSE Responses where the provider calls the synthetic
  tool; Aimee consumes it, returns `function_call_output`, continues the provider
  turn, preserves call ids, and does not leak the internal tool call to Codex.
- **Config/doc surface** — every new flag round-trips through load/save and
  `aimee config`, appears in `docs/gen/configuration.md`, and is covered by
  `test_config`, `test_config_surface`, and `test_cmd_config`.
- **MCP metadata parity** — `mcp_build_tools_list()` and
  `server_mcp_call_table.inc` agree for `memory_get`, `code_span_get`/range read,
  and `rehydrate`, with generated/golden tool metadata updated.
- **Span propagation parity** — `/v1/code/search`, `/v1/code/structure`, the
  server-dispatch `index.structure` route, and the MCP/envelope rendering path
  preserve `line` and `line_end` consistently when span enrichment is enabled.
- **Request-body serialization** (§2.4) asserting client cache controls survive.
- **Runtime OpenAPI freshness** — if `X-Aimee-Compress` is documented, both
  `api/openapi-server-v1.yaml` and generated `src/server/openapi_server_data.h`
  are updated, and the served `/v1/openapi.yaml` / `/v1/openapi.json` content
  includes the header.
- **Latency / KB-call budget** — P0/P1 may add secondary structure lookups and
  handle storage work to a per-turn hot path that already calls code search and
  memory context. Bench output must include p50/p95 ingress build latency and KB
  request counts.

---

## §6.5 Design-review resolutions (roundtable 2026-06-16)

The 2026-06-16 design roundtable returned 9 blockers (+ 6 majors). Each is resolved
below; the decisions are normative for implementation.

**B1 — Request override via thread-local is unsafe (§1.4).** The `X-Aimee-Compress`
override flows through the **per-request context struct**, not a raw thread-local —
the same scoped, RAII-reset request context introduced by ingress-cost-accounting
§2 (set at handler entry, cleared on every exit path, structurally enforced). The
two proposals share that context plumbing; neither may read compression state from
an unscoped thread-local.

**B2 — code_fold transform enum doesn't distinguish lossy sub-transforms (§1.2/1.3).**
Expand the per-entry transform enum to one value per **lossiness class**: `none` ·
`code_whitespace_collapse` (byte-recoverable) · `code_comment_strip` (lossy) ·
`code_signature_span` (lossy, P1b) · `json_fold`. Each value maps to a distinct
lossiness class and a required resolver, so per-entry telemetry and the §3.4
reachability gate can reason about recoverability exactly.

**B3 — Rehydrate conflates byte-exact replay with durable re-read (§3.1/3.4).** Define
one MCP response schema with an explicit `recovery_mode` enum:
`byte_exact` (the ephemeral in-memory handle is still live → returns the exact folded
bytes) · `current_with_drift` (durable re-read → returns CURRENT content + a drift
flag vs the fold-time version) · `current_no_baseline` (durable re-read, no recorded
fold-time version → drift unknown). The response carries
`{recovery_mode, content, source_version_at_fold, source_version_at_read, drifted,
drift_advisory?}`, and the handle metadata encodes the expected `recovery_mode` so the
model knows the recovery guarantee before it calls.

**B4 — code_span_get takes model-supplied path/line (§3.1/3.5).** Concrete input
validation, fail-closed: (1) the path must resolve **within the active workspace
root via verified realpath** (reject symlink/`..` escapes); (2) line ranges are
clamped to a configurable max span (`code_span_max_lines`, default 400); (3) reject
null bytes / control chars in the path; (4) refuse any path resolving outside the
workspace-provider boundary. Reuses the existing workspace-provider boundary checks;
no raw model string ever reaches the filesystem unvalidated.

**B5 — Envelope stability premise has no baseline protocol (§2.2).** Protocol: per
ingress type, sample the candidate prefix bytes across consecutive turns over **≥200
turns / ≥20 sessions**; stability metric = the fraction of turns whose candidate
prefix is byte-identical to the prior turn's; **≥90% unchanged → "stable"** (eligible
for a cache boundary / fold), else "volatile". The classification is recorded and
re-run whenever prefix construction changes. This makes the empirical resolution
falsifiable.

**B6 — "task class" undefined in the §6 gates (§6).** Enumerate the classes the gates
break down by: `code_generation` · `code_review` · `debugging` · `question_answer` ·
`summarization` · `agent_tool_loop`. Labeling is an automated heuristic from the
turn's tools/intent (e.g. `agent_tool_loop` = the turn emits a `function_call`;
`code_*` = code-search/edit tools present), with manual labels for the gold
validation set only. The net-token-economics and forced-rehydration-accuracy gates
report per class.

**B7 — Drift-signal test has no independent oracle (§1.1/3.4).** The drift test
independently computes `sha256` of the source file **before and after** mutation in
the harness (not via the resolver), then asserts the resolver's drift flag equals
`pre != post`. The resolver's own hash is never the oracle, so a buggy resolver hash
fails the test instead of passing trivially.

**B8 — Config hierarchy unchosen (§1.4/3.5/4.5).** Decided: **flat snake_case keys**,
matching the existing `config_fields.c` convention (`kb_evidence_emit_enabled`,
`ingress_preinject_enabled`, `fidelity_check_enabled` are all flat top-level). Master
gates: `ingress_compress_enabled`, `ingress_rehydrate_enabled`; params:
`ingress_compress_min_chars`, `ingress_rehydrate_ttl_s`, `ingress_rehydrate_max_entries`,
`code_span_max_lines`. No nested `[ingress.rehydrate]` tree (would diverge from the
existing flat config and force a migration).

**B9 — Responses inline tool-interception state machine underspecified (§2.6/3.4).**
Concrete spec for the inline Responses continuation loop:
(1) **detect** a synthetic resolver `function_call` by its injected tool name;
(2) **execute** the resolver server-side and **emit** a `function_call_output` with
the **same `call_id`** the provider issued; (3) on resolver error/timeout, emit a
`function_call_output` carrying an error payload (the turn continues — never aborts
mid-stream); (4) **SSE vs buffered parity** — identical logic; the buffered path
accumulates then emits, the SSE path forwards provider continuation chunks; (5) the
synthetic `function_call` and its `function_call_output` are consumed server-side and
**never leaked** into the client stream. Both happy-path (call → output → provider
continuation) and failure-path (resolver error → error output → provider continuation)
are covered by tests asserting client-stream cleanliness and `call_id` matching.

**Majors, resolved.** **P0 equivalence (§7):** P0 (typed-IR refactor of
`ingress_preinject_build`) ships with a **golden-output test** — render envelopes for
N representative sessions covering every `source_kind` before the refactor, assert
byte-identical output after. **Responses session id (§2.5):** the continuation/retry
path carries the session id from the request's bearer/session key (the same identity
the request context resolves), persisted in the work struct across continuations.
**Span-enrichment latency (§1.1/§8):** per-request lookup cache, **top-K code_hits
only** (default K=8), a +50ms p95 added-latency budget, and fallback to path+snippet
mode if the budget is exceeded. **Multi-replica rehydrate (§3.4):** a replica without
the ephemeral handle returns a structured error with a `resolver_hint` (e.g. "use
`code_span_get path=… lines=…`" or "`memory_get id=…`") so the model recovers via a
durable resolver; a deployment-shape flag makes durable resolvers the default for
non-single-process deployments. **Inline-tool authorization (§3.5):** injected
resolver tools carry the same authorization metadata as their MCP equivalents and are
**re-authorized per call** against the request's bearer/session/workspace.
**kb_mining integration (§4.4):** commit to the refactor path — `kb_mining.c` emits
`learning_signals`/proposals (not direct artifacts), dedup key `cluster_key +
event_type`, existing artifacts grandfathered.

Minors (out-of-scope rationale, handle TTL/LRU defaults `TTL=300s`/`max=128`,
`learning_replay.py` branch dependency, capability-table sketch) are recorded for the
implementer and do not gate the first phase.

---

## §7 Phasing

- **P0 — Envelope IR (§1.1). DONE (PR #585).** `ingress_preinject_build()` now
  assembles a typed entry list and renders from it; no behaviour change, no flag.
  Blocks everything else.
- **P1a — Byte-equivalent code fold (§1.2/§1.3). WITHDRAWN — it is a no-op on the
  current resident form.** A design roundtable (2026-06-21, 3 lenses, unanimous)
  found that today's resident code form is already a single collapsed line per
  hit: every `code_hit` snippet is routed through `append_single_line_escaped()`,
  which strips newlines/tabs and collapses whitespace runs **before** the entry is
  built. A "blank-line/whitespace collapse" fold therefore has no input to act on,
  so shipping it would land `ingress_compress_enabled` guarding dead code for zero
  byte savings. P1a is removed from the phase list; the `ingress_compress_enabled`
  flag is **not** introduced until a fold with real input exists (P1b). **Precondition
  carried forward:** audit what `append_single_line_escaped()` already destroys
  (heredocs, multi-line strings, indentation-sensitive snippets like YAML/Make) —
  the resident form is already lossy, so the byte-equivalence framing for code
  snippets is moot and any future fold must be measured against that baseline.
- **P1b — Lossy code fold + resident-form enrichment (§1.2/§1.3/§1.4).** This is
  the next real increment, and it is **P1b-sized, not a small win**: to fold
  anything meaningful the resident form must first carry a **multi-line** code span
  (signature + relevant lines), which pulls in span enrichment, a reachable
  rehydrate resolver, `transform` tags, the `X-Aimee-Compress` per-call escape, the
  per-entry telemetry (#34), and the forced-rehydration accuracy A/B (§6) before any
  default flip. The roundtable also surfaced an alternative worth A/B-ing here — a
  `file:line[:span]` reference resolved through the rehydrate path instead of an
  inline fold — but it is **not** non-lossy-for-free: it shifts bytes to a runtime
  resolve (latency), changes the P0 IR's inline shape, and depends on the same
  resolver/reachability infra, so it lives inside P1b's preconditions, not ahead of
  them. No default flip until P2.
- **P2 — Durable-read reachability + accuracy A/B for the lossy fold (P1b).** A
  lossy code fold recovers via a durable code-span/range read, so its
  forced-rehydration accuracy A/B and its default-flip candidacy need a real
  callable resolver: either add an Aimee MCP `code_span_get`-style tool or prove
  the target client has a scoped native file read. If Aimee adds `code_span_get`,
  it resolves through the workspace-provider / detached-runner boundary and
  includes `line_end` propagation fixes on server-dispatch `index.structure`.
  `memory_get` is required only before memory handles are emitted. This phase
  does **not** require the new in-process handle store or `rehydrate`.
- **P2e — Rehydration handle (§3) for ephemeral folds.** When ephemeral folding
  (JSON/tool-result) enters scope it brings the in-process handle store with the
  full §3.2 safety contract, the `rehydrate` MCP tool + metadata goldens, and the
  injected-tool auth-denial tests. Only ephemeral data needs it; durable folds
  never do.
- **P3 — Cache-prefix placement (§2)** on every Codex/OpenAI injection path:
  capability table, no-op default, integration with `payload_rewrite`, placement
  invariant + body-serialization tests, `agent_execute_messages()` session-scoped
  prefix tracking, and, if synthetic resolver tools are injected, the full
  Responses continuation/interception loop. Realized-cache telemetry is consumed
  from `token_tracker` / the cost-accounting ledger. Does not add a duplicate
  cache-marking flag; ships after the accounting surface.
- **P4 — Failure-mined corrections (§4)** emitting learning signals, default-off,
  promoted through the existing review/Gate-Promote path, preferably by
  refactoring `kb_mining.c` recurrence rather than creating a parallel miner;
  use `kb_mining_enabled` plus a narrower job gate or add a fully plumbed
  `curator_failure_mining_enabled` field.
- **P5 — Anthropic ingress injection (§2.3)** as a separate opt-in phase behind
  its own gate, with system-block-array preservation and the Claude-Code MCP
  tool-call proof.

## §8 Risks

- **§2 changes the live request shape.** A wrong placement could break an ingress.
  Mitigated by the capability table + no-op default (§2.4), the body-serialization
  + prefix-stability tests, and shipping after the accounting surface. The earlier
  "stateless-proxy-preserving" mitigation is withdrawn for Anthropic (§2.3).
- **Lossy folding degrades answers invisibly.** Mitigated by the lossy-by-contract
  framing (§1.3), reachable rehydration (§3.2), and the **forced-rehydration**
  accuracy gate (§6) — never flip a default on token count alone.
- **Rehydration handle as a data-exfil path.** Mitigated by the §3.2 scope +
  capability-binding + log-hygiene contract and the sensitivity fixtures (§6).
  If OpenAI/Codex inline tool interception is implemented, it must also inherit
  the route authorization and remote-client denial rules from the existing
  HTTP/MCP surface (§3.5).
- **Durable resolver as a data-exfil path.** Adding `memory_get` or
  `code_span_get` exposes read surfaces that are separate from the ephemeral
  `rehydrate` store. They need the same workspace/session/bearer scoping and
  denial tests; `find_symbol` metadata alone must not be treated as content
  authorization.
- **Workspace-provider bypass.** A server-side `code_span_get` that opens indexed
  paths directly can read stale mirror content, fail for detached workspaces, or
  cross into a server-local path outside the user's active workspace. Mitigated by
  routing source reads through the workspace provider / detached-runner boundary,
  or by restricting lossy code folds to clients with proven native file-read
  authority for the same workspace.
- **Inert or misplaced flags.** New names such as `ingress_compress_enabled`,
  `ingress_rehydrate_*`, or `curator_failure_mining_enabled` can look reviewed
  while remaining unsettable. Mitigated by requiring config load/save, CLI,
  generated docs, and config-surface tests as part of each phase (§6).
- **Premise wrong (envelope actually stable).** If measurement (§2.2) shows the
  envelope is cache-friendly after all, the two proposals reconcile on the data;
  no code is committed to either premise before the bench reports.
- **Provider cache semantics drift.** §2 depends on current Anthropic/OpenAI
  caching rules — re-verify against the live Claude API reference before
  implementing, not from memory.
- **Hot-path latency regression.** A span-enriched IR can add KB lookups or body
  serialization to every turn. Keep P0 behavior-preserving, cache per-request
  lookups, and block default flips on p95 ingress-build latency as well as
  token/correctness metrics.
- **Duplicate learning outputs.** If §4 mines the same
  `interaction_event_embeddings` clusters that `kb_mining.c` already promotes,
  users will see both artifacts and learning proposals for one failure pattern.
  Refactor the existing mining job or use a disjoint dedup key before enabling.
- **Resolver unreachable on the target ingress.** If a lossy fold's resolver is
  not callable (client lacks Aimee's MCP, the installed Claude config registers the
  hooks but not the MCP tools, the durable code-span read is unavailable, or a
  multi-replica deploy routes `rehydrate` to another process — §3.4), the fold
  silently strands detail the model cannot recover. Mitigated by gating lossy folds
  on proven per-ingress **resolver** reachability (§3.4) and falling back to
  byte-equivalent folds / stand-alone previews otherwise — a P2 acceptance check,
  not an assumption.
- **Source drift on durable re-read.** A re-read pointer returns *current* content,
  not the pre-fold bytes (§1.1); if the agent edited the file/record after the fold,
  the rehydrated detail differs from what was summarized. Usually current content is
  preferable, but the model must not be misled into thinking it recovered the exact
  folded text. Mitigated by carrying the version/hash drift signal and testing it
  (§6), and by reserving the byte-exact guarantee for stored-bytes ephemeral folds.
- **Span truncation through server dispatch.** `line_end` exists in the KB HTTP
  code-structure response, but the server-dispatch `index.structure` route
  currently drops it. A code fold that depends on that route may recover only a
  start line or an over-broad range. Mitigated by span propagation parity tests
  before enabling lossy code folds.
- **Synthetic Responses tool-call leakage.** If Aimee injects resolver functions
  into `agent_execute_messages()` and fails to consume them internally, Codex will
  receive unknown synthetic calls or mismatched `function_call_output` state.
  Mitigated by treating inline interception as a full Responses continuation
  state machine with buffered/SSE parity tests (§2.6, §6).
- **Runtime API spec drift.** Updating `api/openapi-server-v1.yaml` without
  regenerating `src/server/openapi_server_data.h` leaves `/v1/openapi.yaml` stale.
  Mitigated by making the generated embed part of the acceptance gate (§1.4, §6).
- **Recovery round-trips erase the saving.** Resident-token reduction is not net
  reduction: a frequently re-opened fold spends a resolver tool call plus the full
  recovered span, sometimes exceeding what was saved, plus latency — so an
  aggressively-folded but body-heavy task class can net negative while the resident
  numbers look excellent. Mitigated by measuring recovery rate and net token delta
  per task class (§6) and gating the §1 default flip on **net** positive, never on
  resident reduction alone.
- **Aggregate usage hides bad transforms.** Provider usage can show a net session
  win while one transform or resolver is consistently negative. Mitigated by
  per-entry compression/resolver telemetry in the IR and task-class breakdowns
  before any default flip (§1.1, §6).
