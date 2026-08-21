/* Generated event-bus config accessor implementations. */
#include "config.h"
#include "config_client.h"
#include <stdio.h>

int config_memory_maintenance_trigger_inserts(void)
{
   double value = 0;
   (void)config_client_read_number("memory_maintenance_trigger_inserts", &value);
   return (int)value;
}

int config_set_memory_maintenance_trigger_inserts(int value)
{
   return config_client_set_number("memory_maintenance_trigger_inserts", (double)value);
}

int config_audit_action_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("audit_action_enabled", &value);
   return (int)value;
}

int config_set_audit_action_enabled(int value)
{
   return config_client_set_number("audit_action_enabled", (double)value);
}

int config_memory_cognify_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_cognify_enabled", &value);
   return (int)value;
}

int config_set_memory_cognify_enabled(int value)
{
   return config_client_set_number("memory_cognify_enabled", (double)value);
}

int config_tool_output_max_bytes(void)
{
   double value = 0;
   (void)config_client_read_number("tool_output_max_bytes", &value);
   return (int)value;
}

int config_set_tool_output_max_bytes(int value)
{
   return config_client_set_number("tool_output_max_bytes", (double)value);
}

int config_feature_auto_promote(void)
{
   double value = 0;
   (void)config_client_read_number("feature_auto_promote", &value);
   return (int)value;
}

int config_set_feature_auto_promote(int value)
{
   return config_client_set_number("feature_auto_promote", (double)value);
}

int config_kb_typed_facts_promote_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("kb_typed_facts_promote_threshold", &value);
   return (int)value;
}

int config_set_kb_typed_facts_promote_threshold(int value)
{
   return config_client_set_number("kb_typed_facts_promote_threshold", (double)value);
}

int config_kb_pdf_ocr_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_pdf_ocr_enabled", &value);
   return (int)value;
}

int config_set_kb_pdf_ocr_enabled(int value)
{
   return config_client_set_number("kb_pdf_ocr_enabled", (double)value);
}

int config_memory_briefing_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_briefing_enabled", &value);
   return (int)value;
}

int config_set_memory_briefing_enabled(int value)
{
   return config_client_set_number("memory_briefing_enabled", (double)value);
}

int config_memory_lifecycle_ttl_date_days(void)
{
   double value = 0;
   (void)config_client_read_number("memory_lifecycle_ttl_date_days", &value);
   return (int)value;
}

int config_set_memory_lifecycle_ttl_date_days(int value)
{
   return config_client_set_number("memory_lifecycle_ttl_date_days", (double)value);
}

int config_memory_directives_max_matches(void)
{
   double value = 0;
   (void)config_client_read_number("memory_directives_max_matches", &value);
   return (int)value;
}

int config_set_memory_directives_max_matches(int value)
{
   return config_client_set_number("memory_directives_max_matches", (double)value);
}

int config_charter_tone_boundaries_count(void)
{
   double value = 0;
   (void)config_client_read_number("charter_tone_boundaries_count", &value);
   return (int)value;
}

int config_set_charter_tone_boundaries_count(int value)
{
   return config_client_set_number("charter_tone_boundaries_count", (double)value);
}

int config_memory_rewrite_hyde(void)
{
   double value = 0;
   (void)config_client_read_number("memory_rewrite_hyde", &value);
   return (int)value;
}

int config_set_memory_rewrite_hyde(int value)
{
   return config_client_set_number("memory_rewrite_hyde", (double)value);
}

int config_memory_recall_lanes_k_summary(void)
{
   double value = 0;
   (void)config_client_read_number("memory_recall_lanes_k_summary", &value);
   return (int)value;
}

int config_set_memory_recall_lanes_k_summary(int value)
{
   return config_client_set_number("memory_recall_lanes_k_summary", (double)value);
}

int config_memory_episode_summaries_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_episode_summaries_enabled", &value);
   return (int)value;
}

int config_set_memory_episode_summaries_enabled(int value)
{
   return config_client_set_number("memory_episode_summaries_enabled", (double)value);
}

double config_memory_bm25_weight(void)
{
   double value = 0;
   (void)config_client_read_number("memory_bm25_weight", &value);
   return (double)value;
}

int config_set_memory_bm25_weight(double value)
{
   return config_client_set_number("memory_bm25_weight", (double)value);
}

int config_dogfood_autolabel_repair(void)
{
   double value = 0;
   (void)config_client_read_number("dogfood_autolabel_repair", &value);
   return (int)value;
}

int config_set_dogfood_autolabel_repair(int value)
{
   return config_client_set_number("dogfood_autolabel_repair", (double)value);
}

int config_learning_synthesize_k(void)
{
   double value = 0;
   (void)config_client_read_number("learning_synthesize_k", &value);
   return (int)value;
}

int config_set_learning_synthesize_k(int value)
{
   return config_client_set_number("learning_synthesize_k", (double)value);
}

int config_verify_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("verify_enabled", &value);
   return (int)value;
}

int config_set_verify_enabled(int value)
{
   return config_client_set_number("verify_enabled", (double)value);
}

int config_retry_max_ms(void)
{
   double value = 0;
   (void)config_client_read_number("retry_max_ms", &value);
   return (int)value;
}

int config_set_retry_max_ms(int value)
{
   return config_client_set_number("retry_max_ms", (double)value);
}

int config_maximum_total_concurrent_agent_sessions(void)
{
   double value = 0;
   (void)config_client_read_number("maximum_total_concurrent_agent_sessions", &value);
   return (int)value;
}

int config_set_maximum_total_concurrent_agent_sessions(int value)
{
   return config_client_set_number("maximum_total_concurrent_agent_sessions", (double)value);
}

int config_search_fetch_pages(void)
{
   double value = 0;
   (void)config_client_read_number("search_fetch_pages", &value);
   return (int)value;
}

int config_set_search_fetch_pages(int value)
{
   return config_client_set_number("search_fetch_pages", (double)value);
}

int config_coord_closet_budget_bytes(void)
{
   double value = 0;
   (void)config_client_read_number("coord_closet_budget_bytes", &value);
   return (int)value;
}

int config_set_coord_closet_budget_bytes(int value)
{
   return config_client_set_number("coord_closet_budget_bytes", (double)value);
}

int config_fold_freeze_tail_cap_msgs(void)
{
   double value = 0;
   (void)config_client_read_number("fold_freeze_tail_cap_msgs", &value);
   return (int)value;
}

int config_set_fold_freeze_tail_cap_msgs(int value)
{
   return config_client_set_number("fold_freeze_tail_cap_msgs", (double)value);
}

int config_module_workflows(void)
{
   double value = 0;
   (void)config_client_read_number("module_workflows", &value);
   return (int)value;
}

int config_set_module_workflows(int value)
{
   return config_client_set_number("module_workflows", (double)value);
}

int config_autonomy_max_turns(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_max_turns", &value);
   return (int)value;
}

int config_set_autonomy_max_turns(int value)
{
   return config_client_set_number("autonomy_max_turns", (double)value);
}

int config_max_worktrees(void)
{
   double value = 0;
   (void)config_client_read_number("max_worktrees", &value);
   return (int)value;
}

int config_set_max_worktrees(int value)
{
   return config_client_set_number("max_worktrees", (double)value);
}

int config_mcp_osv_cache_ttl_hours(void)
{
   double value = 0;
   (void)config_client_read_number("mcp_osv_cache_ttl_hours", &value);
   return (int)value;
}

int config_set_mcp_osv_cache_ttl_hours(int value)
{
   return config_client_set_number("mcp_osv_cache_ttl_hours", (double)value);
}

int config_virtual_context_assembly_budget(void)
{
   double value = 0;
   (void)config_client_read_number("virtual_context_assembly_budget", &value);
   return (int)value;
}

int config_set_virtual_context_assembly_budget(int value)
{
   return config_client_set_number("virtual_context_assembly_budget", (double)value);
}

int config_transport_kb_gzip_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("transport_kb_gzip_enabled", &value);
   return (int)value;
}

int config_set_transport_kb_gzip_enabled(int value)
{
   return config_client_set_number("transport_kb_gzip_enabled", (double)value);
}

int config_dedup_window_seconds(void)
{
   double value = 0;
   (void)config_client_read_number("dedup_window_seconds", &value);
   return (int)value;
}

int config_set_dedup_window_seconds(int value)
{
   return config_client_set_number("dedup_window_seconds", (double)value);
}

int config_server_api_tls_port(void)
{
   double value = 0;
   (void)config_client_read_number("server_api_tls_port", &value);
   return (int)value;
}

int config_set_server_api_tls_port(int value)
{
   return config_client_set_number("server_api_tls_port", (double)value);
}

int config_calibration_buckets(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_buckets", &value);
   return (int)value;
}

int config_set_calibration_buckets(int value)
{
   return config_client_set_number("calibration_buckets", (double)value);
}

double config_calibration_tau_working_profile_auto(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_tau_working_profile_auto", &value);
   return (double)value;
}

int config_set_calibration_tau_working_profile_auto(double value)
{
   return config_client_set_number("calibration_tau_working_profile_auto", (double)value);
}

int config_kb_ranker_fit_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_ranker_fit_enabled", &value);
   return (int)value;
}

int config_set_kb_ranker_fit_enabled(int value)
{
   return config_client_set_number("kb_ranker_fit_enabled", (double)value);
}

int config_planner_budget_default(void)
{
   double value = 0;
   (void)config_client_read_number("planner_budget_default", &value);
   return (int)value;
}

int config_set_planner_budget_default(int value)
{
   return config_client_set_number("planner_budget_default", (double)value);
}

int config_kb_connection_workers(void)
{
   double value = 0;
   (void)config_client_read_number("kb_connection_workers", &value);
   return (int)value;
}

int config_set_kb_connection_workers(int value)
{
   return config_client_set_number("kb_connection_workers", (double)value);
}

double config_code_hybrid_weight_memory(void)
{
   double value = 0;
   (void)config_client_read_number("code_hybrid_weight_memory", &value);
   return (double)value;
}

int config_set_code_hybrid_weight_memory(double value)
{
   return config_client_set_number("code_hybrid_weight_memory", (double)value);
}

double config_kb_maintenance_lambda(void)
{
   double value = 0;
   (void)config_client_read_number("kb_maintenance_lambda", &value);
   return (double)value;
}

int config_set_kb_maintenance_lambda(double value)
{
   return config_client_set_number("kb_maintenance_lambda", (double)value);
}

int config_trigger_rule_count(void)
{
   double value = 0;
   (void)config_client_read_number("trigger_rule_count", &value);
   return (int)value;
}

int config_set_trigger_rule_count(int value)
{
   return config_client_set_number("trigger_rule_count", (double)value);
}

int config_kb_curator_extract_code_workers(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_extract_code_workers", &value);
   return (int)value;
}

int config_set_kb_curator_extract_code_workers(int value)
{
   return config_client_set_number("kb_curator_extract_code_workers", (double)value);
}

int config_kb_curator_projection_graph_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_projection_graph_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_projection_graph_enabled(int value)
{
   return config_client_set_number("kb_curator_projection_graph_enabled", (double)value);
}

int config_kb_curator_cross_repo_distinctiveness_v(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_distinctiveness_v", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_distinctiveness_v(int value)
{
   return config_client_set_number("kb_curator_cross_repo_distinctiveness_v", (double)value);
}

int config_kb_curator_cross_repo_review_queue_max(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_review_queue_max", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_review_queue_max(int value)
{
   return config_client_set_number("kb_curator_cross_repo_review_queue_max", (double)value);
}

int config_skills_dispatch_max_index(void)
{
   double value = 0;
   (void)config_client_read_number("skills_dispatch_max_index", &value);
   return (int)value;
}

int config_set_skills_dispatch_max_index(int value)
{
   return config_client_set_number("skills_dispatch_max_index", (double)value);
}

int config_aux_default_max_tokens(void)
{
   double value = 0;
   (void)config_client_read_number("aux_default_max_tokens", &value);
   return (int)value;
}

int config_set_aux_default_max_tokens(int value)
{
   return config_client_set_number("aux_default_max_tokens", (double)value);
}

int config_ensemble_min_successful(void)
{
   double value = 0;
   (void)config_client_read_number("ensemble_min_successful", &value);
   return (int)value;
}

int config_set_ensemble_min_successful(int value)
{
   return config_client_set_number("ensemble_min_successful", (double)value);
}

double config_roundtable_pipeline_max_total_cost_usd(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_pipeline_max_total_cost_usd", &value);
   return (double)value;
}

int config_set_roundtable_pipeline_max_total_cost_usd(double value)
{
   return config_client_set_number("roundtable_pipeline_max_total_cost_usd", (double)value);
}

const char *config_claude_model(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("claude_model", value, sizeof(value));
   return value;
}

int config_set_claude_model(const char *value)
{
   return config_client_set_string("claude_model", value);
}

size_t config_claude_model_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("claude_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_embedder_api_key(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("embedder_api_key", value, sizeof(value));
   return value;
}

int config_set_embedder_api_key(const char *value)
{
   return config_client_set_string("embedder_api_key", value);
}

size_t config_embedder_api_key_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("embedder_api_key", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_vault_tpm2_nv_index(void)
{
   static _Thread_local char value[32];
   (void)config_client_read_string("vault_tpm2_nv_index", value, sizeof(value));
   return value;
}

int config_set_vault_tpm2_nv_index(const char *value)
{
   return config_client_set_string("vault_tpm2_nv_index", value);
}

size_t config_vault_tpm2_nv_index_copy(char *out, size_t n)
{
   char value[32];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("vault_tpm2_nv_index", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_delegate_sandbox_package_access(void)
{
   static _Thread_local char value[32];
   (void)config_client_read_string("delegate_sandbox_package_access", value, sizeof(value));
   return value;
}

int config_set_delegate_sandbox_package_access(const char *value)
{
   return config_client_set_string("delegate_sandbox_package_access", value);
}

size_t config_delegate_sandbox_package_access_copy(char *out, size_t n)
{
   char value[32];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("delegate_sandbox_package_access", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_query_expansion_mode(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("memory_query_expansion_mode", value, sizeof(value));
   return value;
}

int config_set_memory_query_expansion_mode(const char *value)
{
   return config_client_set_string("memory_query_expansion_mode", value);
}

size_t config_memory_query_expansion_mode_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_query_expansion_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_verify_cmd(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("verify_cmd", value, sizeof(value));
   return value;
}

int config_set_verify_cmd(const char *value)
{
   return config_client_set_string("verify_cmd", value);
}

size_t config_verify_cmd_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("verify_cmd", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_prompt_tier(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("prompt_tier", value, sizeof(value));
   return value;
}

int config_set_prompt_tier(const char *value)
{
   return config_client_set_string("prompt_tier", value);
}

size_t config_prompt_tier_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("prompt_tier", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_proxy_token(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("proxy_token", value, sizeof(value));
   return value;
}

int config_set_proxy_token(const char *value)
{
   return config_client_set_string("proxy_token", value);
}

size_t config_proxy_token_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("proxy_token", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_mode(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("kb_mode", value, sizeof(value));
   return value;
}

int config_set_kb_mode(const char *value)
{
   return config_client_set_string("kb_mode", value);
}

size_t config_kb_mode_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_server_api_client_transport(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("server_api_client_transport", value, sizeof(value));
   return value;
}

int config_set_server_api_client_transport(const char *value)
{
   return config_client_set_string("server_api_client_transport", value);
}

size_t config_server_api_client_transport_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("server_api_client_transport", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_ranker_fit_objective(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("kb_ranker_fit_objective", value, sizeof(value));
   return value;
}

int config_set_kb_ranker_fit_objective(const char *value)
{
   return config_client_set_string("kb_ranker_fit_objective", value);
}

size_t config_kb_ranker_fit_objective_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_ranker_fit_objective", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_extract_prompt_version(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("kb_curator_extract_prompt_version", value, sizeof(value));
   return value;
}

int config_set_kb_curator_extract_prompt_version(const char *value)
{
   return config_client_set_string("kb_curator_extract_prompt_version", value);
}

size_t config_kb_curator_extract_prompt_version_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_extract_prompt_version", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_provider_model(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("kb_curator_provider_model", value, sizeof(value));
   return value;
}

int config_set_kb_curator_provider_model(const char *value)
{
   return config_client_set_string("kb_curator_provider_model", value);
}

size_t config_kb_curator_provider_model_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_provider_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_roundtable_turns(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("roundtable_turns", value, sizeof(value));
   return value;
}

int config_set_roundtable_turns(const char *value)
{
   return config_client_set_string("roundtable_turns", value);
}

size_t config_roundtable_turns_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("roundtable_turns", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_workspace_sandbox_image(int index)
{
   static _Thread_local char value[256];
   (void)config_client_read_indexed_string("workspace_sandbox_image", index, NULL, value, sizeof(value));
   return value;
}

const char *config_computer_use_allowed_domains(int index)
{
   static _Thread_local char value[128];
   (void)config_client_read_indexed_string("computer_use_allowed_domains", index, NULL, value, sizeof(value));
   return value;
}

const char *config_mcp_client_bearer_token_env(int index)
{
   static _Thread_local char value[128];
   (void)config_client_read_indexed_string("mcp_clients", index, "bearer_token_env", value, sizeof(value));
   return value;
}

const char *config_cron_job_when_context_contains(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_WHEN_CONTEXT];
   (void)config_client_read_indexed_string("cron_jobs", index, "when_context_contains", value, sizeof(value));
   return value;
}

const char *config_trigger_rule_event(int index)
{
   static _Thread_local char value[TRIGGER_RULE_MAX_EVENT];
   (void)config_client_read_indexed_string("trigger_rules", index, "event", value, sizeof(value));
   return value;
}
