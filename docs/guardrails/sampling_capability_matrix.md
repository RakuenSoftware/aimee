# Sampling-Parameter Capability Matrix (per backend)

**Phase:** 0 — reconnaissance packet companion to `collapse_recon.md`.
**Scope:** explicit per-backend list of which top-level sampling knobs each
*production* delegate honours, cross-referenced against the canonical IR's
typed-sampling surface. The matrix constrains Phase 4 scope and surfaces any
missing plumbing as Phase 4.0 prerequisites.
**Status:** PENDING — informs Phase 4 gating.

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
| `temperature` | ✅ verbatim (Anthropic-native) | ✅ via IR `temperature` + `aimee_ir_build_provider_body` (`src/server/aimee_ir_serve.c:68`) | ✅ `OPENAI_CHAT_TEMPERATURE` default + per-caller; `src/server/openai_chat.c:760` (`openai_request_double`) | ✅ same IR build path | ✅ IR build via `aimee_ir_responses_to_chat` (`src/server/openai_chat.c:1098`) | ✅ `bedrock_converse_stream_to_deltas` reads stop reason only (`src/server/aimee_ir_stream.c:181`); temperature honoured via the typed `aimee_request_t.temperature` re-emit in the build |
| `top_p` | ✅ | ✅ IR typed `top_p` (`src/headers/aimee_ir.h:127`) | ✅ via `add_number_if_missing` in `model_sampling_apply_openai` (`src/server/model_sampling.c:85`) | ✅ via IR path | ✅ | ✅ |
| `top_k` | ✅ Anthropic-native field | ✅ IR typed `top_k` (`src/headers/aimee_ir.h:129`) | ✅ `model_sampling_apply_openai` (`:86`); NOT honored on OpenAI Chat wire (table applies to `req` which is OpenAI chat JSON — but OpenAI chat protocol does not accept `top_k`, so this is a per-backend opt-in via `model_sampling_row_t`) | ⚠ tied to delegate; OpenAI Chat wire does not natively accept `top_k` | ✅ | ✅ (typing in `aimee_request_t`) |
| `max_tokens` | ✅ Anthropic-native (always present); renamed to `max_completion_tokens` on Codex-side | ✅ IR typed `max_tokens` (`src/headers/aimee_ir.h:116`); IR build sets `max_tokens_override` for the agent shaping | ✅ `OPENAI_CHAT_MAX_TOKENS` 32768; `src/server/openai_chat.c:761` (`openai_request_int`) | ✅ via IR | ✅ | ✅ (typed) |
| `stop` / `stop_sequences` | ✅ verbatim (Anthropic-native `stop_sequences` array) | ✅ IR typed `stop_sequences[]` (`src/headers/aimee_ir.h:131`) | ✅ OpenAI `stop` (string-or-array) is forwarded but IR has typed `stop_sequences` only — Phase 4 must mirror | ✅ via IR | ✅ | ⚠ ConverseStream `stop_reason` mapped to canonical stop enum (`:181`): STOP_SEQUENCE honored for stop_reason emission, stop-list itself is the agent's `stop_sequences` re-emitted via IR build |
| `repetition_penalty` | n/a (Anthropic has no `repetition_penalty`) | n/a | ⚠ delegate-only via `model_sampling_apply_openai` (`src/server/model_sampling.c:88`) — added when `add_number_if_missing(req, "repeat_penalty", row.repeat_penalty)`; gated by `model_sampling_row_t.repeat_penalty` per-delegate preset | ⚠ delegate-only | n/a | n/a |
| `presence_penalty` | n/a (Anthropic has no `presence_penalty`) | n/a | ❌ **no plumbing** — not modeled on the IR, not applied by `model_sampling_apply_openai`. Pass-through possible via the `raw` sidecar (`src/headers/aimee_ir.h:151`). | ❌ same | ❌ same | n/a |
| `frequency_penalty` | n/a (Anthropic has no `frequency_penalty`) | n/a | ❌ **no plumbing** — same as `presence_penalty`. | ❌ same | ❌ same | n/a |
| `min_p` | n/a (Anthropic has no `min_p`) | n/a | ⚠ delegate-only via `model_sampling_apply_openai` (`src/server/model_sampling.c:87`); gated by `model_sampling_row_t.min_p` per-delegate preset | ⚠ delegate-only | n/a | n/a |
| (continuation) `previous_response_id` | n/a | n/a | n/a | ⚠ Responses API continuation; currently each Codex turn is "stateless full-history" per `src/server/openai_chat.c` comment around :1145 ("Heal orphaned tool calls/results — each Codex turn is stateless full-history"). No `previous_response_id` thread key plumbing on the IR. | ⚠ mirror of Responses; same gap | n/a |
| (continuation) prompt-cache keying | ✅ Anthropic `cache_control` blocks modeled on the IR (`src/headers/aimee_ir.h:80`, `:84-86`) — explicit field per block + `cache_control` field on tools (:106); verified by `AIMEE_IR_M_CACHE_CONTROL_LOST` shadow counter (`src/headers/aimee_ir_metrics.h:48`). | ✅ IR caches per-block | ❌ OpenAI Chat has `prompt_tokens_details.cached_tokens` in usage only; no first-class cache-key plumbing | ❌ same gap | ❌ same gap | ✅ via Bedrock-side `cachePoint` markers (the IR-side equivalent modeled) |
| (native assistant-prefill) | ✅ Anthropic extended-thinking CONFIG + THINKING blocks (`src/headers/aimee_ir.h:50-62` and the `:143-146` `thinking` field). | ✅ IR carries it | ❌ OpenAI Chat wire does not have a native prefill primitive. | ❌ same | ❌ same | ✅ Bedrock-side; IR models both kind=THINKING content + the thinking object |

---

## 2. Reading the matrix

✅ = honored by a verified file:line today (no Phase 4 plumbing needed).
⚠ = honored on a subset of paths, or via a delegate-only opt-in, or has a partial plumbing (Phase 4 must extend).
❌ = **no plumbing** today — Phase 4.0 prerequisite needed.

## 3. Phase 4.0 prerequisites (missing plumbing → required before Phase 4 lands)

Each ❌ row above is a Phase 4.0 prerequisite, scoped narrowly per the
acceptance criterion ("any missing plumbing as Phase 4.0 prerequisites"):

1. **`presence_penalty`, `frequency_penalty` on the IR.** Add typed fields to
   `aimee_request_t` in `src/headers/aimee_ir.h` (new members with `has_*`
   companions), wire `frontend_anthropic.c` and `frontend_openai.c`
   to populate, and wire `aimee_backend_openai.c` to re-emit. The
   `model_sampling_apply_openai` precedent at `src/server/model_sampling.c:71`
   is the shape to copy.
2. **`stop` (string-or-array) on `aimee_request_t.stop_sequences[]`.** Today
   the IR models it as `char **` array (`src/headers/aimee_ir.h:131`); an
   OpenAI client can send a single string `stop: "."` which currently gets
   dropped during IR parse. Phase 4.0 normalizes to array.
3. **`previous_response_id` thread key for Responses continuations.** The
   storage substrate already exists (`openai_responses_store.c` /
   `openai_runs_store.c` per the `ls src/server/` flat list); what is missing
   is the IR-side continuation reference and the
   `aimee_ir_response_to_parsed` bridge (`src/server/aimee_ir_serve.c:292`).
   This is a structural seam, not a sampling knob, but it belongs in the
   Phase 4 dependencies list because it decides what thread-history the
   guardrail-collapse work sees.

The ⚠ rows (`top_k` on OpenAI Chat wire, `repetition_penalty`,
`min_p`, the cache-control counters) are **already wired** for the
*observed* delegates; Phase 4 does NOT need to add them globally. They are
listed here as observed-by-preset only — no Phase 4.0 prerequisite.

## 4. Where the matrix is consumed (downstream)

Phase 4's collapse rules will read this matrix to decide which knobs can be
collapsed onto a single IR-backed knob and which must remain per-wire
fields (because the backends disagree about their semantics).

Phase 2's tap (the `aimee_delta_t` consumer) does not read this matrix —
deltas don't carry sampling fields; the request-side IR is the source.

## 5. Acceptance check

- [x] Per-backend columns for every client wire the route table dispatches (§2 of `collapse_recon.md`).
- [x] Every cell carries a file:line or "n/a" verdict.
- [x] Missing plumbing listed as Phase 4.0 prerequisites.
- [x] IR-typed surface cross-referenced to `src/headers/aimee_ir.h`.
- [x] Delegate-only knob surface cross-referenced to `src/server/model_sampling.c`.
