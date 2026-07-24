# Guardrail-collapse binding anchors

This merged Phase 0 packet gates Phase 1. It records six binding decisions with current-source citations.

## Decision 1 — Paths diverge

Phase 2 must split per handler. Only the Anthropic compatibility relay traverses `openai_chunk_to_deltas` (`src/server/aimee_ir_stream.c:42`) and `anthropic_delta_emit` (`:539`); OpenAI Chat emits at `src/server/openai_chat.c:689`, Responses through `src/server/server_http.c:1260`, webchat through `src/db1/webchat_live.c:10`, and roundtable persists at `src/db1/roundtable_pipeline.c:310-403`.

## Decision 2 — Config source and namespace

`config_t` is at `src/modules/config/config.h:265`, defaults at `src/modules/config/config.c:601-826`, and `config_load` at `:1109`. `parse_config_sections` (`scripts/gen-reference-docs.py:417`, invoked at `:1167`) covers the config sources without an external template. Phase 1 adds existing-style fields under `guardrails.collapse.*`; no Phase 1.0 generator prerequisite is required.

## Decision 3 — Relay choke points and verified symbols

There is no repository-wide choke point; Phase 2 uses the handler sites in `collapse_recon.md`. The typed Anthropic path uses `aimee_delta_type_t` and `aimee_delta_t` from `src/headers/aimee_ir.h:186-210`. Its actual lifecycle constants are `AIMEE_DELTA_TURN_START`, `AIMEE_DELTA_BLOCK_START`, `AIMEE_DELTA_BLOCK_DELTA`, `AIMEE_DELTA_BLOCK_STOP`, `AIMEE_DELTA_TURN_STOP`, and `AIMEE_DELTA_ERROR` (`:188-193`). Text resides in `aimee_delta_t.text_delta` for `AIMEE_DELTA_BLOCK_DELTA` (`:196-210`) and is rendered at `src/server/aimee_ir_stream.c:450-464`.

## Decision 4 — Sampling scope

Phase 4 is constrained to the nine requested controls and continuation/prefix primitives documented in `sampling_capability_matrix.md`, grounded in `src/server/model_sampling.c:1-90`, `src/server/aimee_backend_openai.c:43-170`, `src/server/aimee_backend_anthropic.c:168-239`, and `src/server/aimee_backend_bedrock.c:366-444`. Every absent control listed there is a Phase 4.0 prerequisite.

## Decision 5 — Promotion substrate

Existing promotion is manual: `handle_optimize_promote` is at `src/server/server_state.c:931`, backed by `db2_bandit_promotion_set` at `src/db2/bandit.c:341`; verified arm-stat updates are `db2_bandit_arm_stats_update` at `src/db2/bandit.c:180`. No shadow→canary→default bucketed calibration transition is present. **Phase 5.0 deliverable:** `docs/guardrails/collapse_promotion_bucketing.md` specifies bucketing and introduces its implementation in `src/server/server_state.c` before Phase 5 wires collapse promotion.

## Decision 6 — Audit-store schema

The existing record is the SQLite `audit_event` row declared at `src/modules/audit/audit_worm.c:29-49`. Its discriminator is `action`; the structured collapse payload goes in `detail`, alongside `actor_role`, `actor_principal`, `subject`, and `verdict`. The WORM API is `audit_worm_append` (`:135`), registration/opening is `audit_worm_init_at` (`:125`), checkpoint/sealing are `:233` and `:508`, and the query surface is `audit_worm_read_page` (`:587`). Decision: `collapse_event` is an `audit_event` structured action (`action = collapse_event`, JSON in `detail`), not a new physical record type. **Phase 2.3.0** defines and registers the action/detail schema before Phase 2 emits it.

**Gate:** Phase 1 implementation starts only after this document is merged.
