/* Generated event-bus config accessor implementations. */
#include "config.h"
#include "config_client.h"
#include <stdio.h>

int config_subagent_ban_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("subagent_ban_enabled", &value);
   return (int)value;
}

int config_set_subagent_ban_enabled(int value)
{
   return config_client_set_number("subagent_ban_enabled", (double)value);
}

int config_wfe_proposals_autoscan_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("wfe_proposals_autoscan_enabled", &value);
   return (int)value;
}

int config_set_wfe_proposals_autoscan_enabled(int value)
{
   return config_client_set_number("wfe_proposals_autoscan_enabled", (double)value);
}

int config_memory_coref_window(void)
{
   double value = 0;
   (void)config_client_read_number("memory_coref_window", &value);
   return (int)value;
}

int config_set_memory_coref_window(int value)
{
   return config_client_set_number("memory_coref_window", (double)value);
}

int config_ingress_max_raw_scans(void)
{
   double value = 0;
   (void)config_client_read_number("ingress_max_raw_scans", &value);
   return (int)value;
}

int config_set_ingress_max_raw_scans(int value)
{
   return config_client_set_number("ingress_max_raw_scans", (double)value);
}

int config_prompt_manager_review_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("prompt_manager_review_enabled", &value);
   return (int)value;
}

int config_set_prompt_manager_review_enabled(int value)
{
   return config_client_set_number("prompt_manager_review_enabled", (double)value);
}

int config_gateway_pin_model(void)
{
   double value = 0;
   (void)config_client_read_number("gateway_pin_model", &value);
   return (int)value;
}

int config_set_gateway_pin_model(int value)
{
   return config_client_set_number("gateway_pin_model", (double)value);
}

int config_kb_pdf_blob_recon_secs(void)
{
   double value = 0;
   (void)config_client_read_number("kb_pdf_blob_recon_secs", &value);
   return (int)value;
}

int config_set_kb_pdf_blob_recon_secs(int value)
{
   return config_client_set_number("kb_pdf_blob_recon_secs", (double)value);
}

int config_memory_citations_reprompt_on_miss(void)
{
   double value = 0;
   (void)config_client_read_number("memory_citations_reprompt_on_miss", &value);
   return (int)value;
}

int config_set_memory_citations_reprompt_on_miss(int value)
{
   return config_client_set_number("memory_citations_reprompt_on_miss", (double)value);
}

int config_memory_lifecycle_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_lifecycle_enabled", &value);
   return (int)value;
}

int config_set_memory_lifecycle_enabled(int value)
{
   return config_client_set_number("memory_lifecycle_enabled", (double)value);
}

int config_memory_directives_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_directives_enabled", &value);
   return (int)value;
}

int config_set_memory_directives_enabled(int value)
{
   return config_client_set_number("memory_directives_enabled", (double)value);
}

int config_charter_hard_constraints_count(void)
{
   double value = 0;
   (void)config_client_read_number("charter_hard_constraints_count", &value);
   return (int)value;
}

int config_set_charter_hard_constraints_count(int value)
{
   return config_client_set_number("charter_hard_constraints_count", (double)value);
}

int config_memory_profile_cards_stale_secs(void)
{
   double value = 0;
   (void)config_client_read_number("memory_profile_cards_stale_secs", &value);
   return (int)value;
}

int config_set_memory_profile_cards_stale_secs(int value)
{
   return config_client_set_number("memory_profile_cards_stale_secs", (double)value);
}

int config_memory_query_expansion_k(void)
{
   double value = 0;
   (void)config_client_read_number("memory_query_expansion_k", &value);
   return (int)value;
}

int config_set_memory_query_expansion_k(int value)
{
   return config_client_set_number("memory_query_expansion_k", (double)value);
}

int config_memory_improve_min_cluster(void)
{
   double value = 0;
   (void)config_client_read_number("memory_improve_min_cluster", &value);
   return (int)value;
}

int config_set_memory_improve_min_cluster(int value)
{
   return config_client_set_number("memory_improve_min_cluster", (double)value);
}

double config_memory_abstain_gate(void)
{
   double value = 0;
   (void)config_client_read_number("memory_abstain_gate", &value);
   return (double)value;
}

int config_set_memory_abstain_gate(double value)
{
   return config_client_set_number("memory_abstain_gate", (double)value);
}

int config_dogfood_commit_raw(void)
{
   double value = 0;
   (void)config_client_read_number("dogfood_commit_raw", &value);
   return (int)value;
}

int config_set_dogfood_commit_raw(int value)
{
   return config_client_set_number("dogfood_commit_raw", (double)value);
}

int config_learning_synthesize_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("learning_synthesize_enabled", &value);
   return (int)value;
}

int config_set_learning_synthesize_enabled(int value)
{
   return config_client_set_number("learning_synthesize_enabled", (double)value);
}

int config_learning_implicit_retrieval_outcome(void)
{
   double value = 0;
   (void)config_client_read_number("learning_implicit_retrieval_outcome", &value);
   return (int)value;
}

int config_set_learning_implicit_retrieval_outcome(int value)
{
   return config_client_set_number("learning_implicit_retrieval_outcome", (double)value);
}

int config_retry_max_attempts(void)
{
   double value = 0;
   (void)config_client_read_number("retry_max_attempts", &value);
   return (int)value;
}

int config_set_retry_max_attempts(int value)
{
   return config_client_set_number("retry_max_attempts", (double)value);
}

int config_session_threads(void)
{
   double value = 0;
   (void)config_client_read_number("session_threads", &value);
   return (int)value;
}

int config_set_session_threads(int value)
{
   return config_client_set_number("session_threads", (double)value);
}

int config_concurrency_preempt_requeue_max(void)
{
   double value = 0;
   (void)config_client_read_number("concurrency_preempt_requeue_max", &value);
   return (int)value;
}

int config_set_concurrency_preempt_requeue_max(int value)
{
   return config_client_set_number("concurrency_preempt_requeue_max", (double)value);
}

int config_compact_from_record(void)
{
   double value = 0;
   (void)config_client_read_number("compact_from_record", &value);
   return (int)value;
}

int config_set_compact_from_record(int value)
{
   return config_client_set_number("compact_from_record", (double)value);
}

int config_fold_register_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("fold_register_enabled", &value);
   return (int)value;
}

int config_set_fold_register_enabled(int value)
{
   return config_client_set_number("fold_register_enabled", (double)value);
}

int config_module_governance(void)
{
   double value = 0;
   (void)config_client_read_number("module_governance", &value);
   return (int)value;
}

int config_set_module_governance(int value)
{
   return config_client_set_number("module_governance", (double)value);
}

int config_autonomy_unit_max(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_unit_max", &value);
   return (int)value;
}

int config_set_autonomy_unit_max(int value)
{
   return config_client_set_number("autonomy_unit_max", (double)value);
}

int config_worktree_stale_secs(void)
{
   double value = 0;
   (void)config_client_read_number("worktree_stale_secs", &value);
   return (int)value;
}

int config_set_worktree_stale_secs(int value)
{
   return config_client_set_number("worktree_stale_secs", (double)value);
}

int config_mcp_osv_offline(void)
{
   double value = 0;
   (void)config_client_read_number("mcp_osv_offline", &value);
   return (int)value;
}

int config_set_mcp_osv_offline(int value)
{
   return config_client_set_number("mcp_osv_offline", (double)value);
}

int config_integrity_dry_run(void)
{
   double value = 0;
   (void)config_client_read_number("integrity_dry_run", &value);
   return (int)value;
}

int config_set_integrity_dry_run(int value)
{
   return config_client_set_number("integrity_dry_run", (double)value);
}

int config_transport_server_keepalive_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("transport_server_keepalive_enabled", &value);
   return (int)value;
}

int config_set_transport_server_keepalive_enabled(int value)
{
   return config_client_set_number("transport_server_keepalive_enabled", (double)value);
}

int config_cache_shaping_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("cache_shaping_enabled", &value);
   return (int)value;
}

int config_set_cache_shaping_enabled(int value)
{
   return config_client_set_number("cache_shaping_enabled", (double)value);
}

int config_synthesis_thinking(void)
{
   double value = 0;
   (void)config_client_read_number("synthesis_thinking", &value);
   return (int)value;
}

int config_set_synthesis_thinking(int value)
{
   return config_client_set_number("synthesis_thinking", (double)value);
}

int config_server_api_remote_writes(void)
{
   double value = 0;
   (void)config_client_read_number("server_api_remote_writes", &value);
   return (int)value;
}

int config_set_server_api_remote_writes(int value)
{
   return config_client_set_number("server_api_remote_writes", (double)value);
}

double config_calibration_tau_memory_auto(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_tau_memory_auto", &value);
   return (double)value;
}

int config_set_calibration_tau_memory_auto(double value)
{
   return config_client_set_number("calibration_tau_memory_auto", (double)value);
}

double config_kb_fusion_static_alpha(void)
{
   double value = 0;
   (void)config_client_read_number("kb_fusion_static_alpha", &value);
   return (double)value;
}

int config_set_kb_fusion_static_alpha(double value)
{
   return config_client_set_number("kb_fusion_static_alpha", (double)value);
}

double config_bandit_ipw_weight_cap(void)
{
   double value = 0;
   (void)config_client_read_number("bandit_ipw_weight_cap", &value);
   return (double)value;
}

int config_set_bandit_ipw_weight_cap(double value)
{
   return config_client_set_number("bandit_ipw_weight_cap", (double)value);
}

int config_kb_worker_count(void)
{
   double value = 0;
   (void)config_client_read_number("kb_worker_count", &value);
   return (int)value;
}

int config_set_kb_worker_count(int value)
{
   return config_client_set_number("kb_worker_count", (double)value);
}

double config_code_hybrid_weight_graph(void)
{
   double value = 0;
   (void)config_client_read_number("code_hybrid_weight_graph", &value);
   return (double)value;
}

int config_set_code_hybrid_weight_graph(double value)
{
   return config_client_set_number("code_hybrid_weight_graph", (double)value);
}

int config_kb_maintenance_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_maintenance_enabled", &value);
   return (int)value;
}

int config_set_kb_maintenance_enabled(int value)
{
   return config_client_set_number("kb_maintenance_enabled", (double)value);
}

int config_kb_mining_failure_learning_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_mining_failure_learning_enabled", &value);
   return (int)value;
}

int config_set_kb_mining_failure_learning_enabled(int value)
{
   return config_client_set_number("kb_mining_failure_learning_enabled", (double)value);
}

int config_kb_curator_extract_docs_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_extract_docs_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_extract_docs_enabled(int value)
{
   return config_client_set_number("kb_curator_extract_docs_enabled", (double)value);
}

int config_kb_curator_index_code_unit_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_index_code_unit_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_index_code_unit_enabled(int value)
{
   return config_client_set_number("kb_curator_index_code_unit_enabled", (double)value);
}

int config_kb_curator_synthesize_k(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_synthesize_k", &value);
   return (int)value;
}

int config_set_kb_curator_synthesize_k(int value)
{
   return config_client_set_number("kb_curator_synthesize_k", (double)value);
}

int config_kb_curator_cross_repo_max_candidates(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_max_candidates", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_max_candidates(int value)
{
   return config_client_set_number("kb_curator_cross_repo_max_candidates", (double)value);
}

int config_skills_archive_after_days(void)
{
   double value = 0;
   (void)config_client_read_number("skills_archive_after_days", &value);
   return (int)value;
}

int config_set_skills_archive_after_days(int value)
{
   return config_client_set_number("skills_archive_after_days", (double)value);
}

int config_worktree_gc_max_age_days(void)
{
   double value = 0;
   (void)config_client_read_number("worktree_gc_max_age_days", &value);
   return (int)value;
}

int config_set_worktree_gc_max_age_days(int value)
{
   return config_client_set_number("worktree_gc_max_age_days", (double)value);
}

int config_ensemble_reference_count(void)
{
   double value = 0;
   (void)config_client_read_number("ensemble_reference_count", &value);
   return (int)value;
}

int config_set_ensemble_reference_count(int value)
{
   return config_client_set_number("ensemble_reference_count", (double)value);
}

int config_roundtable_pipeline_max_attempts_per_pass(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_pipeline_max_attempts_per_pass", &value);
   return (int)value;
}

int config_set_roundtable_pipeline_max_attempts_per_pass(int value)
{
   return config_client_set_number("roundtable_pipeline_max_attempts_per_pass", (double)value);
}

const char *config_provider(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("provider", value, sizeof(value));
   return value;
}

int config_set_provider(const char *value)
{
   return config_client_set_string("provider", value);
}

size_t config_provider_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("provider", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_embedder_model(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("embedder_model", value, sizeof(value));
   return value;
}

int config_set_embedder_model(const char *value)
{
   return config_client_set_string("embedder_model", value);
}

size_t config_embedder_model_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("embedder_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_vault_tpm2_blob_path(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("vault_tpm2_blob_path", value, sizeof(value));
   return value;
}

int config_set_vault_tpm2_blob_path(const char *value)
{
   return config_client_set_string("vault_tpm2_blob_path", value);
}

size_t config_vault_tpm2_blob_path_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("vault_tpm2_blob_path", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_pr_base_mode(void)
{
   static _Thread_local char value[32];
   (void)config_client_read_string("pr_base_mode", value, sizeof(value));
   return value;
}

int config_set_pr_base_mode(const char *value)
{
   return config_client_set_string("pr_base_mode", value);
}

size_t config_pr_base_mode_copy(char *out, size_t n)
{
   char value[32];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("pr_base_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_citations_mode(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("memory_citations_mode", value, sizeof(value));
   return value;
}

int config_set_memory_citations_mode(const char *value)
{
   return config_client_set_string("memory_citations_mode", value);
}

size_t config_memory_citations_mode_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_citations_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_learning_embed_model_version(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("learning_embed_model_version", value, sizeof(value));
   return value;
}

int config_set_learning_embed_model_version(const char *value)
{
   return config_client_set_string("learning_embed_model_version", value);
}

size_t config_learning_embed_model_version_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("learning_embed_model_version", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_search_backends(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("search_backends", value, sizeof(value));
   return value;
}

int config_set_search_backends(const char *value)
{
   return config_client_set_string("search_backends", value);
}

size_t config_search_backends_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("search_backends", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_computer_use_default_navigation(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("computer_use_default_navigation", value, sizeof(value));
   return value;
}

int config_set_computer_use_default_navigation(const char *value)
{
   return config_client_set_string("computer_use_default_navigation", value);
}

size_t config_computer_use_default_navigation_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("computer_use_default_navigation", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_client_url(void)
{
   static _Thread_local char value[CONFIG_DB2_URL_LEN];
   (void)config_client_read_string("kb_client_url", value, sizeof(value));
   return value;
}

int config_set_kb_client_url(const char *value)
{
   return config_client_set_string("kb_client_url", value);
}

size_t config_kb_client_url_copy(char *out, size_t n)
{
   char value[CONFIG_DB2_URL_LEN];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_client_url", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_server_api_mtls_client_ca(void)
{
   static _Thread_local char value[MAX_PATH_LEN];
   (void)config_client_read_string("server_api_mtls_client_ca", value, sizeof(value));
   return value;
}

int config_set_server_api_mtls_client_ca(const char *value)
{
   return config_client_set_string("server_api_mtls_client_ca", value);
}

size_t config_server_api_mtls_client_ca_copy(char *out, size_t n)
{
   char value[MAX_PATH_LEN];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("server_api_mtls_client_ca", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_ranker_fit_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("kb_ranker_fit_command", value, sizeof(value));
   return value;
}

int config_set_kb_ranker_fit_command(const char *value)
{
   return config_client_set_string("kb_ranker_fit_command", value);
}

size_t config_kb_ranker_fit_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_ranker_fit_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_trigger_auth_token(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("trigger_auth_token", value, sizeof(value));
   return value;
}

int config_set_trigger_auth_token(const char *value)
{
   return config_client_set_string("trigger_auth_token", value);
}

size_t config_trigger_auth_token_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("trigger_auth_token", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_custom_stages(void)
{
   static _Thread_local char value[4096];
   (void)config_client_read_string("kb_curator_custom_stages", value, sizeof(value));
   return value;
}

int config_set_kb_curator_custom_stages(const char *value)
{
   return config_client_set_string("kb_curator_custom_stages", value);
}

size_t config_kb_curator_custom_stages_copy(char *out, size_t n)
{
   char value[4096];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_custom_stages", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_db2_vector_corpus_index(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("db2_vector_corpus_index", value, sizeof(value));
   return value;
}

int config_set_db2_vector_corpus_index(const char *value)
{
   return config_client_set_string("db2_vector_corpus_index", value);
}

size_t config_db2_vector_corpus_index_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("db2_vector_corpus_index", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_workspace_vcs_remote(int index)
{
   static _Thread_local char value[512];
   (void)config_client_read_indexed_string("workspace_vcs_remote", index, NULL, value,
                                           sizeof(value));
   return value;
}

const char *config_compact_per_tool(int index)
{
   static _Thread_local char value[128];
   (void)config_client_read_indexed_string("compact_per_tool", index, NULL, value, sizeof(value));
   return value;
}

const char *config_mcp_client_cwd(int index)
{
   static _Thread_local char value[CONFIG_MCP_MAX_CWD];
   (void)config_client_read_indexed_string("mcp_clients", index, "cwd", value, sizeof(value));
   return value;
}

const char *config_cron_job_workdir(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_WORKDIR];
   (void)config_client_read_indexed_string("cron_jobs", index, "workdir", value, sizeof(value));
   return value;
}

int config_cron_job_enabled(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("cron_jobs", index, "enabled", &value);
   return (int)value;
}

const char *config_disposition_name(int index)
{
   static _Thread_local char value[CONFIG_DISPOSITION_NAME_LEN];
   (void)config_client_read_indexed_string("dispositions", index, "name", value, sizeof(value));
   return value;
}
