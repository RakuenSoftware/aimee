# Sampling-Parameter Capability Matrix (per backend)

---

## 0a. F3 closure note (review-finding closure)

The prior matrix asserted that `top_k` was **NOT** emitted on every backend
but **WAS** emitted on Anthropic — an internally contradictory pair where
the same verified function `model_sampling_apply_anthropic` was claimed to
emit `top_k` (col 1, line `src/server/model_sampling.c:107`) and the
verified sibling `model_sampling_apply_openai` was simultaneously claimed
NOT to emit `top_k` (col 2, "no `add_int_if_missing` call") even though
both functions are three lines of symmetric code.

Verified live source (`src/server/model_sampling.c`):

- `model_sampling_apply_openai` at `:71-89` — line 86:
  `add_int_if_missing(req, "top_k", row.top_k);`
- `model_sampling_apply_anthropic` at `:91-108` — line 107:
  `add_int_if_missing(req, "top_k", row.top_k);`

Both calls are **delegate-row-gated**: `sampling_for_agent(agent, &row)`
at `:64-69` returns the per-delegate preset at `g_sampling_rows[]`
(`src/server/model_sampling.c:9-22`) and the `add_int_if_missing` is
called only when `has_row` is true (lines `:83/104`). On rows where
`top_k` is `-1` (the "do not emit" sentinel), the helper short-circuits
at `:57-62`. The wire-level shape depends on the downstream backend:

- **Anthropic cloud** wire accepts `top_k` natively — re-emitted.
- **Bedrock Converse** wire is Anthropic-shaped — re-emitted (verified
  by the same `model_sampling_apply_anthropic` call).
- **OpenAI Chat wire (cloud)** has no `top_k` field — providers may drop
  it. Local OpenAI-compatible providers (ollama / llama.cpp) accept it
  (per the index comment at `src/server/aimee_backend_openai.c`).
- **OpenAI Responses wire** is reached via `aimee_ir_responses_to_chat`
  (`src/server/openai_chat.c:1098`), which routes through the same
  OpenAI Chat builder; same wire-drop caveat.

Phase 4.0's `top_k`-emission prerequisite is therefore removed.

---



**Phase:** 0 — reconnaissance packet companion to `collapse_recon.md`.
**Scope:** explicit per-backend list of which top-level sampling knobs each
*production* delegate honours, cross-referenced against the canonical IR's
typed-sampling surface. The matrix constrains Phase 4 scope and surfaces any
missing plumbing as Phase 4.0 prerequisites.
**Verification convention:** every non-`n/a` cell carries a directly relevant
request-parser or backend-builder file:line citation. `n/a` means the wire
protocol does not have that field by design (e.g. Anthropic has no
`repetition_penalty` field).
**Status:** REVIEW-CORRECTED (F3 closed) — the prior `top_k` claims
contradicted the live backend builders (`src/server/model_sampling.c:86/107`
both emit `top_k` via `add_int_if_missing(req, "top_k", row.top_k)`); matrix
now reflects the verified plumbing. Phase 4.0 prerequisite list updated to
remove the phantom `top_k`-emission item. Other Phase 4.0 prerequisites
still listed.

---

## 0. Canonical typed-sampling surface (file:line)

The IR request struct models these sampling fields TYPED (rather than via
the `raw` sidecar), per `src/headers/aimee_ir.h`:

| IR field | Verified at | Notes |
| --- | --- | --- |
| `temperature`, `has_temperature` | `src/headers/aimee_ir.h:118-119` | `has_*` int companion is set when the client supplied it |
| `top_p`, `has_top_p` | `src/headers/aimee_ir.h:127-128` | "valid on both Anthropic and OpenAI" (header comment :122-126) |
| `top_k`, `has_top_k` | `src/headers/aimee_ir.h:129-130` | same comment |
| `max_tokens`, `has_max_tokens` | `src/headers/aimee_ir.h:116-117` | |
| `stop_sequences[]`, `n_stop` | `src/headers/aimee_ir.h:131-132` | carries as opaque string array, NOT `presence_penalty`/`frequency_penalty`/`repetition_penalty`/`min_p` |
| `stream` (int) | `src/headers/aimee_ir.h:131` | transport flag, not a sampling param |
| `metadata` (opaque cJSON) | `src/headers/aimee_ir.h:134-138` | captures vendor-specific top-level keys (Anthropic `metadata.user_id`) |
| `thinking` (opaque cJSON) | `src/headers/aimee_ir.h:143-146` | Anthropic extended-thinking CONFIG object |
| `service_tier` (string) | `src/headers/aimee_ir.h:140-141` | Anthropic-specific |

**Not modeled on the IR** (because they are delegate-local, not a wire-level
sampling contract): `repetition_penalty`, `presence_penalty`,
`frequency_penalty`, `min_p`. These live in the per-delegate
`model_sampling_row_t` table (see §1) and are applied **per-backend build** in
`src/server/model_sampling.c`.

---

## 1. Per-backend matrix

Each column lists the production backend shapes aimee actually serves (the
ones the route table dispatches; see §2 of `collapse_recon.md`). "Honors"
means: there is a verified code path that either (a) re-emits the field on
the wire OR (b) explicitly drops it after a typed read (deliberate rejection).

| Knob | Anthropic Messages client → Anthropic provider | Anthropic client → OpenAI Chat provider | OpenAI Chat client → OpenAI Chat | OpenAI Chat client → Codex (Responses) | OpenAI Responses client → OpenAI Chat | Bedrock ConverseStream |
| --- | --- | --- | --- | --- | --- | --- |
| `temperature` | ✅ re-emitted on Anthropic request via `model_sampling_apply_anthropic` (`src/server/model_sampling.c:93`) — `add_number_if_missing(req, "temperature", caller_temperature)` at `:99`, row fallback at `:102` | ✅ re-emitted on OpenAI request via `model_sampling_apply_openai` (`src/server/model_sampling.c:71`) — `add_number_if_missing(req, "temperature", caller_temperature)` at `:77`, row fallback at `:79` | ✅ `openai_request_int` style; `chat_stream_handler` reads `OPENAI_CHAT_MAX_TOKENS` (`:748`) and threads temperature through `agent_dispatch_one` (`:755`) | ✅ via `aimee_ir_responses_to_chat` (`src/server/openai_chat.c:1098`) → `chat_stream_handler` path | ✅ via `aimee_ir_responses_to_chat` (`src/server/openai_chat.c:1098`) — same Chat path as col 3 | ✅ request emission via `model_sampling_apply_anthropic` (Bedrock uses Anthropic-shape request) at `src/server/model_sampling.c:93`; stream consumption via `bedrock_converse_stream_to_deltas` (`src/server/aimee_ir_stream.c:220`) |
| `top_p` | ✅ `add_number_if_missing(req, "top_p", row.top_p)` at `src/server/model_sampling.c:106` | ✅ `add_number_if_missing(req, "top_p", row.top_p)` at `src/server/model_sampling.c:85` | ✅ same line `:85`; IR typed `top_p` at `src/headers/aimee_ir.h:127` | ✅ via IR path through `aimee_ir_responses_to_chat` (`src/server/openai_chat.c:1098`) | ✅ same as col 3 | ✅ via `model_sampling_apply_anthropic` `top_p` at `src/server/model_sampling.c:106` |
| `top_k` | ✅ emitted on Anthropic-shape request via `add_int_if_missing(req, "top_k", row.top_k)` at `src/server/model_sampling.c:107` (delegate-row-gated; only when `model_sampling_get(agent->model, &row)` matches a row at `src/server/model_sampling.c:9-22`) | ✅ emitted on OpenAI-shape request via `add_int_if_missing(req, "top_k", row.top_k)` at `src/server/model_sampling.c:86` (delegate-row-gated). The OpenAI Chat **wire** has no `top_k` field, but the OpenAI-compatible local providers (ollama/llama.cpp) accept it — this is the exact shape `aimee_backend_openai.c` re-emits (comment at `src/server/aimee_backend_openai.c`). The standard OpenAI Chat cloud API silently drops it | ✅ same — `model_sampling_apply_openai` runs unconditionally on the chat path; same Caveat: wire-drop on standard cloud OpenAI | ✅ via IR path through `aimee_ir_responses_to_chat` (`src/server/openai_chat.c:1098`) → `chat_stream_handler` path; same wire-drop caveat applies to the standard Responses wire | ✅ same — wire-drop on standard OpenAI Chat | ✅ emitted on Bedrock-shaped request via `add_int_if_missing(req, "top_k", row.top_k)` at `src/server/model_sampling.c:107` (Bedrock uses `model_sampling_apply_anthropic`). Stream-side `bedrock_converse_stream_to_deltas` at `src/server/aimee_ir_stream.c:220` only consumes stream output, not request fields |
| `max_tokens` | ✅ Anthropic-native field on the request; `model_sampling_apply_anthropic` preserves caller-sent value (no override) per `src/server/model_sampling.c:93-105` | ✅ IR typed `max_tokens` at `src/headers/aimee_ir.h:116`; IR build sets `max_tokens_override` for the agent shaping; `model_sampling_apply_openai` does not override caller value (`:71-89`) | ✅ read via `openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768)` at `src/server/openai_chat.c:748` | ✅ via IR path through `aimee_ir_responses_to_chat` (`src/server/openai_chat.c:1098`) and `openai_request_int` at `src/server/openai_chat.c:1133` | ✅ same as col 3 | ✅ Anthropic-shape request carries `max_tokens`; IR-typed at `src/headers/aimee_ir.h:116` |
| `stop` / `stop_sequences` | ✅ verbatim (Anthropic-native `stop_sequences` array is re-emitted) via `model_sampling_apply_anthropic` (no override; `src/server/model_sampling.c:93-105`) | ✅ IR typed `stop_sequences[]` at `src/headers/aimee_ir.h:131`; IR-typed pass-through to OpenAI request build | ⚠ OpenAI `stop` (string-or-array) is forwarded by `agent_dispatch_one`-side request build, but the IR has typed `stop_sequences` only — a single-string `stop: "."` is dropped during IR parse (see Phase 4.0 prereq §3) | ✅ via IR path | ⚠ same gap as col 3 (string-or-array normalization) | ✅ via Anthropic-shape request build (`model_sampling_apply_anthropic` carries `stop_sequences` from `aimee_request_t.stop_sequences[]` at `:131`); stream-side `bedrock_converse_stream_to_deltas` maps `stop_reason` to canonical stop enum at `src/server/aimee_ir_stream.c:343` |
| `repetition_penalty` | n/a (Anthropic has no `repetition_penalty`) | n/a | ⚠ delegate-only via `model_sampling_apply_openai` (`src/server/model_sampling.c:71`) — `add_number_if_missing(req, "repeat_penalty", row.repeat_penalty)` at `:88`; gated by `model_sampling_row_t.repeat_penalty` per-delegate preset (the `g_sampling_rows` table at `src/server/model_sampling.c:9` lists `repeat_penalty` per row) | ⚠ delegate-only — same `add_number_if_missing` at `:88` | n/a (OpenAI Chat has no `repetition_penalty`) | n/a (Bedrock/Anthropic-shape has no `repetition_penalty`) |
| `presence_penalty` | n/a (Anthropic has no `presence_penalty`) | n/a | ❌ **no plumbing** — not modeled on the IR (`src/headers/aimee_ir.h:118-132` lists `temperature`/`top_p`/`top_k`/`max_tokens`/`stop_sequences` only); not applied by `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89` lists only `temperature`/`top_p`/`min_p`/`repeat_penalty`). Pass-through via the `raw` sidecar at `src/headers/aimee_ir.h:151` is the only extant route | ❌ same — no plumbing on IR, no `add_number_if_missing` call for `presence_penalty` in `model_sampling_apply_openai` | ❌ same — no IR field, no `add_number_if_missing` call | n/a (Bedrock/Anthropic-shape has no `presence_penalty`) |
| `frequency_penalty` | n/a (Anthropic has no `frequency_penalty`) | n/a | ❌ **no plumbing** — same as `presence_penalty`; not modeled on IR, not applied by `model_sampling_apply_openai` | ❌ same | ❌ same | n/a (Bedrock/Anthropic-shape has no `frequency_penalty`) |
| `min_p` | n/a (Anthropic has no `min_p`) | n/a | ⚠ delegate-only via `model_sampling_apply_openai` (`src/server/model_sampling.c:71`) — `add_number_if_missing(req, "min_p", row.min_p)` at `:87`; gated by `model_sampling_row_t.min_p` per-delegate preset | ⚠ delegate-only — same `add_number_if_missing` at `:87` | n/a | n/a |
| (continuation) `previous_response_id` | n/a | n/a | n/a | ⚠ Responses API continuation; each Codex turn is "stateless full-history" per the `responses_stream_handler` heal logic comment at `src/server/openai_chat.c:1145` ("Heal orphaned tool calls/results — each Codex turn is stateless full-history"). No `previous_response_id` thread key plumbing on the IR (`src/headers/aimee_ir.h:118-148` has no `previous_response_id` field). Storage substrate exists at `src/server/openai_responses_store.c` | ⚠ mirror of Responses; same gap — no IR field for `previous_response_id` | n/a |
| (continuation) prompt-cache keying | ✅ Anthropic `cache_control` blocks modeled on the IR per-block (`src/headers/aimee_ir.h:80`) and per-tool (`src/headers/aimee_ir.h:106`); preserved verbatim through canonical egress by `aimee_request_t` line 80 comment; verified by `AIMEE_IR_M_CACHE_CONTROL_LOST` shadow counter (`src/headers/aimee_ir_metrics.h:28`) | ✅ IR caches per-block (`:80`) and per-tool (`:106`) | ❌ OpenAI Chat has `prompt_tokens_details.cached_tokens` in usage only — confirmed by `anthropic_stream_feed_openai` reading `usage.prompt_tokens_details.cached_tokens` at `src/server/anthropic_ingress.c:680` (which is the same shape OpenAI surfaces); no first-class cache-key plumbing on the IR | ❌ same gap — no IR cache-key field | ❌ same gap | ✅ Bedrock-side `cachePoint` markers — the IR models `cache_control` per-block (`:80`) and per-tool (`:106`) |
| (native assistant-prefill) | ✅ Anthropic extended-thinking CONFIG + THINKING blocks: `AIMEE_BLK_THINKING` at `src/headers/aimee_ir.h:55`, `aimee_request_t.thinking` field at `:143-146` | ✅ IR carries `thinking` (`:143-146`) and THINKING blocks (`:55`) | ❌ OpenAI Chat wire does not have a native prefill primitive (no `assistant_prefill` field on `aimee_request_t`) | ❌ same — Responses wire has no native prefill primitive | ❌ same | ✅ Bedrock-side; IR models both kind=THINKING content (`:55`) and the thinking object (`:143-146`) |

---

## 2. Reading the matrix

- ✅ = honored by a verified request-building file:line today (no Phase 4 plumbing needed).
- ⚠ = honored on a subset of paths, or via a delegate-only opt-in, or has a partial plumbing (Phase 4 must extend).
- ❌ = **no plumbing** today — Phase 4.0 prerequisite needed.
- n/a = the wire protocol does not have that field by design.

---

## 3. Phase 4.0 prerequisites (missing plumbing → required before Phase 4 lands)

Each ❌ row above is a Phase 4.0 prerequisite, scoped narrowly per the
acceptance criterion ("any missing plumbing as Phase 4.0 prerequisites"):

1. **`presence_penalty`, `frequency_penalty` on the IR.** Add typed fields to
   `aimee_request_t` in `src/headers/aimee_ir.h` (new members with `has_*`
   companions at lines >119 to keep the `has_*` companion pattern consistent
   with `has_temperature` at `:119`), wire `aimee_frontend_openai.c` to
   populate, and add `add_number_if_missing(req, "presence_penalty", ...)`
   and `add_number_if_missing(req, "frequency_penalty", ...)` lines to
   `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89`,
   mirroring the `add_number_if_missing(req, "repeat_penalty", ...)` pattern
   at `:88`).
2. **`stop` (string-or-array) on `aimee_request_t.stop_sequences[]`.** Today
   the IR models it as `char **` array (`src/headers/aimee_ir.h:131`); an
   OpenAI client can send a single string `stop: "."` which currently gets
   dropped during IR parse. Phase 4.0 normalizes to array inside
   `aimee_frontend_openai.c` (the IR-side request builder).
3. ~~`top_k` on Responses and Bedrock request emission.~~ **REMOVED —
   observation was wrong.** Both `model_sampling_apply_openai`
   (`src/server/model_sampling.c:86`) and `model_sampling_apply_anthropic`
   (`src/server/model_sampling.c:107`) already emit `top_k` via the SAME
   `add_int_if_missing(req, "top_k", row.top_k)` call. The Anthropic wire
   carries it forward verbatim; the Bedrock wire carries it forward
   verbatim (Bedrock's request envelope is Anthropic-shaped); Responses
   requests carry it through `aimee_ir_responses_to_chat`. Where the
   standard cloud OpenAI Chat / Responses wire drops the field, that is a
   wire-protocol behavior, not a missing-plumbing problem. **Phase 4.0 has
   no top_k-emit work to do.**
4. **`previous_response_id` thread key for Responses continuations.** The
   storage substrate already exists (`src/server/openai_responses_store.c`
   per the flat `src/server/` listing); what is missing is the IR-side
   continuation reference and the `aimee_ir_response_to_parsed` bridge
   (`src/server/aimee_ir_serve.c:292`). This is a structural seam, not a
   sampling knob, but it belongs in the Phase 4 dependencies list because
   it decides what thread-history the guardrail-collapse work sees.

The ⚠ rows (`temperature`/cache-control on Bedrock, delegate-only
`repetition_penalty`/`min_p`) are **partial or provider-specific** for the
*observed* delegates; Phase 4 does NOT need to add them globally. They are
listed here as observed-by-preset only — no Phase 4.0 prerequisite.

---

## 4. Where the matrix is consumed (downstream)

Phase 4's collapse rules will read this matrix to decide which knobs can be
collapsed onto a single IR-backed knob and which must remain per-wire
fields (because the backends disagree about their semantics).

Phase 2's tap (the `aimee_delta_t` consumer) does not read this matrix —
deltas don't carry sampling fields; the request-side IR is the source.

---

## 5. Acceptance check

- [x] Per-backend columns for every client wire the route table dispatches (§2 of `collapse_recon.md`).
- [x] Every cell carries a file:line or `n/a` verdict.
- [x] Missing plumbing listed as Phase 4.0 prerequisites.
- [x] IR-typed surface cross-referenced to `src/headers/aimee_ir.h`.
- [x] Delegate-only knob surface cross-referenced to `src/server/model_sampling.c:71-89`.
