/* Generated event-bus config accessor implementations. */
#include "config.h"
#include "config_client.h"
#include <stdio.h>

int config_embedder_dims(void)
{
   double value = 0;
   (void)config_client_read_number("embedder_dims", &value);
   return (int)value;
}

int config_set_embedder_dims(int value)
{
   return config_client_set_number("embedder_dims", (double)value);
}

int config_client_integrations_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("client_integrations_enabled", &value);
   return (int)value;
}

int config_set_client_integrations_enabled(int value)
{
   return config_client_set_number("client_integrations_enabled", (double)value);
}

int config_memory_cognify_async_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_cognify_async_enabled", &value);
   return (int)value;
}

int config_set_memory_cognify_async_enabled(int value)
{
   return config_client_set_number("memory_cognify_async_enabled", (double)value);
}

int config_code_span_max_lines(void)
{
   double value = 0;
   (void)config_client_read_number("code_span_max_lines", &value);
   return (int)value;
}

int config_set_code_span_max_lines(int value)
{
   return config_client_set_number("code_span_max_lines", (double)value);
}

int config_require_session_worktree(void)
{
   double value = 0;
   (void)config_client_read_number("require_session_worktree", &value);
   return (int)value;
}

int config_set_require_session_worktree(int value)
{
   return config_client_set_number("require_session_worktree", (double)value);
}

int config_kb_typed_facts_auto_promote_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_typed_facts_auto_promote_enabled", &value);
   return (int)value;
}

int config_set_kb_typed_facts_auto_promote_enabled(int value)
{
   return config_client_set_number("kb_typed_facts_auto_promote_enabled", (double)value);
}

int config_kb_pdf_blob_orphan_alarm_mb(void)
{
   double value = 0;
   (void)config_client_read_number("kb_pdf_blob_orphan_alarm_mb", &value);
   return (int)value;
}

int config_set_kb_pdf_blob_orphan_alarm_mb(int value)
{
   return config_client_set_number("kb_pdf_blob_orphan_alarm_mb", (double)value);
}

int config_memory_citations_strip_unverified(void)
{
   double value = 0;
   (void)config_client_read_number("memory_citations_strip_unverified", &value);
   return (int)value;
}

int config_set_memory_citations_strip_unverified(int value)
{
   return config_client_set_number("memory_citations_strip_unverified", (double)value);
}

int config_memory_lifecycle_hide_archived(void)
{
   double value = 0;
   (void)config_client_read_number("memory_lifecycle_hide_archived", &value);
   return (int)value;
}

int config_set_memory_lifecycle_hide_archived(int value)
{
   return config_client_set_number("memory_lifecycle_hide_archived", (double)value);
}

int config_memory_directives_failure_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("memory_directives_failure_threshold", &value);
   return (int)value;
}

int config_set_memory_directives_failure_threshold(int value)
{
   return config_client_set_number("memory_directives_failure_threshold", (double)value);
}

int config_charter_values_count(void)
{
   double value = 0;
   (void)config_client_read_number("charter_values_count", &value);
   return (int)value;
}

int config_set_charter_values_count(int value)
{
   return config_client_set_number("charter_values_count", (double)value);
}

int config_memory_rewrite_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_rewrite_enabled", &value);
   return (int)value;
}

int config_set_memory_rewrite_enabled(int value)
{
   return config_client_set_number("memory_rewrite_enabled", (double)value);
}

int config_memory_recall_lanes_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_recall_lanes_enabled", &value);
   return (int)value;
}

int config_set_memory_recall_lanes_enabled(int value)
{
   return config_client_set_number("memory_recall_lanes_enabled", (double)value);
}

double config_memory_improve_max_confidence(void)
{
   double value = 0;
   (void)config_client_read_number("memory_improve_max_confidence", &value);
   return (double)value;
}

int config_set_memory_improve_max_confidence(double value)
{
   return config_client_set_number("memory_improve_max_confidence", (double)value);
}

double config_memory_chunk_min_confidence(void)
{
   double value = 0;
   (void)config_client_read_number("memory_chunk_min_confidence", &value);
   return (double)value;
}

int config_set_memory_chunk_min_confidence(double value)
{
   return config_client_set_number("memory_chunk_min_confidence", (double)value);
}

int config_dogfood_inline_tagging(void)
{
   double value = 0;
   (void)config_client_read_number("dogfood_inline_tagging", &value);
   return (int)value;
}

int config_set_dogfood_inline_tagging(int value)
{
   return config_client_set_number("dogfood_inline_tagging", (double)value);
}

int config_learning_synthesize_max_tokens(void)
{
   double value = 0;
   (void)config_client_read_number("learning_synthesize_max_tokens", &value);
   return (int)value;
}

int config_set_learning_synthesize_max_tokens(int value)
{
   return config_client_set_number("learning_synthesize_max_tokens", (double)value);
}

int config_autonomous(void)
{
   double value = 0;
   (void)config_client_read_number("autonomous", &value);
   return (int)value;
}

int config_set_autonomous(int value)
{
   return config_client_set_number("autonomous", (double)value);
}

int config_retry_base_ms(void)
{
   double value = 0;
   (void)config_client_read_number("retry_base_ms", &value);
   return (int)value;
}

int config_set_retry_base_ms(int value)
{
   return config_client_set_number("retry_base_ms", (double)value);
}

int config_delegate_max_inflight(void)
{
   double value = 0;
   (void)config_client_read_number("delegate_max_inflight", &value);
   return (int)value;
}

int config_set_delegate_max_inflight(int value)
{
   return config_client_set_number("delegate_max_inflight", (double)value);
}

int config_search_max_results(void)
{
   double value = 0;
   (void)config_client_read_number("search_max_results", &value);
   return (int)value;
}

int config_set_search_max_results(int value)
{
   return config_client_set_number("search_max_results", (double)value);
}

int config_coord_closet_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("coord_closet_enabled", &value);
   return (int)value;
}

int config_set_coord_closet_enabled(int value)
{
   return config_client_set_number("coord_closet_enabled", (double)value);
}

int config_fold_freeze_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("fold_freeze_enabled", &value);
   return (int)value;
}

int config_set_fold_freeze_enabled(int value)
{
   return config_client_set_number("fold_freeze_enabled", (double)value);
}

int config_module_delegates(void)
{
   double value = 0;
   (void)config_client_read_number("module_delegates", &value);
   return (int)value;
}

int config_set_module_delegates(int value)
{
   return config_client_set_number("module_delegates", (double)value);
}

int config_autonomy_ci_retry_max(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_ci_retry_max", &value);
   return (int)value;
}

int config_set_autonomy_ci_retry_max(int value)
{
   return config_client_set_number("autonomy_ci_retry_max", (double)value);
}

int config_max_sessions(void)
{
   double value = 0;
   (void)config_client_read_number("max_sessions", &value);
   return (int)value;
}

int config_set_max_sessions(int value)
{
   return config_client_set_number("max_sessions", (double)value);
}

int config_mcp_osv_enforce(void)
{
   double value = 0;
   (void)config_client_read_number("mcp_osv_enforce", &value);
   return (int)value;
}

int config_set_mcp_osv_enforce(int value)
{
   return config_client_set_number("mcp_osv_enforce", (double)value);
}

int config_virtual_context_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("virtual_context_enabled", &value);
   return (int)value;
}

int config_set_virtual_context_enabled(int value)
{
   return config_client_set_number("virtual_context_enabled", (double)value);
}

int config_transport_thinclient_gzip_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("transport_thinclient_gzip_enabled", &value);
   return (int)value;
}

int config_set_transport_thinclient_gzip_enabled(int value)
{
   return config_client_set_number("transport_thinclient_gzip_enabled", (double)value);
}

int config_extended_thinking_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("extended_thinking_enabled", &value);
   return (int)value;
}

int config_set_extended_thinking_enabled(int value)
{
   return config_client_set_number("extended_thinking_enabled", (double)value);
}

int config_server_api_http_port(void)
{
   double value = 0;
   (void)config_client_read_number("server_api_http_port", &value);
   return (int)value;
}

int config_set_server_api_http_port(int value)
{
   return config_client_set_number("server_api_http_port", (double)value);
}

int config_calibration_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_enabled", &value);
   return (int)value;
}

int config_set_calibration_enabled(int value)
{
   return config_client_set_number("calibration_enabled", (double)value);
}

double config_calibration_tau_memory_flag(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_tau_memory_flag", &value);
   return (double)value;
}

int config_set_calibration_tau_memory_flag(double value)
{
   return config_client_set_number("calibration_tau_memory_flag", (double)value);
}

int config_kb_ranker_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_ranker_enabled", &value);
   return (int)value;
}

int config_set_kb_ranker_enabled(int value)
{
   return config_client_set_number("kb_ranker_enabled", (double)value);
}

int config_bandit_exploration_window_seconds(void)
{
   double value = 0;
   (void)config_client_read_number("bandit_exploration_window_seconds", &value);
   return (int)value;
}

int config_set_bandit_exploration_window_seconds(int value)
{
   return config_client_set_number("bandit_exploration_window_seconds", (double)value);
}

int config_db2_connection_pool_size(void)
{
   double value = 0;
   (void)config_client_read_number("db2_connection_pool_size", &value);
   return (int)value;
}

int config_set_db2_connection_pool_size(int value)
{
   return config_client_set_number("db2_connection_pool_size", (double)value);
}

double config_code_hybrid_weight_vector(void)
{
   double value = 0;
   (void)config_client_read_number("code_hybrid_weight_vector", &value);
   return (double)value;
}

int config_set_code_hybrid_weight_vector(double value)
{
   return config_client_set_number("code_hybrid_weight_vector", (double)value);
}

int config_kb_maintenance_interval_hours(void)
{
   double value = 0;
   (void)config_client_read_number("kb_maintenance_interval_hours", &value);
   return (int)value;
}

int config_set_kb_maintenance_interval_hours(int value)
{
   return config_client_set_number("kb_maintenance_interval_hours", (double)value);
}

int config_trigger_max_concurrent(void)
{
   double value = 0;
   (void)config_client_read_number("trigger_max_concurrent", &value);
   return (int)value;
}

int config_set_trigger_max_concurrent(int value)
{
   return config_client_set_number("trigger_max_concurrent", (double)value);
}

int config_kb_curator_extract_code_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_extract_code_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_extract_code_enabled(int value)
{
   return config_client_set_number("kb_curator_extract_code_enabled", (double)value);
}

int config_kb_curator_link_artifacts_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_link_artifacts_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_link_artifacts_enabled(int value)
{
   return config_client_set_number("kb_curator_link_artifacts_enabled", (double)value);
}

int config_kb_curator_cross_repo_graph_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_graph_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_graph_enabled(int value)
{
   return config_client_set_number("kb_curator_cross_repo_graph_enabled", (double)value);
}

int config_kb_curator_cross_repo_query_timeout_ms(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_query_timeout_ms", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_query_timeout_ms(int value)
{
   return config_client_set_number("kb_curator_cross_repo_query_timeout_ms", (double)value);
}

int config_skills_dispatch_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("skills_dispatch_enabled", &value);
   return (int)value;
}

int config_set_skills_dispatch_enabled(int value)
{
   return config_client_set_number("skills_dispatch_enabled", (double)value);
}

int config_aux_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("aux_enabled", &value);
   return (int)value;
}

int config_set_aux_enabled(int value)
{
   return config_client_set_number("aux_enabled", (double)value);
}

int config_ensemble_reference_persona_count(void)
{
   double value = 0;
   (void)config_client_read_number("ensemble_reference_persona_count", &value);
   return (int)value;
}

int config_set_ensemble_reference_persona_count(int value)
{
   return config_client_set_number("ensemble_reference_persona_count", (double)value);
}

double config_roundtable_pipeline_max_cost_usd(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_pipeline_max_cost_usd", &value);
   return (double)value;
}

int config_set_roundtable_pipeline_max_cost_usd(double value)
{
   return config_client_set_number("roundtable_pipeline_max_cost_usd", (double)value);
}

const char *config_default_persona(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("default_persona", value, sizeof(value));
   return value;
}

int config_set_default_persona(const char *value)
{
   return config_client_set_string("default_persona", value);
}

size_t config_default_persona_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("default_persona", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_embedder_url(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("embedder_url", value, sizeof(value));
   return value;
}

int config_set_embedder_url(const char *value)
{
   return config_client_set_string("embedder_url", value);
}

size_t config_embedder_url_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("embedder_url", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_vault_tpm2_tcti(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("vault_tpm2_tcti", value, sizeof(value));
   return value;
}

int config_set_vault_tpm2_tcti(const char *value)
{
   return config_client_set_string("vault_tpm2_tcti", value);
}

size_t config_vault_tpm2_tcti_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("vault_tpm2_tcti", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_delegate_sandbox_image(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("delegate_sandbox_image", value, sizeof(value));
   return value;
}

int config_set_delegate_sandbox_image(const char *value)
{
   return config_client_set_string("delegate_sandbox_image", value);
}

size_t config_delegate_sandbox_image_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("delegate_sandbox_image", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_rewrite_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("memory_rewrite_command", value, sizeof(value));
   return value;
}

int config_set_memory_rewrite_command(const char *value)
{
   return config_client_set_string("memory_rewrite_command", value);
}

size_t config_memory_rewrite_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_rewrite_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_learning_synthesize_prompt_version(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("learning_synthesize_prompt_version", value, sizeof(value));
   return value;
}

int config_set_learning_synthesize_prompt_version(const char *value)
{
   return config_client_set_string("learning_synthesize_prompt_version", value);
}

size_t config_learning_synthesize_prompt_version_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("learning_synthesize_prompt_version", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_coord_closet_denylist(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("coord_closet_denylist", value, sizeof(value));
   return value;
}

int config_set_coord_closet_denylist(const char *value)
{
   return config_client_set_string("coord_closet_denylist", value);
}

size_t config_coord_closet_denylist_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("coord_closet_denylist", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_proxy_url(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("proxy_url", value, sizeof(value));
   return value;
}

int config_set_proxy_url(const char *value)
{
   return config_client_set_string("proxy_url", value);
}

size_t config_proxy_url_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("proxy_url", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_client_bearer_token(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("kb_client_bearer_token", value, sizeof(value));
   return value;
}

int config_set_kb_client_bearer_token(const char *value)
{
   return config_client_set_string("kb_client_bearer_token", value);
}

size_t config_kb_client_bearer_token_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_client_bearer_token", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_server_api_bearer_token(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("server_api_bearer_token", value, sizeof(value));
   return value;
}

int config_set_server_api_bearer_token(const char *value)
{
   return config_client_set_string("server_api_bearer_token", value);
}

size_t config_server_api_bearer_token_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("server_api_bearer_token", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_ranker_fit_benchmark(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("kb_ranker_fit_benchmark", value, sizeof(value));
   return value;
}

int config_set_kb_ranker_fit_benchmark(const char *value)
{
   return config_client_set_string("kb_ranker_fit_benchmark", value);
}

size_t config_kb_ranker_fit_benchmark_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_ranker_fit_benchmark", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_tier(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("kb_curator_tier", value, sizeof(value));
   return value;
}

int config_set_kb_curator_tier(const char *value)
{
   return config_client_set_string("kb_curator_tier", value);
}

size_t config_kb_curator_tier_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_tier", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_provider_base_url(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("kb_curator_provider_base_url", value, sizeof(value));
   return value;
}

int config_set_kb_curator_provider_base_url(const char *value)
{
   return config_client_set_string("kb_curator_provider_base_url", value);
}

size_t config_kb_curator_provider_base_url_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_provider_base_url", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_ensemble_aggregator(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("ensemble_aggregator", value, sizeof(value));
   return value;
}

int config_set_ensemble_aggregator(const char *value)
{
   return config_client_set_string("ensemble_aggregator", value);
}

size_t config_ensemble_aggregator_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("ensemble_aggregator", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_workspace_vcs_head(int index)
{
   static _Thread_local char value[65];
   (void)config_client_read_indexed_string("workspace_vcs_head", index, NULL, value, sizeof(value));
   return value;
}

const char *config_mcp_osv_allow(int index)
{
   static _Thread_local char value[256];
   (void)config_client_read_indexed_string("mcp_osv_allow", index, NULL, value, sizeof(value));
   return value;
}

const char *config_mcp_client_url(int index)
{
   static _Thread_local char value[512];
   (void)config_client_read_indexed_string("mcp_clients", index, "url", value, sizeof(value));
   return value;
}

const char *config_cron_job_context_from(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_CONTEXT_FROM];
   (void)config_client_read_indexed_string("cron_jobs", index, "context_from", value,
                                           sizeof(value));
   return value;
}

const char *config_trigger_rule_source(int index)
{
   static _Thread_local char value[TRIGGER_RULE_MAX_SOURCE];
   (void)config_client_read_indexed_string("trigger_rules", index, "source", value, sizeof(value));
   return value;
}

double config_disposition_value(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("dispositions", index, "value", &value);
   return (double)value;
}
