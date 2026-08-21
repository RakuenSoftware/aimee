/* Generated event-bus config accessor implementations. */
#include "config.h"
#include "config_client.h"
#include "runtime_secret.h"
#include <stdio.h>

int config_memory_maintenance_trigger_secs(void)
{
   double value = 0;
   (void)config_client_read_number("memory_maintenance_trigger_secs", &value);
   return (int)value;
}

int config_set_memory_maintenance_trigger_secs(int value)
{
   return config_client_set_number("memory_maintenance_trigger_secs", (double)value);
}

int config_memory_salience_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_salience_enabled", &value);
   return (int)value;
}

int config_set_memory_salience_enabled(int value)
{
   return config_client_set_number("memory_salience_enabled", (double)value);
}

int config_memory_context_budget_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_context_budget_enabled", &value);
   return (int)value;
}

int config_set_memory_context_budget_enabled(int value)
{
   return config_client_set_number("memory_context_budget_enabled", (double)value);
}

int config_ingress_compress_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("ingress_compress_enabled", &value);
   return (int)value;
}

int config_set_ingress_compress_enabled(int value)
{
   return config_client_set_number("ingress_compress_enabled", (double)value);
}

int config_require_aimee_memory(void)
{
   double value = 0;
   (void)config_client_read_number("require_aimee_memory", &value);
   return (int)value;
}

int config_set_require_aimee_memory(int value)
{
   return config_client_set_number("require_aimee_memory", (double)value);
}

int config_memory_typed_facts_speculative_ttl_days(void)
{
   double value = 0;
   (void)config_client_read_number("memory_typed_facts_speculative_ttl_days", &value);
   return (int)value;
}

int config_set_memory_typed_facts_speculative_ttl_days(int value)
{
   return config_client_set_number("memory_typed_facts_speculative_ttl_days", (double)value);
}

int config_kb_evidence_emit_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_evidence_emit_enabled", &value);
   return (int)value;
}

int config_set_kb_evidence_emit_enabled(int value)
{
   return config_client_set_number("kb_evidence_emit_enabled", (double)value);
}

int config_memory_briefing_limit_tokens(void)
{
   double value = 0;
   (void)config_client_read_number("memory_briefing_limit_tokens", &value);
   return (int)value;
}

int config_set_memory_briefing_limit_tokens(int value)
{
   return config_client_set_number("memory_briefing_limit_tokens", (double)value);
}

int config_memory_lifecycle_ttl_relative_days(void)
{
   double value = 0;
   (void)config_client_read_number("memory_lifecycle_ttl_relative_days", &value);
   return (int)value;
}

int config_set_memory_lifecycle_ttl_relative_days(int value)
{
   return config_client_set_number("memory_lifecycle_ttl_relative_days", (double)value);
}

int config_disposition_count(void)
{
   double value = 0;
   (void)config_client_read_number("disposition_count", &value);
   return (int)value;
}

int config_set_disposition_count(int value)
{
   return config_client_set_number("disposition_count", (double)value);
}

int config_charter_working_profile_drift_limit(void)
{
   double value = 0;
   (void)config_client_read_number("charter_working_profile_drift_limit", &value);
   return (int)value;
}

int config_set_charter_working_profile_drift_limit(int value)
{
   return config_client_set_number("charter_working_profile_drift_limit", (double)value);
}

int config_memory_rewrite_decompose(void)
{
   double value = 0;
   (void)config_client_read_number("memory_rewrite_decompose", &value);
   return (int)value;
}

int config_set_memory_rewrite_decompose(int value)
{
   return config_client_set_number("memory_rewrite_decompose", (double)value);
}

int config_memory_recall_lanes_k_fact(void)
{
   double value = 0;
   (void)config_client_read_number("memory_recall_lanes_k_fact", &value);
   return (int)value;
}

int config_set_memory_recall_lanes_k_fact(int value)
{
   return config_client_set_number("memory_recall_lanes_k_fact", (double)value);
}

int config_memory_scenes_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_scenes_enabled", &value);
   return (int)value;
}

int config_set_memory_scenes_enabled(int value)
{
   return config_client_set_number("memory_scenes_enabled", (double)value);
}

double config_memory_semantic_weight(void)
{
   double value = 0;
   (void)config_client_read_number("memory_semantic_weight", &value);
   return (double)value;
}

int config_set_memory_semantic_weight(double value)
{
   return config_client_set_number("memory_semantic_weight", (double)value);
}

int config_dogfood_autolabel_continuation(void)
{
   double value = 0;
   (void)config_client_read_number("dogfood_autolabel_continuation", &value);
   return (int)value;
}

int config_set_dogfood_autolabel_continuation(int value)
{
   return config_client_set_number("dogfood_autolabel_continuation", (double)value);
}

int config_learning_implicit_citation_repair(void)
{
   double value = 0;
   (void)config_client_read_number("learning_implicit_citation_repair", &value);
   return (int)value;
}

int config_set_learning_implicit_citation_repair(int value)
{
   return config_client_set_number("learning_implicit_citation_repair", (double)value);
}

int config_roundtable_replay_verify_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_replay_verify_enabled", &value);
   return (int)value;
}

int config_set_roundtable_replay_verify_enabled(int value)
{
   return config_client_set_number("roundtable_replay_verify_enabled", (double)value);
}

int config_max_iterations(void)
{
   double value = 0;
   (void)config_client_read_number("max_iterations", &value);
   return (int)value;
}

int config_set_max_iterations(int value)
{
   return config_client_set_number("max_iterations", (double)value);
}

int config_concurrency_default(void)
{
   double value = 0;
   (void)config_client_read_number("concurrency_default", &value);
   return (int)value;
}

int config_set_concurrency_default(int value)
{
   return config_client_set_number("concurrency_default", (double)value);
}

int config_compact_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("compact_enabled", &value);
   return (int)value;
}

int config_set_compact_enabled(int value)
{
   return config_client_set_number("compact_enabled", (double)value);
}

int config_coord_closet_max_ratio_pct(void)
{
   double value = 0;
   (void)config_client_read_number("coord_closet_max_ratio_pct", &value);
   return (int)value;
}

int config_set_coord_closet_max_ratio_pct(int value)
{
   return config_client_set_number("coord_closet_max_ratio_pct", (double)value);
}

int config_fold_recall_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("fold_recall_enabled", &value);
   return (int)value;
}

int config_set_fold_recall_enabled(int value)
{
   return config_client_set_number("fold_recall_enabled", (double)value);
}

int config_module_roundtable(void)
{
   double value = 0;
   (void)config_client_read_number("module_roundtable", &value);
   return (int)value;
}

int config_set_module_roundtable(int value)
{
   return config_client_set_number("module_roundtable", (double)value);
}

int config_autonomy_max_wall_secs(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_max_wall_secs", &value);
   return (int)value;
}

int config_set_autonomy_max_wall_secs(int value)
{
   return config_client_set_number("autonomy_max_wall_secs", (double)value);
}

int config_max_background_processes(void)
{
   double value = 0;
   (void)config_client_read_number("max_background_processes", &value);
   return (int)value;
}

int config_set_max_background_processes(int value)
{
   return config_client_set_number("max_background_processes", (double)value);
}

int config_mcp_osv_allow_count(void)
{
   double value = 0;
   (void)config_client_read_number("mcp_osv_allow_count", &value);
   return (int)value;
}

int config_set_mcp_osv_allow_count(int value)
{
   return config_client_set_number("mcp_osv_allow_count", (double)value);
}

int config_cache_aware_rewrite_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("cache_aware_rewrite_enabled", &value);
   return (int)value;
}

int config_set_cache_aware_rewrite_enabled(int value)
{
   return config_client_set_number("cache_aware_rewrite_enabled", (double)value);
}

int config_cost_reward_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("cost_reward_enabled", &value);
   return (int)value;
}

int config_set_cost_reward_enabled(int value)
{
   return config_client_set_number("cost_reward_enabled", (double)value);
}

int config_cache_min_chars(void)
{
   double value = 0;
   (void)config_client_read_number("cache_min_chars", &value);
   return (int)value;
}

int config_set_cache_min_chars(int value)
{
   return config_client_set_number("cache_min_chars", (double)value);
}

int config_server_api_mtls(void)
{
   double value = 0;
   (void)config_client_read_number("server_api_mtls", &value);
   return (int)value;
}

int config_set_server_api_mtls(int value)
{
   return config_client_set_number("server_api_mtls", (double)value);
}

double config_calibration_prior_alpha0(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_prior_alpha0", &value);
   return (double)value;
}

int config_set_calibration_prior_alpha0(double value)
{
   return config_client_set_number("calibration_prior_alpha0", (double)value);
}

double config_calibration_tau_working_profile_flag(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_tau_working_profile_flag", &value);
   return (double)value;
}

int config_set_calibration_tau_working_profile_flag(double value)
{
   return config_client_set_number("calibration_tau_working_profile_flag", (double)value);
}

int config_kb_ranker_fit_min_groups(void)
{
   double value = 0;
   (void)config_client_read_number("kb_ranker_fit_min_groups", &value);
   return (int)value;
}

int config_set_kb_ranker_fit_min_groups(int value)
{
   return config_client_set_number("kb_ranker_fit_min_groups", (double)value);
}

double config_planner_exploration_constant(void)
{
   double value = 0;
   (void)config_client_read_number("planner_exploration_constant", &value);
   return (double)value;
}

int config_set_planner_exploration_constant(double value)
{
   return config_client_set_number("planner_exploration_constant", (double)value);
}

int config_kb_bg_ingest_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_bg_ingest_enabled", &value);
   return (int)value;
}

int config_set_kb_bg_ingest_enabled(int value)
{
   return config_client_set_number("kb_bg_ingest_enabled", (double)value);
}

double config_code_hybrid_rrf_k(void)
{
   double value = 0;
   (void)config_client_read_number("code_hybrid_rrf_k", &value);
   return (double)value;
}

int config_set_code_hybrid_rrf_k(double value)
{
   return config_client_set_number("code_hybrid_rrf_k", (double)value);
}

double config_kb_maintenance_floor(void)
{
   double value = 0;
   (void)config_client_read_number("kb_maintenance_floor", &value);
   return (double)value;
}

int config_set_kb_maintenance_floor(double value)
{
   return config_client_set_number("kb_maintenance_floor", (double)value);
}

int config_cron_job_count(void)
{
   double value = 0;
   (void)config_client_read_number("cron_job_count", &value);
   return (int)value;
}

int config_set_cron_job_count(int value)
{
   return config_client_set_number("cron_job_count", (double)value);
}

int config_kb_curator_extract_docs_workers(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_extract_docs_workers", &value);
   return (int)value;
}

int config_set_kb_curator_extract_docs_workers(int value)
{
   return config_client_set_number("kb_curator_extract_docs_workers", (double)value);
}

int config_kb_curator_synthesize_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_synthesize_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_synthesize_enabled(int value)
{
   return config_client_set_number("kb_curator_synthesize_enabled", (double)value);
}

int config_kb_curator_cross_repo_k(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_k", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_k(int value)
{
   return config_client_set_number("kb_curator_cross_repo_k", (double)value);
}

int config_kb_evidence_embed_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_evidence_embed_enabled", &value);
   return (int)value;
}

int config_set_kb_evidence_embed_enabled(int value)
{
   return config_client_set_number("kb_evidence_embed_enabled", (double)value);
}

int config_skills_dispatch_advisory(void)
{
   double value = 0;
   (void)config_client_read_number("skills_dispatch_advisory", &value);
   return (int)value;
}

int config_set_skills_dispatch_advisory(int value)
{
   return config_client_set_number("skills_dispatch_advisory", (double)value);
}

int config_aux_task_count(void)
{
   double value = 0;
   (void)config_client_read_number("aux_task_count", &value);
   return (int)value;
}

int config_set_aux_task_count(int value)
{
   return config_client_set_number("aux_task_count", (double)value);
}

double config_ensemble_max_cost_usd(void)
{
   double value = 0;
   (void)config_client_read_number("ensemble_max_cost_usd", &value);
   return (double)value;
}

int config_set_ensemble_max_cost_usd(double value)
{
   return config_client_set_number("ensemble_max_cost_usd", (double)value);
}

int config_roundtable_pipeline_gate_ttl_h(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_pipeline_gate_ttl_h", &value);
   return (int)value;
}

int config_set_roundtable_pipeline_gate_ttl_h(int value)
{
   return config_client_set_number("roundtable_pipeline_gate_ttl_h", (double)value);
}

const char *config_codex_model(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("codex_model", value, sizeof(value));
   return value;
}

int config_set_codex_model(const char *value)
{
   return config_client_set_string("codex_model", value);
}

size_t config_codex_model_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("codex_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_weight_profile(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("memory_weight_profile", value, sizeof(value));
   return value;
}

int config_set_memory_weight_profile(const char *value)
{
   return config_client_set_string("memory_weight_profile", value);
}

size_t config_memory_weight_profile_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_weight_profile", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_coref_mode(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("memory_coref_mode", value, sizeof(value));
   return value;
}

int config_set_memory_coref_mode(const char *value)
{
   return config_client_set_string("memory_coref_mode", value);
}

size_t config_memory_coref_mode_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_coref_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_pdf_tier(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("kb_pdf_tier", value, sizeof(value));
   return value;
}

int config_set_kb_pdf_tier(const char *value)
{
   return config_client_set_string("kb_pdf_tier", value);
}

size_t config_kb_pdf_tier_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_pdf_tier", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_recall_lanes_summary_kinds(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("memory_recall_lanes_summary_kinds", value, sizeof(value));
   return value;
}

int config_set_memory_recall_lanes_summary_kinds(const char *value)
{
   return config_client_set_string("memory_recall_lanes_summary_kinds", value);
}

size_t config_memory_recall_lanes_summary_kinds_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_recall_lanes_summary_kinds", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_verify_role(void)
{
   static _Thread_local char value[32];
   (void)config_client_read_string("verify_role", value, sizeof(value));
   return value;
}

int config_set_verify_role(const char *value)
{
   return config_client_set_string("verify_role", value);
}

size_t config_verify_role_copy(char *out, size_t n)
{
   char value[32];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("verify_role", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_prompt_file(void)
{
   static _Thread_local char value[MAX_PATH_LEN];
   (void)config_client_read_string("prompt_file", value, sizeof(value));
   return value;
}

int config_set_prompt_file(const char *value)
{
   return config_client_set_string("prompt_file", value);
}

size_t config_prompt_file_copy(char *out, size_t n)
{
   char value[MAX_PATH_LEN];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("prompt_file", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_ingress_trusted_proxy_secret(void)
{
   static _Thread_local char value[128];
   (void)config_client_read_string("ingress_trusted_proxy_secret", value, sizeof(value));
   return value;
}

int config_set_ingress_trusted_proxy_secret(const char *value)
{
   return config_client_set_string("ingress_trusted_proxy_secret", value);
}

size_t config_ingress_trusted_proxy_secret_copy(char *out, size_t n)
{
   char value[128];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("ingress_trusted_proxy_secret", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_aimee_with_llamacpp(void)
{
   static _Thread_local char value[8];
   (void)config_client_read_string("aimee_with_llamacpp", value, sizeof(value));
   return value;
}

int config_set_aimee_with_llamacpp(const char *value)
{
   return config_client_set_string("aimee_with_llamacpp", value);
}

size_t config_aimee_with_llamacpp_copy(char *out, size_t n)
{
   char value[8];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("aimee_with_llamacpp", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_calibration_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("calibration_command", value, sizeof(value));
   return value;
}

int config_set_calibration_command(const char *value)
{
   return config_client_set_string("calibration_command", value);
}

size_t config_calibration_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("calibration_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_reasoning_datalog_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("reasoning_datalog_command", value, sizeof(value));
   return value;
}

int config_set_reasoning_datalog_command(const char *value)
{
   return config_client_set_string("reasoning_datalog_command", value);
}

size_t config_reasoning_datalog_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("reasoning_datalog_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_embed_model_version(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("kb_curator_embed_model_version", value, sizeof(value));
   return value;
}

int config_set_kb_curator_embed_model_version(const char *value)
{
   return config_client_set_string("kb_curator_embed_model_version", value);
}

size_t config_kb_curator_embed_model_version_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_embed_model_version", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_provider_api_key(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("kb_curator_provider_api_key", value, sizeof(value));
   return value;
}

int config_set_kb_curator_provider_api_key(const char *value)
{
   return config_client_set_string("kb_curator_provider_api_key", value);
}

size_t config_kb_curator_provider_api_key_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_provider_api_key", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_roundtable_default(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("roundtable_default", value, sizeof(value));
   return value;
}

int config_set_roundtable_default(const char *value)
{
   return config_client_set_string("roundtable_default", value);
}

size_t config_roundtable_default_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("roundtable_default", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_charter_safety_axioms(int index)
{
   static _Thread_local char value[CONFIG_CHARTER_ENTRY_LEN];
   (void)config_client_read_indexed_string("charter_safety_axioms", index, NULL, value,
                                           sizeof(value));
   return value;
}

const char *config_server_api_bearer_extra(int index)
{
   static _Thread_local char value[256];
   value[0] = 0;
   if (index >= 0 && index < AIMEE_API_BEARER_EXTRA_MAX)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", index);
      (void)runtime_secret_get(name, value, sizeof(value));
   }
   return value;
}

const char *config_cron_job_id(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_ID];
   (void)config_client_read_indexed_string("cron_jobs", index, "id", value, sizeof(value));
   return value;
}

int config_cron_job_skill_count(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("cron_jobs", index, "skill_count", &value);
   return (int)value;
}

const char *config_trigger_rule_schedule(int index)
{
   static _Thread_local char value[TRIGGER_RULE_MAX_SCHEDULE];
   (void)config_client_read_indexed_string("trigger_rules", index, "schedule", value,
                                           sizeof(value));
   return value;
}
