/* Generated event-bus config accessor implementations. */
#include "config.h"
#include "config_client.h"
#include <stdio.h>

int config_db2_pool_size(void)
{
   double value = 0;
   (void)config_client_read_number("db2_pool_size", &value);
   return (int)value;
}

int config_set_db2_pool_size(int value)
{
   return config_client_set_number("db2_pool_size", (double)value);
}

int config_memory_maintenance_summarize_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_maintenance_summarize_enabled", &value);
   return (int)value;
}

int config_set_memory_maintenance_summarize_enabled(int value)
{
   return config_client_set_number("memory_maintenance_summarize_enabled", (double)value);
}

int config_memory_surprise_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_surprise_enabled", &value);
   return (int)value;
}

int config_set_memory_surprise_enabled(int value)
{
   return config_client_set_number("memory_surprise_enabled", (double)value);
}

int config_ingress_preinject_anthropic_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("ingress_preinject_anthropic_enabled", &value);
   return (int)value;
}

int config_set_ingress_preinject_anthropic_enabled(int value)
{
   return config_client_set_number("ingress_preinject_anthropic_enabled", (double)value);
}

int config_delegates_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("delegates_enabled", &value);
   return (int)value;
}

int config_set_delegates_enabled(int value)
{
   return config_client_set_number("delegates_enabled", (double)value);
}

int config_delegate_sandbox_learn_packages(void)
{
   double value = 0;
   (void)config_client_read_number("delegate_sandbox_learn_packages", &value);
   return (int)value;
}

int config_set_delegate_sandbox_learn_packages(int value)
{
   return config_client_set_number("delegate_sandbox_learn_packages", (double)value);
}

int config_kb_pdf_tsr_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_pdf_tsr_enabled", &value);
   return (int)value;
}

int config_set_kb_pdf_tsr_enabled(int value)
{
   return config_client_set_number("kb_pdf_tsr_enabled", (double)value);
}

int config_memory_pagerank_iterations(void)
{
   double value = 0;
   (void)config_client_read_number("memory_pagerank_iterations", &value);
   return (int)value;
}

int config_set_memory_pagerank_iterations(int value)
{
   return config_client_set_number("memory_pagerank_iterations", (double)value);
}

int config_memory_prospective_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_prospective_enabled", &value);
   return (int)value;
}

int config_set_memory_prospective_enabled(int value)
{
   return config_client_set_number("memory_prospective_enabled", (double)value);
}

int config_memory_recall_limit_tokens_session(void)
{
   double value = 0;
   (void)config_client_read_number("memory_recall_limit_tokens_session", &value);
   return (int)value;
}

int config_set_memory_recall_limit_tokens_session(int value)
{
   return config_client_set_number("memory_recall_limit_tokens_session", (double)value);
}

int config_disposition_project_count(void)
{
   double value = 0;
   (void)config_client_read_number("disposition_project_count", &value);
   return (int)value;
}

int config_set_disposition_project_count(int value)
{
   return config_client_set_number("disposition_project_count", (double)value);
}

int config_memory_profile_cards_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_profile_cards_enabled", &value);
   return (int)value;
}

int config_set_memory_profile_cards_enabled(int value)
{
   return config_client_set_number("memory_profile_cards_enabled", (double)value);
}

int config_kb_search_max_results(void)
{
   double value = 0;
   (void)config_client_read_number("kb_search_max_results", &value);
   return (int)value;
}

int config_set_kb_search_max_results(int value)
{
   return config_client_set_number("kb_search_max_results", (double)value);
}

int config_memory_improve_dedupe_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_improve_dedupe_enabled", &value);
   return (int)value;
}

int config_set_memory_improve_dedupe_enabled(int value)
{
   return config_client_set_number("memory_improve_dedupe_enabled", (double)value);
}

double config_memory_failure_detection_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("memory_failure_detection_threshold", &value);
   return (double)value;
}

int config_set_memory_failure_detection_threshold(double value)
{
   return config_client_set_number("memory_failure_detection_threshold", (double)value);
}

int config_memory_fetch_budget_shape_aware(void)
{
   double value = 0;
   (void)config_client_read_number("memory_fetch_budget_shape_aware", &value);
   return (int)value;
}

int config_set_memory_fetch_budget_shape_aware(int value)
{
   return config_client_set_number("memory_fetch_budget_shape_aware", (double)value);
}

int config_learning_proposal_ttl_days(void)
{
   double value = 0;
   (void)config_client_read_number("learning_proposal_ttl_days", &value);
   return (int)value;
}

int config_set_learning_proposal_ttl_days(int value)
{
   return config_client_set_number("learning_proposal_ttl_days", (double)value);
}

int config_learning_implicit_repeated_correction(void)
{
   double value = 0;
   (void)config_client_read_number("learning_implicit_repeated_correction", &value);
   return (int)value;
}

int config_set_learning_implicit_repeated_correction(int value)
{
   return config_client_set_number("learning_implicit_repeated_correction", (double)value);
}

int config_verify_cross_project(void)
{
   double value = 0;
   (void)config_client_read_number("verify_cross_project", &value);
   return (int)value;
}

int config_set_verify_cross_project(int value)
{
   return config_client_set_number("verify_cross_project", (double)value);
}

int config_max_delegation_spawns(void)
{
   double value = 0;
   (void)config_client_read_number("max_delegation_spawns", &value);
   return (int)value;
}

int config_set_max_delegation_spawns(int value)
{
   return config_client_set_number("max_delegation_spawns", (double)value);
}

int config_concurrency_preempt_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("concurrency_preempt_enabled", &value);
   return (int)value;
}

int config_set_concurrency_preempt_enabled(int value)
{
   return config_client_set_number("concurrency_preempt_enabled", (double)value);
}

int config_compact_tail_bytes(void)
{
   double value = 0;
   (void)config_client_read_number("compact_tail_bytes", &value);
   return (int)value;
}

int config_set_compact_tail_bytes(int value)
{
   return config_client_set_number("compact_tail_bytes", (double)value);
}

int config_fold_min_fold_msgs(void)
{
   double value = 0;
   (void)config_client_read_number("fold_min_fold_msgs", &value);
   return (int)value;
}

int config_set_fold_min_fold_msgs(int value)
{
   return config_client_set_number("fold_min_fold_msgs", (double)value);
}

int config_economizer_mode(void)
{
   double value = 0;
   (void)config_client_read_number("economizer_mode", &value);
   return (int)value;
}

int config_set_economizer_mode(int value)
{
   return config_client_set_number("economizer_mode", (double)value);
}

int config_autonomy_fanout(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_fanout", &value);
   return (int)value;
}

int config_set_autonomy_fanout(int value)
{
   return config_client_set_number("autonomy_fanout", (double)value);
}

int config_autonomy_auto_resume_cap_parks(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_auto_resume_cap_parks", &value);
   return (int)value;
}

int config_set_autonomy_auto_resume_cap_parks(int value)
{
   return config_client_set_number("autonomy_auto_resume_cap_parks", (double)value);
}

int config_mcp_client_count(void)
{
   double value = 0;
   (void)config_client_read_number("mcp_client_count", &value);
   return (int)value;
}

int config_set_mcp_client_count(int value)
{
   return config_client_set_number("mcp_client_count", (double)value);
}

int config_computer_use_allowed_domain_count(void)
{
   double value = 0;
   (void)config_client_read_number("computer_use_allowed_domain_count", &value);
   return (int)value;
}

int config_set_computer_use_allowed_domain_count(int value)
{
   return config_client_set_number("computer_use_allowed_domain_count", (double)value);
}

int config_cache_aware_rewrite_max_defer_turns(void)
{
   double value = 0;
   (void)config_client_read_number("cache_aware_rewrite_max_defer_turns", &value);
   return (int)value;
}

int config_set_cache_aware_rewrite_max_defer_turns(int value)
{
   return config_client_set_number("cache_aware_rewrite_max_defer_turns", (double)value);
}

int config_reasoning_cap_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("reasoning_cap_enabled", &value);
   return (int)value;
}

int config_set_reasoning_cap_enabled(int value)
{
   return config_client_set_number("reasoning_cap_enabled", (double)value);
}

double config_guardrails_semantic_block_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("guardrails_semantic_block_threshold", &value);
   return (double)value;
}

int config_set_guardrails_semantic_block_threshold(double value)
{
   return config_client_set_number("guardrails_semantic_block_threshold", (double)value);
}

int config_server_api_max_event_streams(void)
{
   double value = 0;
   (void)config_client_read_number("server_api_max_event_streams", &value);
   return (int)value;
}

int config_set_server_api_max_event_streams(int value)
{
   return config_client_set_number("server_api_max_event_streams", (double)value);
}

int config_calibration_conformal_window(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_conformal_window", &value);
   return (int)value;
}

int config_set_calibration_conformal_window(int value)
{
   return config_client_set_number("calibration_conformal_window", (double)value);
}

double config_demotion_half_life_days(void)
{
   double value = 0;
   (void)config_client_read_number("demotion_half_life_days", &value);
   return (double)value;
}

int config_set_demotion_half_life_days(double value)
{
   return config_client_set_number("demotion_half_life_days", (double)value);
}

int config_reasoning_time_limit_ms(void)
{
   double value = 0;
   (void)config_client_read_number("reasoning_time_limit_ms", &value);
   return (int)value;
}

int config_set_reasoning_time_limit_ms(int value)
{
   return config_client_set_number("reasoning_time_limit_ms", (double)value);
}

int config_kb_synthesize_n_attempts(void)
{
   double value = 0;
   (void)config_client_read_number("kb_synthesize_n_attempts", &value);
   return (int)value;
}

int config_set_kb_synthesize_n_attempts(int value)
{
   return config_client_set_number("kb_synthesize_n_attempts", (double)value);
}

int config_kb_bg_watch_debounce_secs(void)
{
   double value = 0;
   (void)config_client_read_number("kb_bg_watch_debounce_secs", &value);
   return (int)value;
}

int config_set_kb_bg_watch_debounce_secs(int value)
{
   return config_client_set_number("kb_bg_watch_debounce_secs", (double)value);
}

int config_kb_reembed_on_dim_change(void)
{
   double value = 0;
   (void)config_client_read_number("kb_reembed_on_dim_change", &value);
   return (int)value;
}

int config_set_kb_reembed_on_dim_change(int value)
{
   return config_client_set_number("kb_reembed_on_dim_change", (double)value);
}

int config_kb_mining_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_mining_enabled", &value);
   return (int)value;
}

int config_set_kb_mining_enabled(int value)
{
   return config_client_set_number("kb_mining_enabled", (double)value);
}

int config_review_session_cooldown_hours(void)
{
   double value = 0;
   (void)config_client_read_number("review_session_cooldown_hours", &value);
   return (int)value;
}

int config_set_review_session_cooldown_hours(int value)
{
   return config_client_set_number("review_session_cooldown_hours", (double)value);
}

int config_kb_curator_index_claims_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_index_claims_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_index_claims_enabled(int value)
{
   return config_client_set_number("kb_curator_index_claims_enabled", (double)value);
}

int config_kb_curator_extract_max_tokens(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_extract_max_tokens", &value);
   return (int)value;
}

int config_set_kb_curator_extract_max_tokens(int value)
{
   return config_client_set_number("kb_curator_extract_max_tokens", (double)value);
}

int config_kb_curator_cross_repo_len_min(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_len_min", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_len_min(int value)
{
   return config_client_set_number("kb_curator_cross_repo_len_min", (double)value);
}

int config_skills_review_nudge_interval(void)
{
   double value = 0;
   (void)config_client_read_number("skills_review_nudge_interval", &value);
   return (int)value;
}

int config_set_skills_review_nudge_interval(int value)
{
   return config_client_set_number("skills_review_nudge_interval", (double)value);
}

double config_skills_eval_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("skills_eval_threshold", &value);
   return (double)value;
}

int config_set_skills_eval_threshold(double value)
{
   return config_client_set_number("skills_eval_threshold", (double)value);
}

int config_model_meta_capability_routing(void)
{
   double value = 0;
   (void)config_client_read_number("model_meta_capability_routing", &value);
   return (int)value;
}

int config_set_model_meta_capability_routing(int value)
{
   return config_client_set_number("model_meta_capability_routing", (double)value);
}

int config_roundtable_deadline_ms(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_deadline_ms", &value);
   return (int)value;
}

int config_set_roundtable_deadline_ms(int value)
{
   return config_client_set_number("roundtable_deadline_ms", (double)value);
}

const char *config_db1_path(void)
{
   static _Thread_local char value[MAX_PATH_LEN];
   (void)config_client_read_string("db1_path", value, sizeof(value));
   return value;
}

int config_set_db1_path(const char *value)
{
   return config_client_set_string("db1_path", value);
}

size_t config_db1_path_copy(char *out, size_t n)
{
   char value[MAX_PATH_LEN];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("db1_path", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_openai_model(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("openai_model", value, sizeof(value));
   return value;
}

int config_set_openai_model(const char *value)
{
   return config_client_set_string("openai_model", value);
}

size_t config_openai_model_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("openai_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_css_render_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("css_render_command", value, sizeof(value));
   return value;
}

int config_set_css_render_command(const char *value)
{
   return config_client_set_string("css_render_command", value);
}

size_t config_css_render_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("css_render_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_code_context_mode(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("code_context_mode", value, sizeof(value));
   return value;
}

int config_set_code_context_mode(const char *value)
{
   return config_client_set_string("code_context_mode", value);
}

size_t config_code_context_mode_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("code_context_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_ocr_command(void)
{
   static _Thread_local char value[1024];
   (void)config_client_read_string("ocr_command", value, sizeof(value));
   return value;
}

int config_set_ocr_command(const char *value)
{
   return config_client_set_string("ocr_command", value);
}

size_t config_ocr_command_copy(char *out, size_t n)
{
   char value[1024];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("ocr_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_dogfood_log_dir(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("dogfood_log_dir", value, sizeof(value));
   return value;
}

int config_set_dogfood_log_dir(const char *value)
{
   return config_client_set_string("dogfood_log_dir", value);
}

size_t config_dogfood_log_dir_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("dogfood_log_dir", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_search_searxng_url(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("search_searxng_url", value, sizeof(value));
   return value;
}

int config_set_search_searxng_url(const char *value)
{
   return config_client_set_string("search_searxng_url", value);
}

size_t config_search_searxng_url_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("search_searxng_url", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_otel_service_name(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("otel_service_name", value, sizeof(value));
   return value;
}

int config_set_otel_service_name(const char *value)
{
   return config_client_set_string("otel_service_name", value);
}

size_t config_otel_service_name_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("otel_service_name", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_api_bearer_token(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("kb_api_bearer_token", value, sizeof(value));
   return value;
}

int config_set_kb_api_bearer_token(const char *value)
{
   return config_client_set_string("kb_api_bearer_token", value);
}

size_t config_kb_api_bearer_token_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_api_bearer_token", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_synthesis_model(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("synthesis_model", value, sizeof(value));
   return value;
}

int config_set_synthesis_model(const char *value)
{
   return config_client_set_string("synthesis_model", value);
}

size_t config_synthesis_model_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("synthesis_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_fusion_mode(void)
{
   static _Thread_local char value[32];
   (void)config_client_read_string("kb_fusion_mode", value, sizeof(value));
   return value;
}

int config_set_kb_fusion_mode(const char *value)
{
   return config_client_set_string("kb_fusion_mode", value);
}

size_t config_kb_fusion_mode_copy(char *out, size_t n)
{
   char value[32];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_fusion_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_constraint_solver_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("constraint_solver_command", value, sizeof(value));
   return value;
}

int config_set_constraint_solver_command(const char *value)
{
   return config_client_set_string("constraint_solver_command", value);
}

size_t config_constraint_solver_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("constraint_solver_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_stage_order(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("kb_curator_stage_order", value, sizeof(value));
   return value;
}

int config_set_kb_curator_stage_order(const char *value)
{
   return config_client_set_string("kb_curator_stage_order", value);
}

size_t config_kb_curator_stage_order_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_stage_order", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_aux_default_provider(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("aux_default_provider", value, sizeof(value));
   return value;
}

int config_set_aux_default_provider(const char *value)
{
   return config_client_set_string("aux_default_provider", value);
}

size_t config_aux_default_provider_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("aux_default_provider", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_workspaces(int index)
{
   static _Thread_local char value[MAX_PATH_LEN];
   (void)config_client_read_indexed_string("workspaces", index, NULL, value, sizeof(value));
   return value;
}

const char *config_charter_tone_boundaries(int index)
{
   static _Thread_local char value[CONFIG_CHARTER_ENTRY_LEN];
   (void)config_client_read_indexed_string("charter_tone_boundaries", index, NULL, value,
                                           sizeof(value));
   return value;
}

const char *config_mcp_client_name(int index)
{
   static _Thread_local char value[64];
   (void)config_client_read_indexed_string("mcp_clients", index, "name", value, sizeof(value));
   return value;
}

const char *config_cron_job_script(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_SCRIPT];
   (void)config_client_read_indexed_string("cron_jobs", index, "script", value, sizeof(value));
   return value;
}

int config_cron_job_deliver_first_run_silent(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("cron_jobs", index, "deliver_first_run_silent", &value);
   return (int)value;
}

const char *config_trigger_rule_mode(int index)
{
   static _Thread_local char value[TRIGGER_RULE_MAX_MODE];
   (void)config_client_read_indexed_string("trigger_rules", index, "mode", value, sizeof(value));
   return value;
}
