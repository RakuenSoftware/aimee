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

Because those builder calls do not prove parser-to-wire reachability for every backend, all unresolved `top_k` cells remain Phase 4.0 verification prerequisites.

**F3 cell-level corrections:** every matrix cell that previously asserted
provider behaviour without a repo citation (e.g. "the standard OpenAI
Chat cloud API silently drops it", "the Responses API is stateless
full-history", "no first-class cache-key plumbing", "no native prefill
primitive") is re-marked below. Verified citations replace claims where
available; remaining claims are explicitly marked **unverified** and
routed to Phase 4.0 as a prerequisite to verify by either a test
fixture or a `src/server/` call site that the recon did not find.

---

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
**Status:** REVIEW-CORRECTED — the prior `top_k` claims
contradicted the live backend builders (`src/server/model_sampling.c:86/107`
both emit `top_k` via `add_int_if_missing(req, "top_k", row.top_k)`); matrix
now reflects the verified plumbing. Provider-behaviour claims without repo
citations are re-marked **unverified** and assigned Phase 4.0 prerequisites.

---

## 0. Canonical typed-sampling surface (file:line)

The IR request struct models these sampling fields TYPED (rather than via
the `raw` sidecar), per `src/headers/aimee_ir.h`:

| IR field | Verified at | Notes |
| --- | --- | --- |
| `temperature`, `has_temperature` | `src/headers/aimee_ir.h:117-118` (struct opens at `:105`, closes at `:151`); F-CITE-005 closure: prior cite `:118-119` was off by one | `has_*` int companion is set when the client supplied it |
| `top_p`, `has_top_p` | `src/headers/aimee_ir.h:125-126` (F-CITE-005 closure: prior cite `:127-128` was off by one -- those lines are `top_k`/`has_top_k`); header comment at `:122-126` | "valid on both Anthropic and OpenAI" (header comment :122-126) |
| `top_k`, `has_top_k` | `src/headers/aimee_ir.h:127-128` (F-CITE-005 closure: prior cite `:129-130` was off -- those lines are `stream` and the start of the `stop_sequences` comment block) | same comment |
| `max_tokens`, `has_max_tokens` | `src/headers/aimee_ir.h:115-116` (F-CITE-005 closure: prior cite `:116-117` was off by one -- `:116-117` are `has_max_tokens` and `temperature`) | |
| `stop_sequences[]`, `n_stop` | `src/headers/aimee_ir.h:131-132` | carries as opaque string array, NOT `presence_penalty`/`frequency_penalty`/`repetition_penalty`/`min_p` |
| `stream` (int) | `src/headers/aimee_ir.h:130` (F-CITE-005 closure: prior cite `:131` was off by one -- `:131` is `stop_sequences`, `:130` is the `stream` declaration line) | transport flag, not a sampling param |
| `metadata` (opaque cJSON) | `src/headers/aimee_ir.h:139` (F-CITE-005 closure: prior cite `:134-138` was off; the metadata-annotation comment block is at `:133-138`, the actual `metadata` field declaration is at `:139`) | captures vendor-specific top-level keys (Anthropic `metadata.user_id`) |
| `thinking` (opaque cJSON) | `src/headers/aimee_ir.h:147-148` (F-CITE-005 closure: prior cite `:139` was off; `:139` is `metadata`, `:147-148` is the `thinking` field with its annotation comment at `:146`) | Anthropic extended-thinking CONFIG object |
| `service_tier` (string) | `src/headers/aimee_ir.h:144` (F-CITE-005 closure: prior cite `:135` was off; `:144` is the `service_tier` field declaration, `:143` is its annotation comment) | Anthropic-specific |

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
| `temperature` | ✅ parser/builder flow: caller value is passed to `model_sampling_apply_anthropic` and emitted by `add_number_if_missing(req, "temperature", caller_temperature)` (`src/server/model_sampling.c:93-102`) | ✅ builder emission: `model_sampling_apply_openai` emits caller temperature (`src/server/model_sampling.c:71-79`); Phase 4.0 still verifies the Anthropic parser feeding that argument | ✅ parsed at `src/server/openai_chat.c:747` and passed to `agent_dispatch_one` at `:755` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ✅ parsed at `src/server/openai_chat.c:1132` and passed to `agent_execute_messages` at `:1181-1183` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `top_p` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `max_tokens` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ✅ parsed at `src/server/openai_chat.c:748` and passed to `agent_dispatch_one` at `:755` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ✅ parsed at `src/server/openai_chat.c:1133` and passed to `agent_execute_messages` at `:1181-1183` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `stop / stop_sequences` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `repetition_penalty` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ preset-only builder support: `model_sampling_apply_openai` emits `repeat_penalty` from a matched sampling row (`src/server/model_sampling.c:71-89`), but no client parser flow is verified; Phase 4.0 prerequisite | ⚠ preset-only builder support: `model_sampling_apply_openai` emits `repeat_penalty` from a matched sampling row (`src/server/model_sampling.c:71-89`), but no client parser flow is verified; Phase 4.0 prerequisite | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `presence_penalty` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ❌ no verified request plumbing: `aimee_request_t` (`src/headers/aimee_ir.h:105-151`) has no field and `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89`) does not emit it; the response-side `aimee_response_t.raw` is not a request route | ❌ no verified request plumbing: `aimee_request_t` (`src/headers/aimee_ir.h:105-151`) has no field and `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89`) does not emit it; the response-side `aimee_response_t.raw` is not a request route | ❌ no verified request plumbing: `aimee_request_t` (`src/headers/aimee_ir.h:105-151`) has no field and `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89`) does not emit it; the response-side `aimee_response_t.raw` is not a request route | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `frequency_penalty` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ❌ no verified request plumbing: `aimee_request_t` (`src/headers/aimee_ir.h:105-151`) has no field and `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89`) does not emit it; the response-side `aimee_response_t.raw` is not a request route | ❌ no verified request plumbing: `aimee_request_t` (`src/headers/aimee_ir.h:105-151`) has no field and `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89`) does not emit it; the response-side `aimee_response_t.raw` is not a request route | ❌ no verified request plumbing: `aimee_request_t` (`src/headers/aimee_ir.h:105-151`) has no field and `model_sampling_apply_openai` (`src/server/model_sampling.c:71-89`) does not emit it; the response-side `aimee_response_t.raw` is not a request route | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `min_p` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ preset-only builder support: `model_sampling_apply_openai` emits `min_p` from a matched sampling row (`src/server/model_sampling.c:71-89`), but no client parser flow is verified; Phase 4.0 prerequisite | ⚠ preset-only builder support: `model_sampling_apply_openai` emits `min_p` from a matched sampling row (`src/server/model_sampling.c:71-89`), but no client parser flow is verified; Phase 4.0 prerequisite | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `(continuation) previous_response_id` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ❌ no field on `aimee_request_t` (`src/headers/aimee_ir.h:105-151`); Phase 4.0 prerequisite | ❌ no field on `aimee_request_t`; Phase 4.0 prerequisite | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `(continuation) prompt-cache keying` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ❌ no request cache-key field on `aimee_request_t`; response cached-token accounting is not request plumbing | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |
| `(native assistant-prefill)` | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ❌ no `assistant_prefill` member on `aimee_request_t` (`src/headers/aimee_ir.h:105-151`) | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability | ⚠ **unverified** — no concrete parser-to-backend-builder field flow was found; Phase 4.0 must trace and test it before Phase 4 relies on this capability |

---

## 2. Reading the matrix

- ✅ = honored by a verified request-building file:line today (no Phase 4 plumbing needed).
- ⚠ = honored on a subset of paths, or via a delegate-only opt-in, or has a partial plumbing (Phase 4 must extend).
- ❌ = **no plumbing** today — Phase 4.0 prerequisite needed.
- n/a = the wire protocol does not have that field by design.
- **unverified** (F3 closure) = the prior matrix asserted provider behaviour
  without a directly relevant repo file:line for each backend; Phase 4.0
  must verify the behaviour via a fixture or unblock the missing call
  site before collapsing the knob.

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
3. **`top_k` parser-to-wire validation.** Both `model_sampling_apply_openai`
   (`src/server/model_sampling.c:86`) and `model_sampling_apply_anthropic`
   (`src/server/model_sampling.c:107`) already emit `top_k` via the SAME
   `add_int_if_missing(req, "top_k", row.top_k)` call. The Anthropic wire
   carries it forward verbatim; the Bedrock wire carries it forward
   verbatim (Bedrock's request envelope is Anthropic-shaped); Responses
   request builders can emit it, but parser-to-builder and backend wire acceptance are not established for every matrix cell. Phase 4.0 must add fixtures for each backend before treating it as honored.
4. **`previous_response_id` thread key for Responses continuations.** The
   storage substrate already exists (`src/server/openai_responses_store.c`
   per the flat `src/server/` listing); what is missing is the IR-side
   continuation reference and the `aimee_ir_response_to_parsed` bridge
   (`src/server/aimee_ir_serve.c:292`). This is a structural seam, not a
   sampling knob, but it belongs in the Phase 4 dependencies list because
   it decides what thread-history the guardrail-collapse work sees.
   **F3 caveat:** the "stateless full-history" characterisation is an
   inferred behaviour from a heuristic comment, not a verified semantic
   guarantee; Phase 4.0 must verify it before collapsing the knob.
5. **Unverified cells — wire behaviour validation (F-COMPLETE-001 closure: per-cell pointers below).** The
   `top_k` cloud-vs-local distinction, the `previous_response_id`
   stateless-full-history characterisation, the OpenAI Chat cache-key
   primitive question, the assistant-prefill primitive question, and the
   Bedrock cache-key primitive question are all marked **unverified** in
   §1. Phase 4.0 must either confirm each via a fixture (test under
   `tests/`) or refute the claim by finding the missing call site in
   `src/server/`; the result feeds back into the matrix at §1.

   **Per-cell bounded verification tasks (F-COMPLETE-001):**
   - **Cloud OpenAI Chat `top_k` drop vs. local pass-through.**
     Verification target: `tests/test_model_sampling_openai.c`
     (if absent, scaffolding task `src/server/model_sampling.c:71-89` +
     a recorded fixture of an OpenAI-Chat cloud response). Look for
     `add_int_if_missing(req, "top_k", row.top_k)` at
     `src/server/model_sampling.c:86` and assert via the live request
     recorder that the wire-shape contains or omits `top_k`.
   - **`previous_response_id` stateless-full-history.** Verification
     target: read the comment at `src/server/openai_chat.c:1145` (the
     "heal logic" reference); confirm or refute by walking the call
     site `responses_stream_handler` at `src/server/openai_chat.c:1081`
     and verifying whether `previous_response_id` is consumed or
     ignored. Cross-check the storage substrate at
     `src/server/openai_responses_store.c` for a read API.
   - **OpenAI Chat cache-key primitive.** Verification target: search
     `src/server/openai_frontend*.c` (or analogous: `src/server/`
     openai-frontend files) for any `cJSON_AddStringToObject(req,
     "cache_key", ...)` or `cJSON_AddObjectKey(req, "prompt_cache_key", ...)`
     pattern. The only verified cache reference today is the response
     usage at `src/server/anthropic_ingress.c:680`.
   - **Native assistant-prefill on OpenAI Chat / Responses.**
     Verification target: search `src/server/aimee_ir.c` and
     `src/server/aimee_frontend_openai.c` for any
     `assistant_prefill` or `assistant_role_prefix` handling. The
     IR has no `assistant_prefill` field; if the cloud wire accepts
     a literal assistant role prefill message, that path needs a
     Phase 4.0 type-add to `aimee_request_t`.
   - **Bedrock cache-key primitive beyond `cachePoint`.** Verification
     target: search `src/server/aimee_backend_bedrock.c`
     (or analogous: `src/server/bedrock_*.c`) for any
     `cache_key`, `prompt_cache_key`, or analogous
     `cJSON_Add*` pattern beyond `cachePoint`. The IR models
     `cache_control` per-block at `src/headers/aimee_ir.h` and
     per-tool but does not carry a Bedrock-specific cache-key field.

Every ⚠ unverified cell is a Phase 4.0 prerequisite. Preset-only `repetition_penalty` and `min_p` builder support is not client-parameter plumbing and must not be treated as such without a parser-to-builder fixture.

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
- [x] Every cell carries a file:line, an `n/a` verdict, or an **unverified** marker.
- [x] Missing plumbing listed as Phase 4.0 prerequisites.
- [x] IR-typed surface cross-referenced to `src/headers/aimee_ir.h`.
- [x] Delegate-only knob surface cross-referenced to `src/server/model_sampling.c:71-89`.
- [x] F3 closure: every previously-unsubstantiated provider-behaviour claim is either re-cited or marked **unverified** with a Phase 4.0 prerequisite; the prior `top_k` contradiction is removed.
