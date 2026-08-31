# P6c-stream plan: Bedrock Converse stream-event → IR delta parser (P6 §2)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice of P6 (Bedrock). Branch off `testing`. Completes the Bedrock Converse IR mapping: P6c-ir
did the request + NON-streaming response; this does the STREAMING response, mapping each
Converse `ConverseStream` event to `aimee_delta_t` events, the direct analogue of the existing
`openai_chunk_to_deltas`. It bridges the merged P6b eventstream FRAMING decoder (which yields a
per-frame `:event-type` header + payload JSON) to aimee's IR stream-delta surface. With it, the
Bedrock pure cores are complete (auth + framing + request + non-stream-resp + stream-resp) and
the deferred P6c-egress is pure wiring.

## Verified substrate (from the IR map)

- `aimee_ir.h` stream-delta surface: `aimee_delta_type_t` {TURN_START, BLOCK_START, BLOCK_DELTA,
  BLOCK_STOP, TURN_STOP, ERROR}; `aimee_delta_t` {type, block_id, kind (aimee_block_type_t),
  tool_id, tool_name (BLOCK_START of tool_use), text_delta (BLOCK_DELTA text/thinking),
  tool_args_delta (BLOCK_DELTA tool_use arg-JSON fragment), stop_reason + usage_in/usage_out
  (TURN_STOP), error_message (ERROR)}.
- Existing analogue: `openai_chunk_to_deltas(const cJSON *chunk, openai_stream_state_t *st,
  aimee_delta_t *out, int max)` (`aimee_ir_stream.h:31`, impl `aimee_ir_stream.c:39`), with
  `openai_stream_state_t` tracking `tool_block[AIMEE_STREAM_MAX_TOOLS=64]`. MIRROR its shape,
  state pattern, and the `out[max]` bounded fill.
- The P6b decoder (`modules/aws/aws_eventstream.h`, kb-side) already exposes the frame's
  `:event-type` header + the payload {ptr,len}. This parser consumes (event_type, payload JSON).
It does NOT re-parse the binary framing (P6b's job). It is pure JSON→deltas, server-side.
- `converse_stop_reason` mapping already exists in `server/aimee_backend_bedrock.c` (P6c-ir),
reuse it for messageStop's stopReason.

## The Converse stream events (AWS-documented, the mapping source)

Each `ConverseStream` event is one eventstream frame; its `:event-type` header names the shape:
- `messageStart`: `{role}` → **TURN_START**.
- `contentBlockStart`: `{contentBlockIndex, start:{toolUse:{toolUseId,name}}}` (start present
  only for a tool_use block; a text block has no start) → **BLOCK_START** (kind=TOOL_USE with
  tool_id/tool_name, or kind=TEXT).
- `contentBlockDelta`: `{contentBlockIndex, delta:{text:…} | {toolUse:{input:…}} |
  {reasoningContent:{text:…}}}` → **BLOCK_DELTA** (text_delta for text/reasoning;
  tool_args_delta for the toolUse.input JSON fragment).
- `contentBlockStop`: `{contentBlockIndex}` → **BLOCK_STOP**.
- `messageStop`: `{stopReason, additionalModelResponseFields?}` → **TURN_STOP** (stop_reason).
- `metadata`: `{usage:{inputTokens,outputTokens,…}, metrics}` → **TURN_STOP** usage (or a
  usage-carrying delta; decide: emit a TURN_STOP-usage delta, OR fold usage into the messageStop
  TURN_STOP, since messageStop precedes metadata, emit a SEPARATE delta for metadata usage; the
  renderer sums. DECIDE at review: a second TURN_STOP carrying only usage is cleanest + matches
  how the frontend consumes usage_in/out).

## Design decisions

1. **`bedrock_converse_stream_to_deltas(const char *event_type, const cJSON *payload,
   converse_stream_state_t *st, aimee_delta_t *out, int max) → int`** (returns #deltas written,
   ≤max, or -1 on a malformed event). Pure, server-side, in `aimee_ir_stream.{c,h}` next to
   `openai_chunk_to_deltas`. No I/O, no eventstream framing (that's P6b upstream).
2. **`converse_stream_state_t`**: tracks, per `contentBlockIndex` (bounded array like the
   openai state, `AIMEE_STREAM_MAX_TOOLS`), whether the open block is TEXT or TOOL_USE, so a
   `contentBlockDelta` knows its `kind` (text_delta vs tool_args_delta). Converse's delta does
   carry the type, but the state also gives BLOCK_STOP its kind and guards a delta with no prior
   start. `block_id` in the emitted delta = the `contentBlockIndex`.
3. **Bounded + defensive.** Every field read is type-checked (an absent/wrong-typed field →
   skip that delta, not a crash); `out` is filled up to `max` (a single event maps to a small,
   bounded number of deltas, typically 1); an unknown `event_type` → 0 deltas (ignored, not an
   error, forward-compat with new Converse event types); a malformed payload for a KNOWN event
   → -1 + the caller drops the stream (consistent with P6b's fatal-ERROR posture).
4. **stopReason reuse.** messageStop's `stopReason` → `converse_stop_reason` (the P6c-ir
   mapping) → the TURN_STOP delta's `stop_reason`; the raw string is not carried on the delta
   (the delta struct has no raw field; that is fine; the non-stream parse keeps raw).

## Scope (P6c-stream)

1. `bedrock_converse_stream_to_deltas` + `converse_stream_state_t` in
   `src/server/aimee_ir_stream.c` + decls in `src/headers/aimee_ir_stream.h` (mirror the openai
   entrypoints). Reuse `converse_stop_reason` from aimee_backend_bedrock (expose it via
   aimee_backend.h if it is static. Decide: make it a shared `aimee_backend.h` decl, or
   re-derive a tiny local mapping; lean: expose `converse_stop_reason` in aimee_backend.h so
   there is one source of truth).
2. `src/tests/test_aimee_converse_stream.c` + `unit-test-aimee-converse-stream` in Rules.mk
   (mirror `unit-test-aimee-ir-stream`'s link list: test.o + aimee_ir_stream.o + aimee_ir.o +
   aimee_backend_bedrock.o [for converse_stop_reason] + cJSON.o). Tests:
   (a) `messageStart{role:assistant}` → 1 TURN_START.
   (b) `contentBlockStart{contentBlockIndex:0, start:{toolUse:{toolUseId,name}}}` → BLOCK_START
       kind=TOOL_USE with tool_id/tool_name; a text block start (index 1, no start) → BLOCK_START
       kind=TEXT.
   (c) `contentBlockDelta{index:1, delta:{text:"hi"}}` → BLOCK_DELTA text_delta="hi" block_id=1;
       `contentBlockDelta{index:0, delta:{toolUse:{input:"{\"p\":"}}}` → BLOCK_DELTA
       tool_args_delta the fragment, block_id=0 (state says index 0 is tool_use);
       `contentBlockDelta{index:2, delta:{reasoningContent:{text:"…"}}}` → BLOCK_DELTA text_delta.
   (d) `contentBlockStop{index:0}` → BLOCK_STOP block_id=0.
   (e) `messageStop{stopReason:"tool_use"}` → TURN_STOP stop_reason=TOOL_USE;
       `messageStop{stopReason:"guardrail_intervened"}` → TURN_STOP CONTENT_FILTER; an unknown
       stopReason → TURN_STOP UNKNOWN.
   (f) `metadata{usage:{inputTokens:12,outputTokens:34}}` → a TURN_STOP delta with
       usage_in=12/usage_out=34.
   (g) an unknown event_type → 0 deltas (ignored, forward-compat); a malformed KNOWN event
       (e.g. contentBlockDelta with delta not an object) → -1.
   (h) a full sequence (messageStart → 2×contentBlockStart → deltas → contentBlockStop →
       messageStop → metadata) drives the state correctly and each event's block_id/kind is right.

## Explicitly deferred (P6c-egress)

The WIRING that pulls frames off the P6b decoder (kb-side), reads each `:event-type` + payload,
and calls this parser (cross-target: P6b is kb, this is server; the dual-compile vault-core
idiom lands it in both, a P6c-egress task); the live streaming egress; the frontend delta
RENDERER already exists (anthropic_delta_render) and consumes aimee_delta_t unchanged.

## Gate

- `make -j server` links clean (server + kb; the parser is server-side, `kb-target-isolation`
  stays green, but if `converse_stop_reason` is now shared from aimee_backend_bedrock and the kb
  target does NOT link that, confirm no kb reference is introduced; the parser is server-only so
  kb is unaffected); `make lint` green; new files clang-format-19 clean; `make schema-sync-check`
  unaffected.
- `unit-test-aimee-converse-stream` builds + PASSES, the event→delta mapping matrix + the state
  tracking + the unknown-event/ malformed handling are the headline. Pure offline, no DB.

## Non-goals (P6c-stream)

No eventstream framing (P6b), no egress wiring, no cross-target linkage, no live stream, no
frontend renderer change. Pure Converse-stream-event → aimee_delta_t parser, the streaming half
of the Converse response mapping, fixture-tested vs AWS's documented ConverseStream events,
completing the Bedrock pure cores.

## v2 refinements (roundtable-converged; ConverseStream fidelity + robustness)

- **BLOCK_DELTA kind comes from the delta's OWN variant, not state.** `contentBlockDelta.delta`
  is a union that self-identifies: `{text:…}`→text_delta (kind TEXT); `{toolUse:{input:…}}`→
  tool_args_delta (kind TOOL_USE; `input` is a JSON-STRING fragment accumulated across deltas.
Emit the fragment verbatim as tool_args_delta, do NOT parse it); `{reasoningContent:{text:…}}`→
  text_delta (kind THINKING). An unknown delta variant within a well-formed contentBlockDelta
  (e.g. `citation`, `reasoningContent.redactedContent`, a signature-only reasoning delta) → emit
  0 deltas for it (forward-compat SKIP, NOT -1). Per-index state tracks the block KIND (text /
  tool_use / thinking, three, not two) ONLY so BLOCK_STOP can carry the right `kind`.
- **contentBlockStart: TOOL_USE iff `start.toolUse` is present, else TEXT.** Handle `start`
  absent OR an empty object OR a non-toolUse union all as a TEXT block-start (do not require
  `start`). A `start.toolUse` → BLOCK_START kind=TOOL_USE with tool_id/tool_name.
- **Stream-side EXCEPTION events → an ERROR delta.** The known ConverseStream exception
  event-types, `internalServerException`, `modelStreamErrorException`, `validationException`,
  `throttlingException`, `serviceUnavailableException`, map to AIMEE_DELTA_ERROR with
  `error_message` from the payload's `message` field (so the stream surfaces the AWS error, not
  silently ignores it). A genuinely-unknown event_type (a future Converse event) → 0 deltas
  (ignore, forward-compat). A structurally-malformed KNOWN event (e.g. contentBlockDelta whose
  `delta` is not an object) → -1 (drop the stream, P6b-consistent).
- **Usage on TURN_STOP is consistent with `openai_chunk_to_deltas`.** That analogue already
  emits a TURN_STOP carrying `usage_in/usage_out` (incl. a usage-only final chunk), and the
  renderer reads usage from whichever TURN_STOP carries it. So: `messageStop{stopReason}` →
  TURN_STOP with stop_reason (usage 0); `metadata{usage}` → TURN_STOP with usage_in=inputTokens,
  usage_out=outputTokens (stop_reason UNKNOWN/0). `aimee_delta_t` has only usage_in/usage_out
  (no cache fields). Cache tokens are dropped on the stream path (the non-stream parse keeps
  them); documented. This two-TURN_STOP shape matches the existing OpenAI streaming contract, so
  no renderer change is needed.
- **`converse_stop_reason` is currently `static` in aimee_backend_bedrock.c**: expose it via
  `aimee_backend.h` as the single source of truth and reuse it for messageStop (do not re-derive
  a second mapping). The test link list therefore includes `aimee_backend_bedrock.o`.

### Gate additions

- (i) an interleaved sequence, text block 0 + tool_use block 1 + reasoning block 2 streaming,
  then BLOCK_STOP for each, asserts each BLOCK_STOP carries the right kind (state correctness).
- (j) a `throttlingException` event → 1 ERROR delta with error_message; an unknown event → 0.
- (k) a contentBlockDelta with an unknown delta variant (e.g. `citation`) → 0 deltas (skip), NOT
  -1; a contentBlockDelta whose `delta` is not an object → -1.
