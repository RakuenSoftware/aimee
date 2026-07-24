# Guardrail Collapse — Path-by-Path Dispatch Recon

**Phase:** 0 — reconnaissance packet.
**Scope:** verified, citation-backed inventory of every serving / relay path the
guardrail-collapse work must tap. No speculative identifiers. Every file,
function, enum value, and struct member named below is reachable from the
indexed repository at the file:line shown.
**Status:** REVIEW-CORRECTED — Phase 1 is blocked until
`docs/guardrails/collapse_anchors.md` is merged and its workflow gate is marked satisfied.

---

## 0. Convention: paths are file-relative to this worktree.

Every citation below was verified against the worktree by reading the cited
line region. Where the README header in a file already narrates the seam
(the canonical-IR headers each open with the protocol-neutral pivot
contract), the header docstring itself is the second citation.

---

## 1. The binding relay decision (single, unambiguous)

> **BINDING DECISION — PATHS DIVERGE. Phase 2 is split per handler/relay.**
>
> The verified typed relay (`aimee_delta_t` at `src/headers/aimee_ir.h:177`)
> is reachable **only** from the IR-enabled `/v1/messages` branch today.
> Responses, Chat, Webchat, Delegate, and Roundtable paths do NOT converge
> on that single typed relay. They will not converge until each missing
> decoder / renderer / route trace is implemented and verified. Phase 2 must
> therefore be split per handler. There is no third option.

### 1.1 Verified types (file:line) — the typed relay exists, but only one path reaches it

| Artifact | Verified at |
| --- | --- |
| `aimee_delta_t` (struct) | `src/headers/aimee_ir.h:177` (definition; preceding comment at :172) |
| `aimee_delta_type_t` (enum) | `src/headers/aimee_ir.h:167` |
| Enum members actually present | `src/headers/aimee_ir.h:168-174`: `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA`, `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`, `AIMEE_DELTA_ERROR` |
| `aimee_block_type_t` (enum) | `src/headers/aimee_ir.h:48`; members `AIMEE_BLK_TEXT`, `AIMEE_BLK_TOOL_USE`, `AIMEE_BLK_TOOL_RESULT`, `AIMEE_BLK_IMAGE`, `AIMEE_BLK_DOCUMENT`, `AIMEE_BLK_THINKING`, `AIMEE_BLK_UNKNOWN` (:50-57) |
| `aimee_wire_t` (enum) | `src/headers/aimee_ir.h:36`; members `AIMEE_WIRE_UNKNOWN`, `AIMEE_WIRE_ANTHROPIC`, `AIMEE_WIRE_OPENAI_CHAT`, `AIMEE_WIRE_RESPONSES` (:38-42) |
| `aimee_sse_emit_fn` sink typedef | `src/headers/aimee_ir_stream.h:85` (signature-compatible with `server_http_sse_event_emit`) |
| Backend → IR-delta: OpenAI Chat chunk decoder | `openai_chunk_to_deltas` — `src/server/aimee_ir_stream.c:42` (state type at `src/headers/aimee_ir_stream.h:30`) |
| Backend → IR-delta: AWS Bedrock ConverseStream decoder | `bedrock_converse_stream_to_deltas` — `src/server/aimee_ir_stream.c:220` (state type at `src/headers/aimee_ir_stream.h:54`) |
| Frontend ← IR-delta: Anthropic SSE emit | `anthropic_delta_emit` — `src/server/aimee_ir_stream.c:539` (state type at `src/headers/aimee_ir_stream.h:75`); framing helper `delta_build_events` at `src/server/aimee_ir_stream.c:402` |
| Shared event-framing builder | `delta_build_events` — `src/server/aimee_ir_stream.c:402`; comment at :401 declares it "Shared by anthropic_delta_render (frames) and anthropic_delta_emit (callback) so the two never drift" |

### 1.2 Why the decision is "diverge, not converge"

The IR-enabled Messages branch is the only path that reaches the typed relay
today. The other paths have one of three obstacles:

- **No decoder on the wire-shape.** OpenAI Responses uses
  `response.output_text.delta` (`src/server/openai_chat.c:1261`); there is no
  `openai_responses_chunk_to_deltas` mirror of `openai_chunk_to_deltas` in
  `src/server/aimee_ir_stream.c`.
- **Compute-then-chunk, not SSE relay.** OpenAI Chat Completions
  (`src/server/openai_chat.c:720`, comment at :697 declares "compute-then-chunk")
  and OpenAI Responses (`src/server/openai_chat.c:1081`, comment at :1070
  declares "Compute-then-chunk") build the response synchronously, then slice
  the output into chunks for SSE. The text never crosses an SSE feed, so no
  backend decoder is wired.
- **No SSE at all.** Webchat (`/v1/chat/live` — `src/server/server_http_routes.c:2098`,
  verified POLL handler `rh_chat_live` at `:1626`) is a fixed-timer poll that
  mirrors the persisted turn; Roundtable (`src/server/wfe_roundtable_proxy.c:22`,
  `handle_roundtable_review_proxy` at `:15`) is a non-streaming panel-verdict
  proxy; Delegate (`wfe_live_delegate_run` at `src/server/wfe_live_delegate.c:103`)
  runs through the WFE block engine (`wfe_set_delegate_provider` at
  `src/modules/workflows/wfe_blocks.h:95`) and reaches the model via
  `agent_dispatch_one`, not through the request pipeline.

Phase 2 therefore must implement each missing chain and verify it before
tapping it. The Relay choke point (Decision 3 in `collapse_anchors.md`) target
is `aimee_delta_t` — but only the `/v1/messages` branch can reach it today.

### 1.3 Speculative identifiers — explicitly resolved

`AIMEE_DELTA_BLOCK_DELTA` and `aimee_delta_t` were flagged as candidates in
the prior reconnaissance question. Both **do exist** at the verified
locations above, so they are adopted **as the verified identifiers** — but
**only one path reaches them today**. They are not assumed to be reachable
on every path; §2 below proves which paths reach them and which do not.

---

## 2. Path-by-path dispatch

Every row below identifies: the route table entry, the request handler, the
SSE / delta emitter, the convergence verdict, the IR-delta enum values that
actually appear on this path, and any existing scanner-style call site that
the collapse work can use as precedent.

### 2.1 `/v1/messages` (Anthropic Messages API, client ingress)

| | |
| --- | --- |
| Route table entry | `src/server/server_http_routes.c:2081` — `{"POST", "/v1/messages", NULL, RM_EXACT, "chat.send_stream", 0, rh_messages}` |
| Buffered request handler | `rh_messages` at `src/server/server_http_routes.c:832`; producer driver `messages_buffered` at `src/server/anthropic_http.c:434` |
| Streaming request handler (SSE entry from `handle_conn`) | `handle_messages_stream` at `src/server/server_http.c:1290` (one-line wrapper); `g_messages_stream_handler` registered via `server_http_set_messages_stream_handler` at `src/server/server_http.c:817`; called from `messages_stream` at `src/server/anthropic_http.c:1071`. `handle_conn` dispatches at `src/server/server_http.c:2103`. |
| SSE / delta emitter (legacy translator — text_delta production) | `xlate_emit_text_delta` at `src/server/anthropic_ingress.c:557` → emits `content_block_delta { delta.type = "text_delta", delta.text = … }`. Called from `anthropic_stream_feed_openai` at `src/server/anthropic_ingress.c:703` (the path that walks `cJSON_GetObjectItemCaseSensitive(delta, "content")` and forwards the string into `xlate_emit_text_delta`). |
| SSE / delta emitter (IR-delta path, default-OFF today) | `messages_stream_ir_relay` at `src/server/anthropic_http.c:973` (dispatcher) → routes through `openai_chunk_to_deltas` (`src/server/aimee_ir_stream.c:42`) → `anthropic_delta_emit` (`src/server/aimee_ir_stream.c:539`). Wire-up gate at `src/server/anthropic_http.c:1251` (`aimee_ir_stream_relay_enabled()`). |
| Convergent typed-relay symbol | `aimee_delta_t` (`src/headers/aimee_ir.h:177`); enum members observed on this path: `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA` (text), `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP` |
| Existing scanner-style call site usable as precedent | `aimee_ir_shadow_observe_request` invoked at `src/server/anthropic_http.c:1075` (gated no-op by `AIMEE_IR_SHADOW`); `gw_stage_memory` (`src/modules/memory/gw_stage_memory.h:43`) — universal stage seam registered in the same request pipeline that owns `messages_stream` |

**Verdict — REACHES typed relay.** This is the only path with a verified
IR-delta consumer today; the gate at `src/server/anthropic_http.c:1251`
(`aimee_ir_stream_relay_enabled()`) toggles the relay on or off.

### 2.2 `/v1/responses` (OpenAI Responses API, client ingress — Codex)

| | |
| --- | --- |
| Route table entry | `src/server/server_http_routes.c:2076` — `{"POST", "/v1/responses", NULL, RM_EXACT, "chat.send_stream", 0, rh_responses}` |
| Buffered request handler | `rh_responses` at `src/server/server_http_routes.c:826` → `g_responses_handler` registered at `src/server/server_http.c:769` (set at `src/server/openai_chat.c:1587`) → `responses_handler` at `src/server/openai_chat.c:568` (compute-then-chunk) |
| Streaming request handler (SSE entry) | `handle_responses_stream` at `src/server/server_http.c:1280`; called from `handle_conn` at `src/server/server_http.c:2096`; producer `responses_stream_handler` at `src/server/openai_chat.c:1081` (compute-then-chunk; comment at :1070 declares "Compute-then-chunk") |
| SSE / delta emitter | `openai_format_responses_delta` at `src/server/openai_chat.c:1261`; emitted at the same line via `emit(ctx, "response.output_text.delta", dframe)`. The text is produced synchronously by `agent_dispatch_one` (call at `src/server/openai_chat.c:755`), then sliced into 80-char segments (loop at `src/server/openai_chat.c:1254-1262`). **No incremental provider-SSE → client-SSE translator is wired on this path** — the comment at `src/server/openai_chat.c:1070` is explicit: compute-then-chunk, no relay. |
| Reaches typed relay? | **NO.** There is no `openai_responses_chunk_to_deltas` decoder in `src/server/aimee_ir_stream.c` (only `openai_chunk_to_deltas` at `:42` and `bedrock_converse_stream_to_deltas` at `:220`). There is no Responses-shape frontend emitter parallel to `anthropic_delta_emit`. The path produces `response.output_text.delta` directly via `openai_format_responses_delta` and never crosses an `aimee_delta_t` boundary. |
| Existing scanner-style call site | `aimee_ir_responses_to_chat` invoked at `src/server/openai_chat.c:1098` (gated by `aimee_ir_path_enabled()` — `:c:1097`); fallback legacy translator `openai_parse_responses_to_chat` at the same site. Both feed the same `agent_dispatch_one` seam that `chat_stream_handler` (`src/server/openai_chat.c:755`) feeds. **This is a request-side IR bridge, not a delta-stream bridge.** |

**Verdict — DIVERGES; Phase 2.0 prerequisite.** Phase 2.0 must add an
`openai_responses_chunk_to_deltas` backend decoder (in
`src/server/aimee_ir_stream.c`) AND a Responses-shape frontend renderer
paired to the Responses wire (the wire emits `response.output_text.delta`,
not Anthropic SSE). The convergence point (`aimee_delta_t`) is the target
but not the current state.

### 2.3 `/v1/chat/completions` (OpenAI Chat Completions, delegate / external ingress)

| | |
| --- | --- |
| Route table entry | `src/server/server_http_routes.c:2074` — `{"POST", "/v1/chat/completions", NULL, RM_EXACT, "chat.send_stream", 0, rh_chat}` |
| Buffered request handler | `rh_chat` at `src/server/server_http_routes.c:814` → `g_chat_handler` registered at `src/server/server_http.c:766` (set at `src/server/openai_chat.c:1580`) → `chat_completions_handler` at `src/server/openai_chat.c:300` |
| Streaming request handler (SSE entry) | `handle_stream` dispatch at `src/server/server_http.c:2082` (`g_chat_stream_handler` registered at `src/server/server_http.c:784`); producer `chat_stream_handler` at `src/server/openai_chat.c:720`. Comment at `src/server/openai_chat.c:697` declares "compute-then-chunk". |
| SSE / delta emitter | `emit_chunk` at `src/server/openai_chat.c:693`; called inside `chat_stream_handler` at `:761` and fed chunks via `openai_format_chat_chunk` (called at `:707`). The chunked emission iterates `result.response` in 80-char slices — see `OPENAI_STREAM_CHUNK` constant at `src/server/openai_chat.c:687` and the slicing loop at `:775-783`. Chat wire field is `choices[0].delta.content` (assigned at `:701`). |
| Reaches typed relay? | **NO.** The path is compute-then-chunk per the comment at `:697`; the text never crosses an SSE feed. `openai_chunk_to_deltas` (`src/server/aimee_ir_stream.c:42`) consumes `chunk` SSE events arriving on the wire, but the chat-completions handler does not feed it — `chat_stream_handler` produces chunks via `agent_dispatch_one` (call at `:755`) and slices the resulting text into `openai_format_chat_chunk` frames. **No OpenAI-Chat-shape frontend emitter (parallel to `anthropic_delta_emit`) is wired either.** |
| Existing scanner-style call site | `chat_stream_handler` invokes `agent_dispatch_one` (`src/server/openai_chat.c:755`) which is the IR-transform seam when `aimee_ir_path_enabled()` is on (call into `agent_dispatch_one`'s IR branch at `src/server/aimee_ir_serve.c:18`). **Again, request-side, not delta-side.** |

**Verdict — DIVERGES; Phase 2.1 prerequisite.** Phase 2.1 must add an
OpenAI-Chat-shape frontend emitter (parallel to `anthropic_delta_emit`) and
either (a) feed the existing `openai_chunk_to_deltas` from a chunked
upstream or (b) wire a compute-then-chunk producer that emits
`aimee_delta_t[]` directly. The compute-then-chunk architecture is the
reason this path does not reach the typed relay today.

### 2.4 Webchat ingest (`/v1/chat/live`)

| | |
| --- | --- |
| Route table entry | `src/server/server_http_routes.c:2098` — `{"POST", "/v1/chat/live", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_chat_live}` |
| Buffered request handler | `rh_chat_live` at `src/server/server_http_routes.c:1626` (verified); polls the db1 row via `db1_webchat_live_get(sid, since, &turn_id, &text, &status, &rev)` at `:1645` |
| Streaming request handler | **None** — this endpoint is a fixed-timer POLL surface. Returns the latest turn (`{changed, rev, turn_id, text, status}` at `:1649-1656`) when the monotonic `rev` has advanced past `since_rev`. Comment at `:1616-1625` describes it as "the browser's fixed-timer poll for the live turn (db1 webchat_live mirror), replacing client-side SSE reconciliation" |
| SSE / delta emitter | N/A on this surface. The webchat SSE channel is a SEPARATE browser-`/events` stream, not `/v1/chat/live`. |
| Reaches typed relay? | **NO.** This is a downstream consumer of the persisted turn produced by the upstream SSE relay on `/v1/messages` (or `/v1/chat/completions`); the SSE producer is the upstream handler, not this handler. |
| Existing scanner-style call site | The same `aimee_ir_shadow_observe_request` precedent at `src/server/anthropic_http.c:1075` is the model — observe via a parallel no-op seam that toggles on the same config gate (see config namespace decision in `collapse_anchors.md`). |

**Verdict — DIVERGES; Phase 2.2 prerequisite.** Webchat does not produce
text deltas; it polls the persisted turn. The collapse tap must be placed
**upstream** of the webchat mirror (at the SSE emission point named in
§2.1 / §2.2 / §2.3), not at `/v1/chat/live`. No tap is required on this
path itself.

### 2.5 Delegate relay (Live delegate)

| | |
| --- | --- |
| Primary file | `src/server/wfe_live_delegate.c` (the `wfe_live_delegate_run` driver at `:103`) |
| Streaming request handler | **None on this file.** This driver does NOT introduce a new HTTP route. It runs through the WFE block engine via `wfe_set_delegate_provider` (declared at `src/modules/workflows/wfe_blocks.h:95`, called from `src/modules/workflows/wfe_blocks.c:448`). The block engine enqueues a coord job via `db1_coord_job_create(WFE_COORD_PLAN_ID, 1)` at `src/server/wfe_live_delegate.c:131` and waits for the delegate to complete. The actual model call is `agent_dispatch_one` invoked from elsewhere in the delegate system. |
| SSE / delta emitter | The delegate system reaches the model via `agent_dispatch_one` (`src/server/openai_chat.c:755` for chat, `:644` for buffered responses). The chat-shape path is the one used; delegate responses arrive as synchronous text, not as SSE deltas. |
| Reaches typed relay? | **UNRESOLVED.** The delegate-system → `agent_dispatch_one` chain does not reach `aimee_delta_t` today. The delegate result is read by the WFE block engine as a final string, not as a `aimee_delta_t[]` stream. |
| Existing scanner-style call site | Re-uses `gw_stage_memory` (`src/modules/memory/gw_stage_memory.h:43`) for memory pre-injection on Anthropic-shaped requests; `gw_stage_router` (referenced at `src/posix/server_compute.c:1245`) for the request-routing stage. |

**Verdict — DIVERGES; Phase 2.3 prerequisite.** The delegate path reaches
the model via `agent_dispatch_one` (the same seam Chat uses), but the
**output is consumed as a final string**, not as an `aimee_delta_t[]`
stream. A collapse tap on `/v1/chat/completions` does NOT cover delegates
because the consumption shape is different (the WFE block engine reads the
final reply, not the SSE chunks). Phase 2.3 must decide whether to tap
inside `agent_dispatch_one` (preferred) or accept that the delegate path
is out-of-scope for the collapse scanner.

### 2.6 Roundtable relay

| | |
| --- | --- |
| Proxy entry | `handle_roundtable_review_proxy` at `src/server/wfe_roundtable_proxy.c:15`; `wfe_roundtable_proxy` dispatcher at `src/server/wfe_roundtable_proxy.c:22` (and again at `:223`); HTTP route entry at `src/server/server_http_routes.c:2070` — `{"POST", "/v1/roundtable/review", NULL, RM_EXACT, "roundtable.review", 0, rh_dispatch_op_async}` |
| Streaming request handler | `roundtable.review` is dispatched via `rh_dispatch_op_async` (`src/server/server_http_routes.c:2070`); it gathers panel verdicts and returns a non-streaming JSON envelope. There is no incremental SSE on this surface today. |
| SSE / delta emitter | N/A. The roundtable returns discrete JSON panel verdicts. |
| Reaches typed relay? | **NO, and the surface is non-streaming.** The IR-delta struct is not a target for this path. |
| Existing scanner-style call site | The roundtable-side "scanner" is the per-panel verdict-stage registry (workflow layer). The stage-registry pattern (`aimee_ir_transform_fn` registry — see `src/headers/aimee_ir.h:295` and the docstring at :286-298) is the precedent for collapsing rule-side modules without touching the model-call surface. |

**Verdict — DIVERGES; Phase 2.4 prerequisite.** Roundtable is a
non-streaming panel-verdict proxy. Phase 2.4 must use its own observation
seam (likely a per-panel verdict hook) and does not converge on the typed
relay. The conversational model calls that drive the roundtable are
covered by the §2.1 / §2.2 / §2.3 taps, but the panel-verdict aggregation
is a separate concern.

### 2.7 Summary of the binding decision

The six paths do **not** all converge today. Only the IR-enabled Messages
branch reaches the typed relay surface; the divergent paths in §2.2–§2.6
require separate Phase 2 slices. The typed relay surface is defined at
`src/headers/aimee_ir.h:167-178`: one enum (`aimee_delta_type_t`), one
struct (`aimee_delta_t`). Phase 2 collapses onto `aimee_delta_t` on each
path — but each path requires its own implementation and verification
slice.

---

## 3. Verified delta-emission lines (for the actual text_delta production/consumption chain)

The collapse work must tap **producer** and **consumer** sites for
`text_delta` payloads. The verified lines are:

| Role | Path | File:Function:Line |
| --- | --- | --- |
| Producer (legacy translator, OpenAI chat chunk → Anthropic SSE `text_delta`) | `/v1/messages` | `src/server/anthropic_ingress.c:557` — `xlate_emit_text_delta` (type=`text_delta`, attaches to `delta.text`); called from `anthropic_stream_feed_openai` at `src/server/anthropic_ingress.c:703` |
| Producer (IR path: `BLOCK_DELTA` for kind=TEXT / THINKING) | `/v1/messages` | `src/server/aimee_ir_stream.c:445` — `delta_build_events` case `AIMEE_DELTA_BLOCK_DELTA` (writes `delta.type = "text_delta"`, `delta.text = d->text_delta`) |
| Consumer (the `aimee_delta_t.text_delta` field carries the payload) | `/v1/messages` | `src/headers/aimee_ir.h:184` — member declaration `const char *text_delta;  /* BLOCK_DELTA for text/thinking */` (field carries the bytes; lifetime comment at `src/headers/aimee_ir_stream.h:29` says "BORROW into the parsed chunk (transient)") |
| Compute-then-chunk producer (OpenAI chat completions — `text_delta` analogue) | `/v1/chat/completions` | `src/server/openai_chat.c:775-783` — slicing loop emits `openai_format_chat_chunk` frames; chat wire field is `choices[0].delta.content` (assigned at `:701`) |
| Compute-then-chunk producer (Responses wire) | `/v1/responses` | `src/server/openai_chat.c:1261` — `openai_format_responses_delta(item_id, seg, dframe, sizeof(dframe))`; emitted at the same line via `emit(ctx, "response.output_text.delta", dframe)` |

---

## 4. Verified scanner precedents (collapse-style observation seam)

"Scanner" in the collapse context means: a hot-path-observe-don't-mutate
callback that the request pipeline invokes once per inbound request. The
precedents are:

| Scanner | Where | What it does |
| --- | --- | --- |
| `aimee_ir_shadow_observe_request` | declared at `src/headers/aimee_ir_shadow.h:12`; called at `src/server/anthropic_http.c:1075` (SSE stream entry) — gated by `AIMEE_IR_SHADOW` (default-OFF) | Parses the inbound request into the IR, rebuilds it, compares bytes; never affects the turn |
| `aimee_ir_shadow_compare_bodies` | `src/headers/aimee_ir_shadow.h:33` | Compares the legacy-translated provider body to the IR-built body for the SAME inbound request; counts mismatches |
| `aimee_ir_shadow_compare_response` | `src/headers/aimee_ir_shadow.h:55` | Compares IR-parsed provider response to legacy-parsed; same no-op contract |
| `ingress_preinject_query_from_messages` | `src/server/ingress_preinject.c:409`; called from the route handlers in §2.1 | Walks inbound Anthropic `messages[]` and extracts the LAST user-role content; precedent for a per-request shape-agnostic query extractor (the IR-equivalent `aimee_ir_last_user_text` lives in `src/headers/aimee_ir.h:230`) |
| `gw_stage_memory` | `src/modules/memory/gw_stage_memory.h:43`; called per `/v1/messages` and `/v1/chat/completions` via `gw_request_t` registry | The shape-agnostic stage seam that scans/mutates the inbound request before build |

These five call sites are **the precedents** for the Phase 1+ collapse
scanner: a no-op observer that runs alongside the live relay, records
counters, and never affects the served request.

---

## 5. Acceptance check

- [x] `/v1/messages`: handler + SSE/delta emitter + verdict + enums + scanner cited.
- [x] `/v1/responses`: same fields cited. (Caveat: Responses decoder + renderer do not yet exist for the IR-delta surface — flagged in §2.2.)
- [x] `/v1/chat/completions`: same fields cited. (Caveat: compute-then-chunk + no OpenAI-Chat-shape emitter — flagged in §2.3.)
- [x] Webchat ingest: route + handler + poll contract + reason-no-tap cited. (No SSE relay on this surface — flagged in §2.4.)
- [x] Delegate relay: traced to WFE block engine → `agent_dispatch_one`; reachability to `aimee_delta_t` UNRESOLVED — flagged in §2.5.
- [x] Roundtable relay: proxy entry + non-streaming verdict + reason-no-tap cited — flagged in §2.6.
- [x] Single binding decision: PATHS DIVERGE; Phase 2 splits per handler (§1).
- [x] Speculative identifiers explicitly resolved: `AIMEE_DELTA_BLOCK_DELTA` and `aimee_delta_t` are confirmed real at the cited lines; they are NOT assumed to be reachable on every path.
- [x] Existing scanner precedent: §4 lists 5 verified call sites.
