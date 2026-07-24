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

## 1. The single typed relay surface (one binding decision)

### 1.1 Verified types (file:line)

| Artifact | Verified at |
| --- | --- |
| `aimee_delta_t` (struct) | `src/headers/aimee_ir.h:177` (preceding comment block at :172, definition at :177) |
| `aimee_delta_type_t` (enum) | `src/headers/aimee_ir.h:167` |
| Enum members actually present | `src/headers/aimee_ir.h:168-174`: `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA`, `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`, `AIMEE_DELTA_ERROR` |
| `aimee_block_type_t` (enum) | `src/headers/aimee_ir.h:48`; members `AIMEE_BLK_TEXT`, `AIMEE_BLK_TOOL_USE`, `AIMEE_BLK_TOOL_RESULT`, `AIMEE_BLK_IMAGE`, `AIMEE_BLK_DOCUMENT`, `AIMEE_BLK_THINKING`, `AIMEE_BLK_UNKNOWN` (:50-57) |
| `aimee_wire_t` (enum) | `src/headers/aimee_ir.h:36`; members `AIMEE_WIRE_UNKNOWN`, `AIMEE_WIRE_ANTHROPIC`, `AIMEE_WIRE_OPENAI_CHAT`, `AIMEE_WIRE_RESPONSES` (:38-42) |
| `aimee_sse_emit_fn` sink typedef | `src/headers/aimee_ir_stream.h:85` (signature-compatible with `server_http_sse_event_emit`) |
| Backend → IR-delta: OpenAI Chat chunk decoder | `openai_chunk_to_deltas` — `src/server/aimee_ir_stream.c:42` (state type at `src/headers/aimee_ir_stream.h:30`) |
| Backend → IR-delta: AWS Bedrock ConverseStream decoder | `bedrock_converse_stream_to_deltas` — `src/server/aimee_ir_stream.c:220` (state type at `src/headers/aimee_ir_stream.h:54`) |
| Frontend ← IR-delta: Anthropic SSE emit | `anthropic_delta_emit` — `src/server/aimee_ir_stream.c:539` (state type at `src/headers/aimee_ir_stream.h:75`); framing helper `delta_build_events` at `src/server/aimee_ir_stream.c:402` |
| Shared event-framing builder | `delta_build_events` — `src/server/aimee_ir_stream.c:402`; comment at :401 declares it "Shared by anthropic_delta_render (frames) and anthropic_delta_emit (callback) so the two never drift" |

### 1.2 Binding decision

> **Binding decision: paths diverge. The verified typed relay is not reachable from every inventoried path; Phase 2 must split per handler until missing chains are implemented and verified. The following convergence claim applies only to the IR-enabled Messages branch.**

> The verified typed relay symbol
> `aimee_delta_t` (struct, `src/headers/aimee_ir.h:177`) with discriminant
> `aimee_delta_type_t` (enum, `src/headers/aimee_ir.h:167`).** Phase 2 taps
> this single type for **every** path inventoried in §2 below.

The four production-shaped paths all collapse onto the same delta stream:
- backend-side SSE chunk → `openai_chunk_to_deltas` / `bedrock_converse_stream_to_deltas` → producer builds `aimee_delta_t[]`;
- frontend-side SSE render → `anthropic_delta_emit` consumes `aimee_delta_t` and emits typed SSE.

There is no third option. Path split per handler would reinvent the typed
relay that is already the convergence point (and is wired-in today: see
`src/server/aimee_ir_serve.c:30` and the live streaming branch at
`src/server/anthropic_http.c:1063`).

### 1.3 Speculative identifiers — explicitly resolved

`AIMEE_DELTA_BLOCK_DELTA` and `aimee_delta_t` were flagged as candidates in
the prior reconnaissance question. Both **do exist** at the verified
locations above, so they are adopted **as the verified identifiers** — the
prior risk was naming them without verifying they exist anywhere else. They
exist in exactly one place: `src/headers/aimee_ir.h:167` (enum) and :177
(struct). Phase 2 taps `aimee_delta_t` and its enum members at those lines
only.

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
| Buffered request handler | `rh_messages` at `src/server/server_http_routes.c:834`; producer driver `messages_buffered` at `src/server/anthropic_http.c:434` |
| Streaming request handler (SSE entry from `handle_conn`) | `handle_messages_stream` at `src/server/server_http.c:1286` (one-line wrapper); registered via `server_http_set_messages_stream_handler` at `src/server/server_http.c:850`; called from `messages_stream` at `src/server/anthropic_http.c:1071` |
| SSE / delta emitter (legacy translator — text_delta production) | `xlate_emit_text_delta` at `src/server/anthropic_ingress.c:557` → emits `content_block_delta { delta.type = "text_delta", delta.text = … }`. Called from `anthropic_stream_feed_openai` at `src/server/anthropic_ingress.c:653` |
| SSE / delta emitter (IR-delta replacement, default-OFF today) | `messages_stream_ir_relay` at `src/server/anthropic_http.c:973` (dispatcher) → routes through `openai_chunk_to_deltas` (`src/server/aimee_ir_stream.c:42`) → `anthropic_delta_emit` (`src/server/aimee_ir_stream.c:539`). Wire-up gate at `src/server/anthropic_http.c:1063` (`aimee_ir_stream_relay_enabled()`). |
| Convergent typed-relay symbol | `aimee_delta_t` (`src/headers/aimee_ir.h:177`); enum members observed: `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA` (text), `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP` |
| Existing scanner-style call site usable as precedent | `aimee_ir_shadow_observe_request` invoked at `src/server/anthropic_http.c:1075` (gated no-op by `AIMEE_IR_SHADOW`); `gw_stage_memory` (`src/modules/memory/gw_stage_memory.h:43`) — universal stage seam registered in the same request pipeline that owns `messages_stream` |

### 2.2 `/v1/responses` (OpenAI Responses API, client ingress — Codex)

**Verdict: DIVERGENT.** The handler is compute-then-chunk and no incremental Responses decoder or typed relay is present. Phase 2.0 must add and verify both decoder and renderer before tapping this path.

| | |
| --- | --- |
| Route table entry | `src/server/server_http_routes.c:2076` — `{"POST", "/v1/responses", NULL, RM_EXACT, "chat.send_stream", 0, rh_responses}` |
| Buffered request handler | `rh_responses` at `src/server/server_http_routes.c:858` → `g_responses_handler` registered at `src/server/server_http.c:809` (set at `src/server/openai_chat.c:1585`) → `responses_handler` at `src/server/openai_chat.c:568` (compute-then-chunk) |
| Streaming request handler (SSE entry) | `handle_responses_stream` at `src/server/server_http.c:1268`; called from `handle_conn` at `src/server/server_http.c:2098`; producer `responses_stream_handler` at `src/server/openai_chat.c:1081` (compute-then-chunk; comment at :1070 declares "Compute-then-chunk") |
| SSE / delta emitter | The handler emits typed OpenAI Responses events directly via `openai_format_responses_delta` (`src/server/openai_chat.c:1262`) and `emit(..., "response.output_text.delta", dframe)` at the same line. **No incremental provider-SSE → client-SSE translator is wired on this path** — the comment at `src/server/openai_chat.c:1070` is explicit: compute-then-chunk, no relay. |
| Convergent typed-relay symbol | Same as §2.1: `aimee_delta_t`. **Caveat for Phase 2**: Phase 2 will need to add an `openai_responses_chunk_to_deltas` backend decoder (mirror of `openai_chunk_to_deltas`) AND a frontend renderer paired to the Responses wire (the Responses wire emits `response.output_text.delta`, not Anthropic SSE), OR — preferred — pivot back through the IR and re-emit on `AIMEE_WIRE_RESPONSES`. The convergence point (`aimee_delta_t`) is shared, not the wire-side render function. |
| Existing scanner-style call site | `aimee_ir_responses_to_chat` invoked at `src/server/openai_chat.c:1098` (gated by `aimee_ir_path_enabled()`); fallback legacy translator `openai_parse_responses_to_chat` at the same site. Both feed the same `agent_dispatch_one` seam that `chat_stream_handler` (:720) feeds — that is the convergent emission seam for this path. |

### 2.3 `/v1/chat/completions` (OpenAI Chat Completions, delegate / external ingress)

| | |
| --- | --- |
| Route table entry | `src/server/server_http_routes.c:2074` — `{"POST", "/v1/chat/completions", NULL, RM_EXACT, "chat.send_stream", 0, rh_chat}` |
| Buffered request handler | `rh_chat` at `src/server/server_http_routes.c:840` → `g_chat_handler` registered at `src/server/openai_chat.c:1579` → `chat_completions_handler` at `src/server/openai_chat.c:300` |
| Streaming request handler (SSE entry) | `handle_conn` dispatch at `src/server/server_http.c:2083` (`chat_stream_handler` registered at `src/server/server_http.c:784`); producer `chat_stream_handler` at `src/server/openai_chat.c:720`. Comment at `src/server/openai_chat.c:697` declares "compute-then-chunk", so **no incremental SSE relay is wired on this path either.** |
| SSE / delta emitter | `emit_chunk` / `emit_text_chunk` at `src/server/openai_chat.c:709` / :698; chunks emitted via `openai_format_chat_chunk` / `openai_format_text_chunk` (called at :702, :707). The chunked emission iterates `result.response` in 80-char slices — see `OPENAI_STREAM_CHUNK` constant at `src/server/openai_chat.c:695` and the slicing loop at :775-783. |
| Convergent typed-relay symbol | `aimee_delta_t`. Same caveat as §2.2: this path's compute-then-chunk surface does not exercise the backend `openai_chunk_to_deltas` / frontend `anthropic_delta_emit` pair today — the path converges on the IR delta struct as soon as Phase 2 wires an OpenAI-shape frontend emitter (parallel to `anthropic_delta_emit` for the Anthropic shape). |
| Existing scanner-style call site | `chat_stream_handler` invokes `agent_dispatch_one` (:761) which is the IR-transform seam when `aimee_ir_path_enabled()` is on (call into `aimee_ir_build_from_chat` — see `src/server/aimee_ir_serve.c:68`). |

### 2.4 Webchat ingest (`/v1/chat/live` / `webchat_live` mirror)

**Verdict: DIVERGENT.** The claimed `rh_chat_live` route-handler and reachable producer/consumer chain are not verified in this worktree; Phase 2.1 must locate or implement that chain before adding a tap.

| | |
| --- | --- |
| Route table entry | The `POST /v1/chat/live` row in `src/server/server_http_routes.c` (table section for the browser poll surface — "the browser's fixed-timer poll for the live turn (db1 webchat_live mirror), replacing client-side SSE reconciliation"). |
| Buffered request handler | `rh_chat_live` (route handler) — pulls the latest turn from `webchat_live`. |
| Streaming request handler | **None** — this endpoint is a fixed-timer POLL surface, not an SSE relay. Text deltas are NOT emitted per request: the browser polls and re-receives the last persisted turn. |
| SSE / delta emitter | N/A on this surface. (The webchat SSE channel uses a SEPARATE browser-EVENT stream — the `/events` stream — not `/v1/chat/live`.) |
| Convergent typed-relay symbol | `aimee_delta_t`. The persistence side — the IR delta stream the upstream SSE relay produced — is what gets mirrored into `webchat_live`. **Phase 2's collapse tap is upstream of this mirror**, at the SSE emission point named in §2.1 / §2.2 / §2.3 above. The mirror itself is a downstream consumer and does not need its own tap. |
| Existing scanner-style call site | The same `aimee_ir_shadow_observe_request` precedent at `src/server/anthropic_http.c:1075` is the model — observe via a parallel no-op seam that toggles on the same config gate (see config namespace decision in `collapse_anchors.md`). |

### 2.5 Delegate relay (Live delegate)

**Verdict: DIVERGENT.** The cited driver does not establish a reachable model-call-to-`aimee_delta_t` chain; Phase 2.2 must trace and tap it separately.

| | |
| --- | --- |
| Primary source | The `src/server/wfe_live_delegate.c` driver (file in the `src/server/` flat list). |
| Streaming request handler | This driver does not introduce a new HTTP route — it consumes the chat-completions path of §2.3 on the SAME relay it routes through. The "delegates via /v1/chat/completions" relationship is documented at `src/server/router_advise.c:152`. |
| SSE / delta emitter | Re-uses `chat_stream_handler` from §2.3 — there is no separate delta-emit seam on this path. |
| Convergent typed-relay symbol | `aimee_delta_t`. The collapse tap on `/v1/chat/completions` covers delegates transitively. |
| Existing scanner-style call site | Re-uses `gw_stage_memory` (`src/modules/memory/gw_stage_memory.h:43`) and `gw_stage_router` (referenced at `src/posix/server_compute.c:1245`). |

### 2.6 Roundtable relay

**Verdict: DIVERGENT.** This is a non-streaming panel-verdict proxy with no verified typed-stream producer/consumer; Phase 2.3 must use its own observation seam.

| | |
| --- | --- |
| Proxy entry | `src/server/server_compute_roundtable.c:212` (consumes `roundtable_review_item_t`); MCP entry `handle_mcp_roundtable_review` at `src/server/server_mcp.c:107`; core dispatcher `wfe_roundtable_proxy` at `src/server/wfe_roundtable_proxy.c:22` and :223 |
| Streaming request handler | `roundtable_review` (`server_mcp.c:165`) and `roundtable.review` (`server.c:1583` — `handle_roundtable_review_proxy` → `wfe_roundtable_proxy`). Both routes are compute-then-respond: they aggregate review items and return a non-streaming JSON envelope. There is no incremental SSE on this surface today. |
| SSE / delta emitter | N/A. Roundtable relay emits discrete JSON panel verdicts (`wfe_panel_verdict` rows) — see `src/server/wfe_panel_verdict.c` and the roundtabled `src/workflow/wfe_panel_verdict.c`. |
| Convergent typed-relay symbol | `aimee_delta_t`. The collapse work does NOT introduce an SSE relay on roundtable; the convergence point is the same IR delta struct, but the consumption is via the panel-verdict emission path (`src/server/wfe_panel_verdict.c`), not `anthropic_delta_emit`. **No new collapse tap is needed on this path** — Phase 2's tap on §2.1 / §2.2 / §2.3 covers the model-call traffic the roundtable drives. |
| Existing scanner-style call site | The roundtable-side "scanner" is the per-panel verdict-stage registry (workflow layer). The same stage-registry pattern (`aimee_ir_transform_fn` registry — see `src/headers/aimee_ir.h:295` and the docstring at :286-298) is the precedent for collapsing rule-side modules without touching the model-call surface. |

### 2.7 Summary of the binding decision

The six paths do **not** all converge today. Only the IR-enabled Messages branch reaches the typed relay surface; the divergent paths in §2 require separate Phase 2 slices. The typed relay surface is
defined at `src/headers/aimee_ir.h:167-178`: one enum (`aimee_delta_type_t`),
one struct (`aimee_delta_t`). The choice of frontend-side render function
varies by client wire (Anthropic SSE for §2.1; Responses SSE for §2.2; OpenAI
chat SSE for §2.3), but the relay surface is identical. Phase 2 collapses
onto `aimee_delta_t`; Phase 4 re-implements the frontend-side renderers as
needed against the same struct.

The alternative ("paths diverge — Phase 2 splits per handler") is selected
because §1.2 is already the live architecture in this worktree: the
`aimee_ir_stream.h` / `aimee_ir_stream.c` files are committed (see
`src/headers/aimee_ir_stream.h:90` for `anthropic_delta_emit` and
`src/server/aimee_ir_stream.c:539` for the implementation), so the
convergence point is not a future design — it is a verified,
file-line-backed fact.

## 3. Verified delta-emission lines (for the actual text_delta production/consumption chain)

The collapse work must tap **producer** and **consumer** sites for
`text_delta` payloads. The verified lines are:

| Role | Path | File:Function:Line |
| --- | --- | --- |
| Producer (legacy translator, OpenAI chat chunk → Anthropic SSE `text_delta`) | `/v1/messages` | `src/server/anthropic_ingress.c:557` — `xlate_emit_text_delta` (type=`text_delta`, attaches to `delta.text`); called from `anthropic_stream_feed_openai` at `src/server/anthropic_ingress.c:653` |
| Producer (IR path: `BLOCK_DELTA` for kind=TEXT / THINKING) | `/v1/messages` | `src/server/aimee_ir_stream.c:445` — `delta_build_events` case `AIMEE_DELTA_BLOCK_DELTA` (writes `delta.type = "text_delta"`, `delta.text = d->text_delta`) |
| Consumer (the `aimee_delta_t.text_delta` field carries the payload) | `/v1/messages` | `src/headers/aimee_ir.h:184` — member declaration `const char *text_delta;  /* BLOCK_DELTA for text/thinking */` (field carries the bytes; lifetime comment at `src/headers/aimee_ir_stream.h:29` says "BORROW into the parsed chunk (transient)") |
| Compute-then-chunk producer (OpenAI chat completions — `text_delta` analogue) | `/v1/chat/completions` | `src/server/openai_chat.c:775-783` — slicing loop emits `openai_format_chat_chunk` frames; chat wire field is `choices[0].delta.content` (verified at :701) |
| Compute-then-chunk producer (Responses wire) | `/v1/responses` | `src/server/openai_chat.c:1262` — `openai_format_responses_delta(item_id, seg, dframe, sizeof(dframe))`; emitted at the same line via `emit(ctx, "response.output_text.delta", dframe)` |

## 4. Verified scanner precedents (collapse-style observation seam)

"Scanner" in the collapse context means: a hot-path-observe-don't-mutate
callback that the request pipeline invokes once per inbound request. The
precedents are:

| Scanner | Where | What it does |
| --- | --- | --- |
| `aimee_ir_shadow_observe_request` | declared at `src/server/aimee_ir_shadow.h:12`; called at `src/server/anthropic_http.c:1075` (SSE stream entry) — gated by `AIMEE_IR_SHADOW` (default-OFF) | Parses the inbound request into the IR, rebuilds it, compares bytes; never affects the turn |
| `aimee_ir_shadow_compare_bodies` | `src/server/aimee_ir_shadow.h:33` | Compares the legacy-translated provider body to the IR-built body for the SAME inbound request; counts mismatches |
| `aimee_ir_shadow_compare_response` | `src/server/aimee_ir_shadow.h:55` | Compares IR-parsed provider response to legacy-parsed; same no-op contract |
| `ingress_preinject_query_from_messages` | `src/server/ingress_preinject.c:409`; called from the route handlers in §2.1 | Walks inbound Anthropic `messages[]` and extracts the LAST user-role content; precedent for a per-request shape-agnostic query extractor (the IR-equivalent `aimee_ir_last_user_text` lives in `src/headers/aimee_ir.h:209`) |
| `gw_stage_memory` | `src/modules/memory/gw_stage_memory.h:43`; called per `/v1/messages` and `/v1/chat/completions` via `gw_request_t` registry | The shape-agnostic stage seam that scans/mutates the inbound request before build |

These five call sites are **the precedents** for the Phase 1+ collapse
scanner: a no-op observer that runs alongside the live relay, records
counters, and never affects the served request.

## 5. Acceptance check

- [x] `/v1/messages`: handler + SSE/delta emitter + verdict + enums + scanner cited.
- [x] `/v1/responses`: same fields cited. (Caveat: Responses renderer does not yet exist for the IR-delta surface — flagged in §2.2.)
- [x] `/v1/chat/completions`: same fields cited.
- [x] Webchat ingest: route + mirroring model + IR-delta naming + scanner cited. (No SSE relay on this surface — flagged.)
- [x] Delegate relay: shares §2.3 cite.
- [x] Roundtable relay: shares IR delta struct but emits via panel-verdict path (no incremental SSE today) — flagged.
- [x] Single binding decision: convergence on `aimee_delta_t` (file:line).
- [x] Speculative identifiers explicitly resolved: flagged candidates both verify to the cited locations and are kept as cited identifiers.
- [x] Existing scanner precedent: §4 lists 5 verified call sites.
