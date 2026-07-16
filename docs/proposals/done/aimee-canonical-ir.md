# Aimee canonical IR — protocol-neutral request/response (no direct translation)

- **State:** DONE — the goal is **shipped and live-proven**; all slices are landed or
  roundtable-sequenced as sanctioned follow-ups; filed to `done/`. On the default live path
  (`AIMEE_IR_PATH` default-ON) every client request PARSES to the IR and every provider request BUILDS
  from the IR (adapter matrix complete + three-way golden IR-equality), and the original bug — **codex
  primary breaks Claude Code** — is fixed + proven live on `.254` with real streamed content + tools
  (#936). **Precision (per the completeness roundtable):** the request parse+build path carries no
  direct translation by default; the one remaining incremental-*streaming* direct translator
  (`anthropic_stream_feed_openai`, used only for **non-codex OpenAI-chat** streaming) now has its
  IR-delta replacement wired but shipping **dark** behind `AIMEE_IR_STREAM_RELAY` (default-OFF), so
  legacy remains the streaming default until parity-gated enablement. **Enablement/exit criteria:** flip
  `AIMEE_IR_STREAM_RELAY` on once the shadow divergence metrics (`ir_rebuild_mismatch_bytes` etc.) stay
  clean on live non-codex streaming traffic; the eventual legacy *deletion* is gated on that same
  parity. See the **§Close-out** for the slice→PR map + the named sanctioned follow-ups.
- **Author:** JBailes
- **Date:** 2026-07-01

## Problem

Aimee proxies LLM traffic between clients (Claude Code = Anthropic `/v1/messages`;
Codex/OpenAI clients = `/v1/chat/completions`, `/v1/responses`) and upstream
providers. Today the ingress handler **binds the single configured primary agent
and translates the client's wire shape DIRECTLY into the primary's shape**
(`anthropic_messages_to_openai`, `anthropic_tools_to_responses`,
`anthropic_response_from_parsed`, …). So:

- Set primary = codex → a Claude Code (`/v1/messages`) request is force-translated
  Anthropic→Responses; fragile and breaks (the whole `fix/anthropic-codex-stream`
  branch is patching this direct-translation path).
- Direct translation is **N×M**: every client-shape × provider-shape pair needs its
  own converter, in both request and response directions, buffered and streaming.

## Current state (from the scope map) — the intermediate exists but is OpenAI-shaped

There already IS an intermediate; it's just not neutral:
- **Request intermediate = OpenAI-chat-shaped `messages + tools + system`.**
  `translate_request` (anthropic_http.c:171) turns an Anthropic request into that
  via `anthropic_ingress.c` (`anthropic_messages_to_openai`,
  `anthropic_tools_to_openai/_to_responses`, `emit_user/assistant/tool_result_*`).
  So Anthropic is DIRECTLY converted to OpenAI shape — the exact anti-pattern.
- **Response intermediate = `parsed_response_t`** (agent_protocol.h:19-37: text,
  stop_reason, tool_calls[], usage) — already fairly neutral; the seed of the
  response IR. Rendered out by `anthropic_response_from_parsed` (→ Anthropic) or
  `openai_format_*` (→ OpenAI/Responses).
- **Backend adapters already exist** as the `delegate_driver_t` vtable
  (build_request / parse_response / build_tools / build_url / get_caps) with
  drivers openai / chatgpt(Responses) / anthropic / gemini / … The builders live
  in `agent_bridge.c` (`agent_build_request_*`) and consume `messages+tools+system`.
- **Frontend parse/render layers** = `anthropic_ingress.c` (Anthropic) and
  `openai_shape.c` (OpenAI chat/completions/Responses parse + format).
- **Streaming translation** = `anthropic_ingress.c` `anthropic_stream_feed_openai`
  + the `xlate_*` state machine + `make_*` SSE builders (feeds an OpenAI SSE and
  emits an Anthropic SSE — a direct provider→client shape bridge).

So the refactor is **"make the intermediate neutral,"** not "invent one from
nothing." Replace the OpenAI-chat-shaped `messages+tools+system` with a neutral
`aimee_request_t`; evolve `parsed_response_t` into the neutral `aimee_response_t`.
The driver vtable stays as the backend-adapter seam; the frontend parse/render code
(`anthropic_ingress.c`, `openai_shape.c`) is rewritten to speak IR, and the direct
`anthropic_*_to_openai*` translators + `openai_parse_responses_to_chat` are deleted.

## Target architecture

A **canonical, protocol-neutral aimee IR** sits in the middle. Two adapter layers:

```
  CLIENT (frontend wire)                          PROVIDER (backend wire)
  Anthropic /v1/messages  ─┐                    ┌─ Anthropic Messages API
  OpenAI  /v1/chat        ─┼─▶ frontend.parse ─▶ IR ─▶ backend.build ─┼─ OpenAI Chat
  OpenAI  /v1/responses   ─┘        ▲            │          │          └─ Responses (codex)
                                    │         core stages   ▼
  frontend.render ◀── IR ◀──────────┘         (memory,   backend.parse ◀── provider resp
  (client's own shape)                     guardrails,        (any backend shape → IR)
                                            router, tool-strip,
                                            enforce, model-pin)
```

**Invariant: there is NO direct client-shape→provider-shape path.** Every
conversion goes *through the IR*. Frontend adapters convert the client's protocol
to/from the IR; backend adapters convert the IR to/from the provider's protocol.
This is **N+M** adapters, and it decouples the client protocol from the backend
model — Claude Code (Anthropic frontend) can be served by codex (Responses backend)
with zero Anthropic↔OpenAI code: `anthropic.parse → IR → responses.build`, then
`responses.parse → IR → anthropic.render`.

### The IR (two structs)

`aimee_request_t` — model, system (canonical text/blocks), messages[] (role +
canonical content blocks: text / tool_use / tool_result / image), tools[] (name,
description, JSON schema), tool_choice, max_tokens, temperature, stream, stop, …

`aimee_response_t` — id, model, role, content blocks (text / tool_use), stop_reason
(canonical enum), usage (input/output tokens). (Likely folds in the existing
`parsed_response_t`.)

Both are pure cJSON-free-ish C structs (or a thin cJSON wrapper with typed
accessors) so the core stages operate on the IR, not on shape-tagged raw JSON.

### Frontend adapters (`*_frontend.c`) — client protocol ↔ IR
- `anthropic_frontend_parse(req_json) → aimee_request_t`
- `anthropic_frontend_render(aimee_response_t, stream) → resp_json / SSE`
- `openai_frontend_parse` / `openai_frontend_render`
- `responses_frontend_parse` / `responses_frontend_render`

### Backend adapters (`*_backend.c`) — IR ↔ provider protocol
- `anthropic_backend_build(aimee_request_t) → provider_req_json`
- `anthropic_backend_parse(provider_resp) → aimee_response_t` (+ SSE stream parse)
- `openai_backend_build` / `openai_backend_parse`
- `responses_backend_build` / `responses_backend_parse`

The `delegate_driver_t` collapses into "the backend adapter to use," selected by
the chosen backend model's provider — independent of the frontend.

### Core stages operate on the IR
`gw_request_t.raw` (shape-tagged JSON) becomes `aimee_request_t`. The stages
(memory injection, tool-policing/subagent-strip, S1 router, S2 enforce, model-pin)
read/mutate the IR, so they are written ONCE, not per-shape (today memory injection
already has an Anthropic arm and an OpenAI arm — those collapse to one).

## KB / memory must consume the IR, not the wire

The user's insight: the KB is likely ingesting messages **incorrectly** because it
reads conversation content off a **shape-dependent** representation (the raw
Anthropic vs OpenAI request, or a lossily-translated one). Symptoms this predicts:
- `ingress_preinject_query_from_messages` has separate Anthropic-arm and
  OpenAI-arm extraction — so the "query" used for retrieval is derived differently
  (and possibly wrongly) depending on which client/protocol the turn came in on.
- Fact/memory extraction and the curator/synth pipeline that read tool_use /
  tool_result / multi-block content will mis-parse when the shape differs or when a
  direct translation reshaped the blocks (ids, ordering, tool_result nesting).
- The kb_client sends aimee→kb payloads in whatever shape happened to be on the
  wire, so the KB server stores heterogeneous, protocol-flavored records.

**Design requirement:** every KB/memory/learning/curator entry point that reads
conversation content consumes the **canonical `aimee_request_t` / `aimee_response_t`
IR**, never the raw wire JSON. Concretely:
- Retrieval query extraction becomes `aimee_ir_last_user_text(ir)` — ONE
  implementation over the IR (deletes the Anthropic/OpenAI arms).
- Fact/memory extraction and curator read IR content blocks (typed: text /
  tool_use / tool_result / image) — one code path, shape-agnostic.
- The kb_client sends a **canonical, versioned turn record** (IR-shaped) to the kb
  server, so stored conversation/memory records are protocol-neutral and stable.
- Interaction/turn logging records the IR form (or a stable projection of it).

**Confirmed bug sites (from the KB scope map):**
- `ingress_preinject_query_from_messages` (ingress_preinject.c:393-420) — the single
  extraction seam. `append_content_text` (374-391) assumes Anthropic `{type,text}`
  blocks, so it SILENTLY DROPS non-text blocks and takes only the LAST user message's
  text. When the turn arrives in a different shape (or was translated), the query the
  KB retrieves + learns against is wrong/incomplete. 4 call sites (gw_stage_memory
  Anthropic arm + OpenAI/Responses arm, router_advise, openai_chat learning).
- `kb_curator_llm.c` `build_messages` (13-40) hardcodes Anthropic `{role,content}` for
  the curator's OWN provider calls — malformed on a non-Anthropic curator backend; it
  must build via the backend adapter (IR → provider shape), not a hardcoded shape.
- The KB consumes already-extracted TEXT (kb_client sends `{query}` only; facts come
  from `memory_extract_patterns` over text). So the extraction seam is exactly where
  corruption enters — fixing it at the IR fixes the KB with no kb-server change needed
  for retrieval, and a canonical turn-record is the follow-on for richer ingestion.

This makes the KB a first-class consumer of the IR: the same parse-once boundary
that fixes the proxy also fixes what the KB sees. `query_from_messages` becomes
`aimee_ir_last_user_text(ir)` (typed block iteration, no dropped content); the
curator builds its calls through the backend adapter. Add a slice for it (below), and
add golden tests that a turn arriving as Anthropic vs OpenAI produces the SAME IR →
the SAME KB input (the regression that proves the "getting messages incorrectly"
bug is closed).

## Slices (each: pure cores + tests first, then wire, roundtable before PR)
0. Define the IR structs + typed accessors + unit tests (pure).
1. Frontend adapters: anthropic + openai + responses parse/render (+ golden tests
   vs the current direct converters, to prove byte-parity on representative
   payloads).
2. Backend adapters: build/parse for each provider (reuse agent_build_request_* +
   parsed_response_t as the starting point).
3. Rewire the buffered ingresses (anthropic_http.c, openai_chat.c) to
   parse→IR→stages→backend.build→send→backend.parse→IR→render.
4. Streaming: SSE frontend render + backend parse over the IR.
5. Delete the direct translators; make the core stages IR-native (one memory arm).
6. Backend selection = the configured primary/model's provider, decoupled from the
   frontend protocol (fixes "codex primary breaks Claude Code").
7. **KB/memory on the IR**: route every KB/memory/learning/curator message-read
   through the IR (`aimee_ir_last_user_text`, IR content-block iteration); make the
   kb_client emit a canonical versioned turn record; delete the per-shape arms.
   Golden test: Anthropic-shaped turn and OpenAI-shaped turn with identical
   semantics produce identical IR → identical KB input.

## Roundtable rulings (2026-07-01, non-degraded, 36 items) — RESOLVED

**Q1 IR schema → HYBRID: typed core + retained raw-JSON sidecar for unknown fields**
(pure struct is lossy → drops unknown provider fields → cache/correctness breakage;
pure cJSON = weak semantics). Typed core is the source of truth + testable; a raw
subtree round-trips unknown keys on same-protocol build. Streaming is a SEPARATE IR
surface: `IR_DELTA` event stream {turn_start, block_start{kind}, block_delta{id,kind,
delta}, block_stop, turn_stop{stop_reason,usage}, error} — not the buffered structs.
Must-have fields RANKED by blast radius: (1) **tool_use/tool_result IDs + stable
ordering for PARALLEL calls** (mismatched ids route outputs to the wrong tool — also
an injection surface); (2) **cache_control per-block** (Anthropic cost/latency
ship-blocker); (3) **thinking/reasoning blocks** (Claude Code + o-series depend on
it); (4) **system-as-ordered-blocks** (not a string); (5) image/document blocks; (6)
**stop_sequences**; (7) response metadata (Responses item ids); (8) usage detail
(cached/reasoning tokens); logprobs optional. `stop_reason` = canonical enum PLUS a
`raw_stop_reason` string. tool name + arguments are OPAQUE at the trust boundary — no
normalize/trim/reorder; preserve key order, numeric precision, Unicode exactly.

**Q2 prompt-cache parity → OPTION B default + C fallback (the #1 risk, gates all rewire).**
Same frontend==backend protocol → RAW PASSTHROUGH fast-path (no IR rebuild); IR
carries the original raw bytes + typed view for observability. Decide fast-path vs
IR-path UP-FRONT by asking the stage pipeline whether it WILL mutate; if any stage
mutates → drop to IR + accept the cache miss (memory-inject already invalidates the
cached suffix — TOCTOU, so decide before, not after). Per-SECTION cache: cache_control
on messages[] survives memory-inject (touches system only); system-block cache
invalidated as expected. Stage-effect classes: no-op / append-only-after-prefix /
prefix-mutating / tool-mutating / full-rebuild. **SECURITY: authN/authZ + size limits
+ content-policy run PRE-fast-path and protocol-agnostic (raw bytes/headers, not IR)
— a passthrough that skips auth is a bypass.**

**Q3 streaming → OPTION A (neutral IR-delta) as the target, same-protocol raw-SSE
passthrough as the fast-path/fallback.** (B recreates N×M in the riskiest path; C's
latency is unacceptable for Claude Code.) Golden-test streamed==buffered equivalence:
multiple blocks, interleaved text+tool_use, partial JSON args split across chunks,
parallel tool calls, usage-at-end, provider-error-mid-stream, stop_reason mapping.
SECURITY: hard caps per-event AND total bytes/turn (slow-stream DoS) enforced in the
delta model; ONE SSE-emit primitive owns CRLF/colon escaping (adapters never format
SSE themselves).

**Q4 backend decoupled → OPTION (a), CONFIRMED BY USER (2026-07-01): cross-protocol,
primary answers all.** The configured primary (e.g. codex) answers EVERY client
regardless of protocol. Frontend = the client's wire (anthropic.parse/render for
Claude Code, openai.parse/render for OpenAI clients); backend = the primary/model's
provider (Responses for codex). Claude Code → anthropic.parse → IR → responses.build
→ codex; response codes back responses.parse → IR → anthropic.render. No direct
translation; the IR is the pivot. "No translated message" = forbid DIRECT (non-IR)
shape translation, NOT cross-protocol backends. Original ruling text:
"OPTION (a): 'no translated message' = forbid DIRECT (non-IR) shape translation." IR-mediated cross-protocol
(frontend=anthropic, backend=responses) is permitted and IS the point. Frontend =
ingress path/client protocol; backend = configured primary/provider. **NEEDS USER
CONFIRMATION (blocking): does primary=codex + Claude Code mean (a) codex answers via
IR round-trip, or (b) an Anthropic client always routes to an Anthropic backend?**
Panel reads (a). Add BACKEND CAPABILITY NEGOTIATION: before building the provider
request, validate the IR against backend caps (images, tool use, parallel calls,
reasoning, cache_control, stop_sequences, streaming, schema dialect) → clear error if
unsupported, never silently drop.

**Q5 KB → immediate `aimee_ir_last_user_text(ir)` fix (the 4 preinject sites) +
golden test; DEFER the richer versioned turn_record_v1 to a fast-follow.** Curator
routes its provider calls through the backend adapter (or is explicitly declared
Anthropic-only) — no hidden protocol island. **SECURITY: the canonical turn record
separates DATA from INSTRUCTIONS — opaque typed fields; the curator re-renders
through its OWN template, never concatenates stored user text as instructions
(prompt-injection).** Define retention/redaction/scrubbing for raw JSON + KB records +
golden fixtures (no raw bodies in logs/KB/fixtures).

**Q6 sequencing → APPROVED with 3 changes.** (1) **Slice 0 includes the golden-test
harness + byte-parity capture fixture** (record an anonymized Claude Code session,
snapshot upstream bytes + emitted SSE) — without it slice 1 can't validate. (2)
**Move the KB extraction fix EARLIER** (runs every request, drops data today); **do
NOT delete the direct translators until slice 6 proves cross-protocol parity on real
traffic.** (3) **Runtime flag = CONFIG-ONLY, never request-controlled** (a per-request
toggle is an auth-bypass primitive); gate buffered-IR / streaming-IR /
backend-decoupling / same-protocol-passthrough SEPARATELY; old path is the fallback
until parity is proven on .254. Shadow-mode metrics from slice 0:
ir_parse/render_failures{frontend}, ir_rebuild_mismatch_bytes (nonzero=bug),
stage_mutation_count, cache_control preservation, tool-call-id preservation,
stop_reason mapping — detect parity drift in shadow before flipping the flag.

## Risks / open questions (superseded by the rulings above; historical)
- Streaming is the hard part: incremental IR from a provider SSE, re-emitted as the
  frontend's SSE, without buffering the whole turn (latency). Can the IR represent
  partial/delta content?
- Byte-parity with Claude Code's cached prefix: the current "parity passthrough"
  forwards the Anthropic request verbatim to an Anthropic backend to preserve
  prompt-cache. IR round-trip (parse→build) must be byte-identical for the
  Anthropic→Anthropic case or we lose cache hits. Do we keep a fast-path passthrough
  when frontend==backend protocol, or guarantee lossless round-trip?
- Tool-call / tool-result fidelity across shapes (ids, ordering, parallel calls).
- Count_tokens + error passthrough + Retry-After relay.
- Scope: this is a large rewrite; sequence to keep each slice shippable + tested.

## Close-out (2026-07-03)

The refactor's **goal is delivered**: aimee's LLM proxy is now
**backend ↔ aimee(IR) ↔ frontend with no directly-translated message on the live default path**. The
IR is the pivot for both the request PARSE (client wire → IR) and the request BUILD (IR → provider
wire) on the live `AIMEE_IR_PATH` (default-ON), across all three protocols (Anthropic / OpenAI-chat /
Responses); the adapter matrix is complete + symmetric and a three-way golden test proves the same
semantic turn converges to identical IR. The original operator bug (primary = codex serving Claude
Code's `/v1/messages`) is fixed and **live-proven on `.254`** with real streamed content + tool calls.

### Slice → PR map (proposal §Slices numbering)
- **0** IR structs + typed accessors + `aimee_ir_last_user_text` + shadow metrics + golden fixtures — #936.
- **1** Frontend adapters (anthropic/openai/responses parse + render) + cross-protocol golden — #936.
- **2** Backend adapters (build/parse for each provider) + tool-conversation split/merge inverses — #936.
- **3** Rewire the buffered ingress behind the flag in SHADOW mode; **live-validated on `.254`** (zero
  round-trip mismatches on real Anthropic traffic) — #936.
- **4** IR-delta streaming core (`aimee_ir_stream`: `openai_chunk_to_deltas` + `anthropic_delta_render`) — #936.
- **5 (partial — see below)** Delete the direct translators / IR-native core.
- **6** Backend selection decoupled from the frontend protocol (the codex↔Claude-Code fix) — #936, live-proven.
- **5-wire (this closeout)** Wire the IR-delta streaming relay into the **live** SSE path
  (`anthropic_http.c`), replacing the last live direct-translation site (`anthropic_stream_feed_openai`),
  behind a **separate default-OFF flag `AIMEE_IR_STREAM_RELAY`** (Q6: gate streaming-IR independently).
  New callback-emit variant `anthropic_delta_emit` (the live relay's sink is split `(ctx,event,data)`,
  not a frame); `anthropic_delta_render` + `anthropic_delta_emit` share one event-builder so they never
  drift (a test asserts `emit == render` byte-for-byte). Finish-safety + usage-tap included. Ships dark.

### Sanctioned follow-ups (NOT loose ends — sequenced by the proposal's own rulings)
- **Legacy-translator *deletion* (Slice 5 proper).** Q6 **explicitly gates deletion** on broad
  cross-protocol parity proven on real traffic: *"do NOT delete the direct translators until slice 6
  proves cross-protocol parity … old path is the fallback until parity is proven."* The legacy
  translators (`anthropic_messages_to_openai`, `openai_parse_responses_to_chat`,
  `anthropic_stream_feed_openai`, …) are therefore **retained as the config-gated fallback** — the
  *sanctioned rollout state*, not incompleteness. Enablement of `AIMEE_IR_STREAM_RELAY` and the eventual
  deletion are a rollout decision, data-gated on the shadow **divergence metrics**
  (`ir_rebuild_mismatch_bytes` etc., already emitted since Slice 0/3) staying clean on live traffic.
  **Update (2026-07-16): the RESPONSE-parser half of this is now DONE** — the canonical IR is the sole
  response parser on every wire and the legacy `agent_parse_response_*` translators are deleted, after
  the parity gate was met on live `.254` traffic. See
  [`docs/features/ir-only-response-parsing.md`](../../features/ir-only-response-parsing.md).
- **`turn_record_v1` + routing every KB/memory read through the IR (Slice 7).** Q5 **explicitly
  DEFERRED** the richer versioned turn record to a fast-follow; the immediate KB primitive
  (`aimee_ir_last_user_text`, typed-block extraction) is shipped. The legacy preinject extractor
  remains for query extraction (last-user, text-parts) and is not a demonstrated live data-loss on real
  requests — the roundtable ruled it not load-bearing without such a demonstration.

### Guardrail (roundtable 2026-07-03)
**No new direct translation.** Any new client protocol or provider is added as a **frontend adapter**
(wire ↔ IR) and/or a **backend adapter** (IR ↔ wire) — never as a direct client-shape→provider-shape
converter. The IR is the only pivot. New adapters must carry the golden IR-equality test against an
existing protocol; the shadow divergence metrics are the live guard that the IR path stays faithful
before any flag is enabled.

### Scope of "done"
Like the primary-as-manager closeout, "done" here = **goal delivered + mechanism shipped**, with the
legacy path retained as the *sanctioned, parity-gated* fallback and `turn_record_v1` as the Q5-deferred
fast-follow — both sequenced by the proposal's own rulings, not omissions.
