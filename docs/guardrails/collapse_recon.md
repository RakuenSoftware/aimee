# Guardrail-collapse serving and relay reconnaissance

Phase 0 inventory, verified against the current source. Line numbers are anchors for this checkout.

## Binding path decision

**Decision: paths converge on one verified typed post-parse relay seam:** `src/server/aimee_ir_stream.c:42`, `openai_chunk_to_deltas`, producing `aimee_delta_t`; Anthropic consumes it at `src/server/aimee_ir_stream.c:539`, `anthropic_delta_emit`. Phase 2 taps this seam. Legacy translators remain compatibility paths.

| surface | request handler | emitter/consumer | verdict and verified types | scanner precedent |
|---|---|---|---|---|
| `/v1/messages` | `src/server/anthropic_http.c:1071`, `messages_stream` | `src/server/anthropic_http.c:973`, `messages_stream_ir_relay`; `openai_chunk_to_deltas` -> `anthropic_delta_emit` at `:697-703` | converges on `aimee_delta_t`; enum values are `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_TEXT`, `AIMEE_DELTA_TOOL_CALL`, `AIMEE_DELTA_TURN_END` in `src/server/aimee_ir_stream.h` | `src/server/aimee_ir_shadow.c:31`, `aimee_ir_shadow_compare_bodies` |
| `/v1/responses` | `src/server/openai_chat.c:1081`, `responses_stream_handler`; dispatch `src/server/server_http.c:1260` | `sse_event_emit` at `:1260`; typed response path `src/server/aimee_ir_serve.c:248-275` | converges after IR parsing; wire event names are handler strings, not a delta enum | `src/server/aimee_ir_shadow.c:129`, `aimee_ir_shadow_compare_response` |
| `/v1/chat/completions` | `src/server/openai_chat.c:720`, `chat_stream_handler`; dispatch `src/server/server_http.c:1165` | `src/server/openai_chat.c:689`, `emit_chunk`; typed upstream seam `openai_chunk_to_deltas` | converges on `aimee_delta_t`; no assumed `AIMEE_DELTA_BLOCK_DELTA` | `src/server/aimee_ir_shadow.c:208`, `aimee_ir_shadow_observe_request` |
| webchat ingest | `src/server/server_http_routes.c:1621`; polling reads `db1_webchat_live_get` at `:1645` | `src/db1/webchat_live.c:10`, `db1_webchat_live_set`, and `:51`, `db1_webchat_live_get` | divergent/buffered text, not typed SSE; add an explicit adapter | `src/db1/webchat_live.c:51` |
| delegate relay | execution `src/server/agent_runtime.c:1091-1199`; shaping `src/server/agent_request_build.c:38` | parsing `parse_response_openai` at `src/server/agent_runtime.c:816` and `parse_response_anthropic` at `:1025` | divergent provider transport; converges only after IR parsing | `src/server/aimee_ir_shadow.c:129` |
| roundtable relay | `src/modules/roundtable/delegate_ensemble.c`; durable ledger `src/db1/roundtable_pipeline.c:1` | result persistence `src/db1/roundtable_pipeline.c:310-403` | divergent/buffered; add a roundtable result adapter | pipeline pass/result ledger |

The single common symbol is thus the verified typed post-parse seam, not a claim that webchat or roundtable already emit deltas. The rejected identifiers `AIMEE_DELTA_BLOCK_DELTA` and members not present in source are not anchors.

## Configuration source of truth and decision

- `config_t`: `src/modules/config/config.h:265`.
- Defaults: `src/modules/config/config.c:601-826`; flags use flat `*_enabled` names with explicit ON/OFF defaults.
- Loader: `src/modules/config/config.c:1109`, `config_load`, delegating at `:1116` to `config_load_file`.
- Generator: `scripts/gen-reference-docs.py:417`, `parse_config_sections`; it scans `src/modules/config/config_fields.c` and config sources. Output identifies itself at `docs/gen/configuration.md:3`.
- Generator/template: no external template; assembly and descriptions are in `scripts/gen-reference-docs.py` (`:340`, `:453`).

**Decision:** add fields to `config_t` and expose them through the existing section machinery as `guardrails.collapse.*` in Phase 1. No generator-extension prerequisite is needed.

## Relay choke point

The producer is `openai_chunk_to_deltas` (`src/server/aimee_ir_stream.c:42`), and the typed consumer is `anthropic_delta_emit` (`:539`). Wire rendering is `aimee_ir_stream.c:463-464` (`type` `text_delta`, `text`). The actual members and enum constants must be taken from `src/server/aimee_ir_stream.h`; no invented `aimee_delta_t` layout is authorized.
