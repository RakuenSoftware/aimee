/* config_fields.c: shared get/set allowlist for top-level config_t fields.
 *
 * Extracted from cmd_data.c so the legacy `aimee config` command AND the
 * server's config.show/get/set handlers operate on one table. CORE layer:
 * depends only on config.h and cJSON.
 *
 * NB: the three guardrails_semantic_*_threshold fields are doubles; they were
 * historically (mis)typed CFG_STRING in cmd_data.c, which only worked because
 * that table was unreachable. They are CFG_FLOAT here. */
#include "config_accessors.h" /* config_field_read */
#include "config_fields.h"
#include "runtime_secret.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Entries omit the trailing reload_class -> RELOAD_HOT (0) by C zero-fill; suppress the
 * pedantic missing-field-initializer warning for the whole intentional table (P2). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const config_field_t config_fields[] = {
    {"db2_url", offsetof(config_t, db2_url), sizeof(((config_t *)0)->db2_url), 0, CFG_STRING,
     RELOAD_RESTART, FGROUP_RUNTIME, "AIMEE_DB2_URL"}, /* the postgres pool is opened at startup */
    {"provider", offsetof(config_t, provider), sizeof(((config_t *)0)->provider), 0, CFG_STRING},
    {"default_persona", offsetof(config_t, default_persona),
     sizeof(((config_t *)0)->default_persona), 0, CFG_STRING},
    {"claude_model", offsetof(config_t, claude_model), sizeof(((config_t *)0)->claude_model), 0,
     CFG_STRING},
    {"openai_endpoint", offsetof(config_t, openai_endpoint),
     sizeof(((config_t *)0)->openai_endpoint), 0, CFG_STRING},
    {"openai_model", offsetof(config_t, openai_model), sizeof(((config_t *)0)->openai_model), 0,
     CFG_STRING},
    {"openai_key_cmd", offsetof(config_t, openai_key_cmd), sizeof(((config_t *)0)->openai_key_cmd),
     0, CFG_STRING},
    {"guardrail_mode", offsetof(config_t, guardrail_mode), sizeof(((config_t *)0)->guardrail_mode),
     0, CFG_STRING},
    /* Embedder. The env names are declared HERE so config ingests them at startup
     * like every other field. They used to be read by ad-hoc getenv() in
     * config_database.c while these rows declared no env var at all — which is
     * why they had to be hand-plumbed through compose YAML as a second surface. */
    {"embedder_command", offsetof(config_t, embedder_command),
     sizeof(((config_t *)0)->embedder_command), 0, CFG_STRING},
    {"embedder_model", offsetof(config_t, embedder_model), sizeof(((config_t *)0)->embedder_model),
     0, CFG_STRING, RELOAD_HOT, FGROUP_RUNTIME},
    {"embedder_url", offsetof(config_t, embedder_url), sizeof(((config_t *)0)->embedder_url), 0,
     CFG_STRING, RELOAD_HOT, FGROUP_RUNTIME},
    {"embedder_api_key", offsetof(config_t, embedder_api_key),
     sizeof(((config_t *)0)->embedder_api_key), 0, CFG_STRING, RELOAD_HOT, FGROUP_RUNTIME,
     "EMBEDDER_API_KEY"},
    /* One-way door once anything is embedded: DB2 records the column width and
     * refuses startup on drift. 1..EMBED_MAX_DIM (4000, the DB2 ceiling). */
    {"embedder_dims", offsetof(config_t, embedder_dims), sizeof(((config_t *)0)->embedder_dims), 0,
     CFG_INT, RELOAD_RESTART, FGROUP_RUNTIME},
    /* Setup-wizard page 2: KB mode + per-role LLM backend record (see config.h).
     * All wizard-settable; the deploy layer reads them. RELOAD_RESTART because the
     * deploy topology (what containers run) only changes on a restart. */
    {"kb_mode", offsetof(config_t, kb_mode), sizeof(((config_t *)0)->kb_mode), 0, CFG_STRING,
     RELOAD_RESTART},
    {"kb_client_url", offsetof(config_t, kb_client_url), sizeof(((config_t *)0)->kb_client_url), 0,
     CFG_STRING, RELOAD_RESTART},
    {"kb_client_bearer_token", offsetof(config_t, kb_client_bearer_token),
     sizeof(((config_t *)0)->kb_client_bearer_token), 0, CFG_STRING, RELOAD_RESTART, FGROUP_RUNTIME,
     "AIMEE_KB_CLIENT_BEARER_TOKEN"},
    /* A fact about the running image, not a preference: the Dockerfile sets
     * AIMEE_WITH_LLAMACPP in every variant. RELOAD_RESTART because it cannot change
     * without replacing the image. */
    {"aimee_with_llamacpp", offsetof(config_t, aimee_with_llamacpp),
     sizeof(((config_t *)0)->aimee_with_llamacpp), 0, CFG_STRING, RELOAD_RESTART, FGROUP_RUNTIME},
    /* Which model the image bakes. Also image-set: it cannot change without
     * pulling a different tag. */
    {"aimee_synthesis_model", offsetof(config_t, aimee_synthesis_model),
     sizeof(((config_t *)0)->aimee_synthesis_model), 0, CFG_STRING, RELOAD_RESTART, FGROUP_RUNTIME},
    /* Synthesis. ONE endpoint: an aimee-kb *-llm image runs gemma-4 on the same
     * host as the kb, so a bundled model is reached at a 127.0.0.1 URL and needs no
     * second variable. Empty = synthesis off, which is supported.
     *
     * llm_embed_backend / llm_synth_backend / llm_synth_host / llm_synth_gpu /
     * llm_synth_tier are GONE. They chose where to place the retired aimee-llm
     * container; the aimee-kb image variant now encodes that. */
    {"synthesis_endpoint", offsetof(config_t, synthesis_endpoint),
     sizeof(((config_t *)0)->synthesis_endpoint), 0, CFG_STRING, RELOAD_RESTART, FGROUP_RUNTIME},
    {"synthesis_model", offsetof(config_t, synthesis_model),
     sizeof(((config_t *)0)->synthesis_model), 0, CFG_STRING, RELOAD_HOT, FGROUP_RUNTIME},
    {"synthesis_api_key", offsetof(config_t, synthesis_api_key),
     sizeof(((config_t *)0)->synthesis_api_key), 0, CFG_STRING, RELOAD_HOT, FGROUP_RUNTIME,
     "SYNTHESIS_API_KEY"},
    /* Global, not per-stage: one switch the operator owns. Default on. */
    {"synthesis_thinking", offsetof(config_t, synthesis_thinking), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_RUNTIME},
    {"memory_coref_mode", offsetof(config_t, memory_coref_mode),
     sizeof(((config_t *)0)->memory_coref_mode), 0, CFG_STRING},
    {"memory_coref_window", offsetof(config_t, memory_coref_window),
     sizeof(((config_t *)0)->memory_coref_window), 0, CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_rerank_mode", offsetof(config_t, memory_rerank_mode),
     sizeof(((config_t *)0)->memory_rerank_mode), 0, CFG_STRING},
    {"ingress_preinject_enabled", offsetof(config_t, ingress_preinject_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"code_context_mode", offsetof(config_t, code_context_mode),
     sizeof(((config_t *)0)->code_context_mode), 0, CFG_STRING, RELOAD_HOT, FGROUP_ADVANCED},
    {"ingress_preinject_anthropic_enabled", offsetof(config_t, ingress_preinject_anthropic_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"ingress_compress_enabled", offsetof(config_t, ingress_compress_enabled), sizeof(int), 0,
     CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"ingress_cache_placement_enabled", offsetof(config_t, ingress_cache_placement_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"ingress_compress_min_chars", offsetof(config_t, ingress_compress_min_chars), sizeof(int), 0,
     CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"gateway_prevent_subagents", offsetof(config_t, gateway_prevent_subagents), sizeof(int), 0,
     CFG_BOOL},
    {"gateway_pin_model", offsetof(config_t, gateway_pin_model), sizeof(int), 0, CFG_BOOL},
    {"ingress_preinject_assembly_budget", offsetof(config_t, ingress_preinject_assembly_budget),
     sizeof(int), 0, CFG_INT},
    {"ingress_max_raw_scans", offsetof(config_t, ingress_max_raw_scans), sizeof(int), 0, CFG_INT},
    {"code_span_max_lines", offsetof(config_t, code_span_max_lines), sizeof(int), 0, CFG_INT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"tool_output_max_bytes", offsetof(config_t, tool_output_max_bytes), sizeof(int), 0, CFG_INT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"require_session_worktree", offsetof(config_t, require_session_worktree), sizeof(int), 0,
     CFG_BOOL},
    {"session_worktree_base", offsetof(config_t, session_worktree_base),
     sizeof(((config_t *)0)->session_worktree_base), 0, CFG_STRING, RELOAD_HOT, FGROUP_ADVANCED},
    {"require_aimee_memory", offsetof(config_t, require_aimee_memory), sizeof(int), 0, CFG_BOOL},
    {"require_aimee_git", offsetof(config_t, require_aimee_git), sizeof(int), 0, CFG_BOOL},
    {"subagent_ban_enabled", offsetof(config_t, subagent_ban_enabled), sizeof(int), 0, CFG_BOOL},
    {"delegate_sandbox", offsetof(config_t, delegate_sandbox), sizeof(int), 0, CFG_BOOL},
    {"delegate_sandbox_package_access", offsetof(config_t, delegate_sandbox_package_access),
     sizeof(((config_t *)0)->delegate_sandbox_package_access), 0, CFG_STRING},
    {"delegate_sandbox_require_isolation", offsetof(config_t, delegate_sandbox_require_isolation),
     sizeof(int), 0, CFG_BOOL},
    {"delegate_sandbox_learn_packages", offsetof(config_t, delegate_sandbox_learn_packages),
     sizeof(int), 0, CFG_BOOL},
    {"typed_facts_enabled", offsetof(config_t, typed_facts_enabled), sizeof(int), 0, CFG_BOOL},
    {"kb_pdf_tier", offsetof(config_t, kb_pdf_tier), sizeof(((config_t *)0)->kb_pdf_tier), 0,
     CFG_STRING, RELOAD_RESTART},
    {"kb_pdf_ingest_enabled", offsetof(config_t, kb_pdf_ingest_enabled), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_pdf_vector_enabled", offsetof(config_t, kb_pdf_vector_enabled), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_pdf_tsr_enabled", offsetof(config_t, kb_pdf_tsr_enabled), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"tsr_command", offsetof(config_t, tsr_command), sizeof(((config_t *)0)->tsr_command), 0,
     CFG_STRING},
    {"kb_pdf_assets_enabled", offsetof(config_t, kb_pdf_assets_enabled), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_pdf_blob_dir", offsetof(config_t, kb_pdf_blob_dir),
     sizeof(((config_t *)0)->kb_pdf_blob_dir), 0, CFG_STRING, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_pdf_blob_recon_secs", offsetof(config_t, kb_pdf_blob_recon_secs), sizeof(int), 0, CFG_INT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_pdf_blob_orphan_alarm_mb", offsetof(config_t, kb_pdf_blob_orphan_alarm_mb), sizeof(int), 0,
     CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_pdf_ocr_enabled", offsetof(config_t, kb_pdf_ocr_enabled), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"ocr_command", offsetof(config_t, ocr_command), sizeof(((config_t *)0)->ocr_command), 0,
     CFG_STRING},
    {"css_style_graph_enabled", offsetof(config_t, css_style_graph_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"code_cochange_git_enabled", offsetof(config_t, code_cochange_git_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"wfe_live_forge_enabled", offsetof(config_t, wfe_live_forge_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"wfe_proposals_autoscan_enabled", offsetof(config_t, wfe_proposals_autoscan_enabled),
     sizeof(int), 0, CFG_BOOL},
    {"client_integrations_enabled", offsetof(config_t, client_integrations_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"audit_action_enabled", offsetof(config_t, audit_action_enabled), sizeof(int), 0, CFG_BOOL},
    {"audit_worm_enabled", offsetof(config_t, audit_worm_enabled), sizeof(int), 0, CFG_BOOL},
    {"css_render_command", offsetof(config_t, css_render_command),
     sizeof(((config_t *)0)->css_render_command), 0, CFG_STRING},
    {"vault.custody", offsetof(config_t, vault_custody), sizeof(((config_t *)0)->vault_custody), 0,
     CFG_STRING, RELOAD_RESTART, FGROUP_ADVANCED},
    {"vault.tpm2.blob_path", offsetof(config_t, vault_tpm2_blob_path),
     sizeof(((config_t *)0)->vault_tpm2_blob_path), 0, CFG_STRING, RELOAD_RESTART, FGROUP_ADVANCED},
    {"vault.tpm2.tcti", offsetof(config_t, vault_tpm2_tcti),
     sizeof(((config_t *)0)->vault_tpm2_tcti), 0, CFG_STRING, RELOAD_RESTART, FGROUP_ADVANCED},
    {"vault.tpm2.nv_index", offsetof(config_t, vault_tpm2_nv_index),
     sizeof(((config_t *)0)->vault_tpm2_nv_index), 0, CFG_STRING, RELOAD_RESTART, FGROUP_ADVANCED},
    {"kb_evidence_emit_enabled", offsetof(config_t, kb_evidence_emit_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"fidelity_check_enabled", offsetof(config_t, fidelity_check_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_query_expansion_mode", offsetof(config_t, memory_query_expansion_mode),
     sizeof(((config_t *)0)->memory_query_expansion_mode), 0, CFG_STRING},
    {"memory_query_expansion_k", offsetof(config_t, memory_query_expansion_k), sizeof(int), 0,
     CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    /* KB retrieval fusion: rrf (default) | static_alpha | dynamic_alpha. Settable so
     * an operator can pick a mode from the GUI/CLI; kb_search_fused reads it as the
     * default when no per-request fusion_mode override is supplied. */
    {"kb_fusion_mode", offsetof(config_t, kb_fusion_mode), sizeof(((config_t *)0)->kb_fusion_mode),
     0, CFG_STRING},
    {"kb_fusion_static_alpha", offsetof(config_t, kb_fusion_static_alpha), sizeof(double), 0,
     CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"autonomous", offsetof(config_t, autonomous), sizeof(int), 1, CFG_BOOL},
    {"cross_verify", offsetof(config_t, cross_verify), sizeof(int), 1, CFG_BOOL},
    {"verify_cmd", offsetof(config_t, verify_cmd), sizeof(((config_t *)0)->verify_cmd), 0,
     CFG_STRING},
    {"verify_role", offsetof(config_t, verify_role), sizeof(((config_t *)0)->verify_role), 0,
     CFG_STRING},
    {"verify_prompt", offsetof(config_t, verify_prompt), sizeof(((config_t *)0)->verify_prompt), 0,
     CFG_STRING},
    {"max_iterations", offsetof(config_t, max_iterations), sizeof(int), 0, CFG_INT},
    {"max_iterations_delegate", offsetof(config_t, max_iterations_delegate), sizeof(int), 0,
     CFG_INT},
    {"memory_maintenance_trigger_inserts", offsetof(config_t, memory_maintenance_trigger_inserts),
     sizeof(int), 0, CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_maintenance_trigger_secs", offsetof(config_t, memory_maintenance_trigger_secs),
     sizeof(int), 0, CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    /* memory_query_expansion_{mode,k} is already declared once above. The duplicate rows
     * that used to sit here were shadowed dead weight — config_field_lookup returns the
     * first match — and were removed. */
    {"memory_improve_dedupe_enabled", offsetof(config_t, memory_improve_dedupe_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_improve_summarise_enabled", offsetof(config_t, memory_improve_summarise_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_profile_cards_enabled", offsetof(config_t, memory_profile_cards_enabled), sizeof(int),
     0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_profile_cards_min_obs", offsetof(config_t, memory_profile_cards_min_obs), sizeof(int),
     0, CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_profile_cards_stale_secs", offsetof(config_t, memory_profile_cards_stale_secs),
     sizeof(int), 0, CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_rewrite_enabled", offsetof(config_t, memory_rewrite_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_rewrite_command", offsetof(config_t, memory_rewrite_command),
     sizeof(((config_t *)0)->memory_rewrite_command), 0, CFG_STRING},
    {"memory_rewrite_hyde", offsetof(config_t, memory_rewrite_hyde), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_rewrite_decompose", offsetof(config_t, memory_rewrite_decompose), sizeof(int), 0,
     CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_rewrite_max_subqueries", offsetof(config_t, memory_rewrite_max_subqueries),
     sizeof(int), 0, CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_window_radius", offsetof(config_t, memory_window_radius), sizeof(int), 0, CFG_INT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_search_max_results", offsetof(config_t, kb_search_max_results), sizeof(int), 0, CFG_INT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_negation_enabled", offsetof(config_t, memory_negation_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_scenes_enabled", offsetof(config_t, memory_scenes_enabled), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_bm25_weight", offsetof(config_t, memory_bm25_weight), sizeof(double), 0, CFG_FLOAT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"code_hybrid_weight_code", offsetof(config_t, code_hybrid_weight_code), sizeof(double), 0,
     CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"code_hybrid_weight_graph", offsetof(config_t, code_hybrid_weight_graph), sizeof(double), 0,
     CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"code_hybrid_weight_vector", offsetof(config_t, code_hybrid_weight_vector), sizeof(double), 0,
     CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"code_hybrid_weight_memory", offsetof(config_t, code_hybrid_weight_memory), sizeof(double), 0,
     CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"code_hybrid_rrf_k", offsetof(config_t, code_hybrid_rrf_k), sizeof(double), 0, CFG_FLOAT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"code_trust_actuation_enabled", offsetof(config_t, code_trust_actuation_enabled), sizeof(int),
     0, CFG_BOOL},
    {"code_surprising_precision_floor", offsetof(config_t, code_surprising_precision_floor),
     sizeof(double), 0, CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_semantic_weight", offsetof(config_t, memory_semantic_weight), sizeof(double), 0,
     CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_semantic_floor_scale", offsetof(config_t, memory_semantic_floor_scale), sizeof(double),
     0, CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_fetch_budget_enabled", offsetof(config_t, memory_fetch_budget_enabled), sizeof(int), 0,
     CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_fetch_budget_base", offsetof(config_t, memory_fetch_budget_base), sizeof(int), 0,
     CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_fetch_budget_shape_aware", offsetof(config_t, memory_fetch_budget_shape_aware),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_abstain_enabled", offsetof(config_t, memory_abstain_enabled), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_abstain_gate", offsetof(config_t, memory_abstain_gate), sizeof(double), 0, CFG_FLOAT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_chunk_min_confidence", offsetof(config_t, memory_chunk_min_confidence), sizeof(double),
     0, CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"memory_hard_negative_log", offsetof(config_t, memory_hard_negative_log),
     sizeof(((config_t *)0)->memory_hard_negative_log), 0, CFG_STRING, RELOAD_HOT, FGROUP_ADVANCED},
    {"dogfood_enabled", offsetof(config_t, dogfood_enabled), sizeof(int), 0, CFG_BOOL, RELOAD_HOT,
     FGROUP_DEV},
    {"dogfood_log_dir", offsetof(config_t, dogfood_log_dir),
     sizeof(((config_t *)0)->dogfood_log_dir), 0, CFG_STRING, RELOAD_HOT, FGROUP_DEV},
    {"dogfood_commit_raw", offsetof(config_t, dogfood_commit_raw), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_DEV},
    {"dogfood_inline_tagging", offsetof(config_t, dogfood_inline_tagging), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_DEV},
    {"dogfood_autolabel_repair", offsetof(config_t, dogfood_autolabel_repair), sizeof(int), 0,
     CFG_BOOL, RELOAD_HOT, FGROUP_DEV},
    {"dogfood_autolabel_continuation", offsetof(config_t, dogfood_autolabel_continuation),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_DEV},
    {"dogfood_autolabel_repeat_question", offsetof(config_t, dogfood_autolabel_repeat_question),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_DEV},
    {"learning_router_enabled", offsetof(config_t, learning_router_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"learning_proposal_ttl_days", offsetof(config_t, learning_proposal_ttl_days), sizeof(int), 0,
     CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"learning_max_commits_per_week", offsetof(config_t, learning_max_commits_per_week),
     sizeof(int), 0, CFG_INT, RELOAD_HOT, FGROUP_ADVANCED},
    {"learning_implicit_citation_repair", offsetof(config_t, learning_implicit_citation_repair),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"learning_implicit_citation_continuation",
     offsetof(config_t, learning_implicit_citation_continuation), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"learning_implicit_repeat_question", offsetof(config_t, learning_implicit_repeat_question),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"learning_implicit_repeated_correction",
     offsetof(config_t, learning_implicit_repeated_correction), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"learning_implicit_workflow_repetition",
     offsetof(config_t, learning_implicit_workflow_repetition), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"learning_implicit_retrieval_outcome", offsetof(config_t, learning_implicit_retrieval_outcome),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"identity_working_profile_injection_enabled",
     offsetof(config_t, identity_working_profile_injection_enabled), sizeof(int), 0, CFG_BOOL},
    {"integrity_enabled", offsetof(config_t, integrity_enabled), sizeof(int), 0, CFG_BOOL},
    {"integrity_dry_run", offsetof(config_t, integrity_dry_run), sizeof(int), 0, CFG_BOOL},
    {"virtual_context_enabled", offsetof(config_t, virtual_context_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"virtual_context_assembly_budget", offsetof(config_t, virtual_context_assembly_budget),
     sizeof(int), 0, CFG_INT},
    {"cache_aware_rewrite_enabled", offsetof(config_t, cache_aware_rewrite_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"transport.kb_pool_enabled", offsetof(config_t, transport_kb_pool_enabled), sizeof(int), 0,
     CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"transport.server_keepalive_enabled", offsetof(config_t, transport_server_keepalive_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"transport.thinclient_gzip_enabled", offsetof(config_t, transport_thinclient_gzip_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"transport.kb_gzip_enabled", offsetof(config_t, transport_kb_gzip_enabled), sizeof(int), 0,
     CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"cost_reward_enabled", offsetof(config_t, cost_reward_enabled), sizeof(int), 0, CFG_BOOL},
    {"cost_reward_lambda_pct", offsetof(config_t, cost_reward_lambda_pct), sizeof(int), 0, CFG_INT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"cost_reward_ref_usd_milli", offsetof(config_t, cost_reward_ref_usd_milli), sizeof(int), 0,
     CFG_INT},
    {"reasoning_cap_enabled", offsetof(config_t, reasoning_cap_enabled), sizeof(int), 0, CFG_BOOL},
    {"dedup_enabled", offsetof(config_t, dedup_enabled), sizeof(int), 0, CFG_BOOL},
    {"cache_shaping_enabled", offsetof(config_t, cache_shaping_enabled), sizeof(int), 0, CFG_BOOL},
    {"ingress_usage_accounting_enabled", offsetof(config_t, ingress_usage_accounting_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"ingress_audit_async", offsetof(config_t, ingress_audit_async), sizeof(int), 0, CFG_BOOL},
    {"ingress_trusted_proxy_secret", offsetof(config_t, ingress_trusted_proxy_secret),
     sizeof(((config_t *)0)->ingress_trusted_proxy_secret), 0, CFG_STRING, RELOAD_HOT,
     FGROUP_RUNTIME, "AIMEE_INGRESS_PROXY_SECRET"},
    {"dedup_window_seconds", offsetof(config_t, dedup_window_seconds), sizeof(int), 0, CFG_INT},
    {"cache_min_chars", offsetof(config_t, cache_min_chars), sizeof(int), 0, CFG_INT, RELOAD_HOT,
     FGROUP_ADVANCED},
    {"guardrails_semantic_mode", offsetof(config_t, guardrails_semantic_mode),
     sizeof(((config_t *)0)->guardrails_semantic_mode), 0, CFG_STRING},
    {"guardrails_blast_radius_advisory_enabled",
     offsetof(config_t, guardrails_blast_radius_advisory_enabled), sizeof(int), 0, CFG_BOOL},
    {"guardrails_semantic_command", offsetof(config_t, guardrails_semantic_command),
     sizeof(((config_t *)0)->guardrails_semantic_command), 0, CFG_STRING},
    {"guardrails_semantic_warn_threshold", offsetof(config_t, guardrails_semantic_warn_threshold),
     sizeof(double), 0, CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"guardrails_semantic_prompt_threshold",
     offsetof(config_t, guardrails_semantic_prompt_threshold), sizeof(double), 0, CFG_FLOAT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"guardrails_semantic_block_threshold", offsetof(config_t, guardrails_semantic_block_threshold),
     sizeof(double), 0, CFG_FLOAT, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_api_http_port", offsetof(config_t, kb_api_http_port), sizeof(int), 0, CFG_INT,
     RELOAD_RESTART},
    {"kb_api_bearer_token", offsetof(config_t, kb_api_bearer_token),
     sizeof(((config_t *)0)->kb_api_bearer_token), 0, CFG_STRING, RELOAD_RESTART, FGROUP_RUNTIME,
     "AIMEE_KB_API_BEARER_TOKEN"},
    {"telemetry.metrics_token", offsetof(config_t, telemetry_metrics_token),
     sizeof(((config_t *)0)->telemetry_metrics_token), 0, CFG_STRING, RELOAD_RESTART,
     FGROUP_RUNTIME, "AIMEE_TELEMETRY_METRICS_TOKEN"},
    {"kb_mining_enabled", offsetof(config_t, kb_mining_enabled), sizeof(int), 0, CFG_BOOL},
    {"kb_mining_min_poll_s", offsetof(config_t, kb_mining_min_poll_s), sizeof(int), 0, CFG_INT,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"verify_enabled", offsetof(config_t, verify_enabled), sizeof(int), 1, CFG_BOOL},
    {"delegate_graph_context_enabled", offsetof(config_t, delegate_graph_context_enabled),
     sizeof(int), 0, CFG_BOOL},
    {"roundtable.replay_verify_enabled", offsetof(config_t, roundtable_replay_verify_enabled),
     sizeof(int), 1, CFG_BOOL},
    {"roundtable.chair_synthesis", offsetof(config_t, roundtable_chair_synthesis), sizeof(int), 0,
     CFG_BOOL},
    {"roundtable.require_evidence", offsetof(config_t, roundtable_require_evidence), sizeof(int), 1,
     CFG_BOOL},
    {"verify_cross_project", offsetof(config_t, verify_cross_project), sizeof(int), 1, CFG_BOOL},
    {"roundtable.max_rounds", offsetof(config_t, roundtable_max_rounds), sizeof(int), 0, CFG_INT},
    {"roundtable.converge_threshold", offsetof(config_t, roundtable_converge_threshold),
     sizeof(int), 0, CFG_INT},
    {"roundtable.deadline_ms", offsetof(config_t, roundtable_deadline_ms), sizeof(int), 0, CFG_INT},
    {"roundtable.turns", offsetof(config_t, roundtable_turns),
     sizeof(((config_t *)0)->roundtable_turns), 0, CFG_STRING},
    {"roundtable.default", offsetof(config_t, roundtable_default),
     sizeof(((config_t *)0)->roundtable_default), 0, CFG_STRING},
    {"roundtable.pipeline_done_bar", offsetof(config_t, roundtable_pipeline_done_bar),
     sizeof(((config_t *)0)->roundtable_pipeline_done_bar), 0, CFG_STRING},
    {"roundtable.pipeline_max_passes", offsetof(config_t, roundtable_pipeline_max_passes),
     sizeof(int), 0, CFG_INT},
    {"roundtable.pipeline_max_attempts_per_pass",
     offsetof(config_t, roundtable_pipeline_max_attempts_per_pass), sizeof(int), 0, CFG_INT},
    {"roundtable.pipeline_max_cost_usd", offsetof(config_t, roundtable_pipeline_max_cost_usd),
     sizeof(double), 0, CFG_FLOAT},
    {"roundtable.pipeline_max_total_cost_usd",
     offsetof(config_t, roundtable_pipeline_max_total_cost_usd), sizeof(double), 0, CFG_FLOAT},
    {"roundtable.pipeline_gate_ttl_h", offsetof(config_t, roundtable_pipeline_gate_ttl_h),
     sizeof(int), 0, CFG_INT},
    {"roundtable.pipeline_parked_releases_slot",
     offsetof(config_t, roundtable_pipeline_parked_releases_slot), sizeof(int), 1, CFG_BOOL},
    {"roundtable.pipeline_unknown_context_tokens",
     offsetof(config_t, roundtable_pipeline_unknown_context_tokens), sizeof(int), 0, CFG_INT},
    /* Trigger admission policy. The scheduler reads this from the live config snapshot on
     * every sweep, so GUI changes take effect without a restart. */
    {"trigger.max_concurrent", offsetof(config_t, trigger_max_concurrent), sizeof(int), 0, CFG_INT},
    /* The only economizer control: off, safe, or aggressive. */
    {"economizer.mode", offsetof(config_t, economizer_mode), sizeof(int), 0, CFG_ECON_MODE,
     RELOAD_HOT},
    /* Autonomous-development pipeline knobs (Phase-C). New config_t fields bridged to the
     * AIMEE_AUTONOMY_* env vars at startup (a set env var still overrides); a change
     * applies on the next server start. */
    {"autonomy.skeptics", offsetof(config_t, autonomy_skeptics), sizeof(int), 0, CFG_INT},
    {"autonomy.fanout", offsetof(config_t, autonomy_fanout), sizeof(int), 1, CFG_BOOL},
    {"autonomy.unit_retry", offsetof(config_t, autonomy_unit_retry), sizeof(int), 0, CFG_INT},
    {"autonomy.unit_max", offsetof(config_t, autonomy_unit_max), sizeof(int), 0, CFG_INT},
    {"autonomy.ci_retry_max", offsetof(config_t, autonomy_ci_retry_max), sizeof(int), 0, CFG_INT},
    /* Run safety caps + auto-resume policy — config-backed + live via config_autonomy_lookup
     * (an exported AIMEE_AUTONOMY_* still overrides). Surfaced in the Workflows GUI's Run
     * policy panel. */
    {"autonomy.max_turns", offsetof(config_t, autonomy_max_turns), sizeof(int), 0, CFG_INT},
    {"autonomy.max_wall_secs", offsetof(config_t, autonomy_max_wall_secs), sizeof(int), 0, CFG_INT},
    {"autonomy.stale_abandon_secs", offsetof(config_t, autonomy_stale_abandon_secs), sizeof(int), 0,
     CFG_INT},
    {"autonomy.concurrency", offsetof(config_t, autonomy_concurrency), sizeof(int), 0, CFG_INT},
    {"autonomy.auto_resume_cap_parks", offsetof(config_t, autonomy_auto_resume_cap_parks),
     sizeof(int), 1, CFG_BOOL},
    {"autonomy.max_resumes", offsetof(config_t, autonomy_max_resumes), sizeof(int), 0, CFG_INT},
    /* Curator pipeline stage gates (kb.curator.*) — exposed so the GUI pipeline editor
     * can toggle stages. Flat config_t fields; config_save reserializes them into the
     * nested kb.curator.* YAML the KB reads on its next load. These MUST stay ahead of the
     * {NULL} terminator below: every consumer (config.show/get/set, the CLI key listing)
     * iterates until the first NULL key, so a terminator placed before them makes them
     * unreachable — the bug this array previously had. */
    {"kb_curator_tier", offsetof(config_t, kb_curator_tier),
     sizeof(((config_t *)0)->kb_curator_tier), 0, CFG_STRING, RELOAD_RESTART},
    {"kb_curator_extract_docs_enabled", offsetof(config_t, kb_curator_extract_docs_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_extract_docs_workers", offsetof(config_t, kb_curator_extract_docs_workers),
     sizeof(int), 0, CFG_INT},
    {"kb_curator_stage_order", offsetof(config_t, kb_curator_stage_order),
     sizeof(((config_t *)0)->kb_curator_stage_order), 0, CFG_STRING, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_user_presets", offsetof(config_t, kb_curator_user_presets),
     sizeof(((config_t *)0)->kb_curator_user_presets), 0, CFG_STRING, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_custom_stages", offsetof(config_t, kb_curator_custom_stages),
     sizeof(((config_t *)0)->kb_curator_custom_stages), 0, CFG_STRING, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_extract_code_enabled", offsetof(config_t, kb_curator_extract_code_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_extract_code_workers", offsetof(config_t, kb_curator_extract_code_workers),
     sizeof(int), 0, CFG_INT},
    {"kb_curator_resolve_entities_enabled", offsetof(config_t, kb_curator_resolve_entities_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_index_narrative_enabled", offsetof(config_t, kb_curator_index_narrative_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_index_claims_enabled", offsetof(config_t, kb_curator_index_claims_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_detect_contradictions_enabled",
     offsetof(config_t, kb_curator_detect_contradictions_enabled), sizeof(int), 0, CFG_BOOL,
     RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_index_code_unit_enabled", offsetof(config_t, kb_curator_index_code_unit_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_link_artifacts_enabled", offsetof(config_t, kb_curator_link_artifacts_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_projection_graph_enabled", offsetof(config_t, kb_curator_projection_graph_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_synthesize_enabled", offsetof(config_t, kb_curator_synthesize_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_promote_entity_enabled", offsetof(config_t, kb_curator_promote_entity_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_curator_cross_repo_graph_enabled", offsetof(config_t, kb_curator_cross_repo_graph_enabled),
     sizeof(int), 0, CFG_BOOL, RELOAD_HOT, FGROUP_ADVANCED},
    {"kb_evidence_embed_enabled", offsetof(config_t, kb_evidence_embed_enabled), sizeof(int), 0,
     CFG_BOOL},
    {NULL, 0, 0, 0, CFG_STRING},
};
#pragma GCC diagnostic pop

const config_field_t *config_field_lookup(const char *key)
{
   if (!key)
      return NULL;
   for (int i = 0; config_fields[i].key; i++)
      if (strcmp(key, config_fields[i].key) == 0)
         return &config_fields[i];
   return NULL;
}

const char *config_field_reload_verdict(const config_field_t *f)
{
   switch (f ? f->reload_class : RELOAD_HOT)
   {
   case RELOAD_RESTART:
      return "saved — restart the server for this to take effect";
   case RELOAD_REAPPLIABLE:
      return "applied live";
   case RELOAD_HOT:
   default:
      return "applied live";
   }
}

const char *config_field_group_name(const config_field_t *f)
{
   switch (f ? f->group : FGROUP_RUNTIME)
   {
   case FGROUP_DEPLOY:
      return "deploy";
   case FGROUP_ADVANCED:
      return "advanced";
   case FGROUP_DEV:
      return "dev";
   case FGROUP_RUNTIME:
   default:
      return "runtime";
   }
}

cJSON *config_field_value_json(const config_t *cfg, const config_field_t *f)
{
   if (!cfg || !f)
      return cJSON_CreateNull();
   const char *base = (const char *)cfg + f->offset;
   if (f->is_bool || f->type == CFG_BOOL)
      return cJSON_CreateBool(*(const int *)base ? 1 : 0);
   if (f->type == CFG_INT)
      return cJSON_CreateNumber(*(const int *)base);
   if (f->type == CFG_FLOAT)
      return cJSON_CreateNumber(*(const double *)base);
   if (f->type == CFG_ECON_MODE)
      return cJSON_CreateString(econ_mode_name(*(const int *)base));
   return cJSON_CreateString(base);
}

const char *config_field_secret_name(const config_field_t *f)
{
   return f ? f->secret_name : NULL;
}

cJSON *config_field_public_value_json(const config_t *cfg, const config_field_t *f)
{
   if (!cfg || !f)
      return cJSON_CreateNull();
   if (!config_field_secret_name(f))
      return config_field_value_json(cfg, f);
   const char *base = (const char *)cfg + f->offset;
   return cJSON_CreateBool(base[0] ? 1 : 0);
}

/* Live-config form for callers outside this module, which under the
 * encapsulation rule may not hold a config_t. Heap, not stack: config_t is
 * ~750 KB, and this is called per field while rendering a console page. */
cJSON *config_field_public_value_json_current(const config_field_t *f)
{
   if (!f)
      return cJSON_CreateNull();
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return cJSON_CreateNull();
   (void)config_load(cfg);
   cJSON *out = config_field_public_value_json(cfg, f);
   free(cfg);
   return out;
}

/* Widest string field config_fields[] exposes; a longer one is truncated here
 * rather than read past. */
#define CONFIG_FIELD_RENDER_MAX 4096

/* Render one field for display, without the caller holding a config_t. The
 * `aimee config get` path: it used to load a whole config and index into it by
 * f->offset, which is the config_t layout leaking into a CLI. Writes the same
 * text that path printed, including "(unset)" for an empty string and
 * "configured"/"not configured" for a Vault-backed secret -- a secret's VALUE is
 * never rendered here. Returns 0 on success. */
int config_field_render(const config_field_t *f, char *out, size_t n)
{
   if (!f || !out || n == 0)
      return -1;
   out[0] = '\0';

   /* Read f->size, never a fixed buffer size: config_field_read copies exactly
    * what it is asked for from f->offset, so over-asking walks off the end of the
    * field into whatever config_t happens to hold next. */
   if (config_field_secret_name(f))
   {
      char buf[CONFIG_FIELD_RENDER_MAX] = "";
      size_t want = f->size < sizeof(buf) ? f->size : sizeof(buf);
      (void)config_field_read(f->offset, want, buf);
      buf[want ? want - 1 : 0] = '\0';
      snprintf(out, n, "%s", buf[0] ? "configured" : "not configured");
      runtime_secret_wipe(buf, sizeof(buf));
      return 0;
   }

   if (f->is_bool || f->type == CFG_BOOL)
   {
      int v = 0;
      (void)config_field_read(f->offset, sizeof(v), &v);
      snprintf(out, n, "%s", v ? "true" : "false");
   }
   else if (f->type == CFG_INT)
   {
      int v = 0;
      (void)config_field_read(f->offset, sizeof(v), &v);
      snprintf(out, n, "%d", v);
   }
   else if (f->type == CFG_FLOAT)
   {
      double v = 0.0;
      (void)config_field_read(f->offset, sizeof(v), &v);
      snprintf(out, n, "%g", v);
   }
   else
   {
      char buf[CONFIG_FIELD_RENDER_MAX] = "";
      size_t want = f->size < sizeof(buf) ? f->size : sizeof(buf);
      (void)config_field_read(f->offset, want, buf);
      buf[want ? want - 1 : 0] = '\0';
      snprintf(out, n, "%s", buf[0] ? buf : "(unset)");
   }
   return 0;
}

int config_field_set_value(config_t *cfg, const config_field_t *f, const char *value)
{
   if (!cfg || !f || !value)
      return -1;
   char *base = (char *)cfg + f->offset;
   if (f->is_bool || f->type == CFG_BOOL)
   {
      if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
         *(int *)base = 1;
      else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
         *(int *)base = 0;
      else
         return -1;
   }
   else if (f->type == CFG_INT)
      *(int *)base = atoi(value);
   else if (f->type == CFG_FLOAT)
      *(double *)base = atof(value);
   else if (f->type == CFG_ECON_MODE)
   {
      int parsed = econ_mode_parse(value);
      if (parsed < 0)
         return -1;
      *(int *)base = parsed;
   }
   else
      snprintf(base, f->size, "%s", value);
   return 0;
}

/* Flat-field defaults (Proposal A, step 1). Single home for the default value of
 * every FLAT config field (see test_config_field_eligibility.c). config_set_defaults
 * applies these table-driven instead of hand-assigning each — so a default lives in
 * exactly one place (here), keyed by the same name as its config_fields[] descriptor.
 * Values are in config_field_set_value's string form (bool: "true"/"false"; int: a
 * decimal; string: the literal). Non-flat fields keep their bespoke defaults in
 * config_set_defaults (side effects, env derivation, or computed values). */
static const struct
{
   const char *key;
   const char *value;
} config_flat_defaults[] = {
    {"db2_url", ""},
    /* EMPTY on purpose: a fresh install has no primary until one is chosen.
     *
     * This defaulted to "claude", which pre-populated an install with a provider
     * the operator never picked. chat_agent_add_builtin_provider then synthesized
     * a claude tmux-CLI agent for it, and chat_agent_select_provider PINNED the
     * turn to that agent — so on a machine with no claude CLI every turn died
     * with "no agent available for role 'code'", naming an internal role, while
     * the agents the operator had actually added sat right there disabled for the
     * turn. Completing the setup wizard did not help: the wizard writes
     * agents.json's default_agent, and this pin overrode it.
     *
     * Empty means "not chosen": select_provider returns without pinning, every
     * configured agent stays eligible, and agents.json's default_agent decides —
     * which is what the wizard set. Setting a primary explicitly still pins, as
     * before. Only NEW installs see this; an existing config has its own value
     * persisted and is unaffected. */
    {"provider", ""},
    {"default_persona", "engineer"},
    {"claude_model", ""},
    {"openai_endpoint", "https://api.openai.com/v1"},
    {"openai_model", "gpt-4o"},
    {"openai_key_cmd", ""},
    {"guardrail_mode", "approve"},
    {"embedder_command", ""},
    {"embedder_model", ""},
    {"embedder_url", ""},
    {"kb_client_url", ""},
    {"kb_client_bearer_token", ""},
    {"memory_rerank_mode", ""},
    {"synthesis_thinking", "true"},
    {"ingress_preinject_enabled", "true"},
    {"code_context_mode", "on"},
    {"ingress_preinject_anthropic_enabled", "false"},
    {"ingress_compress_enabled", "true"},
    {"gateway_prevent_subagents", "false"},
    {"gateway_pin_model", "false"},
    {"require_session_worktree", "true"},
    {"require_aimee_memory", "true"},
    {"require_aimee_git", "true"},
    {"subagent_ban_enabled", "true"},
    {"delegate_sandbox_require_isolation", "false"},
    {"delegate_sandbox_learn_packages", "true"},
    {"typed_facts_enabled", "true"},
    {"kb_pdf_ingest_enabled", "false"},
    {"kb_pdf_vector_enabled", "false"},
    {"kb_pdf_tsr_enabled", "false"},
    {"tsr_command", ""},
    {"kb_pdf_assets_enabled", "false"},
    {"kb_pdf_blob_dir", ""},
    {"kb_pdf_blob_recon_secs", "3600"},
    {"kb_pdf_blob_orphan_alarm_mb", "1024"},
    {"kb_pdf_ocr_enabled", "false"},
    {"ocr_command", ""},
    {"css_style_graph_enabled", "true"},
    {"code_cochange_git_enabled", "true"},
    {"wfe_live_forge_enabled", "false"},
    {"wfe_proposals_autoscan_enabled", "false"},
    {"client_integrations_enabled", "true"},
    {"audit_action_enabled", "true"},
    {"audit_worm_enabled", "false"},
    {"css_render_command",
     "curl -s --max-time 30 --data-binary @- http://aimee-css-render:8780/render"},
    {"kb_evidence_emit_enabled", "false"},
    {"fidelity_check_enabled", "false"},
    {"autonomous", "false"},
    {"max_iterations", "0"},
    {"max_iterations_delegate", "0"},
    {"verify_enabled", "false"},
    {"delegate_graph_context_enabled", "false"},
    {"verify_cross_project", "false"},
    {"cross_verify", "false"},
    {"verify_cmd", ""},
    {"verify_role", ""},
    {"verify_prompt", ""},
    {NULL, NULL}, /* sentinel — config_apply_flat_defaults iterates until .key is NULL */
};

void config_apply_flat_defaults(config_t *cfg)
{
   if (!cfg)
      return;
   for (int i = 0; config_flat_defaults[i].key; i++)
   {
      const config_field_t *f = config_field_lookup(config_flat_defaults[i].key);
      if (f)
         (void)config_field_set_value(cfg, f, config_flat_defaults[i].value);
   }
}

/* Assign a flat field from its parsed JSON node, matching the inline config_load
 * parse exactly: a present, correctly-typed value is assigned; anything else
 * leaves the field at its default. Strings use the non-empty guard (an explicit
 * "" leaves the default) — the form 45 of the 51 genericised string fields already
 * used, and behaviour-identical for the rest because their default is "". */
static void config_field_set_from_json(config_t *cfg, const config_field_t *f, const cJSON *item)
{
   char *base = (char *)cfg + f->offset;
   switch (f->type)
   {
   case CFG_BOOL:
      if (cJSON_IsBool(item))
         *(int *)base = cJSON_IsTrue(item);
      break;
   case CFG_INT:
      if (cJSON_IsNumber(item))
         *(int *)base = (int)item->valuedouble;
      break;
   case CFG_FLOAT:
      if (cJSON_IsNumber(item))
         *(double *)base = item->valuedouble;
      break;
   case CFG_STRING:
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(base, f->size, "%s", item->valuestring);
      break;
   case CFG_ECON_MODE:
      break; /* not a flat field; parsed by its bespoke handler */
   }
}

/* Table-driven parse of the flat scalar fields (Proposal A, step 3): replaces the
 * per-field inline `item = GetObjectItem(root, key); if (typed) cfg->x = ...` blocks
 * in config_load with one loop over the flat set. css_render_command is the sole
 * exception — its default is non-empty AND its inline guard admits an explicit ""
 * (to disable rendering), so it keeps its bespoke block to preserve that behaviour. */
void config_parse_flat_fields(config_t *cfg, const cJSON *root)
{
   if (!cfg || !root)
      return;
   for (int i = 0; config_flat_defaults[i].key; i++)
   {
      const char *key = config_flat_defaults[i].key;
      if (strcmp(key, "css_render_command") == 0)
         continue; /* kept inline: non-empty default + empty-string is meaningful */
      const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, key);
      if (!item)
         continue;
      const config_field_t *f = config_field_lookup(key);
      if (f)
         config_field_set_from_json(cfg, f, item);
   }
}
