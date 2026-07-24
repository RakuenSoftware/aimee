# Guardrail-collapse binding anchors

This merged Phase 0 packet gates Phase 1. It records six decisions with current-source citations.

1. **Path divergence:** use the verified typed post-parse seam `openai_chunk_to_deltas` / `aimee_delta_t` at `src/server/aimee_ir_stream.c:42`, with `anthropic_delta_emit` at `:539`; webchat (`src/db1/webchat_live.c:10`) and roundtable (`src/db1/roundtable_pipeline.c:310`) require adapters. Phase 2 must keep those adapters separate.
2. **Config:** `config_t` at `src/modules/config/config.h:265`, loaded by `config_load` at `src/modules/config/config.c:1109`; generator `scripts/gen-reference-docs.py:417` covers the struct/config tables and emits `docs/gen/configuration.md:3`. New fields go under `guardrails.collapse.*`; existing `*_enabled` defaults are explicit in `config.c:601-826`.
3. **Relay choke point:** `openai_chunk_to_deltas` (`src/server/aimee_ir_stream.c:42`) produces typed deltas, `anthropic_delta_emit` (`:539`) consumes them, and wire `text_delta` rendering is `:463-464`. No assumed enum/member names are permitted.
4. **Sampling:** matrix scope is the nine requested controls plus continuation/prefix primitives, constrained by backend builders (`src/server/agent_bridge.c:165-321`; `src/server/aimee_backend_openai.c:43-56`; `src/server/aimee_backend_anthropic.c:168-210`; `src/server/aimee_backend_bedrock.c:366`). Missing controls/plumbing are Phase 4.0 prerequisites.
5. **Promotion gate:** bandit promotion substrate exists at `src/server/server_state.c:929` (`optimize.promote`), with arm statistics in `src/db2/bandit.c:180`. No verified shadow→canary→default calibration transition exists; Phase 5.0 introduces bucketed calibration before wiring collapse promotion.
6. **Audit store:** WORM record schema/triggers are initialized in `src/modules/audit/audit_worm.c:29-49`; append API `audit_worm_append` is at `:135`; registration/init is `audit_worm_init_at` at `:125`; checkpoint/seal are `:233` and `:508`; query is `audit_worm_read_page` at `:587`. Decision: `collapse_event` is a new structured action record represented in the existing WORM fields, planned in Phase 2.3.0; no new physical table is required.

**Gate:** Phase 1 implementation starts only after this document is merged.
