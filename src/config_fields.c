/* config_fields.c: shared get/set allowlist for top-level config_t fields.
 *
 * Extracted from cmd_data.c so the legacy `aimee config` command AND the
 * server's config.show/get/set handlers operate on one table. CORE layer:
 * depends only on config.h and cJSON.
 *
 * NB: the three guardrails_semantic_*_threshold fields are doubles; they were
 * historically (mis)typed CFG_STRING in cmd_data.c, which only worked because
 * that table was unreachable. They are CFG_FLOAT here. */
#include "config_fields.h"
#include <stdlib.h>
#include <string.h>

const config_field_t config_fields[] = {
    {"db2_url", offsetof(config_t, db2_url), sizeof(((config_t *)0)->db2_url), 0, CFG_STRING},
    {"provider", offsetof(config_t, provider), sizeof(((config_t *)0)->provider), 0, CFG_STRING},
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
    {"embedding_command", offsetof(config_t, embedding_command),
     sizeof(((config_t *)0)->embedding_command), 0, CFG_STRING},
    {"embedding_model", offsetof(config_t, embedding_model),
     sizeof(((config_t *)0)->embedding_model), 0, CFG_STRING},
    {"embedding_endpoint", offsetof(config_t, embedding_endpoint),
     sizeof(((config_t *)0)->embedding_endpoint), 0, CFG_STRING},
    {"embedding_dim", offsetof(config_t, embedding_dim), sizeof(((config_t *)0)->embedding_dim), 0,
     CFG_INT},
    {"memory_coref_mode", offsetof(config_t, memory_coref_mode),
     sizeof(((config_t *)0)->memory_coref_mode), 0, CFG_STRING},
    {"memory_coref_window", offsetof(config_t, memory_coref_window),
     sizeof(((config_t *)0)->memory_coref_window), 0, CFG_INT},
    {"memory_rerank_mode", offsetof(config_t, memory_rerank_mode),
     sizeof(((config_t *)0)->memory_rerank_mode), 0, CFG_STRING},
    {"memory_rerank_enabled", offsetof(config_t, memory_rerank_enabled), sizeof(int), 0, CFG_BOOL},
    {"ingress_preinject_enabled", offsetof(config_t, ingress_preinject_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"ingress_preinject_anthropic_enabled", offsetof(config_t, ingress_preinject_anthropic_enabled),
     sizeof(int), 0, CFG_BOOL},
    {"ingress_compress_enabled", offsetof(config_t, ingress_compress_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"ingress_cache_placement_enabled", offsetof(config_t, ingress_cache_placement_enabled),
     sizeof(int), 0, CFG_BOOL},
    {"ingress_compress_min_chars", offsetof(config_t, ingress_compress_min_chars), sizeof(int), 0,
     CFG_INT},
    {"gateway_prevent_subagents", offsetof(config_t, gateway_prevent_subagents), sizeof(int), 0,
     CFG_BOOL},
    {"gateway_pin_model", offsetof(config_t, gateway_pin_model), sizeof(int), 0, CFG_BOOL},
    {"ingress_preinject_assembly_budget", offsetof(config_t, ingress_preinject_assembly_budget),
     sizeof(int), 0, CFG_INT},
    {"ingress_max_raw_scans", offsetof(config_t, ingress_max_raw_scans), sizeof(int), 0, CFG_INT},
    {"code_span_max_lines", offsetof(config_t, code_span_max_lines), sizeof(int), 0, CFG_INT},
    {"require_session_worktree", offsetof(config_t, require_session_worktree), sizeof(int), 0,
     CFG_BOOL},
    {"typed_facts_enabled", offsetof(config_t, typed_facts_enabled), sizeof(int), 0, CFG_BOOL},
    {"kb_pdf_ingest_enabled", offsetof(config_t, kb_pdf_ingest_enabled), sizeof(int), 0, CFG_BOOL},
    {"css_style_graph_enabled", offsetof(config_t, css_style_graph_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"wfe_live_forge_enabled", offsetof(config_t, wfe_live_forge_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"css_render_command", offsetof(config_t, css_render_command),
     sizeof(((config_t *)0)->css_render_command), 0, CFG_STRING},
    {"kb_evidence_emit_enabled", offsetof(config_t, kb_evidence_emit_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"fidelity_check_enabled", offsetof(config_t, fidelity_check_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_rerank_command", offsetof(config_t, memory_rerank_command),
     sizeof(((config_t *)0)->memory_rerank_command), 0, CFG_STRING},
    {"memory_rerank_top_k", offsetof(config_t, memory_rerank_top_k), sizeof(int), 0, CFG_INT},
    {"memory_query_expansion_mode", offsetof(config_t, memory_query_expansion_mode),
     sizeof(((config_t *)0)->memory_query_expansion_mode), 0, CFG_STRING},
    {"memory_query_expansion_k", offsetof(config_t, memory_query_expansion_k), sizeof(int), 0,
     CFG_INT},
    {"autonomous", offsetof(config_t, autonomous), sizeof(int), 1, CFG_BOOL},
    {"cross_verify", offsetof(config_t, cross_verify), sizeof(int), 1, CFG_BOOL},
    {"ecomode", offsetof(config_t, ecomode), sizeof(int), 1, CFG_BOOL},
    {"max_iterations", offsetof(config_t, max_iterations), sizeof(int), 0, CFG_INT},
    {"max_iterations_delegate", offsetof(config_t, max_iterations_delegate), sizeof(int), 0,
     CFG_INT},
    {"memory_maintenance_trigger_inserts", offsetof(config_t, memory_maintenance_trigger_inserts),
     sizeof(int), 0, CFG_INT},
    {"memory_maintenance_trigger_secs", offsetof(config_t, memory_maintenance_trigger_secs),
     sizeof(int), 0, CFG_INT},
    {"memory_rerank_enabled", offsetof(config_t, memory_rerank_enabled), sizeof(int), 0, CFG_BOOL},
    {"memory_improve_dedupe_enabled", offsetof(config_t, memory_improve_dedupe_enabled),
     sizeof(int), 0, CFG_BOOL},
    {"memory_improve_summarise_enabled", offsetof(config_t, memory_improve_summarise_enabled),
     sizeof(int), 0, CFG_BOOL},
    {"memory_rerank_command", offsetof(config_t, memory_rerank_command),
     sizeof(((config_t *)0)->memory_rerank_command), 0, CFG_STRING},
    {"memory_rerank_top_k", offsetof(config_t, memory_rerank_top_k), sizeof(int), 0, CFG_INT},
    {"memory_query_expansion_mode", offsetof(config_t, memory_query_expansion_mode),
     sizeof(((config_t *)0)->memory_query_expansion_mode), 0, CFG_STRING},
    {"memory_query_expansion_k", offsetof(config_t, memory_query_expansion_k), sizeof(int), 0,
     CFG_INT},
    {"memory_profile_cards_enabled", offsetof(config_t, memory_profile_cards_enabled), sizeof(int),
     0, CFG_BOOL},
    {"memory_profile_cards_min_obs", offsetof(config_t, memory_profile_cards_min_obs), sizeof(int),
     0, CFG_INT},
    {"memory_profile_cards_stale_secs", offsetof(config_t, memory_profile_cards_stale_secs),
     sizeof(int), 0, CFG_INT},
    {"memory_rewrite_enabled", offsetof(config_t, memory_rewrite_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_rewrite_command", offsetof(config_t, memory_rewrite_command),
     sizeof(((config_t *)0)->memory_rewrite_command), 0, CFG_STRING},
    {"memory_rewrite_hyde", offsetof(config_t, memory_rewrite_hyde), sizeof(int), 0, CFG_BOOL},
    {"memory_rewrite_decompose", offsetof(config_t, memory_rewrite_decompose), sizeof(int), 0,
     CFG_BOOL},
    {"memory_rewrite_max_subqueries", offsetof(config_t, memory_rewrite_max_subqueries),
     sizeof(int), 0, CFG_INT},
    {"memory_window_radius", offsetof(config_t, memory_window_radius), sizeof(int), 0, CFG_INT},
    {"memory_kb_neighbour_expand", offsetof(config_t, memory_kb_neighbour_expand), sizeof(int), 0,
     CFG_BOOL},
    {"kb_search_max_results", offsetof(config_t, kb_search_max_results), sizeof(int), 0, CFG_INT},
    {"memory_negation_enabled", offsetof(config_t, memory_negation_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_scenes_enabled", offsetof(config_t, memory_scenes_enabled), sizeof(int), 0, CFG_BOOL},
    {"memory_scenes_min_cluster_size", offsetof(config_t, memory_scenes_min_cluster_size),
     sizeof(int), 0, CFG_INT},
    {"memory_scenes_top_m", offsetof(config_t, memory_scenes_top_m), sizeof(int), 0, CFG_INT},
    {"memory_bm25_weight", offsetof(config_t, memory_bm25_weight), sizeof(double), 0, CFG_FLOAT},
    {"code_hybrid_weight_code", offsetof(config_t, code_hybrid_weight_code), sizeof(double), 0,
     CFG_FLOAT},
    {"code_hybrid_weight_graph", offsetof(config_t, code_hybrid_weight_graph), sizeof(double), 0,
     CFG_FLOAT},
    {"code_hybrid_weight_vector", offsetof(config_t, code_hybrid_weight_vector), sizeof(double), 0,
     CFG_FLOAT},
    {"code_hybrid_weight_memory", offsetof(config_t, code_hybrid_weight_memory), sizeof(double), 0,
     CFG_FLOAT},
    {"code_hybrid_rrf_k", offsetof(config_t, code_hybrid_rrf_k), sizeof(double), 0, CFG_FLOAT},
    {"code_surprising_precision_floor", offsetof(config_t, code_surprising_precision_floor),
     sizeof(double), 0, CFG_FLOAT},
    {"memory_semantic_weight", offsetof(config_t, memory_semantic_weight), sizeof(double), 0,
     CFG_FLOAT},
    {"memory_semantic_floor_scale", offsetof(config_t, memory_semantic_floor_scale), sizeof(double),
     0, CFG_FLOAT},
    {"memory_fetch_budget_enabled", offsetof(config_t, memory_fetch_budget_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_fetch_budget_base", offsetof(config_t, memory_fetch_budget_base), sizeof(int), 0,
     CFG_INT},
    {"memory_fetch_budget_shape_aware", offsetof(config_t, memory_fetch_budget_shape_aware),
     sizeof(int), 0, CFG_BOOL},
    {"memory_abstain_enabled", offsetof(config_t, memory_abstain_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_abstain_gate", offsetof(config_t, memory_abstain_gate), sizeof(double), 0, CFG_FLOAT},
    {"memory_chunk_min_confidence", offsetof(config_t, memory_chunk_min_confidence), sizeof(double),
     0, CFG_FLOAT},
    {"memory_hard_negative_log", offsetof(config_t, memory_hard_negative_log),
     sizeof(((config_t *)0)->memory_hard_negative_log), 0, CFG_STRING},
    {"dogfood_enabled", offsetof(config_t, dogfood_enabled), sizeof(int), 0, CFG_BOOL},
    {"dogfood_log_dir", offsetof(config_t, dogfood_log_dir),
     sizeof(((config_t *)0)->dogfood_log_dir), 0, CFG_STRING},
    {"dogfood_commit_raw", offsetof(config_t, dogfood_commit_raw), sizeof(int), 0, CFG_BOOL},
    {"dogfood_inline_tagging", offsetof(config_t, dogfood_inline_tagging), sizeof(int), 0,
     CFG_BOOL},
    {"dogfood_autolabel_repair", offsetof(config_t, dogfood_autolabel_repair), sizeof(int), 0,
     CFG_BOOL},
    {"dogfood_autolabel_continuation", offsetof(config_t, dogfood_autolabel_continuation),
     sizeof(int), 0, CFG_BOOL},
    {"dogfood_autolabel_repeat_question", offsetof(config_t, dogfood_autolabel_repeat_question),
     sizeof(int), 0, CFG_BOOL},
    {"learning_router_enabled", offsetof(config_t, learning_router_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"learning_proposal_ttl_days", offsetof(config_t, learning_proposal_ttl_days), sizeof(int), 0,
     CFG_INT},
    {"learning_max_commits_per_week", offsetof(config_t, learning_max_commits_per_week),
     sizeof(int), 0, CFG_INT},
    {"learning_implicit_citation_repair", offsetof(config_t, learning_implicit_citation_repair),
     sizeof(int), 0, CFG_BOOL},
    {"learning_implicit_citation_continuation",
     offsetof(config_t, learning_implicit_citation_continuation), sizeof(int), 0, CFG_BOOL},
    {"learning_implicit_repeat_question", offsetof(config_t, learning_implicit_repeat_question),
     sizeof(int), 0, CFG_BOOL},
    {"learning_implicit_repeated_correction",
     offsetof(config_t, learning_implicit_repeated_correction), sizeof(int), 0, CFG_BOOL},
    {"learning_implicit_workflow_repetition",
     offsetof(config_t, learning_implicit_workflow_repetition), sizeof(int), 0, CFG_BOOL},
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
    {"cost_reward_enabled", offsetof(config_t, cost_reward_enabled), sizeof(int), 0, CFG_BOOL},
    {"cost_reward_lambda_pct", offsetof(config_t, cost_reward_lambda_pct), sizeof(int), 0, CFG_INT},
    {"cost_reward_ref_usd_milli", offsetof(config_t, cost_reward_ref_usd_milli), sizeof(int), 0,
     CFG_INT},
    {"reasoning_cap_enabled", offsetof(config_t, reasoning_cap_enabled), sizeof(int), 0, CFG_BOOL},
    {"dedup_enabled", offsetof(config_t, dedup_enabled), sizeof(int), 0, CFG_BOOL},
    {"cache_shaping_enabled", offsetof(config_t, cache_shaping_enabled), sizeof(int), 0, CFG_BOOL},
    {"ingress_usage_accounting_enabled", offsetof(config_t, ingress_usage_accounting_enabled),
     sizeof(int), 0, CFG_BOOL},
    {"ingress_audit_async", offsetof(config_t, ingress_audit_async), sizeof(int), 0, CFG_BOOL},
    {"ingress_trusted_proxy_secret", offsetof(config_t, ingress_trusted_proxy_secret),
     sizeof(((config_t *)0)->ingress_trusted_proxy_secret), 0, CFG_STRING},
    {"dedup_window_seconds", offsetof(config_t, dedup_window_seconds), sizeof(int), 0, CFG_INT},
    {"cache_min_chars", offsetof(config_t, cache_min_chars), sizeof(int), 0, CFG_INT},
    {"guardrails_semantic_enabled", offsetof(config_t, guardrails_semantic_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"guardrails_blast_radius_advisory_enabled",
     offsetof(config_t, guardrails_blast_radius_advisory_enabled), sizeof(int), 0, CFG_BOOL},
    {"guardrails_semantic_dry_run", offsetof(config_t, guardrails_semantic_dry_run), sizeof(int), 0,
     CFG_BOOL},
    {"guardrails_semantic_command", offsetof(config_t, guardrails_semantic_command),
     sizeof(((config_t *)0)->guardrails_semantic_command), 0, CFG_STRING},
    {"guardrails_semantic_warn_threshold", offsetof(config_t, guardrails_semantic_warn_threshold),
     sizeof(double), 0, CFG_FLOAT},
    {"guardrails_semantic_prompt_threshold",
     offsetof(config_t, guardrails_semantic_prompt_threshold), sizeof(double), 0, CFG_FLOAT},
    {"guardrails_semantic_block_threshold", offsetof(config_t, guardrails_semantic_block_threshold),
     sizeof(double), 0, CFG_FLOAT},
    {"guardrails_semantic_allow_ml_only_block",
     offsetof(config_t, guardrails_semantic_allow_ml_only_block), sizeof(int), 0, CFG_BOOL},
    {"kb_api_http_port", offsetof(config_t, kb_api_http_port), sizeof(int), 0, CFG_INT},
    {"kb_api_bearer_token", offsetof(config_t, kb_api_bearer_token),
     sizeof(((config_t *)0)->kb_api_bearer_token), 0, CFG_STRING},
    {"kb_mining_enabled", offsetof(config_t, kb_mining_enabled), sizeof(int), 0, CFG_BOOL},
    {"kb_mining_min_poll_s", offsetof(config_t, kb_mining_min_poll_s), sizeof(int), 0, CFG_INT},
    {"verify_enabled", offsetof(config_t, verify_enabled), sizeof(int), 1, CFG_BOOL},
    {"claude_cli_delegate_enabled", offsetof(config_t, claude_cli_delegate_enabled), sizeof(int), 1,
     CFG_BOOL},
    {"delegate_graph_context_enabled", offsetof(config_t, delegate_graph_context_enabled),
     sizeof(int), 0, CFG_BOOL},
    {"roundtable.replay_verify_enabled", offsetof(config_t, roundtable_replay_verify_enabled),
     sizeof(int), 1, CFG_BOOL},
    {"verify_cross_project", offsetof(config_t, verify_cross_project), sizeof(int), 1, CFG_BOOL},
    {"roundtable.max_rounds", offsetof(config_t, roundtable_max_rounds), sizeof(int), 0, CFG_INT},
    {"roundtable.converge_threshold", offsetof(config_t, roundtable_converge_threshold),
     sizeof(int), 0, CFG_INT},
    {"roundtable.deadline_ms", offsetof(config_t, roundtable_deadline_ms), sizeof(int), 0, CFG_INT},
    {"roundtable.turns", offsetof(config_t, roundtable_turns),
     sizeof(((config_t *)0)->roundtable_turns), 0, CFG_STRING},
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
    {NULL, 0, 0, 0, CFG_STRING},
};

const config_field_t *config_field_lookup(const char *key)
{
   if (!key)
      return NULL;
   for (int i = 0; config_fields[i].key; i++)
      if (strcmp(key, config_fields[i].key) == 0)
         return &config_fields[i];
   return NULL;
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
   return cJSON_CreateString(base);
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
   else
      snprintf(base, f->size, "%s", value);
   return 0;
}
