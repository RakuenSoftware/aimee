/* Generated event-bus config accessor implementations. */
#include "config.h"
#include "config_client.h"
#include <stdio.h>

int config_workspace_count(void)
{
   double value = 0;
   (void)config_client_read_number("workspace_count", &value);
   return (int)value;
}

int config_set_workspace_count(int value)
{
   return config_client_set_number("workspace_count", (double)value);
}

int config_code_cochange_git_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("code_cochange_git_enabled", &value);
   return (int)value;
}

int config_set_code_cochange_git_enabled(int value)
{
   return config_client_set_number("code_cochange_git_enabled", (double)value);
}

double config_memory_surprise_weight(void)
{
   double value = 0;
   (void)config_client_read_number("memory_surprise_weight", &value);
   return (double)value;
}

int config_set_memory_surprise_weight(double value)
{
   return config_client_set_number("memory_surprise_weight", (double)value);
}

int config_ingress_preinject_assembly_budget(void)
{
   double value = 0;
   (void)config_client_read_number("ingress_preinject_assembly_budget", &value);
   return (int)value;
}

int config_set_ingress_preinject_assembly_budget(int value)
{
   return config_client_set_number("ingress_preinject_assembly_budget", (double)value);
}

int config_prompt_manager_block_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("prompt_manager_block_enabled", &value);
   return (int)value;
}

int config_set_prompt_manager_block_enabled(int value)
{
   return config_client_set_number("prompt_manager_block_enabled", (double)value);
}

int config_gateway_prevent_subagents(void)
{
   double value = 0;
   (void)config_client_read_number("gateway_prevent_subagents", &value);
   return (int)value;
}

int config_set_gateway_prevent_subagents(int value)
{
   return config_client_set_number("gateway_prevent_subagents", (double)value);
}

int config_kb_pdf_assets_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_pdf_assets_enabled", &value);
   return (int)value;
}

int config_set_kb_pdf_assets_enabled(int value)
{
   return config_client_set_number("kb_pdf_assets_enabled", (double)value);
}

double config_memory_pagerank_weight(void)
{
   double value = 0;
   (void)config_client_read_number("memory_pagerank_weight", &value);
   return (double)value;
}

int config_set_memory_pagerank_weight(double value)
{
   return config_client_set_number("memory_pagerank_weight", (double)value);
}

int config_memory_prospective_max_matches(void)
{
   double value = 0;
   (void)config_client_read_number("memory_prospective_max_matches", &value);
   return (int)value;
}

int config_set_memory_prospective_max_matches(int value)
{
   return config_client_set_number("memory_prospective_max_matches", (double)value);
}

int config_memory_recall_limit_tokens_turn(void)
{
   double value = 0;
   (void)config_client_read_number("memory_recall_limit_tokens_turn", &value);
   return (int)value;
}

int config_set_memory_recall_limit_tokens_turn(int value)
{
   return config_client_set_number("memory_recall_limit_tokens_turn", (double)value);
}

int config_charter_safety_axioms_count(void)
{
   double value = 0;
   (void)config_client_read_number("charter_safety_axioms_count", &value);
   return (int)value;
}

int config_set_charter_safety_axioms_count(int value)
{
   return config_client_set_number("charter_safety_axioms_count", (double)value);
}

int config_memory_profile_cards_min_obs(void)
{
   double value = 0;
   (void)config_client_read_number("memory_profile_cards_min_obs", &value);
   return (int)value;
}

int config_set_memory_profile_cards_min_obs(int value)
{
   return config_client_set_number("memory_profile_cards_min_obs", (double)value);
}

int config_memory_negation_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_negation_enabled", &value);
   return (int)value;
}

int config_set_memory_negation_enabled(int value)
{
   return config_client_set_number("memory_negation_enabled", (double)value);
}

int config_memory_improve_summarise_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_improve_summarise_enabled", &value);
   return (int)value;
}

int config_set_memory_improve_summarise_enabled(int value)
{
   return config_client_set_number("memory_improve_summarise_enabled", (double)value);
}

int config_memory_abstain_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_abstain_enabled", &value);
   return (int)value;
}

int config_set_memory_abstain_enabled(int value)
{
   return config_client_set_number("memory_abstain_enabled", (double)value);
}

int config_dogfood_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("dogfood_enabled", &value);
   return (int)value;
}

int config_set_dogfood_enabled(int value)
{
   return config_client_set_number("dogfood_enabled", (double)value);
}

int config_learning_max_commits_per_week(void)
{
   double value = 0;
   (void)config_client_read_number("learning_max_commits_per_week", &value);
   return (int)value;
}

int config_set_learning_max_commits_per_week(int value)
{
   return config_client_set_number("learning_max_commits_per_week", (double)value);
}

int config_learning_implicit_workflow_repetition(void)
{
   double value = 0;
   (void)config_client_read_number("learning_implicit_workflow_repetition", &value);
   return (int)value;
}

int config_set_learning_implicit_workflow_repetition(int value)
{
   return config_client_set_number("learning_implicit_workflow_repetition", (double)value);
}

int config_cross_verify(void)
{
   double value = 0;
   (void)config_client_read_number("cross_verify", &value);
   return (int)value;
}

int config_set_cross_verify(int value)
{
   return config_client_set_number("cross_verify", (double)value);
}

int config_compute_threads(void)
{
   double value = 0;
   (void)config_client_read_number("compute_threads", &value);
   return (int)value;
}

int config_set_compute_threads(int value)
{
   return config_client_set_number("compute_threads", (double)value);
}

int config_concurrency_preempt_single_slot_only(void)
{
   double value = 0;
   (void)config_client_read_number("concurrency_preempt_single_slot_only", &value);
   return (int)value;
}

int config_set_concurrency_preempt_single_slot_only(int value)
{
   return config_client_set_number("concurrency_preempt_single_slot_only", (double)value);
}

int config_compact_per_tool_count(void)
{
   double value = 0;
   (void)config_client_read_number("compact_per_tool_count", &value);
   return (int)value;
}

int config_set_compact_per_tool_count(int value)
{
   return config_client_set_number("compact_per_tool_count", (double)value);
}

int config_fold_excerpt_bytes(void)
{
   double value = 0;
   (void)config_client_read_number("fold_excerpt_bytes", &value);
   return (int)value;
}

int config_set_fold_excerpt_bytes(int value)
{
   return config_client_set_number("fold_excerpt_bytes", (double)value);
}

int config_module_memory(void)
{
   double value = 0;
   (void)config_client_read_number("module_memory", &value);
   return (int)value;
}

int config_set_module_memory(int value)
{
   return config_client_set_number("module_memory", (double)value);
}

int config_autonomy_unit_retry(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_unit_retry", &value);
   return (int)value;
}

int config_set_autonomy_unit_retry(int value)
{
   return config_client_set_number("autonomy_unit_retry", (double)value);
}

int config_autonomy_max_resumes(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_max_resumes", &value);
   return (int)value;
}

int config_set_autonomy_max_resumes(int value)
{
   return config_client_set_number("autonomy_max_resumes", (double)value);
}

int config_mcp_osv_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("mcp_osv_enabled", &value);
   return (int)value;
}

int config_set_mcp_osv_enabled(int value)
{
   return config_client_set_number("mcp_osv_enabled", (double)value);
}

int config_integrity_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("integrity_enabled", &value);
   return (int)value;
}

int config_set_integrity_enabled(int value)
{
   return config_client_set_number("integrity_enabled", (double)value);
}

int config_cache_aware_rewrite_segment_check_turns(void)
{
   double value = 0;
   (void)config_client_read_number("cache_aware_rewrite_segment_check_turns", &value);
   return (int)value;
}

int config_set_cache_aware_rewrite_segment_check_turns(int value)
{
   return config_client_set_number("cache_aware_rewrite_segment_check_turns", (double)value);
}

int config_dedup_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("dedup_enabled", &value);
   return (int)value;
}

int config_set_dedup_enabled(int value)
{
   return config_client_set_number("dedup_enabled", (double)value);
}

int config_kb_api_http_port(void)
{
   double value = 0;
   (void)config_client_read_number("kb_api_http_port", &value);
   return (int)value;
}

int config_set_kb_api_http_port(int value)
{
   return config_client_set_number("kb_api_http_port", (double)value);
}

int config_server_api_cli_session_forwarding(void)
{
   double value = 0;
   (void)config_client_read_number("server_api_cli_session_forwarding", &value);
   return (int)value;
}

int config_set_server_api_cli_session_forwarding(int value)
{
   return config_client_set_number("server_api_cli_session_forwarding", (double)value);
}

double config_calibration_conformal_epsilon(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_conformal_epsilon", &value);
   return (double)value;
}

int config_set_calibration_conformal_epsilon(double value)
{
   return config_client_set_number("calibration_conformal_epsilon", (double)value);
}

int config_demotion_n_min(void)
{
   double value = 0;
   (void)config_client_read_number("demotion_n_min", &value);
   return (int)value;
}

int config_set_demotion_n_min(int value)
{
   return config_client_set_number("demotion_n_min", (double)value);
}

double config_bandit_exploration_fraction(void)
{
   double value = 0;
   (void)config_client_read_number("bandit_exploration_fraction", &value);
   return (double)value;
}

int config_set_bandit_exploration_fraction(double value)
{
   return config_client_set_number("bandit_exploration_fraction", (double)value);
}

int config_kb_reflection_synthesis_shadow(void)
{
   double value = 0;
   (void)config_client_read_number("kb_reflection_synthesis_shadow", &value);
   return (int)value;
}

int config_set_kb_reflection_synthesis_shadow(int value)
{
   return config_client_set_number("kb_reflection_synthesis_shadow", (double)value);
}

double config_code_hybrid_weight_code(void)
{
   double value = 0;
   (void)config_client_read_number("code_hybrid_weight_code", &value);
   return (double)value;
}

int config_set_code_hybrid_weight_code(double value)
{
   return config_client_set_number("code_hybrid_weight_code", (double)value);
}

int config_kb_purge_fence_ttl_s(void)
{
   double value = 0;
   (void)config_client_read_number("kb_purge_fence_ttl_s", &value);
   return (int)value;
}

int config_set_kb_purge_fence_ttl_s(int value)
{
   return config_client_set_number("kb_purge_fence_ttl_s", (double)value);
}

int config_kb_mining_min_poll_s(void)
{
   double value = 0;
   (void)config_client_read_number("kb_mining_min_poll_s", &value);
   return (int)value;
}

int config_set_kb_mining_min_poll_s(int value)
{
   return config_client_set_number("kb_mining_min_poll_s", (double)value);
}

int config_review_batch_cap(void)
{
   double value = 0;
   (void)config_client_read_number("review_batch_cap", &value);
   return (int)value;
}

int config_set_review_batch_cap(int value)
{
   return config_client_set_number("review_batch_cap", (double)value);
}

int config_kb_curator_detect_contradictions_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_detect_contradictions_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_detect_contradictions_enabled(int value)
{
   return config_client_set_number("kb_curator_detect_contradictions_enabled", (double)value);
}

int config_kb_curator_max_attempts(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_max_attempts", &value);
   return (int)value;
}

int config_set_kb_curator_max_attempts(int value)
{
   return config_client_set_number("kb_curator_max_attempts", (double)value);
}

int config_kb_curator_cross_repo_caller_collision_c(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_caller_collision_c", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_caller_collision_c(int value)
{
   return config_client_set_number("kb_curator_cross_repo_caller_collision_c", (double)value);
}

int config_skills_stale_after_days(void)
{
   double value = 0;
   (void)config_client_read_number("skills_stale_after_days", &value);
   return (int)value;
}

int config_set_skills_stale_after_days(int value)
{
   return config_client_set_number("skills_stale_after_days", (double)value);
}

int config_worktree_gc_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("worktree_gc_enabled", &value);
   return (int)value;
}

int config_set_worktree_gc_enabled(int value)
{
   return config_client_set_number("worktree_gc_enabled", (double)value);
}

int64_t config_db2_vector_corpus_diskann_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("db2_vector_corpus_diskann_threshold", &value);
   return (int64_t)value;
}

int config_set_db2_vector_corpus_diskann_threshold(int64_t value)
{
   return config_client_set_number("db2_vector_corpus_diskann_threshold", (double)value);
}

int config_roundtable_pipeline_max_passes(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_pipeline_max_passes", &value);
   return (int)value;
}

int config_set_roundtable_pipeline_max_passes(int value)
{
   return config_client_set_number("roundtable_pipeline_max_passes", (double)value);
}

const char *config_db2_url(void)
{
   static _Thread_local char value[CONFIG_DB2_URL_LEN];
   (void)config_client_read_string("db2_url", value, sizeof(value));
   return value;
}

int config_set_db2_url(const char *value)
{
   return config_client_set_string("db2_url", value);
}

size_t config_db2_url_copy(char *out, size_t n)
{
   char value[CONFIG_DB2_URL_LEN];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("db2_url", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_openai_key_cmd(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("openai_key_cmd", value, sizeof(value));
   return value;
}

int config_set_openai_key_cmd(const char *value)
{
   return config_client_set_string("openai_key_cmd", value);
}

size_t config_openai_key_cmd_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("openai_key_cmd", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_vault_custody(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("vault_custody", value, sizeof(value));
   return value;
}

int config_set_vault_custody(const char *value)
{
   return config_client_set_string("vault_custody", value);
}

size_t config_vault_custody_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("vault_custody", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_session_worktree_base(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("session_worktree_base", value, sizeof(value));
   return value;
}

int config_set_session_worktree_base(const char *value)
{
   return config_client_set_string("session_worktree_base", value);
}

size_t config_session_worktree_base_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("session_worktree_base", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_pagerank_relations(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("memory_pagerank_relations", value, sizeof(value));
   return value;
}

int config_set_memory_pagerank_relations(const char *value)
{
   return config_client_set_string("memory_pagerank_relations", value);
}

size_t config_memory_pagerank_relations_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_pagerank_relations", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_learning_synthesize_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("learning_synthesize_command", value, sizeof(value));
   return value;
}

int config_set_learning_synthesize_command(const char *value)
{
   return config_client_set_string("learning_synthesize_command", value);
}

size_t config_learning_synthesize_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("learning_synthesize_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_search_tavily_api_key(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("search_tavily_api_key", value, sizeof(value));
   return value;
}

int config_set_search_tavily_api_key(const char *value)
{
   return config_client_set_string("search_tavily_api_key", value);
}

size_t config_search_tavily_api_key_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("search_tavily_api_key", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_mcp_osv_endpoint(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("mcp_osv_endpoint", value, sizeof(value));
   return value;
}

int config_set_mcp_osv_endpoint(const char *value)
{
   return config_client_set_string("mcp_osv_endpoint", value);
}

size_t config_mcp_osv_endpoint_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("mcp_osv_endpoint", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_telemetry_metrics_token(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("telemetry_metrics_token", value, sizeof(value));
   return value;
}

int config_set_telemetry_metrics_token(const char *value)
{
   return config_client_set_string("telemetry_metrics_token", value);
}

size_t config_telemetry_metrics_token_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("telemetry_metrics_token", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_synthesis_api_key(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("synthesis_api_key", value, sizeof(value));
   return value;
}

int config_set_synthesis_api_key(const char *value)
{
   return config_client_set_string("synthesis_api_key", value);
}

size_t config_synthesis_api_key_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("synthesis_api_key", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_ranker_fuse_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("ranker_fuse_command", value, sizeof(value));
   return value;
}

int config_set_ranker_fuse_command(const char *value)
{
   return config_client_set_string("ranker_fuse_command", value);
}

size_t config_ranker_fuse_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("ranker_fuse_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_synthesize_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("kb_synthesize_command", value, sizeof(value));
   return value;
}

int config_set_kb_synthesize_command(const char *value)
{
   return config_client_set_string("kb_synthesize_command", value);
}

size_t config_kb_synthesize_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_synthesize_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_user_presets(void)
{
   static _Thread_local char value[4096];
   (void)config_client_read_string("kb_curator_user_presets", value, sizeof(value));
   return value;
}

int config_set_kb_curator_user_presets(const char *value)
{
   return config_client_set_string("kb_curator_user_presets", value);
}

size_t config_kb_curator_user_presets_copy(char *out, size_t n)
{
   char value[4096];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_user_presets", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_aux_default_model(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("aux_default_model", value, sizeof(value));
   return value;
}

int config_set_aux_default_model(const char *value)
{
   return config_client_set_string("aux_default_model", value);
}

size_t config_aux_default_model_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("aux_default_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_workspace_providers(int index)
{
   static _Thread_local char value[16];
   (void)config_client_read_indexed_string("workspace_providers", index, NULL, value, sizeof(value));
   return value;
}

const char *config_identity_working_profile_injection_fields(int index)
{
   static _Thread_local char value[CONFIG_WORKING_PROFILE_FIELD_LEN];
   (void)config_client_read_indexed_string("identity_working_profile_injection_fields", index, NULL, value, sizeof(value));
   return value;
}

int config_mcp_client_command_count(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("mcp_clients", index, "command_count", &value);
   return (int)value;
}

const char *config_cron_job_prompt(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_PROMPT];
   (void)config_client_read_indexed_string("cron_jobs", index, "prompt", value, sizeof(value));
   return value;
}

int config_cron_job_pre_wake_gate(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("cron_jobs", index, "pre_wake_gate", &value);
   return (int)value;
}

double config_trigger_rule_max_spend_usd(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("trigger_rules", index, "max_spend_usd", &value);
   return (double)value;
}
