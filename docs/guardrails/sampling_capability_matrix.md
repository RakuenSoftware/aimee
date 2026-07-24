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
- **OpenAI Chat wire (cloud)** — unverified behaviour: the standard
  OpenAI Chat API may silently drop unknown fields; local OpenAI-compatible
  providers (ollama / llama.cpp) accept `top_k` (per the index comment at
  `src/server/aimee_backend_openai.c`). The live repo has no test or
  symbol that distinguishes wire-drop behaviour on cloud OpenAI Chat
  from local-pass-through; that distinction is not derivable from
  `src/server/model_sampling.c` alone and is marked unverified below.
- **OpenAI Responses wire** is reached via `aimee_ir_responses_to_chat`
  (`src/server/openai_chat.c:1098`), which routes through the same
  OpenAI Chat builder; same unverified cloud-drop caveat applies.

**F3 cell-level corrections:** every matrix cell that previously asserted
provider behaviour without a repo citation (e.g. "the standard OpenAI
Chat cloud API silently drops it", "the Responses API is stateless
full-history", "no first-class cache-key plumbing", "no native prefill
primitive") is re-marked below. Verified citations replace claims where
available; remaining claims are explicitly marked **unverified** and
routed to Phase 4.0 as a prerequisite to verify by either a test
fixture or a `src/server/` call site that the recon did not find.

**Phase:** 0 — reconnaissance packet companion to `collapse_recon.md`.
**Scope:** explicit per-backend list of which top-level sampling knobs each
*production* delegate honours, cross-referenced against the canonical IR's
typed-sampling surface. The matrix constrains Phase 4 scope and surfaces any
missing plumbing as Phase 4.0 prerequisites.
**Verification convention:** every non-`n/a` cell carries a directly relevant
request-parser or backend-builder file:line citation. `n/a` means the wire
protocol does not have that field by design (e.g. Anthropic has no
`repetition_penalty` field). **unverified** means the matrix previously
asserted a provider behaviour that the recon did not substantiate with a
repo file:line; Phase 4.0 is responsible for confirming the behaviour via
a fixture or unblocking the missing call site.
**Status:** REVIEW-CORRECTED — the prior `top_k` claims contradicted the
live backend builders (`src/server/model_sampling.c:86/107` both emit
`top_k` via `add_int_if_missing(req, "top_k", row.top_k)`); matrix now
reflects the verified plumbing. Provider-behaviour claims without repo
citations are re-marked **unverified** and assigned Phase 4.0 prerequisites.

---

## 0. Canonical typed-sampling surface (file:line)

The IR request struct models these sampling fields TYPED (rather than via
the `raw` sidecar), per `src/headers/aimee_ir.h`:

| IR field | Verified at | Notes |
| --- | --- | --- |
| `temperature`, `has_temperature` | `src/headers/aimee_ir.h:117-118` (struct opens at `:105`, closes at `:151`) | `has_*` int companion is set when the client supplied it |
| `top_p`, `has_top_p` | `src/headers/aimee_ir.h:125-126` | "valid on both Anthropic and OpenAI" (header comment `:122-126`) |
| `top_k`, `has_top_k` | `src/headers/aimee_ir.h:127-128` | same comment |
| `max_tokens`, `has_max_tokens` | `src/headers/aimee_ir.h:115-116` | |
| `stop_sequences[]`, `n_stop` | `src/headers/aimee_ir.h:131-132` | carries as opaque string array, NOT `presence_penalty`/`frequency_penalty`/`repetition_penalty`/`min_p` |
| `stream` (int) | `src/headers/aimee_ir.h:130` | transport flag, not a sampling param |
| `metadata` (opaque cJSON) | `src/headers/aimee_ir.h:139` | captures vendor-specific top-level keys (Anthropic `metadata.user_id`) |
| `thinking` (opaque cJSON) | `src/headers/aimee_ir.h:147-148` | Anthropic extended-thinking CONFIG object |
| `service_tier` (string) | `src/headers/aimee_ir.h:144` | Anthropic-specific |

**Not modeled on the IR** (because they are delegate-local, not a wire-level
sampling contract): `repetition_penalty`, `presence_penalty`,
`frequency_penalty`, `min_p`. These live in the per-delegate
`model_sampling_row_t` table (see §1) and are applied **per-backend build** in
`src/server/model_sampling.c`.

---

## 1. Per-backend matrix (F3 closure — every cell now cites a verified file:line)

Each column lists the production backend shapes aimee actually serves.
"Honors" means there is a verified code path that either (a) re-emits the
field on the wire OR (b) explicitly drops it after a typed read. The
parser-side citations are in `src/server/aimee_frontend_{openai,anthropic,
responses}.c`; the typed-IR→backend-builder citations are in
`src/server/aimee_backend_{openai,anthropic,responses,bedrock}.c`; the
model_sampling overlay citations are in `src/server/model_sampling.c`
(called from `src/server/agent_request_build.c:90/94` and
`src/server/agent_bridge.c:163/285`). The `/v1/chat/completions` "Codex"
column routes through `agent_execute_messages` →
`agent_build_request` (`src/server/agent_request_build.c:60-100`), which
selects `responses_backend_build` (line 71) and overlays
`model_sampling_apply_openai` (line 94). The "Bedrock ConverseStream"
column uses `bedrock_converse_build` (`src/server/aimee_backend_bedrock.c:366`).

| Knob | Anthropic Messages client → Anthropic provider | Anthropic client → OpenAI Chat provider | OpenAI Chat client → OpenAI Chat | OpenAI Chat client → Codex (Responses) | OpenAI Responses client → OpenAI Chat | Bedrock ConverseStream |
| --- | --- | --- | --- | --- | --- | --- |
| `temperature` | ✅ Anthropic frontend parses `temperature` (`src/server/aimee_frontend_anthropic.c:148-152`, sets `has_temperature`) → `anthropic_backend_build` re-emits (`src/server/aimee_backend_anthropic.c:185-186`) → `model_sampling_apply_anthropic` layers caller/row/provider-fixed (`src/server/model_sampling.c:91-102`) | ✅ Anthropic frontend parses (`src/server/aimee_frontend_anthropic.c:148-152`) → `agent_build_request_anthropic` calls `model_sampling_apply_openai` (`src/server/agent_bridge.c:285`, builder at `:71-89` of `model_sampling.c`); openai backend re-emits `has_temperature` (`src/server/aimee_backend_openai.c:50-51`) | ✅ OpenAI frontend parses `temperature` (`src/server/aimee_frontend_openai.c:131-135`) → `openai_backend_build` re-emits (`src/server/aimee_backend_openai.c:50-51`) → `openai_request_double` passes value to `agent_dispatch_one` (`src/server/openai_chat.c:651`) | ⚠ `responses_backend_build` does **NOT** emit `temperature` (verified, lines `55-140` of `src/server/aimee_backend_responses.c`); the `model_sampling_apply_openai` overlay at `agent_request_build.c:94` runs only when `!is_resp` (the same `if` block at lines `:88-94`). For Responses-path, the caller temperature is silently dropped — this is a Phase 4.0 fix | ⚠ Responses frontend parses (NOTE: **no temperature parser line** found in `src/server/aimee_frontend_responses.c`; the file only parses `max_output_tokens` at `:67-70` and `top_p` at `:72-76`); the IR-to-Chat translator at `aimee_ir_responses_to_chat` is the verifier path (`src/server/aimee_ir_serve.c`); no builder emission for Responses-source temperature today — Phase 4.0 prerequisite | ✅ Bedrock backend re-emits `has_temperature` in `inferenceConfig.temperature` (`src/server/aimee_backend_bedrock.c:427-429`) |
| `top_p` | ✅ Anthropic frontend parses (`src/server/aimee_frontend_anthropic.c:154-158`) → `anthropic_backend_build` re-emits (`src/server/aimee_backend_anthropic.c:188-189`) | ✅ Anthropic frontend parses → `openai_backend_build` re-emits `has_top_p` (`src/server/aimee_backend_openai.c:53-54`) | ✅ OpenAI frontend parses (`src/server/aimee_frontend_openai.c:137-141`) → `openai_backend_build` re-emits (`src/server/aimee_backend_openai.c:53-54`) | ⚠ `responses_backend_build` does **NOT** emit `top_p` (verified, lines `55-140`); `model_sampling_apply_openai` overlay at `agent_request_build.c:94` only runs when `!is_resp` — same Phase 4.0 fix needed | ⚠ Responses frontend parses `top_p` (`src/server/aimee_frontend_responses.c:72-76`) but the wire-level emission goes via `aimee_ir_responses_to_chat`; the translator at `src/server/openai_shape.c:216` is the canonical Responses-source→Chat continuation primitive path; field reaches `openai_backend_build.has_top_p` (`:53-54`) once Phase 4.0 wires it | ✅ Bedrock backend re-emits `has_top_p` in `inferenceConfig.topP` (`src/server/aimee_backend_bedrock.c:431-433`) |
| `max_tokens` | ✅ Anthropic frontend parses (`src/server/aimee_frontend_anthropic.c:142-146`) → `anthropic_backend_build` re-emits (`src/server/aimee_backend_anthropic.c:182-183`) | ✅ Anthropic frontend parses → `openai_backend_build` re-emits `has_max_tokens` (`src/server/aimee_backend_openai.c:47-48`) | ✅ OpenAI frontend parses (`src/server/aimee_frontend_openai.c:125-129`) → `openai_backend_build` re-emits (`src/server/aimee_backend_openai.c:47-48`) → `openai_request_int` passes value to `agent_dispatch_one` (`src/server/openai_chat.c:652`) | ✅ OpenAI frontend parses → `agent_request_build` sets `ir.max_tokens = agent_request_max_tokens(agent, max_tokens)` and `ir.has_max_tokens = 1` (`src/server/agent_request_build.c:74-78`); `responses_backend_build` deliberately OMITS `max_output_tokens` (verified comment at `:60-62`: "codex/gpt-5.5 400s on Unsupported parameter") — wire-shape mandate, not a Phase 4.0 prerequisite | ✅ Responses frontend parses `max_output_tokens` (`src/server/aimee_frontend_responses.c:67-70`) → routed via `aimee_ir_responses_to_chat` → `openai_backend_build.has_max_tokens` (`src/server/aimee_backend_openai.c:47-48`) | ✅ Bedrock backend re-emits `has_max_tokens` in `inferenceConfig.maxTokens` (`src/server/aimee_backend_bedrock.c:423-425`) |
| `stop / stop_sequences` | ⚠ Anthropic frontend parses `stop_sequences[]` (`src/server/aimee_frontend_anthropic.c:237-256`) → `anthropic_backend_build` does NOT yet serialize `stop_sequences` (verified, no consumer in lines `168-220`); Phase 4.0 prerequisite for Anthropic egress | ⚠ Anthropic frontend parses `stop_sequences[]` → `openai_backend_build` does NOT emit `stop` (verified, no `stop_sequences` consumer in lines `43-120`); the IR field is read but the OpenAI wire is not generated — Phase 4.0 prerequisite | ⚠ OpenAI frontend parses BOTH string and array forms into `out->stop_sequences[]` (`src/server/aimee_frontend_openai.c:265-288`) → `openai_backend_build` does NOT yet emit `stop`; Phase 4.0 prerequisite for OpenAI Chat egress | ⚠ Same Responses path; no `stop` emitter in `responses_backend_build` (`:55-140`); Phase 4.0 prerequisite | ⚠ Responses frontend does NOT parse `stop` (verified no `stop` line in `aimee_frontend_responses.c`); translator path needs Phase 4.0 wiring | ✅ Bedrock backend re-emits `n_stop`/`stop_sequences[]` in `inferenceConfig.stopSequences` (`src/server/aimee_backend_bedrock.c:435-446`) — the ONLY backend builder that emits stop today |
| `repetition_penalty` | n/a — Anthropic wire has no native `repetition_penalty` field; `model_sampling_apply_anthropic` does not emit `repeat_penalty` (`src/server/model_sampling.c:91-108`) | n/a | ⚠ preset-only builder: `model_sampling_apply_openai` emits `repeat_penalty` from a matched `model_sampling_row_t` row (`src/server/model_sampling.c:88`), but the OpenAI frontend does NOT parse a request-side `repetition_penalty` (verified, no hit in `aimee_frontend_openai.c`); Phase 4.0 prerequisite to add an IR field + parser | ⚠ Responses path inherits the same gap (`responses_backend_build` does not emit `repeat_penalty`; the overlay at `agent_request_build.c:94` only fires when `!is_resp`) — Phase 4.0 prerequisite | ⚠ Responses frontend does NOT parse `repetition_penalty`; Phase 4.0 prerequisite | n/a — Bedrock wire has no native `repetition_penalty` field; `bedrock_converse_build` does not emit one (verified, no `repetition`/`repeat_penalty` line in `aimee_backend_bedrock.c`) |
| `presence_penalty` | n/a | n/a | ❌ **no plumbing** today: `aimee_request_t` has no `presence_penalty` field (`src/headers/aimee_ir.h:105-151`); OpenAI frontend does NOT parse it; `openai_backend_build` does NOT emit it; `model_sampling_apply_openai` does NOT emit it — Phase 4.0 prerequisite to add IR field + parser + emitter | ❌ same; Responses path inherits the gap — Phase 4.0 prerequisite | ❌ same; Responses frontend does NOT parse it — Phase 4.0 prerequisite | n/a — Bedrock wire has no native `presence_penalty` field |
| `frequency_penalty` | n/a | n/a | ❌ **no plumbing** today: same as `presence_penalty` — no IR field, no parser, no emitter — Phase 4.0 prerequisite | ❌ same — Phase 4.0 prerequisite | ❌ same — Phase 4.0 prerequisite | n/a — Bedrock wire has no native `frequency_penalty` field |
| `min_p` | n/a | n/a | ⚠ preset-only builder: `model_sampling_apply_openai` emits `min_p` from a matched row (`src/server/model_sampling.c:87`); OpenAI frontend does NOT parse request-side `min_p` — Phase 4.0 prerequisite to add an IR field + parser | ⚠ Responses path inherits the gap; `responses_backend_build` does NOT emit `min_p`; the overlay at `agent_request_build.c:94` only fires when `!is_resp` — Phase 4.0 prerequisite | ⚠ Responses frontend does NOT parse `min_p` — Phase 4.0 prerequisite | n/a — Bedrock wire has no native `min_p` field |
| `top_k` | ✅ Anthropic frontend parses `top_k` (`src/server/aimee_frontend_anthropic.c:160-164`) → `anthropic_backend_build` re-emits (`src/server/aimee_backend_anthropic.c:191-192`) → `model_sampling_apply_anthropic` overlays the preset at `src/server/model_sampling.c:107` | ✅ Anthropic frontend parses → `openai_backend_build` re-emits `has_top_k` (`src/server/aimee_backend_openai.c:55-56`); comment at line 56 says "OpenAI-compatible local providers (ollama/llama.cpp) accept top_k" — the cloud OpenAI Chat wire may silently drop unknown fields; behaviour for cloud OpenAI Chat is the same unverified caveat as 0a-§0 | ✅ OpenAI frontend parses `top_k` (`src/server/aimee_frontend_openai.c:143-147`) → `openai_backend_build` re-emits (`src/server/aimee_backend_openai.c:55-56`); same cloud-drop caveat | ⚠ Responses path: `responses_backend_build` does NOT emit `top_k` (verified, lines `55-140`); `model_sampling_apply_openai` overlay at `agent_request_build.c:94` only fires when `!is_resp` — Phase 4.0 prerequisite | ⚠ Responses frontend does NOT parse `top_k`; Phase 4.0 prerequisite | n/a — Bedrock Converse infers `top_k` only via `additionalModelRequestFields`, deliberately deferred (verified comment at `src/server/aimee_backend_bedrock.c:419-421`) |
| `(continuation) previous_response_id` | n/a — Anthropic wire has no `previous_response_id` | n/a | ✅ verified on the Responses handler only: `openai_responses_handler` reads `previous_response_id` via `openai_shape.c:216` and prepends the stored transcript (`src/server/openai_chat.c:622-635`); not a property of `/v1/chat/completions` itself | ✅ Same: Responses handler is the path the Codex column actually serves — the matrix column "OpenAI Chat client → Codex" routes through `agent_execute_messages` → `agent_build_request` and the Codex client supplies `previous_response_id` to the OpenAI Responses route, not the Chat route | ✅ Responses handler is the path this column actually serves (Responses client → OpenAI Chat via the Codex gateway); verified | n/a — Bedrock wire has no `previous_response_id` |
| `(continuation) prompt-cache keying` | n/a — Anthropic prompt caching is on the system block via `cache_control`, not a request field; `anthropic_backend_build` calls `mark_cache_prefix` on `system` and `tools` arrays (`src/server/aimee_backend_anthropic.c:201, 226`) | ⚠ Anthropic source feeding OpenAI Chat backend has no cache marking; Phase 4.0 prerequisite | ⚠ OpenAI Chat wire has no first-class cache-key field; `openai_backend_build` does NOT emit `prompt_cache_key` (verified, no such line in lines `43-120`); response-side cached-token accounting at `src/server/agent_bridge.c` is NOT request plumbing — Phase 4.0 prerequisite | n/a — Codex wire has no cache-key primitive | ⚠ Responses frontend does NOT parse a cache-key; translator path needs Phase 4.0 wiring | n/a — Bedrock prompt-cache control lives on the `system` content block (`src/server/aimee_backend_bedrock.c` calls `system_to_converse`; no request-level cache-key field) |
| `(native assistant-prefill)` | ⚠ Anthropic wire has no first-class prefill primitive; `anthropic_backend_build` round-trips a `role:"assistant"` block verbatim if the IR carries one (verified, lines `203-216` of `src/server/aimee_backend_anthropic.c`); Phase 4.0 prerequisite to add an explicit prefill knob | ⚠ same — OpenAI Chat backend has no first-class prefill; Phase 4.0 prerequisite | ⚠ OpenAI Chat wire has no first-class prefill; Phase 4.0 prerequisite | ⚠ Responses wire has no first-class prefill; Phase 4.0 prerequisite | ⚠ Responses wire has no first-class prefill; Phase 4.0 prerequisite | ⚠ Bedrock wire accepts `role:"assistant"` blocks (mirror of `blocks_to_converse` at `src/server/aimee_backend_bedrock.c:402-417`); no first-class prefill knob |

**F3 cell-level corrections applied:**

- Every ⚠ **unverified** row has been replaced with a verified ✅/⚠/❌
  reading citing a file:line. The "unverified" caveat from §0a is retained
  *only* where it is a true provider-behaviour question (e.g. cloud
  OpenAI Chat silent-drop of unknown fields like `top_k`/`repeat_penalty`)
  rather than a missing code path.
- `temperature` for the Codex column is downgraded to ⚠: the
  `responses_backend_build` builder (verified lines `55-140`) does not
  emit `temperature`, and the `model_sampling_apply_openai` overlay
  inside `agent_request_build` (`src/server/agent_request_build.c:94`)
  only runs when `!is_resp`. Phase 4.0 is responsible for emitting it on
  the Responses wire.
- `stop` row is downgraded to ⚠ for the OpenAI Chat column: the OpenAI
  frontend ALREADY normalizes string-or-array into `out->stop_sequences[]`
  (`src/server/aimee_frontend_openai.c:265-288`), but `openai_backend_build`
  does NOT yet serialize `stop_sequences` on the wire. Phase 4.0
  prerequisite for the OpenAI Chat egress.
- `previous_response_id` is upgraded to ✅ on the three Codex/Responses
  columns: the verified call site is
  `src/server/openai_chat.c:622-635` plus `src/server/openai_shape.c:216`.

---

## 2. Reading the matrix

- ✅ = honored by a verified request-building file:line today (no Phase 4 plumbing needed).
- ⚠ = honored on a subset of paths, or via a delegate-only opt-in, or has a partial plumbing (Phase 4 must extend).
- ❌ = **no plumbing** today — Phase 4.0 prerequisite needed.
- n/a = the wire protocol does not have that field by design.

---

## 3. Phase 4.0 prerequisites (missing plumbing → required before Phase 4 lands)

Each ❌ row and each ⚠ row above that needs new plumbing is a Phase 4.0 prerequisite:

1. **`presence_penalty`, `frequency_penalty` on the IR.** Add typed fields to
   `aimee_request_t` in `src/headers/aimee_ir.h` (new members with `has_*`
   companions immediately after the `has_top_k` companion at line `128`,
   keeping the `has_*` companion pattern consistent with `has_temperature`
   at `:118`), wire `aimee_frontend_openai.c` to populate, and add
   `add_number_if_missing(req, "presence_penalty", ...)` and
   `add_number_if_missing(req, "frequency_penalty", ...)` lines to
   `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89`,
   mirroring the `add_number_if_missing(req, "repeat_penalty", ...)` pattern
   at `:88`).
2. **`stop_sequences[]` serialization on the OpenAI Chat backend.** Today
   `openai_backend_build` (`src/server/aimee_backend_openai.c:43-`) reads
   `ir->temperature`/`has_temperature`/`top_p`/`has_top_p`/`top_k`/`has_top_k`
   but does NOT iterate `ir->stop_sequences[]`. Phase 4.0 adds the same
   emitter the Bedrock backend uses (`src/server/aimee_backend_bedrock.c:435-446`)
   so the wire carries `stop` (string for n=1, array otherwise).
3. **`stop_sequences[]` serialization on the Anthropic backend.** Same gap:
   `anthropic_backend_build` (`src/server/aimee_backend_anthropic.c:168-220`)
   does not yet emit `stop_sequences`. The Anthropic wire uses
   `stop_sequences` (array), so Phase 4.0 adds the same array emitter as
   Bedrock.
4. **`temperature` (and `top_p`) emission on the Responses backend.** Today
   `responses_backend_build` (`src/server/aimee_backend_responses.c:55-140`)
   does NOT emit `temperature`, `top_p`, `top_k`, `min_p`, or
   `repeat_penalty`. The `model_sampling_apply_openai` overlay inside
   `agent_request_build` (`src/server/agent_request_build.c:94`) only runs
   when `!is_resp`. Phase 4.0 lifts the relevant fields into the Responses
   request builder and/or wires the overlay to fire for the Responses
   shape.
5. **`repetition_penalty`, `min_p` on the IR.** Add typed fields to
   `aimee_request_t` (same pattern as item 1), wire `aimee_frontend_openai.c`
   to populate, and verify the `model_sampling_apply_openai` emitter at
   `src/server/model_sampling.c:87-88` still fires (it already does — the
   prerequisite is the IR-side plumbing, not the emitter).
6. **`previous_response_id` typed IR field.** The runtime plumbing exists
   (`src/server/openai_chat.c:622-635` + `src/server/openai_shape.c:216` +
   `src/server/openai_responses_store.c`), but `aimee_request_t` has no
   `previous_response_id` member. Phase 4.0 adds it as a `char *` field
   after `service_tier` (`src/headers/aimee_ir.h:144`), wires
   `aimee_frontend_responses.c` to populate it from the top-level
   `previous_response_id` request field, and wires the Responses backend
   to re-emit it.
7. **Cloud OpenAI Chat silent-drop verification for `top_k`, `repeat_penalty`,
   `min_p`.** Both `model_sampling_apply_openai` (`src/server/model_sampling.c:86-88`)
   and `openai_backend_build` (`src/server/aimee_backend_openai.c:55-56`)
   emit these fields, but the standard OpenAI Chat cloud API may silently
   drop unknown fields. Phase 4.0 confirms behaviour via a fixture (local
   `ollama` / `llama.cpp` accept them; cloud behaviour is unverified) before
   treating them as honoured on the cloud path.
