# Guardrail-collapse binding anchors

This merged Phase 0 packet gates Phase 1. It records six binding decisions with current-source citations.

## Citation verification note

Every `file:line` citation in this document and in `collapse_recon.md` / `sampling_capability_matrix.md` was verified against the source tree at commit `d45ccc41` (2026-07-24), not against a symbol index. The symbol index may lag behind the source; the source is the authority. Each named symbol was located with `grep -n` and the surrounding context was inspected to confirm the cited line is the definition site (or the cited call site, when the citation refers to a call), not a coincidental occurrence. Reviewers re-verifying a citation should reread the cited file at the cited line; if the file has moved, the citation must be updated as part of the change that moved the file.

## Decision 1 -- Paths diverge

Phase 2 must split per handler. Only the Anthropic compatibility relay traverses `openai_chunk_to_deltas` (`src/server/aimee_ir_stream.c:42`) and `anthropic_delta_emit` (`:539`); OpenAI Chat emits at `src/server/openai_chat.c:689` (`emit_chunk`, called from `chat_stream_handler` `src/server/openai_chat.c:720`), Responses through `sse_event_emit` at `src/server/server_http.c:1260` (handler `responses_stream_handler`, `src/server/openai_chat.c:1081`), webchat through `db1_webchat_live_set`/`db1_webchat_live_get` at `src/db1/webchat_live.c:10` and `:51`, delegate relay through `agent_execute` (`src/server/agent_runtime.c:1100`) and `agent_ir_parse_json_response` (`src/posix/agent_ir_parse.c:86`), and roundtable relay through `live_panel` (`src/modules/workflows/wfe_live_panel.c:125`) calling `delegate_roundtable_run` (`src/modules/roundtable/delegate_ensemble.c:1906`) persisting via `rtp_pass_*` in `src/db1/roundtable_pipeline.c:300-403`.

The five divergent paths do not consume `aimee_delta_type_t` constants; each one's concrete replacement representation is named in `collapse_recon.md` (Responses: `"response.created"`/`"response.completed"` event-name strings at `src/server/openai_chat.c:1106,1109,1122,1125`; Chat: `finish` int at `src/server/openai_chat.c:689-696`; webchat: SQLite `webchat_live` row + `status`/`rev` columns at `src/db1/webchat_live.c:18-21`; delegate: `parsed_response_t` struct members at `src/headers/agent_protocol.h:21-37`; roundtable: `roundtable_result_t` struct + `original_request_alignment[16]` field at `src/modules/roundtable/roundtable_types.h:111-134`). Invented constants like `AIMEE_DELTA_TEXT` are rejected.

## Decision 2 -- Config source and namespace

`config_t` is at `src/modules/config/config.h:265`, defaults at `src/modules/config/config.c:601-826`, and `config_load` at `:1109`. `parse_config_sections` (`scripts/gen-reference-docs.py:417`, invoked at `:1167`) covers the config sources without an external template. Phase 1 adds existing-style fields under `guardrails.collapse.*`; no Phase 1.0 generator prerequisite is required.

## Decision 3 -- Relay choke points and verified symbols

There is no repository-wide choke point; Phase 2 uses the handler sites in `collapse_recon.md`. The typed Anthropic path uses `aimee_delta_type_t` and `aimee_delta_t` from `src/headers/aimee_ir.h:186-210`. Its actual lifecycle constants are `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA`, `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`, and `AIMEE_DELTA_ERROR` (`:188-193`). Text resides in `aimee_delta_t.text_delta` for `AIMEE_DELTA_BLOCK_DELTA` (`:196-210`) and is rendered at `src/server/aimee_ir_stream.c:450-464`. The other five surfaces (`/v1/responses`, `/v1/chat/completions`, webchat, delegate, roundtable) do not traverse this envelope and require handler-specific adapters (Phase 2.1 and Phase 2.2). Each divergent surface's replacement representation is cited in `collapse_recon.md` (Responses: `"response.created"`/`"response.completed"` strings; Chat: `finish` int; webchat: `webchat_live` row; delegate: `parsed_response_t`; roundtable: `roundtable_result_t`).

## Decision 4 -- Sampling scope

Phase 4 is constrained to the nine requested controls and continuation/prefix primitives documented in `sampling_capability_matrix.md`, grounded in `src/server/model_sampling.c:1-90`, `src/server/aimee_backend_openai.c:43-170`, `src/server/aimee_backend_anthropic.c:168-239`, and `src/server/aimee_backend_bedrock.c:366-444`. Every absent control listed there is a Phase 4.0 prerequisite.

## Decision 5 -- Promotion substrate

Existing promotion is manual: `handle_optimize_promote` is at `src/server/server_state.c:931`, backed by `db2_bandit_promotion_set` at `src/db2/bandit.c:341`; verified arm-stat updates are `db2_bandit_arm_stats_update` at `src/db2/bandit.c:180`. No shadow->canary->default bucketed calibration transition is present. **Phase 5.0 deliverable:** `docs/guardrails/collapse_promotion_bucketing.md` specifies shadowed, canary, and default bucket states, calibration queries, and introduces bucketing in `src/server/server_state.c` around `handle_optimize_promote`; persistence extends `db2_bandit_promotion_set` (`src/db2/bandit.c:341`) before Phase 5 wires collapse promotion.

## Decision 6 -- Audit-store schema

The existing record is the SQLite `audit_event` row declared at `src/modules/audit/audit_worm.c:29-49`. Its discriminator is `action`; the structured collapse payload goes in `detail`, alongside `actor_role`, `actor_principal`, `subject`, and `verdict`. The WORM API is `audit_worm_append` (`:135`), registration/opening is `audit_worm_init_at` (`:125`), checkpoint/sealing are `:233` and `:508`, and the query surface is `audit_worm_read_page` (`:587`). Decision: `collapse_event` is an `audit_event` structured action (`action = collapse_event`, JSON in `detail`), not a new physical record type. **Phase 2.3.0** defines and registers the action/detail schema before Phase 2.3 emits events.

**Gate:** Phase 1 implementation starts only after this document is merged.
