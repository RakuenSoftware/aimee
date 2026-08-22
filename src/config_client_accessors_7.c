/* Generated event-bus config accessor implementations. */
#include "config.h"
#include "config_client.h"
#include <stdio.h>

int config_memory_maintenance_interval_seconds(void)
{
   double value = 0;
   (void)config_client_read_number("memory_maintenance_interval_seconds", &value);
   return (int)value;
}

int config_set_memory_maintenance_interval_seconds(int value)
{
   return config_client_set_number("memory_maintenance_interval_seconds", (double)value);
}

int config_memory_salience_window_size(void)
{
   double value = 0;
   (void)config_client_read_number("memory_salience_window_size", &value);
   return (int)value;
}

int config_set_memory_salience_window_size(int value)
{
   return config_client_set_number("memory_salience_window_size", (double)value);
}

int config_ingress_preinject_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("ingress_preinject_enabled", &value);
   return (int)value;
}

int config_set_ingress_preinject_enabled(int value)
{
   return config_client_set_number("ingress_preinject_enabled", (double)value);
}

int config_ingress_cache_placement_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("ingress_cache_placement_enabled", &value);
   return (int)value;
}

int config_set_ingress_cache_placement_enabled(int value)
{
   return config_client_set_number("ingress_cache_placement_enabled", (double)value);
}

int config_delegate_sandbox_require_isolation(void)
{
   double value = 0;
   (void)config_client_read_number("delegate_sandbox_require_isolation", &value);
   return (int)value;
}

int config_set_delegate_sandbox_require_isolation(int value)
{
   return config_client_set_number("delegate_sandbox_require_isolation", (double)value);
}

int config_kb_pdf_ingest_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_pdf_ingest_enabled", &value);
   return (int)value;
}

int config_set_kb_pdf_ingest_enabled(int value)
{
   return config_client_set_number("kb_pdf_ingest_enabled", (double)value);
}

int config_memory_pagerank_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_pagerank_enabled", &value);
   return (int)value;
}

int config_set_memory_pagerank_enabled(int value)
{
   return config_client_set_number("memory_pagerank_enabled", (double)value);
}

int config_memory_aggregation_max_items(void)
{
   double value = 0;
   (void)config_client_read_number("memory_aggregation_max_items", &value);
   return (int)value;
}

int config_set_memory_aggregation_max_items(int value)
{
   return config_client_set_number("memory_aggregation_max_items", (double)value);
}

int config_memory_recall_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_recall_enabled", &value);
   return (int)value;
}

int config_set_memory_recall_enabled(int value)
{
   return config_client_set_number("memory_recall_enabled", (double)value);
}

int config_disposition_workspace_count(void)
{
   double value = 0;
   (void)config_client_read_number("disposition_workspace_count", &value);
   return (int)value;
}

int config_set_disposition_workspace_count(int value)
{
   return config_client_set_number("disposition_workspace_count", (double)value);
}

int config_identity_working_profile_injection_fields_count(void)
{
   double value = 0;
   (void)config_client_read_number("identity_working_profile_injection_fields_count", &value);
   return (int)value;
}

int config_set_identity_working_profile_injection_fields_count(int value)
{
   return config_client_set_number("identity_working_profile_injection_fields_count",
                                   (double)value);
}

int config_memory_window_radius(void)
{
   double value = 0;
   (void)config_client_read_number("memory_window_radius", &value);
   return (int)value;
}

int config_set_memory_window_radius(int value)
{
   return config_client_set_number("memory_window_radius", (double)value);
}

int config_memory_recall_lanes_floor_fact(void)
{
   double value = 0;
   (void)config_client_read_number("memory_recall_lanes_floor_fact", &value);
   return (int)value;
}

int config_set_memory_recall_lanes_floor_fact(int value)
{
   return config_client_set_number("memory_recall_lanes_floor_fact", (double)value);
}

int config_memory_failure_detection_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_failure_detection_enabled", &value);
   return (int)value;
}

int config_set_memory_failure_detection_enabled(int value)
{
   return config_client_set_number("memory_failure_detection_enabled", (double)value);
}

int config_memory_fetch_budget_base(void)
{
   double value = 0;
   (void)config_client_read_number("memory_fetch_budget_base", &value);
   return (int)value;
}

int config_set_memory_fetch_budget_base(int value)
{
   return config_client_set_number("memory_fetch_budget_base", (double)value);
}

int config_learning_router_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("learning_router_enabled", &value);
   return (int)value;
}

int config_set_learning_router_enabled(int value)
{
   return config_client_set_number("learning_router_enabled", (double)value);
}

int config_learning_implicit_repeat_question(void)
{
   double value = 0;
   (void)config_client_read_number("learning_implicit_repeat_question", &value);
   return (int)value;
}

int config_set_learning_implicit_repeat_question(int value)
{
   return config_client_set_number("learning_implicit_repeat_question", (double)value);
}

int config_roundtable_chair_synthesis(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_chair_synthesis", &value);
   return (int)value;
}

int config_set_roundtable_chair_synthesis(int value)
{
   return config_client_set_number("roundtable_chair_synthesis", (double)value);
}

int config_max_delegation_depth(void)
{
   double value = 0;
   (void)config_client_read_number("max_delegation_depth", &value);
   return (int)value;
}

int config_set_max_delegation_depth(int value)
{
   return config_client_set_number("max_delegation_depth", (double)value);
}

int config_concurrency_per_provider_count(void)
{
   double value = 0;
   (void)config_client_read_number("concurrency_per_provider_count", &value);
   return (int)value;
}

int config_set_concurrency_per_provider_count(int value)
{
   return config_client_set_number("concurrency_per_provider_count", (double)value);
}

int config_compact_head_bytes(void)
{
   double value = 0;
   (void)config_client_read_number("compact_head_bytes", &value);
   return (int)value;
}

int config_set_compact_head_bytes(int value)
{
   return config_client_set_number("compact_head_bytes", (double)value);
}

int config_fold_retained_msgs(void)
{
   double value = 0;
   (void)config_client_read_number("fold_retained_msgs", &value);
   return (int)value;
}

int config_set_fold_retained_msgs(int value)
{
   return config_client_set_number("fold_retained_msgs", (double)value);
}

int config_fold_recall_inject(void)
{
   double value = 0;
   (void)config_client_read_number("fold_recall_inject", &value);
   return (int)value;
}

int config_set_fold_recall_inject(int value)
{
   return config_client_set_number("fold_recall_inject", (double)value);
}

int config_autonomy_skeptics(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_skeptics", &value);
   return (int)value;
}

int config_set_autonomy_skeptics(int value)
{
   return config_client_set_number("autonomy_skeptics", (double)value);
}

int config_autonomy_concurrency(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_concurrency", &value);
   return (int)value;
}

int config_set_autonomy_concurrency(int value)
{
   return config_client_set_number("autonomy_concurrency", (double)value);
}

int config_lsp_server_count(void)
{
   double value = 0;
   (void)config_client_read_number("lsp_server_count", &value);
   return (int)value;
}

int config_set_lsp_server_count(int value)
{
   return config_client_set_number("lsp_server_count", (double)value);
}

int config_computer_use_redact_sensitive_screenshots(void)
{
   double value = 0;
   (void)config_client_read_number("computer_use_redact_sensitive_screenshots", &value);
   return (int)value;
}

int config_set_computer_use_redact_sensitive_screenshots(int value)
{
   return config_client_set_number("computer_use_redact_sensitive_screenshots", (double)value);
}

double config_cache_aware_rewrite_hard_context_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("cache_aware_rewrite_hard_context_threshold", &value);
   return (double)value;
}

int config_set_cache_aware_rewrite_hard_context_threshold(double value)
{
   return config_client_set_number("cache_aware_rewrite_hard_context_threshold", (double)value);
}

int config_cost_reward_ref_usd_milli(void)
{
   double value = 0;
   (void)config_client_read_number("cost_reward_ref_usd_milli", &value);
   return (int)value;
}

int config_set_cost_reward_ref_usd_milli(int value)
{
   return config_client_set_number("cost_reward_ref_usd_milli", (double)value);
}

double config_guardrails_semantic_prompt_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("guardrails_semantic_prompt_threshold", &value);
   return (double)value;
}

int config_set_guardrails_semantic_prompt_threshold(double value)
{
   return config_client_set_number("guardrails_semantic_prompt_threshold", (double)value);
}

int config_server_api_rate_limit_per_min(void)
{
   double value = 0;
   (void)config_client_read_number("server_api_rate_limit_per_min", &value);
   return (int)value;
}

int config_set_server_api_rate_limit_per_min(int value)
{
   return config_client_set_number("server_api_rate_limit_per_min", (double)value);
}

double config_calibration_credible_delta(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_credible_delta", &value);
   return (double)value;
}

int config_set_calibration_credible_delta(double value)
{
   return config_client_set_number("calibration_credible_delta", (double)value);
}

int config_demotion_window(void)
{
   double value = 0;
   (void)config_client_read_number("demotion_window", &value);
   return (int)value;
}

int config_set_demotion_window(int value)
{
   return config_client_set_number("demotion_window", (double)value);
}

int config_reasoning_row_budget(void)
{
   double value = 0;
   (void)config_client_read_number("reasoning_row_budget", &value);
   return (int)value;
}

int config_set_reasoning_row_budget(int value)
{
   return config_client_set_number("reasoning_row_budget", (double)value);
}

double config_kb_mdl_bump_drift_alert(void)
{
   double value = 0;
   (void)config_client_read_number("kb_mdl_bump_drift_alert", &value);
   return (double)value;
}

int config_set_kb_mdl_bump_drift_alert(double value)
{
   return config_client_set_number("kb_mdl_bump_drift_alert", (double)value);
}

int config_kb_bg_watch_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_bg_watch_enabled", &value);
   return (int)value;
}

int config_set_kb_bg_watch_enabled(int value)
{
   return config_client_set_number("kb_bg_watch_enabled", (double)value);
}

double config_code_surprising_precision_floor(void)
{
   double value = 0;
   (void)config_client_read_number("code_surprising_precision_floor", &value);
   return (double)value;
}

int config_set_code_surprising_precision_floor(double value)
{
   return config_client_set_number("code_surprising_precision_floor", (double)value);
}

int config_kb_maintenance_orphan_days(void)
{
   double value = 0;
   (void)config_client_read_number("kb_maintenance_orphan_days", &value);
   return (int)value;
}

int config_set_kb_maintenance_orphan_days(int value)
{
   return config_client_set_number("kb_maintenance_orphan_days", (double)value);
}

int config_review_idle_trigger_minutes(void)
{
   double value = 0;
   (void)config_client_read_number("review_idle_trigger_minutes", &value);
   return (int)value;
}

int config_set_review_idle_trigger_minutes(int value)
{
   return config_client_set_number("review_idle_trigger_minutes", (double)value);
}

int config_kb_curator_index_narrative_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_index_narrative_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_index_narrative_enabled(int value)
{
   return config_client_set_number("kb_curator_index_narrative_enabled", (double)value);
}

int config_kb_curator_promote_min_sources(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_promote_min_sources", &value);
   return (int)value;
}

int config_set_kb_curator_promote_min_sources(int value)
{
   return config_client_set_number("kb_curator_promote_min_sources", (double)value);
}

int config_kb_curator_cross_repo_p_pct(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_p_pct", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_p_pct(int value)
{
   return config_client_set_number("kb_curator_cross_repo_p_pct", (double)value);
}

int config_skills_review_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("skills_review_enabled", &value);
   return (int)value;
}

int config_set_skills_review_enabled(int value)
{
   return config_client_set_number("skills_review_enabled", (double)value);
}

int config_skills_eval_gate_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("skills_eval_gate_enabled", &value);
   return (int)value;
}

int config_set_skills_eval_gate_enabled(int value)
{
   return config_client_set_number("skills_eval_gate_enabled", (double)value);
}

int config_model_meta_refresh_minutes(void)
{
   double value = 0;
   (void)config_client_read_number("model_meta_refresh_minutes", &value);
   return (int)value;
}

int config_set_model_meta_refresh_minutes(int value)
{
   return config_client_set_number("model_meta_refresh_minutes", (double)value);
}

int config_roundtable_converge_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_converge_threshold", &value);
   return (int)value;
}

int config_set_roundtable_converge_threshold(int value)
{
   return config_client_set_number("roundtable_converge_threshold", (double)value);
}

int config_roundtable_pipeline_unknown_context_tokens(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_pipeline_unknown_context_tokens", &value);
   return (int)value;
}

int config_set_roundtable_pipeline_unknown_context_tokens(int value)
{
   return config_client_set_number("roundtable_pipeline_unknown_context_tokens", (double)value);
}

const char *config_openai_endpoint(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("openai_endpoint", value, sizeof(value));
   return value;
}

int config_set_openai_endpoint(const char *value)
{
   return config_client_set_string("openai_endpoint", value);
}

size_t config_openai_endpoint_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("openai_endpoint", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_client_tool_transport_preference(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("client_tool_transport_preference", value, sizeof(value));
   return value;
}

int config_set_client_tool_transport_preference(const char *value)
{
   return config_client_set_string("client_tool_transport_preference", value);
}

size_t config_client_tool_transport_preference_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("client_tool_transport_preference", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_cognify_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("memory_cognify_command", value, sizeof(value));
   return value;
}

int config_set_memory_cognify_command(const char *value)
{
   return config_client_set_string("memory_cognify_command", value);
}

size_t config_memory_cognify_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_cognify_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_pdf_blob_dir(void)
{
   static _Thread_local char value[1024];
   (void)config_client_read_string("kb_pdf_blob_dir", value, sizeof(value));
   return value;
}

int config_set_kb_pdf_blob_dir(const char *value)
{
   return config_client_set_string("kb_pdf_blob_dir", value);
}

size_t config_kb_pdf_blob_dir_copy(char *out, size_t n)
{
   char value[1024];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_pdf_blob_dir", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_hard_negative_log(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("memory_hard_negative_log", value, sizeof(value));
   return value;
}

int config_set_memory_hard_negative_log(const char *value)
{
   return config_client_set_string("memory_hard_negative_log", value);
}

size_t config_memory_hard_negative_log_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_hard_negative_log", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_search_backend(void)
{
   static _Thread_local char value[32];
   (void)config_client_read_string("search_backend", value, sizeof(value));
   return value;
}

int config_set_search_backend(const char *value)
{
   return config_client_set_string("search_backend", value);
}

size_t config_search_backend_copy(char *out, size_t n)
{
   char value[32];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("search_backend", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_otel_endpoint(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("otel_endpoint", value, sizeof(value));
   return value;
}

int config_set_otel_endpoint(const char *value)
{
   return config_client_set_string("otel_endpoint", value);
}

size_t config_otel_endpoint_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("otel_endpoint", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_guardrails_semantic_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("guardrails_semantic_command", value, sizeof(value));
   return value;
}

int config_set_guardrails_semantic_command(const char *value)
{
   return config_client_set_string("guardrails_semantic_command", value);
}

size_t config_guardrails_semantic_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("guardrails_semantic_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_synthesis_endpoint(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("synthesis_endpoint", value, sizeof(value));
   return value;
}

int config_set_synthesis_endpoint(const char *value)
{
   return config_client_set_string("synthesis_endpoint", value);
}

size_t config_synthesis_endpoint_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("synthesis_endpoint", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_calibration_model_version(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("calibration_model_version", value, sizeof(value));
   return value;
}

int config_set_calibration_model_version(const char *value)
{
   return config_client_set_string("calibration_model_version", value);
}

size_t config_calibration_model_version_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("calibration_model_version", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_planner_search_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("planner_search_command", value, sizeof(value));
   return value;
}

int config_set_planner_search_command(const char *value)
{
   return config_client_set_string("planner_search_command", value);
}

size_t config_planner_search_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("planner_search_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_extract_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("kb_curator_extract_command", value, sizeof(value));
   return value;
}

int config_set_kb_curator_extract_command(const char *value)
{
   return config_client_set_string("kb_curator_extract_command", value);
}

size_t config_kb_curator_extract_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_extract_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_synthesize_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("kb_curator_synthesize_command", value, sizeof(value));
   return value;
}

int config_set_kb_curator_synthesize_command(const char *value)
{
   return config_client_set_string("kb_curator_synthesize_command", value);
}

size_t config_kb_curator_synthesize_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_synthesize_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_context_engine(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("context_engine", value, sizeof(value));
   return value;
}

int config_set_context_engine(const char *value)
{
   return config_client_set_string("context_engine", value);
}

size_t config_context_engine_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("context_engine", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_charter_values(int index)
{
   static _Thread_local char value[CONFIG_CHARTER_ENTRY_LEN];
   (void)config_client_read_indexed_string("charter_values", index, NULL, value, sizeof(value));
   return value;
}

const char *config_ensemble_reference_personas(int index)
{
   static _Thread_local char value[64];
   (void)config_client_read_indexed_string("ensemble_reference_personas", index, NULL, value,
                                           sizeof(value));
   return value;
}

const char *config_cron_job_mode(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_MODE];
   (void)config_client_read_indexed_string("cron_jobs", index, "mode", value, sizeof(value));
   return value;
}

int config_cron_job_deliver_only_if_changed(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("cron_jobs", index, "deliver_only_if_changed", &value);
   return (int)value;
}

const char *config_trigger_rule_workspace(int index)
{
   static _Thread_local char value[TRIGGER_RULE_MAX_WS];
   (void)config_client_read_indexed_string("trigger_rules", index, "workspace", value,
                                           sizeof(value));
   return value;
}
