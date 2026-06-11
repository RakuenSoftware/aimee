# Proposal: Envelope compression, cache-prefix alignment, reversible rehydration, and failure-mined corrections

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-11 (revised post-PR-#181 sixth review)
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
  `bench/ingress_token_bench.py` + `benchmarks/learning/learning_replay.py`
  for the accuracy/token A/B, config surface tests (`src/tests/test_config.c`,
  `src/tests/test_config_surface.c`, `src/tests/test_cmd_config.c`), generated
  config docs (`docs/gen/configuration.md` via `scripts/gen-reference-docs.py`),
  unit + integration tests, docs. No new long-lived service; the ML prose
  compressor is explicitly out of scope (§5).

## Design at a glance

For readers not following the review history below (gaps 1–31 record how the
contract was hardened and can be read as an appendix), the design is four
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
- `docs/proposals/pending/ingress-cost-accounting-and-optimizations.md` owns
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

## PR #181 review — gaps and how they are resolved

The first revision was directionally useful but under-specified the
implementation contract. The eight blocking gaps found in review against the
current tree are captured below, each annotated with where the revised design
resolves it. The body sections (§1–§8) are rewritten to match.

### 1. The current ingress envelope is not typed enough for the proposed router

`ingress_preinject_build()` currently assembles one rendered string from three
sources: a compact code-hit list (`ingress_preinject_format_code_block()`), the
opaque rendered `memory.context_block`, and the optional audit context file. It
does **not** carry per-entry content-type tags, raw tool-output entries, or a
structured list of records that a ContentRouter-style compressor can dispatch
over.

That means §1 cannot be implemented as a small hook over today's final envelope.
It needs an explicit P0/P1 dependency on a structured envelope IR (or on
recall-economy's preview/read contract) before rendering. The proposal should
define that IR minimally: source kind, record id/handle, sensitivity/scope,
rendered preview, original bytes pointer/handle, byte budget metadata, and the
compression transform applied. Without that, JSON folding has no reliable target
and code folding risks re-parsing already-rendered prose.

Related correction: "JSON tool-output entries" are not currently part of the
pre-injection envelope, so JSON compression should either be scoped to future
memory/tool-result previews or removed from the first phase.

**→ Resolved in §1.1** (the envelope IR is now P0, defined field-by-field) **and
§1.2** (the first phase folds only the code block; JSON folding is deferred to
when typed tool-result previews exist).

### 2. Several "structural" transforms are lossy, not information-preserving

Dropping null/empty JSON fields, eliding repeated array elements, stripping
comments, and replacing code with signatures + spans can all remove information
that matters to a task. §1 currently says these are information-preserving for
the LLM consumer; that is too strong. The safe contract should be:

- the folded resident form is intentionally lossy/summarized unless proven
  byte-equivalent;
- every lossy fold must carry an explicit `rehydrate` handle and enough visible
  metadata for the model to know when to open it;
- default-on is blocked until the accuracy A/B includes tasks where the omitted
  detail is necessary and confirms the model actually rehydrates when needed.

The byte-exact original in §3 mitigates loss only if the handle is available,
authorized, and discoverable in that ingress. It does not make the resident text
itself information-preserving.

**→ Resolved in §1.3** ("intentionally lossy unless byte-equivalent"; every fold
carries a visible handle + transform tag) **and §6** (the forced-rehydration
accuracy gate blocks any default flip).

### 3. Cache-prefix alignment overlaps and conflicts with the cost-accounting proposal

`docs/proposals/pending/ingress-cost-accounting-and-optimizations.md` already
claims ownership of usage capture, cached-token accounting, and Anthropic
`cache_control` marking for the ingress envelope. This proposal also claims
cache-token telemetry and cache placement. The two need an explicit ownership
split or they will design the same flags and request mutations twice.

Suggested split:

- cost-accounting owns provider usage extraction, USD pricing, ledger writes,
  `/v1/usage/*`, and generic cache-token reporting;
- this proposal owns only the byte-placement invariant needed for pre-injected
  context after the accounting surface exists;
- the flag names must not fork (`ingress_cache_marking_enabled` vs. any new
  cache-alignment flag), and benchmarks should consume the same ledger fields.

There is also a premise mismatch: the cost proposal describes the envelope as
"large and constant across a session", while this proposal correctly treats it
as volatile per turn. The merged design should state which parts are stable
enough to cache (system/persona/history) and which parts are intentionally after
the cache boundary (`<aimee-context>`).

**→ Resolved in §2.1** (ownership ceded: cost-accounting owns marking + the flag;
this proposal owns only the placement invariant and does not add a duplicate
cache-marking flag)
**and §2.2** (the premise is reconciled empirically — the envelope is per-turn
volatile, so the stable prefix is system/persona/history and `<aimee-context>`
goes *after* the cache boundary, settled by measured cache reads not assertion).

### 4. Anthropic injection is not a simple stateless-proxy-preserving change

`src/server/anthropic_http.c` explicitly documents the Anthropic Messages path
as a stateless wire-format proxy that does not run Aimee's agent loop, memory,
persona, or toolset because Claude Code owns its own context and tools. Injecting
an Aimee envelope into that path would require:

- extracting a query from Anthropic messages without changing client-owned tool
  state;
- calling `ingress_preinject_build()` from the Anthropic path;
- preserving Anthropic `system` block arrays and `cache_control` metadata instead
  of flattening everything through `anthropic_system_to_text()`;
- adding per-request and config gates separate from the existing OpenAI/Codex
  seam; and
- proving Claude Code can actually call the advertised Aimee MCP tools if the
  envelope tells it to `rehydrate` or use a durable pull-handle such as
  `memory_get`.

The current text says injection can happen "without abandoning" the stateless
contract because the proxy still forwards. That is not precise enough. The
proposal should define Anthropic support as a separate opt-in phase with tests
for direct Anthropic-provider passthrough and non-Anthropic delegate translation.

**→ Resolved in §2.3** (Anthropic is carved out as a separate opt-in phase, P5,
behind its own gate, with the precise mutation list and the `anthropic_system_to_text()`
block-array preservation requirement) — the false "without abandoning" claim is
removed.

### 5. Provider cache semantics are provider-specific, not one placement rule

Anthropic, OpenAI, OpenAI-compatible delegates, and Gemini-style providers expose
different cache controls. Anthropic has explicit `cache_control` blocks; OpenAI
prompt caching is automatic and prefix/min-token dependent; many compatible
providers expose no usable controls. §2 should not imply one "latest point" or
"breakpoint" abstraction covers every ingress.

Required implementation contract:

- provider capability detection before mutating any request;
- no-op behavior for providers without explicit cache controls;
- tests that serialize the exact provider request body and assert that existing
  client-supplied cache controls are preserved;
- benchmark output that separates resident-token reduction from realized
  provider cache reads/writes.

**→ Resolved in §2.4** (a per-provider capability table; no-op default; a
body-serialization test that asserts client cache controls are preserved) **and
§6** (resident vs. realized-cache reporting is a distinct acceptance gate).

### 6. Rehydration handles need security, lifetime, and topology rules

An in-process TTL LRU is a reasonable first storage primitive, but the proposal
does not yet specify the safety contract. Rehydration handles must be:

- scoped to session, workspace, and memory sensitivity/tenant;
- unguessable or capability-bound, not just short ids;
- invalidated on source record version/hash changes;
- safe across concurrent requests and server restarts, or explicitly documented
  as best-effort;
- hidden from logs where they would expose access to sensitive originals; and
- unavailable/expired in a way the model can recover from (`memory_get` /
  durable source fallback, rerun recall, or clear error text).

Also, the MCP tool surface lives in `server_mcp_call_table.inc` /
`server_mcp.c`, not only `server_mcp.c`; tool discovery metadata in
`mcp_tools.c` and tests/goldens must be included in scope.

**→ Resolved in §3.2** (the full handle safety contract: scope, capability-bound
ids, version invalidation, best-effort-across-restart, log hygiene, expiry
fallback) **and the Scope line** (`server_mcp_call_table.inc` + `mcp_tools.c` +
goldens added).

### 7. Failure-mined corrections should route through existing learning machinery

The repo already has a learning proposal surface (`learning_propose`,
`learning_review`), implicit learning detectors, `interaction_events`, and
`guardrail_events`. §4 should not write directly to durable memory after mining a
bad session. It should emit a learning signal/proposal with evidence and let the
existing review / Gate-Promote machinery decide whether to promote it into a
memory, rule, or artifact.

The proposed signal sources also need grounding:

- the attention guard's per-session log is a Claude Code hook artifact, not a
  general server-side session outcome log;
- "abandoned/retried turn" is not currently a reliable observable across all
  ingresses;
- explicit user corrections overlap with `learning_implicit_repeated_correction`
  and should reuse that path rather than create a parallel detector.

**→ Resolved in §4** (rewritten to emit a `db2_learning_signal_insert` →
`db2_learning_proposal_insert` signal that the existing review/accept path
promotes to a sink; no direct memory write). Signal sources are re-grounded on
the `interaction_event_embeddings` table (`failure_mode`/`cluster_key`); the
Claude-Code-hook-only nature of the guard log and the "abandoned turn" caveat are
stated; explicit-correction mining defers to the existing implicit-correction
detector.

### 8. Validation dependencies are not all complete

The PR adds only this proposal file. `benchmarks/learning/learning_replay.py`
exists on `origin/testing`, but it is not yet a live compression/rehydration
accuracy harness: it needs a prediction emitter and fixtures that force
rehydration. `bench/ingress_token_bench.py` exists, but today it is a preinject
on/off token bench; it does not yet provide per-stage resident tokens, provider
cache reads/writes, compression-on/off arms, latency, or correctness outcomes.
Those harness upgrades are part of the proposal's required work, not existing
validation.

Add acceptance gates for:

- resident bytes/tokens by section and transform;
- realized provider cache read/write tokens via the accounting proposal's ledger;
- answer correctness with forced rehydration-needed cases;
- handle expiry and unauthorized rehydration behavior; and
- prompt-injection/sensitivity fixtures for compressed previews and originals.

**→ Resolved in §6** (every gate above is enumerated; the harness upgrades are
listed as in-scope work; `learning_replay.py` is treated as a partial dependency,
not a complete gate).

## PR #181 second review — remaining gaps after the update

The updated revision resolves the first eight blockers, but a second pass against
`origin/testing` found eight remaining implementation gaps (9–16) that must be
carried by the proposal.

### 9. Code-search hits do not expose symbol spans

`code_search_hit_t` currently carries only `project`, `file_path`, `snippet`, and
`rank`. `line` / `line_end` exist on `term_hit_t` (`find_symbol`) and
`definition_t` (`index_structure`), and the MCP `find_symbol` tool renders those
spans, but `kb_client_index_code_search()` does not parse or return them. The
code folder therefore cannot simply "use existing `line_start`/`line_end`" from
the preinject search result.

**→ Resolved in §1.2** by making span enrichment part of P0/P1: either extend
`/v1/code/search` and `code_search_hit_t` with `line`/`line_end`/`kind`, or run a
bounded secondary structure lookup per selected file. The first code folder must
fall back to snippet/path-only folding when spans are absent.

### 10. The compression escape header needs HTTP plumbing

The proposal adds `x-aimee-compress: 0`, but only `X-Aimee-Preinject` is parsed
today in `server_http.c` and forwarded through a thread-local override. A new
compression header is not just config work; it needs request-header parsing,
thread-local/request-scoped state, tests for buffered and streaming routes, and
OpenAPI/API docs if it becomes a supported header.

**→ Resolved in §1.4** and P1 scope.

### 11. Cache-prefix work must integrate with `payload_rewrite`

The repo already has `payload_rewrite_prefix_hash()`,
`payload_rewrite_track_request()`, DB1 `payload_rewrite_state`, and the
`payload_rewrite_status` MCP tool behind `cache_aware_rewrite_enabled`. The
proposal's placement invariant and prefix-stability tests should not create a
second prefix-hash/state machine. They must either extend this subsystem or
explicitly supersede it.

**→ Resolved in §2.5** and P3.

### 12. "Reuse `ingress_cache_marking_enabled`" is too strong

`ingress_cache_marking_enabled` belongs to the cost-accounting proposal's
request-mutation mechanism. Placement can be behaviorally relevant even when
marking is off, and it may need shadow telemetry before marking is enabled.
Tying placement entirely to the marking flag couples two rollout risks.

**→ Resolved in §2.1** by narrowing the claim: no duplicate marking flag, but P3
may need an internal/shadow placement gate or use `cache_aware_rewrite_enabled`
for the existing prefix subsystem.

### 13. OpenAI ingress has multiple injection sites

`openai_chat.c` calls `ingress_preinject_build()` in legacy chat/completions,
Responses buffered continuations, streaming chat/completions, and Responses
streaming. The proposal says "Codex/OpenAI seam" but P3 cannot be implemented at
one call site and be done; all four paths need equivalent placement, disable
semantics, body serialization tests, and continuation behavior checks.

**→ Resolved in §2.6** and P3.

### 14. Failure mining overlaps existing KB mining

`interaction_event_embeddings` are already mined by `src/kb/kb_mining.c`, and
the recurrence job writes `workflow_pattern` artifacts directly with
`db2_artifact_write()`. If this proposal introduces a learning-signal path for
failure mining, it must decide whether to extend/replace that mining job, or it
will create parallel artifacts/proposals from the same failure clusters.

**→ Resolved in §4.4** by making the mining integration explicit.

### 15. Validation text is stale on `learning_replay.py`

`benchmarks/learning/learning_replay.py` exists on `origin/testing`. The
remaining gap is not that the file is untracked; it is that the harness is a
schema/metric runner and still needs a live `aimee learning replay` prediction
emitter plus compression-specific fixtures.

**→ Resolved in §6**.

### 16. Rehydration assumes the external agent can call Aimee's MCP tools

The reversibility lever (§3) — and, in fact, the existing `explore-with` steer
shipped in `context-preinjection-ingress.md` — only pays off if the model can
actually invoke the named tools. `ingress_preinject_format_envelope()` injects
envelope *text* that *names* tools (`explore-with: …`); on the OpenAI/Codex seam
`openai_chat.c` forwards the **client's** `tools` array and runs a `function_call`
loop where tool calls are executed by the client. Injecting text that names
`rehydrate` does **not** make it callable: the external agent can only call it if
it has Aimee's MCP server configured on its side, or if the ingress injects the
tool definition into the request and intercepts the resulting tool call inline.
The first revision raised reachability only for Anthropic (§2.3/§3.3); it is in
fact a precondition on **every** ingress, including the primary OpenAI/Codex one.

**→ Resolved in §3.4** (reachability is a stated per-ingress precondition; a
*lossy* fold is permitted only on an ingress where `rehydrate` reachability is
proven end-to-end, otherwise the fold degrades to byte-equivalent-only or a
stand-alone-sufficient preview; reachability becomes a P2 acceptance check, not
an assumption) — this also makes the §1.3 "lossy is safe because §3 recovers it"
claim conditional on a tested fact.

## PR #181 third review — remaining gaps after the reachability update

The reachability update closes the largest conceptual hole, but a third pass
against the current tree found five more gaps that need to be part of the
proposal before implementation starts.

### 17. New config flags need the full generated config surface

`ingress_compress_enabled`, `ingress_rehydrate_ttl_s`,
`ingress_rehydrate_max_entries`, and `curator_failure_mining_enabled` do not
exist in the codebase today. Adding names in prose is not enough: every new flag
needs an explicit section/field decision, load/save wiring, CLI config
get/set/list behavior, generated documentation, and tests. Existing related
surfaces are split today: `ingress_preinject_enabled` is a top-level field,
while `cache_aware_rewrite_enabled` is exposed under
`transport.cache_aware_rewrite`. The proposal must decide whether the new fields
are top-level ingress fields, nested transport fields, or a new section, and it
must not leave inert names that the operator cannot set or inspect.

**→ Resolved in §1.4, §3.2, §3.5, §4.5, §6, and §7** by making config placement,
docs generation, and config tests explicit acceptance work.

### 18. Recall-economy's pull-handle is `memory_get`, not a generic `fetch`

The recall-economy proposal's concrete pull surface is a sibling MCP
`memory_get` tool over the existing `memory.get` backend (`kb_client_memory_get`),
with `get_context_block` extension only as an option. The current text's generic
`fetch` verb would fork that design and create handles the agent may not be able
to resolve. If this proposal wants a generic `fetch`, recall-economy must adopt
that too; otherwise this proposal should align to `memory_get` for durable
memory records and reserve `rehydrate` for folded ephemeral/compressed originals.

**→ Resolved in §3.1 and §3.5** by removing the assumed two-verb `fetch`/`rehydrate`
namespace and aligning the durable read path to recall-economy's `memory_get`
surface.

### 19. Responses/Codex cache telemetry does not currently carry session state

`agent_execute_messages()` forwards the caller's Responses tools and reports
tool calls back to the client, but it does not call `payload_rewrite_track_request()`
and its provider retry call currently passes no session id into the retry/context
path. If P3 only hooks the older `agent_execute()` path, cache-prefix state and
prefix hashes will not be session-scoped for the primary Codex wire ingress. The
proposal must explicitly include the Responses buffered and streaming paths in
the payload-rewrite/session-id integration, not just the envelope insertion
points.

**→ Resolved in §2.5, §2.6, §6, and §7** by adding a P3 requirement for
`agent_execute_messages()` to record the provider-facing prefix in the existing
payload-rewrite subsystem with real session scope.

### 20. Tool definition injection would expand the server's read authority

The OpenAI/Codex fallback in §3.4 says Aimee could inject a `rehydrate` tool
definition and intercept the resulting tool call inline. That is a materially
different security surface from merely naming an MCP tool in text. It would
create a server-mediated function-call bridge for compressed originals and
possibly memory/code reads. That bridge must inherit the same route authorization,
workspace/session scoping, read-only bearer behavior, and remote TCP restrictions
as the rest of the HTTP/MCP surface; it cannot become an unauthenticated way for
remote clients to ask Aimee to read hidden originals.

**→ Resolved in §3.4, §3.5, §6, and §8** by making injected-tool interception a
separate capability-gated implementation path with denial tests.

### 21. Claude Code MCP reachability should be proven from installation config

The proposal currently infers Claude Code reachability from the completed
context-preinjection proposal. The implementation should prove it from the
actual integration installer/config and an end-to-end hook-session fixture: the
model should see the compressed handle and be able to call the registered MCP
tool in the same session. This is especially important because lossy compression
is gated on reachability.

**→ Resolved in §3.4 and §6** by weakening the claim from "callable today" to
"must be validated from installed config" and adding a fixture requirement.

## PR #181 fourth review — internal consistency after the resolver split

The third review split recovery into three resolvers (§3.1: `memory_get` for
durable memory, code/file-span read for durable code, `rehydrate` for ephemeral),
but the lossy-fold gate, the byte-exact promise, and the phasing still read as if
`rehydrate` were the only recovery path. Four consistency gaps follow.

### 22. The reachability gate targets `rehydrate`, but the first phase recovers via a code-span read

§3.4(a) gates a lossy fold on proving **`rehydrate`** is reachable. But the first
shipped fold is the **code folder** (§1.2), whose resolver is the durable
code/file-span read surface (§3.1), *not* `rehydrate`. As written, the very first
phase's reachability check is aimed at the wrong tool — and `rehydrate` may not
even be built yet when the code folder ships.

**→ Resolved in §3.4** by generalizing the gate to *the fold's resolver*
(`memory_get` / code-span read / `rehydrate`, whichever a given fold depends on),
so the code folder is gated on code-span-read reachability — a surface that
already exists wherever the preinject `explore-with` tools do.

### 23. A re-read pointer does not return the byte-exact pre-fold original

§1.1 calls `original_ref` "the byte-exact pre-fold original," but for durable
**re-read pointers** the source can drift between fold time (turn N) and read time
(the agent may have edited the file or the record mid-session). A re-read returns
the **current** content plus a version/hash drift signal — equal to the pre-fold
bytes only when nothing changed. Only **stored bytes** (ephemeral) are
unconditionally byte-exact. The §6 "byte-exact rehydration round-trip" invariant
silently assumes the stored-bytes case.

**→ Resolved in §1.1** (recovery guarantee split by resolver) **and §6** (the
round-trip test is split: stored-bytes = byte-exact; re-read pointer = current
content with a correct drift flag).

### 24. The rehydrate handle store is not on the critical path to the first flip

Because the first fold (code) recovers via a re-read, the in-process handle store
and the `rehydrate` tool (P2) are not required to ship the code folder or to take
its forced-rehydration accuracy A/B. The handle store is only needed once
**ephemeral** folding (JSON/tool-result) lands, since that is the only data that
cannot be re-read.

**→ Resolved in §7** by moving the handle store onto the ephemeral-folding path
and letting P1's code folder reach its default-flip candidate on code-span-read
reachability alone — shortening the path to the first measurable win.

### 25. "Claude config has Aimee's MCP registered" is a different install step from the hooks

The done context-preinjection proposal wired Claude Code **hooks**
(SessionStart/UserPromptSubmit/PreCompact/PreToolUse) — it did not necessarily
register Aimee's **MCP server** with the client. Hooks inject context; they do not
make `memory_get`/`rehydrate` callable. Reachability depends on the separate MCP
registration step, which the hook installer may not perform.

**→ Resolved in §3.4** by stating the two installer actions are distinct and that
the reachability fixture must assert the MCP-tool registration, not just the hook
wiring.

## PR #181 fifth review — callable resolver surfaces and generated artifacts

The fourth review made the resolver split internally consistent, but another
pass against the current tree found five implementation gaps around *whether the
named resolvers actually exist on the surfaces the envelope steers the agent to*.

### 26. Durable code-span read is not an Aimee MCP tool today

`ingress_preinject.c` steers co-registered agents to
`find_symbol, lsp_references, ast_grep_search, search_graph, get_context_block`.
The MCP call table exposes `find_symbol`, but that tool returns file paths and
line ranges only; it does not return file contents. The MCP tool list does not
currently expose `read_file`, `code_search`, or a range-read tool. Those exist in
the delegate/agent toolset, but not as Aimee MCP tools. Therefore the fourth
review's claim that the code folder can recover via an "existing durable
code/file-span read surface" is true only for clients that already have their own
filesystem tools, not for the Aimee MCP surface that the envelope advertises.

**→ Resolved in §3.1, §3.4, §3.5, §6, §7, and §8** by requiring either a real
Aimee MCP `code_span_get`/range-read resolver with tool metadata and auth tests,
or an explicit per-client native-file-tool reachability proof before lossy code
folds are allowed.

### 27. `memory_get` is a proposal dependency, not an existing MCP tool

The backend chain exists (`memory.get` / `kb_client_memory_get`), but
`mcp_build_tools_list()` and `server_mcp_call_table.inc` do not expose a
`memory_get` MCP tool today. If this proposal emits memory preview handles before
the recall-economy proposal lands, the model will see handles it cannot open.

**→ Resolved in §3.1, §3.5, and §7** by making recall-economy's `memory_get`
tool either an explicit prerequisite or an in-scope wrapper with metadata,
goldens, and handler wiring.

### 28. The goal text still promises a rehydration handle for every folded entry

After the resolver split, durable code and durable memory do not recover through
the rehydration handle store; they recover through code-span read or `memory_get`.
The goal section still says every resident form is paired with a rehydration
handle, which would reintroduce unnecessary handle-store work for durable data
and contradict §1.1's re-read-pointer design.

**→ Resolved in the Goal and §3.1** by replacing "rehydration handle" with
"recovery resolver" for durable folds and reserving `rehydrate` for ephemeral
stored bytes.

### 29. OpenAPI changes require regenerating the embedded server spec

The proposal scopes `api/openapi-server-v1.yaml`, but `/v1/openapi.yaml` and
`/v1/openapi.json` are served from generated `src/server/openapi_server_data.h`.
`src/Makefile` regenerates that header from the YAML. If `X-Aimee-Compress` or
any other request header becomes public documentation, updating only the YAML
leaves the runtime-served spec stale.

**→ Resolved in §1.4, §6, and §7** by adding the generated header and API
conformance/docs generation to the acceptance work.

### 30. Inline Responses interception is a state-machine change, not just a tool definition

`agent_execute_messages()` is deliberately a wire adapter: it forwards the
client's `tools` array, calls the provider once, and surfaces `function_call`
items back to Codex for the client to execute. It does not run Aimee's agent
tool loop. If Aimee injects a synthetic `rehydrate` or `code_span_get` function
definition, the provider can return a synthetic function call that Aimee must
consume itself, produce a `function_call_output`, continue the provider turn,
and hide or account for that internal call in both buffered and SSE Responses.
Otherwise the synthetic call leaks to Codex as an unknown client tool.

**→ Resolved in §2.6, §3.5, §6, and §8** by making inline interception a separate
request/response state-machine design with call-id handling, streaming parity,
and tests that synthetic resolver calls are not leaked to the client.

## PR #181 sixth review — net token economics, not just resident reduction

The first five rounds hardened the *mechanism* (resolvers, handles, reachability,
security, generated artifacts). One economic gap remains: nothing yet proves the
levers net out positive once the agent's recovery round-trips are counted.

### 31. Validation measures resident reduction but not net cost or recovery rate

A lossy fold saves resident tokens on the turn it is injected, but if the agent
then calls the resolver (`code_span_get` / `memory_get` / `rehydrate`) to recover
the folded-out detail, that round-trip costs a tool call **plus the full recovered
span** — often as many tokens as were saved, plus latency. The headline "token
reduction" therefore only holds when recovery is *rare*. §6 measures resident
reduction, realized cache, and forced-rehydration correctness, but never the
**net** token delta (resident savings − recovery overhead) nor the **recovery
rate** that determines the sign of that delta. On body-heavy task classes, net
savings can be near zero or negative while resident reduction still looks large —
the same trap behind headroom's resident-only "60–95%" numbers.

**→ Resolved in §6** (a net-economics gate: report recovery rate and net
per-session token delta including resolver round-trips, by task class; the §1
default flip requires **net** positive, not just resident reduction) **and §8**
(recovery-cost risk).

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
- `transform` — which fold was applied (`none` | `code_fold` | `json_fold`), so
  the metadata is visible to the model and the test suite.

Where recall-economy has already landed its preview/read contract, this IR
**is** that contract extended with `original_ref` + `transform`; it is not a
second scheme.

### §1.2 First phase folds the code block only (resolves review #1, correction)

Correction from review: JSON tool-output entries are **not** in today's envelope,
so there is no reliable JSON target yet. The first phase therefore ships exactly
one compressor:

- **Code folder** (headroom's `CodeCompressor`, AST-aware in spirit; a
  conservative line/brace folder in C to start): for `code_hit` entries, strip
  blank-line runs and optionally comment bodies, and — after P0/P1 span
  enrichment — prefer signature + relevant span over whole blocks.

The **JSON folder** (headroom's `SmartCrusher`) is deferred to when typed
`tool_result` previews exist in the IR (memory/tool-result previews from
recall-economy, or a future tool-output envelope). It is specified here only so
the IR's `transform` enum reserves room for it.

Span-aware folding has a real data dependency: P0/P1 must either extend
`/v1/code/search` and `code_search_hit_t` with `line`/`line_end`/`kind`, or do a
bounded secondary `index_structure`/`find_symbol` lookup for selected files. When
spans are absent, the folder falls back to path + snippet folding and must not
pretend it has a byte-exact body span.

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
- carry it as request-scoped/thread-local state into `ingress_preinject_build()`;
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
  proven client-native range read), not merely `find_symbol` metadata;
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
  same gate.
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
  MCP wrapper if memory handles are emitted.
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

## §7 Phasing

- **P0 — Envelope IR (§1.1).** Refactor `ingress_preinject_build()` to assemble a
  typed entry list and render from it. No behavior change, no flag; pure
  enablement. Blocks everything else.
- **P1 — Code folder (§1.2/§1.3/§1.4)** behind `ingress_compress_enabled`,
  including the span-enrichment fallback and `X-Aimee-Compress` plumbing if the
  header is exposed, plus the full config/docs/test surface for that flag and
  regenerated OpenAPI embed if the header is public. Lossy-by-contract, every
  fold carrying a `transform` tag. Token/latency A/B + harness upgrades; no
  default flip.
- **P2 — Durable-read reachability + accuracy A/B for the code folder.** The code
  folder recovers via a durable code-span/range read, so its
  forced-rehydration accuracy A/B and its default-flip candidacy need a real
  callable resolver: either add an Aimee MCP `code_span_get`-style tool or prove
  the target client has a scoped native file read. `memory_get` is required only
  before memory handles are emitted. This phase does **not** require the new
  in-process handle store or `rehydrate`.
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
