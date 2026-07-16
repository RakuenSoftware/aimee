# IR-only response parsing

The canonical IR (`aimee_request_t` / `aimee_response_t`, with typed blocks
`TEXT`, `TOOL_USE`, `THINKING`) is now the **sole response parser on every wire**
(`anthropic`, `openai-chat`, `responses`/`codex`). The legacy per-wire
translators — `agent_parse_response_openai`, `agent_parse_response_anthropic`,
and `agent_parse_response_responses` in `server/agent_bridge.c` — are gone.

This completes the "legacy-translator deletion (Slice 5)" deferred in
`docs/proposals/done/aimee-canonical-ir.md`. Every existing response path now
flows through one IR, one parser family, one block taxonomy.

## What changed

The migration shipped as six staged, CI-gated, shadow-validated PRs, each
removing a piece of the dual-parser world:

1. **JSON wires go IR-only in the delegate runtime.** Removed the legacy
   fallback and the legacy-vs-IR response shadow comparator for
   `anthropic` and `openai-chat`; deleted the dead `AIMEE_IR_RESPONSE_PATH`
   flag. (#1398)
2. **New IR parser for the `responses` / SSE codex wire**
   (`agent_ir_parse_responses`): extracts the response object from the SSE
   stream, parses via `responses_backend_parse`, owns the XML tool-call
   rescue, and sets `assistant_message` to the output-item array for
   multi-turn replay. The codex wire becomes IR-primary. (#1399)
3. **Delegate driver** (`server/delegate_openai.c`) parses via the IR.
   (#1407)
4. **Gateway proxy no-driver fallbacks** (`server/anthropic_http.c`) parse
   via the IR. (#1412)
5. **Simple completion runtime** (`server/agent_runtime.c` `agent_execute`)
   parses via the IR. (#1412)
6. **Legacy parsers and dead helpers deleted.** `strip_thinking_blocks`,
   `openai_content_to_text`, `parse_capture_model`, and the three
   `agent_parse_response_*` translators — ~342 lines removed. (#1412)

## Reasoning is stored, not stripped

Reasoning models on the openai wire embed chain-of-thought inline in
content: as a `<think>...</think>` prefix, as `thinking` / `reasoning`
items in a content-parts array, or in a `reasoning_content` field. The
legacy parser **stripped and discarded** all of it. The IR openai parser
now **splits** it:

- the chain-of-thought is **stored** as an `AIMEE_BLK_THINKING` block;
- the answer is stored as a `TEXT` block;
- the content accessor excludes `THINKING`, so callers continue to see
  only the answer — but the reasoning is preserved as structured data,
  available to anything that opts into the IR.

The split also closed real gaps that had shipped unnoticed: malformed
content-as-array handling (e.g. Mistral-style shapes) and scaffold
stripping had never been exercised by the shadow because the `.254` test
box is codex-only, so the legacy-vs-IR comparator never saw those openai
edge cases. With the IR as the sole parser, those shapes now flow through
one well-tested path.

## What was kept

Deleting the legacy translators did not mean deleting their neighbors:

- **SSE helper subtree** + `agent_responses_sse_response_object` — kept;
  shared with the IR `responses` extractor.
- **`server/delegate_xml_fallback.c`** — kept; the XML dialect parser the
  IR rescue depends on.
- **`server/shadow_mirror.c`** — kept; the #1382 shadow-traffic mirror
  still records traffic.

Only the legacy parse-comparator was removed. The shadow traffic mirror
remains available for future wire regressions.

## Follow-ups

- **Reasoning → history.** Reasoning now lives in the IR
  `aimee_response_t`, but the transitional legacy `parsed_response_t`
  bridge has no `THINKING` field, so the reasoning is dropped at that
  boundary. Surfacing it end-to-end (persisting thinking to history) is
  a follow-up.
- **Read the shadow parity metrics.** `aimee_ir_metric_get` currently
  has no callers, so the shadow parity metrics are write-only. Wiring
  a reader is a follow-up.
