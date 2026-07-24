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
> The verified typed relay (`aimee_delta_t` typedef-open at `src/headers/aimee_ir.h:196`; struct closes at `:209`)
> is reachable **only** from the IR-enabled `/v1/messages` branch today.
> Responses, Chat, Webchat, Delegate, and Roundtable paths do NOT converge
> on that single typed relay. They will not converge until each missing
> decoder / renderer / route trace is implemented and verified. Phase 2 must
> therefore be split per handler. There is no third option.

### 1.1 Verified types (file:line) — the typed relay exists, but only one path reaches it

| Artifact | Verified at |
| --- | --- |
| `aimee_delta_t` (struct) | `src/headers/aimee_ir.h:196` (typedef-open line; struct closes at `:209`; member `text_delta` at `:204`) |
| `aimee_delta_type_t` (enum) | `src/headers/aimee_ir.h:186` (typedef-open line; enum closes at `:194`; enum members at `:188-193`) |
| Enum members actually present | `src/headers/aimee_ir.h:188-193`: `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA`, `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`, `AIMEE_DELTA_ERROR` |
| `aimee_block_type_t` (enum) | `src/headers/aimee_ir.h:48`; members `AIMEE_BLK_TEXT`, `AIMEE_BLK_TOOL_USE`, `AIMEE_BLK_TOOL_RESULT`, `AIMEE_BLK_IMAGE`, `AIMEE_BLK_DOCUMENT`, `AIMEE_BLK_THINKING`, `AIMEE_BLK_UNKNOWN` (:50-57) |
| `aimee_wire_t` (enum) | `src/headers/aimee_ir.h:36`; members `AIMEE_WIRE_UNKNOWN`, `AIMEE_WIRE_ANTHROPIC`, `AIMEE_WIRE_OPENAI_CHAT`, `AIMEE_WIRE_RESPONSES` (:38-42) |
| `aimee_sse_emit_fn` sink typedef | `src/headers/aimee_ir_stream.h:82` (signature-compatible with `server_http_sse_event_emit`) |
| Backend → IR-delta: OpenAI Chat chunk decoder | `openai_chunk_to_deltas` — `src/server/aimee_ir_stream.c:42` (state type `openai_stream_state_t` opens at `src/headers/aimee_ir_stream.h:18`; decoder declared at `:31`) |
| Backend → IR-delta: AWS Bedrock ConverseStream decoder | `bedrock_converse_stream_to_deltas` — `src/server/aimee_ir_stream.c:220` (state type `converse_stream_state_t` opens at `src/headers/aimee_ir_stream.h:41`; decoder declared at `:64`) |
| Frontend ← IR-delta: Anthropic SSE emit | `anthropic_delta_emit` — `src/server/aimee_ir_stream.c:539` (state type `anthropic_stream_state_t` opens at `src/headers/aimee_ir_stream.h:68`; emitter declared at `:90`); framing helper `delta_build_events` at `src/server/aimee_ir_stream.c:402` |
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
| SSE / delta emitter (IR-delta path, default-OFF today) | `messages_stream_ir_relay` at `src/server/anthropic_http.c:973` (definition verified directly in the current source; gate and call at `:1251-1254`) (dispatcher) → routes through `openai_chunk_to_deltas` (`src/server/aimee_ir_stream.c:42`) → `anthropic_delta_emit` (`src/server/aimee_ir_stream.c:539`). Wire-up gate at `src/server/anthropic_http.c:1251` (`aimee_ir_stream_relay_enabled()`). |
| Wire event/block representation observed on this path (= canonical IR enum, this is the only path that reaches it) | `aimee_delta_t` (typedef-open at `src/headers/aimee_ir.h:196`; struct member `text_delta` at `:204`; struct closes at `:209`); enum members observed on this path: `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA` (text), `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`. Wire-level event is Anthropic SSE `content_block_delta { delta.type = "text_delta", delta.text = <bytes> }` emitted by `xlate_emit_text_delta` at `src/server/anthropic_ingress.c:557` (legacy translator) and by `anthropic_delta_emit` at `src/server/aimee_ir_stream.c:539` (IR-delta path, default-OFF today). **This is the canonical IR surface**; the divergent paths in §2.2–§2.6 emit different wire shapes (recorded on their own rows) and do NOT cross `aimee_delta_t`. |
| Existing scanner-style call site usable as precedent | `aimee_ir_shadow_observe_request` invoked at `src/server/anthropic_http.c:1074` (gated no-op by `AIMEE_IR_SHADOW`); `gw_stage_memory` (`src/modules/memory/gw_stage_memory.h:43`) — universal stage seam registered in the same request pipeline that owns `messages_stream` |

**Verdict — REACHES typed relay.** This is the only path with a verified
IR-delta consumer today; the gate at `src/server/anthropic_http.c:1251`
(`aimee_ir_stream_relay_enabled()`) toggles the relay on or off.

### 2.2 `/v1/responses` (OpenAI Responses API, client ingress — Codex)

| | |
| --- | --- |
| Route table entry | `src/server/server_http_routes.c:2076` — `{"POST", "/v1/responses", NULL, RM_EXACT, "chat.send_stream", 0, rh_responses}` |
| Buffered request handler | `rh_responses` at `src/server/server_http_routes.c:826` → `g_responses_handler` registered at `src/server/server_http.c:769` (set at `src/server/openai_chat.c:1587`) → `responses_handler` at `src/server/openai_chat.c:568` (compute-then-chunk) |
| Streaming request handler (SSE entry) | `handle_responses_stream` at `src/server/server_http.c:1280`; called from `handle_conn` at `src/server/server_http.c:2096`; producer `responses_stream_handler` at `src/server/openai_chat.c:1081` (compute-then-chunk; comment at :1070 declares "Compute-then-chunk") |
| SSE / delta emitter | `openai_format_responses_delta` at `src/server/openai_chat.c:1261`; emitted at the same line via `emit(ctx, "response.output_text.delta", dframe)`. The text is produced synchronously by the Responses-specific `agent_execute_messages` call at `src/server/openai_chat.c:1181-1183`, then sliced into 80-char segments (loop at `src/server/openai_chat.c:1254-1262`). **No incremental provider-SSE → client-SSE translator is wired on this path** — the comment at `src/server/openai_chat.c:1070` is explicit: compute-then-chunk, no relay. |
| Reaches typed relay? | **NO.** There is no `openai_responses_chunk_to_deltas` decoder in `src/server/aimee_ir_stream.c` (only `openai_chunk_to_deltas` at `:42` and `bedrock_converse_stream_to_deltas` at `:220`). There is no Responses-shape frontend emitter parallel to `anthropic_delta_emit`. The path produces `response.output_text.delta` directly via `openai_format_responses_delta` and never crosses an `aimee_delta_t` boundary. |
| Wire event/block representation observed on this path (NOT the canonical IR enums) | SSE event: `response.output_text.delta` (the OpenAI Responses wire delta type), produced at `src/server/openai_chat.c:1261` via `emit(ctx, "response.output_text.delta", dframe)`. The payload carries a `delta` JSON object with `text` set to the 80-char-segment slice (slicing loop at `src/server/openai_chat.c:1254-1262`); the SSE frame is shaped as `event: response.output_text.delta\ndata: {"delta":{"text":"..."},"item_id":"..."}\n\n` (per `OPENAI_STREAM_CHUNK` and `openai_format_responses_delta`). **This wire event is NOT a member of `aimee_delta_type_t`** — the canonical IR enum (members at `src/headers/aimee_ir.h:188-193`) is `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA`, `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`, `AIMEE_DELTA_ERROR`. None of those values appears on this wire today; the only path that emits them is the IR-enabled `/v1/messages` branch. |
| Existing scanner-style call site | `aimee_ir_responses_to_chat` invoked at `src/server/openai_chat.c:1098` (gated by `aimee_ir_path_enabled()` — `:c:1097`); fallback legacy translator `openai_parse_responses_to_chat` at the same site. Both feed the Responses-specific `agent_execute_messages` seam at `src/server/openai_chat.c:1181-1183`; they do not call the Chat handler’s `agent_dispatch_one` site. **This is a request-side IR bridge, not a delta-stream bridge.** |

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
| Wire event/block representation observed on this path (NOT the canonical IR enums) | Chat wire field: `choices[0].delta.content` (assigned at `src/server/openai_chat.c:701` inside `openai_format_chat_chunk`); the SSE frame name is not an explicit event label (chat-completions does not use named SSE events) but a `data: {...}\n\n` frame whose JSON object carries `choices[0].delta.content = "<text>"` and `choices[0].finish_reason`. Chunking constant `OPENAI_STREAM_CHUNK` is defined at `src/server/openai_chat.c:687`; the slicing loop is at `src/server/openai_chat.c:775-783`. The terminal non-streaming frame carries `choices[0].finish_reason = "stop"` and an empty `delta.content`. **None of the canonical `aimee_delta_type_t` values (verified at `src/headers/aimee_ir.h:188-193`) is emitted on this wire today**; the chat-completions wire is `choices[0].delta.content` slices, not the IR-delta enum. |
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
| **Verified upstream producer (the actual text_delta source)** | `live_mirror_locked(compute_ctx_t *cctx, const char *event, const char *value)` at `src/posix/server_compute.c:158` — receives `("text", delta)` events from every primary chat worker and upserts the accumulated `cctx->live_text` into the db1 `webchat_live` row via `db1_webchat_live_set(...)` (call at `:168` turn_start, `:192` text growth, `:198` turn_end, `:203` error). The producer is **shared** with every chat worker — not divergent. |
| Verified upstream callers of `live_mirror_locked` | `stream_event(compute_ctx_t *cctx, ...)` at `src/posix/server_compute.c:214` (the unified stream sink, with `live_mirror_locked(cctx, event, value)` called at `:219`); `stream_event` is itself driven by: `codex_stream_event_cb` (`:394`), `chat_cli_stream_cb` (`:733`), the synchronous text branches (`:829` post-drift notice, `:921` buffered-completion, `:1037` compact-prompt, `:1117` compact-confirmation, `:1190` retry message), and tool-event hook `chat_tool_event_cb` (`:714`) |
| **Browser-facing surface** | `rh_chat_live` at `src/server/server_http_routes.c:1626` (F-CITE-004 closure: function definition at `:1626`; preceding comment block describing the poll contract at `:1616-1625`; the symbol-index `find_symbol` reports `:1395` for this function — this is a stale index entry, the live source line is the authoritative reference per "code outranks your repro"); calls `db1_webchat_live_get(sid, since, &turn_id, &text, &status, &rev)` at `:1645` and returns `{changed, rev, turn_id, text, status}` at `:1649-1656` when `rev > since_rev`. The matching `/v1/chat/live` route table entry is at `src/server/server_http_routes.c:2098`. |
| Streaming request handler | **None on `/v1/chat/live`** — this endpoint is a fixed-timer POLL surface. Comment at `:1616-1625` describes it as "the browser's fixed-timer poll for the live turn (db1 webchat_live mirror), replacing client-side SSE reconciliation". The browser SSE channel is a SEPARATE `/events` stream, not `/v1/chat/live`. |
| SSE / delta emitter | N/A on `/v1/chat/live`. The text-delta emitter for webchat is the upstream `live_mirror_locked` at `src/posix/server_compute.c:158` (mirroring accumulated text into db1). Browser-side SSE is delivered out-of-band via the presence-event ring (`presence_emit_turn_delta`) published from `ring_publish_event_locked` at `src/posix/server_compute.c:144` (per `src/posix/server_compute.c:204-208`). |
| Persisted-turn table | `webchat_live(session_id, turn_id, rev, text, status, updated_at)` — schema at `src/db1/schema.sql:34`; writer `db1_webchat_live_set` at `src/db1/webchat_live.c:10`; reader `db1_webchat_live_get` at `src/db1/webchat_live.c:51`. One row per session; `rev` increments on every write so the poller can tell the row advanced without diffing the text (`src/db1/webchat_live.c:18-20`). |
| Reaches typed relay? | **NO** on `/v1/chat/live` (it is a downstream consumer). The shared upstream producer (`stream_event` at `src/posix/server_compute.c:214` → `live_mirror_locked` at `:158`) does NOT cross `aimee_delta_t` either: it operates on `(event, key, value)` string tuples, not the typed delta struct. |
| Wire event/block representation observed on this path (NOT the canonical IR enums) | Browser-facing surface: db1 row in the `webchat_live` table — schema `webchat_live(session_id, turn_id, rev, text, status, updated_at)` at `src/db1/schema.sql:34`; the poller returns `{changed, rev, turn_id, text, status}` from `rh_chat_live` at `src/server/server_http_routes.c:1649-1656` when `rev > since_rev`. The browser SSE channel is delivered out-of-band via the presence-event ring; the canonical event is `presence_emit_turn_delta` published from `ring_publish_event_locked` at `src/posix/server_compute.c:144`. Upstream text-delta carrier is the `(event="text", key="content", value=<delta>)` string-tuple fed into `stream_event` at `src/posix/server_compute.c:214` and then `live_mirror_locked` at `:158`. **None of this is a `aimee_delta_type_t` value** — the carrier is a `(event, key, value)` C-string triple, and the browser-side wire is a JSON envelope with `text`, `turn_id`, `rev`, `status`. The canonical IR enum (`src/headers/aimee_ir.h:188-193`) does not appear on this path. |
| Existing scanner-style call site | The same `aimee_ir_shadow_observe_request` precedent at `src/server/anthropic_http.c:1074` is the model — observe via a parallel no-op seam that toggles on the same config gate (see config namespace decision in `collapse_anchors.md`). The structural precedent for a "tap, don't mutate" mirror is `live_mirror_locked` itself at `src/posix/server_compute.c:158` (writes to db1 on growth, never affects the served request). |

**Verdict — DIVERGES; Phase 2.2 prerequisite.** Webchat `/v1/chat/live`
is a downstream POLL consumer, not a producer. The verified producer is
the shared `stream_event` → `live_mirror_locked` chain at
`src/posix/server_compute.c:214/158`, which feeds the db1 row read by
`rh_chat_live`. The collapse scanner must tap `stream_event`
(`src/posix/server_compute.c:214`) — that one tap covers every primary
chat worker AND every delegate (because `delegate_worker` drives
`chat_stream_worker_agent`, which calls `stream_event`). No tap is
required on `/v1/chat/live` itself. The producer is **shared**, not
divergent — divergence with the `/v1/messages` path is only in the
non-existence of an IR-delta adapter on top of `stream_event` today.

### 2.5 Delegate relay (Live delegate)

| | |
| --- | --- |
| WFE block driver | `wfe_live_delegate_run(const char *workdir, const char *role, const char *prompt, const char *artifact_path, char out_commit_sha[64], char *err, size_t errlen)` at `src/server/wfe_live_delegate.c:103`. Registered as the live delegate via `wfe_set_delegate_provider(&WFE_LIVE_DELEGATE)` at `src/server/wfe_live_delegate.c:502` (the static `WFE_LIVE_DELEGATE` struct is at `:227`). |
| Block-engine seam | `wfe_set_delegate_provider(const wfe_delegate_provider_t *p)` declared at `src/modules/workflows/wfe_blocks.h:95`; static state at `src/modules/workflows/wfe_blocks.c:446` (`g_delegate`); setter at `src/modules/workflows/wfe_blocks.c:448`. |
| Coord enqueue (WFE → dispatch) | `db1_coord_job_create(WFE_COORD_PLAN_ID, 1)` at `src/server/wfe_live_delegate.c:131`; `db1_coord_job_add_task(job_id, 0, "[]", delegate_role, prompt, workdir, persona)` at `:137`; the live delegate waits for completion via `wfe_coord_task_wait` (see the loop at `:63-86`, returning the final result into `result` at `:146-148`). |
| Streaming request handler | **None — delegate relay is not an HTTP route.** The async dispatcher (`server_coord_dispatcher.c`) claims tasks and submits them via `server_compute_dispatch_coord_task` at `src/server/server_coord_dispatcher.c:122` (calls `gw_orch_delegates_run` at `:137`); `coord_spawn_delegate` (`src/server/server_coord_dispatcher.c:60-100`) builds a `compute_ctx_t` and calls `delegate_spawn_ondemand(cctx)` at `:99`, which spawns a detached thread that runs `delegate_worker(cctx)` (`src/server/server_delegate_ondemand.c:79`). |
| **Verified delegate worker → primary chat stream sink** | `delegate_worker(cctx)` at `src/server/server_compute.c:668` resolves the request body, picks a provider, and (for non-primary-session providers) ends up at `chat_stream_worker_agent(cctx, message, cwd, aimee_sid, provider, model_override, &cfg)` at `src/posix/server_compute.c:1293`. That worker invokes `stream_event(cctx, "text", "content", ...)` at `src/posix/server_compute.c:738` (the tmux CLI incremental path `chat_cli_stream_cb` → `stream_event(c->cctx, "text", "content", delta)` at `:738`), plus the synchronous full-text emit at `:921/1037/1117/1190`, plus tool events at `:714`. All of those calls converge on `stream_event(cctx, event, key, value)` at `src/posix/server_compute.c:214`. |
| SSE / delta emitter | The delegate's text-delta path is the **same** `stream_event` → `live_mirror_locked` chain that webchat uses (`src/posix/server_compute.c:214` → `:158`). The delegate worker also publishes its final result back to db1 via `db1_coord_task_set_result` (consumer side: `wfe_coord_task_wait` at `src/server/wfe_live_delegate.c:147`), which is **non-streaming text**. |
| Reaches typed relay? | **NO** — and the lack-of-convergence is now fully resolved. The delegate relay's incremental text reaches `stream_event` (a string-tuple sink), then `live_mirror_locked` (db1 mirror), then `presence_emit_turn_delta` (ring). It does NOT cross `aimee_delta_t`. The final answer is delivered to WFE as a string via `db1_coord_task_set_result`, not as an `aimee_delta_t[]` stream. (There is no `openai_responses_chunk_to_deltas`-style decoder wired on the `agent_dispatch_one` → `chat_stream_worker_agent` → `stream_event` path, and no `aimee_sse_emit_fn` sink is registered on the on-demand delegate thread.) |
| Wire event/block representation observed on this path (NOT the canonical IR enums) | Carriers: (a) the tmux CLI incremental path uses `stream_event(c->cctx, "text", "content", delta)` at `src/posix/server_compute.c:738` (a `(event="text", key="content", value=<delta>)` string-tuple); (b) synchronous text emits at `src/posix/server_compute.c:921` (buffered completion), `:1037` (compact prompt), `:1117` (compact confirmation), `:1190` (retry message); (c) tool-event hook `chat_tool_event_cb` at `src/posix/server_compute.c:714`; (d) browser-side, the same `presence_emit_turn_delta` ring event from §2.4; (e) terminal hand-back to WFE via `db1_coord_task_set_result` consumed by `wfe_coord_task_wait` at `src/server/wfe_live_delegate.c:147` (a single text blob in `db1`, not an SSE stream). **None of these carriers is a `aimee_delta_type_t` value** — there is no `AIMEE_DELTA_TURN_START` / `AIMEE_DELTA_BLOCK_DELTA` / `AIMEE_DELTA_TURN_STOP` emission on the delegate path. The only place those canonical IR enums are emitted today is the IR-enabled `/v1/messages` branch (see §2.1). |
| Existing scanner-style call site | The structural precedent is `aimee_ir_shadow_observe_request` (`src/server/aimee_ir_shadow.c:208`, called at `src/server/anthropic_http.c:1074` — gated by `AIMEE_IR_SHADOW`, default-OFF). The behavioral precedent is `live_mirror_locked` itself at `src/posix/server_compute.c:158` (writes to db1 on growth, never affects the served request, toggled by `presence_session[0]`). `gw_stage_memory` (`src/modules/memory/gw_stage_memory.h:43`) and `gw_stage_router` (`src/posix/server_compute.c:1245`) sit upstream on the request pipeline, not the delta stream. |

**Verdict — DIVERGES; Phase 2.3 prerequisite.** The delegate relay's
**incremental stream path converges with webchat on
`stream_event`** (`src/posix/server_compute.c:214`), so a single collapse
tap at `stream_event` covers primary chat workers, the tmux CLI stream,
and every on-demand delegate (because every delegate funnels through
`delegate_worker` → `chat_stream_worker_agent` → `stream_event`).
Convergence stops at `stream_event`'s string-tuple surface; the
downstream `live_mirror_locked` mirror writes to db1 (not
`aimee_delta_t`), and the final delegate result is a string handed back
to WFE via `wfe_coord_task_wait` at `src/server/wfe_live_delegate.c:147`.
Phase 2.3 must therefore wire an IR-delta adapter on top of
`stream_event` (parallel to `openai_chunk_to_deltas` in
`src/server/aimee_ir_stream.c:42`) if the collapse scanner is to see
typed deltas on the delegate path; otherwise the scanner records its
counters from `stream_event` directly.

### 2.6 Roundtable relay

| | |
| --- | --- |
| Proxy entry | `handle_roundtable_review_proxy` at `src/server/wfe_roundtable_proxy.c:15`; `wfe_roundtable_proxy` dispatcher at `src/server/wfe_roundtable_proxy.c:22` (and again at `:223`); HTTP route entry at `src/server/server_http_routes.c:2070` — `{"POST", "/v1/roundtable/review", NULL, RM_EXACT, "roundtable.review", 0, rh_dispatch_op_async}` |
| Streaming request handler | `roundtable.review` is dispatched via `rh_dispatch_op_async` (`src/server/server_http_routes.c:2070`); it gathers panel verdicts and returns a non-streaming JSON envelope. There is no incremental SSE on this surface today. |
| SSE / delta emitter | N/A. The roundtable returns discrete JSON panel verdicts. |
| Reaches typed relay? | **NO, and the surface is non-streaming.** The IR-delta struct is not a target for this path. |
| Wire event/block representation observed on this path (NOT the canonical IR enums) | Wire shape: a non-streaming JSON envelope returned from `roundtable.review` (dispatched via `rh_dispatch_op_async` at `src/server/server_http_routes.c:2070`); proxy entry `handle_roundtable_review_proxy` at `src/server/wfe_roundtable_proxy.c:15`; dispatcher `wfe_roundtable_proxy` at `src/server/wfe_roundtable_proxy.c:22/223`. There is no SSE on this surface today — the response is a discrete JSON panel-verdict envelope, not a stream of `aimee_delta_t` values. **No `aimee_delta_type_t` member (verified at `src/headers/aimee_ir.h:188-193`) is emitted on this path**; the IR-enum is a streaming-relay concept, and this surface is not a stream. The conversational model calls that drive the roundtable are covered by the §2.1 / §2.2 / §2.3 taps — those do emit canonical IR enums (or their wire-shape analogues), but the panel-verdict aggregation itself does not. |
| Existing scanner-style call site | The roundtable-side "scanner" is the per-panel verdict-stage registry (workflow layer). The stage-registry pattern (`aimee_ir_transform_fn` registry — see `src/headers/aimee_ir.h:288` and the docstring at :294-300) is the precedent for collapsing rule-side modules without touching the model-call surface. |

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
`src/headers/aimee_ir.h:186-209`: one enum (`aimee_delta_type_t`), one
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
| Consumer (the `aimee_delta_t.text_delta` field carries the payload) | `/v1/messages` | `src/headers/aimee_ir.h:204` — member declaration `const char *text_delta;  /* BLOCK_DELTA for text/thinking */` (field carries the bytes; lifetime comment at `src/headers/aimee_ir_stream.h:29` says "BORROW into the parsed chunk (transient)") |
| Compute-then-chunk producer (OpenAI chat completions — `text_delta` analogue) | `/v1/chat/completions` | `src/server/openai_chat.c:775-783` — slicing loop emits `openai_format_chat_chunk` frames; chat wire field is `choices[0].delta.content` (assigned at `:701`) |
| Compute-then-chunk producer (Responses wire) | `/v1/responses` | `src/server/openai_chat.c:1261` — `openai_format_responses_delta(item_id, seg, dframe, sizeof(dframe))`; emitted at the same line via `emit(ctx, "response.output_text.delta", dframe)` |

---

## 4. Verified scanner precedents (collapse-style observation seam)

"Scanner" in the collapse context means: a hot-path-observe-don't-mutate
callback that the request pipeline invokes once per inbound request. The
precedents are:

| Scanner | Where | What it does |
| --- | --- | --- |
| `aimee_ir_shadow_observe_request` | declared at `src/headers/aimee_ir_shadow.h:17` (file-comment header at `:1-4`; function-purpose comment at `:13`); called at `src/server/anthropic_http.c:1074` (SSE stream entry) — gated by `AIMEE_IR_SHADOW` (default-OFF) | Parses the inbound request into the IR, rebuilds it, compares bytes; never affects the turn |
| `aimee_ir_shadow_compare_bodies` | `src/headers/aimee_ir_shadow.h:32` | Compares the legacy-translated provider body to the IR-built body for the SAME inbound request; counts mismatches |
| `aimee_ir_shadow_compare_response` | `src/headers/aimee_ir_shadow.h:50` | Compares IR-parsed provider response to legacy-parsed; same no-op contract |
| `ingress_preinject_query_from_messages` | `src/server/ingress_preinject.c:409`; called from the route handlers in §2.1 | Walks inbound Anthropic `messages[]` and extracts the LAST user-role content; precedent for a per-request shape-agnostic query extractor (the IR-equivalent `aimee_ir_last_user_text` lives in `src/headers/aimee_ir.h:280`) |
| `gw_stage_memory` | `src/modules/memory/gw_stage_memory.h:43`; called per `/v1/messages` and `/v1/chat/completions` via `gw_request_t` registry | The shape-agnostic stage seam that scans/mutates the inbound request before build |

These five call sites are **the precedents** for the Phase 1+ collapse
scanner: a no-op observer that runs alongside the live relay, records
counters, and never affects the served request.

---

## 5. Acceptance check

- [x] `/v1/messages`: handler + SSE/delta emitter + verdict + enums + scanner cited.
- [x] `/v1/responses`: same fields cited. (Caveat: Responses decoder + renderer do not yet exist for the IR-delta surface — flagged in §2.2.)
- [x] `/v1/chat/completions`: same fields cited. (Caveat: compute-then-chunk + no OpenAI-Chat-shape emitter — flagged in §2.3.)
- [x] Webchat ingest: route + handler + poll contract + reason-no-tap cited, AND the verified upstream producer `live_mirror_locked` at `src/posix/server_compute.c:158` (driven by `stream_event` at `:214`) — flagged in §2.4.
- [x] Delegate relay: traced through WFE block engine → coord-job enqueue → `delegate_worker` (`src/server/server_compute.c:668`) → `chat_stream_worker_agent` (`src/posix/server_compute.c:752/1293`) → `stream_event` (`:214`); reachability to `aimee_delta_t` resolved as **NO** (string-tuple sink only) — flagged in §2.5.
- [x] Roundtable relay: proxy entry + non-streaming verdict + reason-no-tap cited — flagged in §2.6.
- [x] Single binding decision: PATHS DIVERGE; Phase 2 splits per handler (§1).
- [x] Speculative identifiers explicitly resolved: `AIMEE_DELTA_BLOCK_DELTA` and `aimee_delta_t` are confirmed real at the cited lines; they are NOT assumed to be reachable on every path.
- [x] Existing scanner precedent: §4 lists 5 verified call sites.

### F1 + F2 verification (review-finding closure)

- **F1 (delegate reachability):** the prior draft marked reachability
  "UNRESOLVED". Verified chain in §2.5 now names every link with file:line:
  `wfe_live_delegate_run` (`src/server/wfe_live_delegate.c:103`) →
  `wfe_set_delegate_provider` (`src/modules/workflows/wfe_blocks.h:95`,
  state at `src/modules/workflows/wfe_blocks.c:446-448`) →
  `db1_coord_job_create` + `db1_coord_job_add_task`
  (`src/server/wfe_live_delegate.c:131/137`) →
  `server_compute_dispatch_coord_task` (`src/server/server_coord_dispatcher.c:122`)
  → `coord_spawn_delegate` (`src/server/server_coord_dispatcher.c:60`) →
  `delegate_spawn_ondemand` (`src/server/server_delegate_ondemand.c:84`,
  thread runs `delegate_worker`) → `delegate_worker`
  (`src/server/server_compute.c:668`) → `chat_stream_worker_agent`
  (`src/posix/server_compute.c:752`, dispatched at `:1293`) →
  `stream_event` (`src/posix/server_compute.c:214`, called at `:738` for
  tmux CLI streaming and `:921/1037/1117/1190` for synchronous text).
  Reachability verdict: **NO** (the chain terminates at `stream_event`,
  which is a string-tuple sink; `aimee_delta_t` is NOT crossed).
- **F2 (webchat upstream producer):** the prior draft did not name the
  upstream producer. §2.4 now names `live_mirror_locked`
  (`src/posix/server_compute.c:158`), driven by `stream_event` at
  `src/posix/server_compute.c:214`, and the db1 row read by `rh_chat_live`
  (`src/db1/webchat_live.c:51`, polled at `src/server/server_http_routes.c:1645`).
  The producer is **shared** with every chat worker, not divergent.
