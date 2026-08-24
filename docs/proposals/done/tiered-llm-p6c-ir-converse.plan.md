# P6c-ir plan — Bedrock Converse ↔ IR serializer (P6 §2, the 4th backend adapter)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice of P6 (Bedrock). Branch off `testing`. Completes the pure Bedrock cores: P6a (SigV4/STS
auth) + P6b (eventstream framing) + **this (IR↔Converse mapping)**. The proposal §2: "Converse
maps directly onto the IR for every family — the default adapter." This ships that mapping as a
**new backend adapter** (`aimee_backend_bedrock.c`), pure IR↔cJSON exactly like the existing
anthropic/openai/responses backends — deterministic, fixture-testable against AWS's documented
Converse schema, no AWS-substrate dependency, no live endpoint. With it, the deferred
P6c-egress becomes pure wiring (resolve catalog → build Converse → SigV4 (P6a) → dispatch →
parse/decode (P6b) → IR).

## Verified substrate (from the IR substrate map)

- `src/headers/aimee_ir.h`: `aimee_request_t` (model, system blocks, messages, tools,
  tool_choice, max_tokens/temperature/top_p/top_k + has_* flags, stop_sequences, stream, …);
  `aimee_block_t` (type ∈ TEXT/TOOL_USE/TOOL_RESULT/IMAGE/DOCUMENT/THINKING/UNKNOWN + tool_id/
  tool_name/tool_input/tool_result/tool_is_error/media_type/media_ref/raw); `aimee_tool_t`
  (name, description, schema); `aimee_response_t` (id, model, role, content blocks, stop_reason,
  usage_in/out/cache_read/cache_write/reasoning). `aimee_stop_reason_t`.
- Backend pattern: `src/server/aimee_backend_anthropic.c` — `anthropic_backend_build(ir)→cJSON*`
  (`:182`, cJSON idiom: block_to_anthropic/blocks_to_anthropic), `anthropic_backend_parse(resp,
  out,err,n)→int` (`:265`, ostr() getter, usage mapping `:325-341`). Entrypoints declared in
  `src/headers/aimee_backend.h`. cJSON everywhere (`vendor/headers/cJSON.h`), never hand-rolled.
- **Placement decided (no isolation tension):** the Converse serializer is pure IR↔cJSON with
  NO dependency on the AWS substrate (sigv4/eventstream). It lives in `src/server/
  aimee_backend_bedrock.c` in `SERVER_SRCS` alongside the other 3 backends (shared server-side,
  like them). The kb-side egress reaching it is a P6c-egress concern handled by the documented
  dual-compile "vault-core" idiom (`Makefile:324-329`) — NOT built now (nothing in kb calls it
  yet; adding kb linkage now would be speculative).
- Test pattern: `unit-test-aimee-backend` (`tests/Rules.mk:1651`) links the backend .o + IR +
  cJSON — mirror it for `unit-test-aimee-backend-bedrock`.

## The Converse wire (AWS-documented, the mapping target)

- **Request body:** `messages[]` (`{role: user|assistant, content[]}`) where content parts are
  `{text}` | `{toolUse:{toolUseId,name,input}}` | `{toolResult:{toolUseId,content[],status}}` |
  `{image:{format,source:{bytes}}}`; `system[]` (`{text}` only for the generic path);
  `inferenceConfig{maxTokens,temperature,topP,stopSequences}`; `toolConfig{tools[]:{toolSpec:
  {name,description,inputSchema:{json:<schema>}}}, toolChoice}`. (`top_k` is NOT an
  inferenceConfig field — it is a per-family `additionalModelRequestFields` value; see §deferred.)
- **Response body:** `output.message{role,content[]}`; `stopReason` ∈ end_turn|tool_use|
  max_tokens|stop_sequence|guardrail_intervened|content_filtered; `usage{inputTokens,
  outputTokens,totalTokens,cacheReadInputTokens,cacheWriteInputTokens}`.

## Design decisions

1. **`bedrock_converse_build(const aimee_request_t*) → cJSON*`** — mirror anthropic_backend_build
   (fresh cJSON object, caller owns + PrintUnformatted + Delete). Map:
   - system blocks (TEXT) → `system[]` = `[{text: ...}]` (non-text system blocks: emit their
     text if any, else skip — Converse system is text/guardContent only).
   - messages → `messages[]` `{role, content[]}` via a `block_to_converse` per-part emitter:
     TEXT→`{text}`; TOOL_USE→`{toolUse:{toolUseId:tool_id, name:tool_name, input:<dup
     tool_input>}}`; TOOL_RESULT→`{toolResult:{toolUseId:tool_id, content:[<tool_result as
     {json:…} or {text:…}>], status: tool_is_error?"error":"success"}}`; IMAGE→`{image:{format:
     <from media_type>, source:{bytes: media_ref}}}`; THINKING→`{reasoningContent:{reasoningText:
     {text}}}` (or skip if empty); UNKNOWN→replay raw only if it is already a Converse-shaped
     block, else skip (documented — no openai-catch-all leakage).
   - `inferenceConfig` (only present sub-fields): maxTokens (has_max_tokens), temperature
     (has_temperature), topP (has_top_p), stopSequences (n_stop>0).
   - `toolConfig.tools[]` from ir->tools: `{toolSpec:{name, description, inputSchema:{json:<dup
     schema>}}}`; `toolConfig.toolChoice` mapped from ir->tool_choice when present (the opaque
     cJSON is translated to Converse's `{auto|any|tool:{name}}` shape — a bounded mapping).
   - The BODY is identical for Converse and ConverseStream (they differ only by endpoint), so
     build is stream-agnostic — `ir->stream` does NOT change the body.
2. **`bedrock_converse_parse(const cJSON *resp, aimee_response_t *out, char *err, size_t n) →
   int`** — mirror anthropic_backend_parse. Read `output.message.role` → out->role;
   `output.message.content[]` → out->content blocks (`{text}`→TEXT; `{toolUse}`→TOOL_USE with
   tool_id/tool_name/tool_input; `{reasoningContent}`→THINKING; unknown→UNKNOWN+raw).
   `stopReason` → out->stop_reason + raw_stop_reason (end_turn→END_TURN, tool_use→TOOL_USE,
   max_tokens→MAX_TOKENS, stop_sequence→STOP_SEQUENCE, content_filtered→CONTENT_FILTER,
   guardrail_intervened→CONTENT_FILTER or ERROR — decide: CONTENT_FILTER). `usage`:
   inputTokens→usage_in, outputTokens→usage_out, cacheReadInputTokens→usage_cache_read,
   cacheWriteInputTokens→usage_cache_write. A malformed/absent output.message → return -1 + err.
3. **Opaque-verbatim discipline (IR trust boundary).** tool_id / tool_name / tool_input are
   carried VERBATIM (Converse toolUseId/name/input ↔ IR tool_id/tool_name/tool_input) — a
   round-trip preserves them byte-for-byte (the IR header's opaque-names rule).
4. **Entrypoints in `aimee_backend.h`** next to the anthropic/openai/responses decls. No dispatch
   wiring in aimee_ir_serve.c this slice (that is the deferred egress — nothing selects the
   bedrock backend yet; the entrypoints + test are the deliverable).

## Scope (P6c-ir)

1. `src/server/aimee_backend_bedrock.c` + decls in `src/headers/aimee_backend.h`:
   `bedrock_converse_build` + `bedrock_converse_parse` (+ static block_to_converse /
   blocks_to_converse / converse_stop_reason helpers). Add to `SERVER_SRCS`.
2. `src/tests/test_aimee_backend_bedrock.c` + `unit-test-aimee-backend-bedrock` in Rules.mk
   (mirror unit-test-aimee-backend's link list). Tests:
   (a) build: an IR request (system text + a user text message + an assistant tool_use + a user
   tool_result + 2 tools + max_tokens/temperature/topP/stop) → assert the Converse JSON has the
   exact paths: `system[0].text`, `messages[].role/content[].text`,
   `content[].toolUse.{toolUseId,name,input}`, `content[].toolResult.{toolUseId,content,status}`,
   `inferenceConfig.{maxTokens,temperature,topP,stopSequences}`,
   `toolConfig.tools[].toolSpec.{name,description,inputSchema.json}`; absent optionals are
   OMITTED (no maxTokens key when !has_max_tokens); the body is identical whether stream=0/1.
   (b) parse: a hand-written Converse response (`output.message.content` = text + toolUse,
   `stopReason`=tool_use, `usage` with all 4 token fields) → assert out->content blocks,
   stop_reason==TOOL_USE, usage_in/out/cache_read/cache_write; a missing output.message → -1.
   (c) round-trip: tool_id/tool_name preserved verbatim through build; a THINKING block →
   reasoningContent and back; an unknown stopReason string → UNKNOWN + raw_stop_reason kept.
   (d) no-catch-all: the builder never emits an OpenAI/Anthropic-shaped key (a spot check that a
   TOOL_USE becomes `toolUse`, NOT `tool_calls` or `input_schema`).

## Explicitly deferred (P6c-egress)

The STREAMING Converse parser (`contentBlockDelta`/`messageStart`/`messageStop`/`metadata` →
`aimee_delta_t`, the analogue of `openai_chunk_to_deltas`) — it consumes the kb-side P6b
eventstream decoder + needs cross-target linkage; the dispatch WIRING (selecting the bedrock
backend for a `provider='bedrock'` catalog row) in the egress path; `additionalModelRequestFields`
per-family (top_k etc.); the native `InvokeModel` (non-Converse) adapters; image/document
`source.bytes` base64 exactness against a live endpoint; the kb dual-compile of the serializer.

## Gate

- `make -j server` links clean (the new backend compiles into aimee-server; aimee-kb unaffected —
  the file is server-side, `kb-target-isolation-check` stays green); `make lint` green; new files
  clang-format-19 clean; `make schema-sync-check` unaffected (no schema).
- `unit-test-aimee-backend-bedrock` builds + PASSES — the Converse build/parse paths + the
  opaque round-trip + the no-catch-all check are the headline. No DB / no real-PG (pure offline).

## Non-goals (P6c-ir)

No streaming parser, no egress/dispatch wiring, no SigV4 call, no eventstream coupling, no native
InvokeModel, no additionalModelRequestFields, no kb linkage, no live Bedrock. Pure Converse↔IR
request+response serializer — the 4th backend adapter — fixture-tested against AWS's documented
Converse schema, completing the pure Bedrock cores (auth + framing + IR-map).

## v2 refinements (roundtable-converged; exact Converse wire fidelity)

- **`toolChoice` wraps auto/any in an OBJECT, never bare strings:** `{"auto":{}}` | `{"any":{}}`
  | `{"tool":{"name":"X"}}`. Read the opaque `ir->tool_choice`'s `type` (auto|any|tool + name,
  the Anthropic-style shape the IR carries); an unrecognized/absent shape → OMIT `toolChoice`
  entirely (safe default), never emit a malformed one.
- **`image.source.bytes` is a base64 STRING.** Emit `{image:{format:<from media_type>,source:
  {bytes:<media_ref>}}}` ONLY when `media_ref` is a base64 payload; if `media_ref` is a URL
  (the IR allows either), Converse has no URL image input on the generic path — OMIT the block
  (documented; S3 `source.s3Location` is a P6c-egress concern). Never put a URL in `source.bytes`.
- **`reasoningContent` includes the signature + is family-shaped.** THINKING → `{reasoningContent:
  {reasoningText:{text:<text>, signature:<thinking_signature when present>}}}`. (It is a
  Claude-on-Bedrock feature; the serializer emits it whenever the IR carries a THINKING block —
  the IR only has one if the request had it — and family-appropriateness is the catalog/egress's
  job, not the serializer's.)
- **`toolResult.content[]` is typed parts:** a plain-string `tool_result` → `[{text:<str>}]`; a
  structured (object/array) `tool_result` → `[{json:<dup>}]`. `status` = `tool_is_error ?
  "error" : "success"`.
- **`inferenceConfig` is OMITTED ENTIRELY when no sub-field is set** — never emit an empty
  `inferenceConfig:{}`. Same for `toolConfig` (omit when `n_tools==0` AND no toolChoice) and
  `system` (omit when no system blocks produce a part). Test (a) asserts the empty-omission.
- **`stopReason` — map the known set, everything else → UNKNOWN + raw preserved.** end_turn→
  END_TURN, tool_use→TOOL_USE, max_tokens→MAX_TOKENS, stop_sequence→STOP_SEQUENCE,
  content_filtered→CONTENT_FILTER, guardrail_intervened→CONTENT_FILTER; ANY other/unknown value
  (e.g. `context_exceeded`) → AIMEE_STOP_UNKNOWN. In EVERY case `raw_stop_reason` keeps the
  provider string verbatim, so no information is lost and the egress can distinguish
  guardrail-vs-filter from the raw.
- **`inputSchema.json` is the tool's JSON-Schema object, duplicated as-is** (`cJSON_Duplicate`);
  the IR `aimee_tool_t.schema` already holds a JSON-Schema object. (Numeric/whitespace/key-order
  reformatting from the dup is semantically irrelevant for a schema — acceptable.)
- **top_k-only fidelity (documented limitation):** Converse has no `inferenceConfig.topK` (it is
  a per-family `additionalModelRequestFields` value, deferred to P6c-egress). A request that sets
  ONLY `top_k` (not `top_p`) therefore serializes with no `topK` this slice — documented; the
  egress adds `additionalModelRequestFields`. `top_p` (has_top_p) → `topP` as normal.

### Gate additions

- (a2) a request with NO inference params + NO tools + NO system → the body has NO
  `inferenceConfig`, NO `toolConfig`, NO `system` keys (empty-omission).
- (b2) a Converse response with `stopReason:"guardrail_intervened"` → stop_reason preserved in
  `raw_stop_reason` verbatim (and mapped to CONTENT_FILTER); an unknown stopReason → UNKNOWN +
  raw kept.
- (e) `toolChoice` round-trips as `{"auto":{}}` / `{"tool":{"name":…}}` (object-wrapped, not a
  bare string).
