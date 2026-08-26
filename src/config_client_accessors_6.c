/* Generated event-bus config accessor implementations. */
#include "config.h"
#include "config_client.h"
#include "runtime_secret.h"
#include <stdio.h>

int config_memory_maintenance_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_maintenance_enabled", &value);
   return (int)value;
}

int config_set_memory_maintenance_enabled(int value)
{
   return config_client_set_number("memory_maintenance_enabled", (double)value);
}

double config_memory_salience_weight(void)
{
   double value = 0;
   (void)config_client_read_number("memory_salience_weight", &value);
   return (double)value;
}

int config_set_memory_salience_weight(double value)
{
   return config_client_set_number("memory_salience_weight", (double)value);
}

int config_memory_context_budget_tokens(void)
{
   double value = 0;
   (void)config_client_read_number("memory_context_budget_tokens", &value);
   return (int)value;
}

int config_set_memory_context_budget_tokens(int value)
{
   return config_client_set_number("memory_context_budget_tokens", (double)value);
}

int config_ingress_compress_min_chars(void)
{
   double value = 0;
   (void)config_client_read_number("ingress_compress_min_chars", &value);
   return (int)value;
}

int config_set_ingress_compress_min_chars(int value)
{
   return config_client_set_number("ingress_compress_min_chars", (double)value);
}

int config_require_aimee_git(void)
{
   /* Fail CLOSED. This is a guard rail, and it defaulted to 0 -- so a config
    * service that was unreachable, or a key nobody had set, silently turned the
    * guard OFF. The client-side accessor for the same setting has always passed
    * a default of 1, so the two disagreed about what an absent value means, and
    * the one that fails open is the one the server-side chokepoint uses.
    *
    * cmd_hooks.c already documents this dial as "an unreadable config reads as
    * ENFORCING", which is what it now does.
    *
    * read_number leaves value untouched when it cannot answer, so an explicit
    * `require_aimee_git: false` still turns the guard off -- only silence enforces. */
   double value = 1;
   (void)config_client_read_number("require_aimee_git", &value);
   return (int)value;
}

int config_set_require_aimee_git(int value)
{
   return config_client_set_number("require_aimee_git", (double)value);
}

int config_memory_typed_facts_promote_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("memory_typed_facts_promote_threshold", &value);
   return (int)value;
}

int config_set_memory_typed_facts_promote_threshold(int value)
{
   return config_client_set_number("memory_typed_facts_promote_threshold", (double)value);
}

int config_fidelity_check_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("fidelity_check_enabled", &value);
   return (int)value;
}

int config_set_fidelity_check_enabled(int value)
{
   return config_client_set_number("fidelity_check_enabled", (double)value);
}

int config_memory_aggregation_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_aggregation_enabled", &value);
   return (int)value;
}

int config_set_memory_aggregation_enabled(int value)
{
   return config_client_set_number("memory_aggregation_enabled", (double)value);
}

int config_memory_lifecycle_ttl_open_ended_days(void)
{
   double value = 0;
   (void)config_client_read_number("memory_lifecycle_ttl_open_ended_days", &value);
   return (int)value;
}

int config_set_memory_lifecycle_ttl_open_ended_days(int value)
{
   return config_client_set_number("memory_lifecycle_ttl_open_ended_days", (double)value);
}

int config_disposition_global_count(void)
{
   double value = 0;
   (void)config_client_read_number("disposition_global_count", &value);
   return (int)value;
}

int config_set_disposition_global_count(int value)
{
   return config_client_set_number("disposition_global_count", (double)value);
}

int config_identity_working_profile_injection_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("identity_working_profile_injection_enabled", &value);
   return (int)value;
}

int config_set_identity_working_profile_injection_enabled(int value)
{
   return config_client_set_number("identity_working_profile_injection_enabled", (double)value);
}

int config_memory_rewrite_max_subqueries(void)
{
   double value = 0;
   (void)config_client_read_number("memory_rewrite_max_subqueries", &value);
   return (int)value;
}

int config_set_memory_rewrite_max_subqueries(int value)
{
   return config_client_set_number("memory_rewrite_max_subqueries", (double)value);
}

int config_memory_recall_lanes_floor_summary(void)
{
   double value = 0;
   (void)config_client_read_number("memory_recall_lanes_floor_summary", &value);
   return (int)value;
}

int config_set_memory_recall_lanes_floor_summary(int value)
{
   return config_client_set_number("memory_recall_lanes_floor_summary", (double)value);
}

double config_memory_scenes_global_escape_ratio(void)
{
   double value = 0;
   (void)config_client_read_number("memory_scenes_global_escape_ratio", &value);
   return (double)value;
}

int config_set_memory_scenes_global_escape_ratio(double value)
{
   return config_client_set_number("memory_scenes_global_escape_ratio", (double)value);
}

int config_memory_fetch_budget_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("memory_fetch_budget_enabled", &value);
   return (int)value;
}

int config_set_memory_fetch_budget_enabled(int value)
{
   return config_client_set_number("memory_fetch_budget_enabled", (double)value);
}

int config_dogfood_autolabel_repeat_question(void)
{
   double value = 0;
   (void)config_client_read_number("dogfood_autolabel_repeat_question", &value);
   return (int)value;
}

int config_set_dogfood_autolabel_repeat_question(int value)
{
   return config_client_set_number("dogfood_autolabel_repeat_question", (double)value);
}

int config_learning_implicit_citation_continuation(void)
{
   double value = 0;
   (void)config_client_read_number("learning_implicit_citation_continuation", &value);
   return (int)value;
}

int config_set_learning_implicit_citation_continuation(int value)
{
   return config_client_set_number("learning_implicit_citation_continuation", (double)value);
}

int config_roundtable_require_evidence(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_require_evidence", &value);
   return (int)value;
}

int config_set_roundtable_require_evidence(int value)
{
   return config_client_set_number("roundtable_require_evidence", (double)value);
}

int config_max_iterations_delegate(void)
{
   double value = 0;
   (void)config_client_read_number("max_iterations_delegate", &value);
   return (int)value;
}

int config_set_max_iterations_delegate(int value)
{
   return config_client_set_number("max_iterations_delegate", (double)value);
}

int config_concurrency_per_model_count(void)
{
   double value = 0;
   (void)config_client_read_number("concurrency_per_model_count", &value);
   return (int)value;
}

int config_set_concurrency_per_model_count(int value)
{
   return config_client_set_number("concurrency_per_model_count", (double)value);
}

int config_compact_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("compact_threshold", &value);
   return (int)value;
}

int config_set_compact_threshold(int value)
{
   return config_client_set_number("compact_threshold", (double)value);
}

int config_fold_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("fold_enabled", &value);
   return (int)value;
}

int config_set_fold_enabled(int value)
{
   return config_client_set_number("fold_enabled", (double)value);
}

int config_fold_recall_ttl_turns(void)
{
   double value = 0;
   (void)config_client_read_number("fold_recall_ttl_turns", &value);
   return (int)value;
}

int config_set_fold_recall_ttl_turns(int value)
{
   return config_client_set_number("fold_recall_ttl_turns", (double)value);
}

int config_module_economizer(void)
{
   double value = 0;
   (void)config_client_read_number("module_economizer", &value);
   return (int)value;
}

int config_set_module_economizer(int value)
{
   return config_client_set_number("module_economizer", (double)value);
}

int config_autonomy_stale_abandon_secs(void)
{
   double value = 0;
   (void)config_client_read_number("autonomy_stale_abandon_secs", &value);
   return (int)value;
}

int config_set_autonomy_stale_abandon_secs(int value)
{
   return config_client_set_number("autonomy_stale_abandon_secs", (double)value);
}

int config_rewind_auto_snapshot(void)
{
   double value = 0;
   (void)config_client_read_number("rewind_auto_snapshot", &value);
   return (int)value;
}

int config_set_rewind_auto_snapshot(int value)
{
   return config_client_set_number("rewind_auto_snapshot", (double)value);
}

int config_computer_use_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("computer_use_enabled", &value);
   return (int)value;
}

int config_set_computer_use_enabled(int value)
{
   return config_client_set_number("computer_use_enabled", (double)value);
}

int config_cache_aware_rewrite_min_savings_tokens(void)
{
   double value = 0;
   (void)config_client_read_number("cache_aware_rewrite_min_savings_tokens", &value);
   return (int)value;
}

int config_set_cache_aware_rewrite_min_savings_tokens(int value)
{
   return config_client_set_number("cache_aware_rewrite_min_savings_tokens", (double)value);
}

int config_cost_reward_lambda_pct(void)
{
   double value = 0;
   (void)config_client_read_number("cost_reward_lambda_pct", &value);
   return (int)value;
}

int config_set_cost_reward_lambda_pct(int value)
{
   return config_client_set_number("cost_reward_lambda_pct", (double)value);
}

double config_guardrails_semantic_warn_threshold(void)
{
   double value = 0;
   (void)config_client_read_number("guardrails_semantic_warn_threshold", &value);
   return (double)value;
}

int config_set_guardrails_semantic_warn_threshold(double value)
{
   return config_client_set_number("guardrails_semantic_warn_threshold", (double)value);
}

int config_server_api_bearer_extra_count(void)
{
   int count = 0;
   for (; count < AIMEE_API_BEARER_EXTRA_MAX; count++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", count);
      if (!runtime_secret_has(name))
         break;
   }
   return count;
}

int config_set_server_api_bearer_extra_count(int value)
{
   (void)value;
   return -1;
}

double config_calibration_prior_beta0(void)
{
   double value = 0;
   (void)config_client_read_number("calibration_prior_beta0", &value);
   return (double)value;
}

int config_set_calibration_prior_beta0(double value)
{
   return config_client_set_number("calibration_prior_beta0", (double)value);
}

int config_demotion_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("demotion_enabled", &value);
   return (int)value;
}

int config_set_demotion_enabled(int value)
{
   return config_client_set_number("demotion_enabled", (double)value);
}

int config_kb_ranker_fit_bench_k(void)
{
   double value = 0;
   (void)config_client_read_number("kb_ranker_fit_bench_k", &value);
   return (int)value;
}

int config_set_kb_ranker_fit_bench_k(int value)
{
   return config_client_set_number("kb_ranker_fit_bench_k", (double)value);
}

int config_kb_mdl_tiebreak_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_mdl_tiebreak_enabled", &value);
   return (int)value;
}

int config_set_kb_mdl_tiebreak_enabled(int value)
{
   return config_client_set_number("kb_mdl_tiebreak_enabled", (double)value);
}

int config_kb_bg_ingest_interval_hours(void)
{
   double value = 0;
   (void)config_client_read_number("kb_bg_ingest_interval_hours", &value);
   return (int)value;
}

int config_set_kb_bg_ingest_interval_hours(int value)
{
   return config_client_set_number("kb_bg_ingest_interval_hours", (double)value);
}

int config_code_trust_actuation_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("code_trust_actuation_enabled", &value);
   return (int)value;
}

int config_set_code_trust_actuation_enabled(int value)
{
   return config_client_set_number("code_trust_actuation_enabled", (double)value);
}

int config_kb_maintenance_min_age_days(void)
{
   double value = 0;
   (void)config_client_read_number("kb_maintenance_min_age_days", &value);
   return (int)value;
}

int config_set_kb_maintenance_min_age_days(int value)
{
   return config_client_set_number("kb_maintenance_min_age_days", (double)value);
}

int config_review_scheduler_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("review_scheduler_enabled", &value);
   return (int)value;
}

int config_set_review_scheduler_enabled(int value)
{
   return config_client_set_number("review_scheduler_enabled", (double)value);
}

int config_kb_curator_resolve_entities_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_resolve_entities_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_resolve_entities_enabled(int value)
{
   return config_client_set_number("kb_curator_resolve_entities_enabled", (double)value);
}

int config_kb_curator_promote_entity_enabled(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_promote_entity_enabled", &value);
   return (int)value;
}

int config_set_kb_curator_promote_entity_enabled(int value)
{
   return config_client_set_number("kb_curator_promote_entity_enabled", (double)value);
}

int config_kb_curator_cross_repo_m(void)
{
   double value = 0;
   (void)config_client_read_number("kb_curator_cross_repo_m", &value);
   return (int)value;
}

int config_set_kb_curator_cross_repo_m(int value)
{
   return config_client_set_number("kb_curator_cross_repo_m", (double)value);
}

int config_kb_evidence_embed_batch(void)
{
   double value = 0;
   (void)config_client_read_number("kb_evidence_embed_batch", &value);
   return (int)value;
}

int config_set_kb_evidence_embed_batch(int value)
{
   return config_client_set_number("kb_evidence_embed_batch", (double)value);
}

int config_skills_capability_autostub(void)
{
   double value = 0;
   (void)config_client_read_number("skills_capability_autostub", &value);
   return (int)value;
}

int config_set_skills_capability_autostub(int value)
{
   return config_client_set_number("skills_capability_autostub", (double)value);
}

int config_prefer_local_agents(void)
{
   double value = 0;
   (void)config_client_read_number("prefer_local_agents", &value);
   return (int)value;
}

int config_set_prefer_local_agents(int value)
{
   return config_client_set_number("prefer_local_agents", (double)value);
}

int config_roundtable_max_rounds(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_max_rounds", &value);
   return (int)value;
}

int config_set_roundtable_max_rounds(int value)
{
   return config_client_set_number("roundtable_max_rounds", (double)value);
}

int config_roundtable_pipeline_parked_releases_slot(void)
{
   double value = 0;
   (void)config_client_read_number("roundtable_pipeline_parked_releases_slot", &value);
   return (int)value;
}

int config_set_roundtable_pipeline_parked_releases_slot(int value)
{
   return config_client_set_number("roundtable_pipeline_parked_releases_slot", (double)value);
}

const char *config_model_reasoning_effort(void)
{
   static _Thread_local char value[32];
   (void)config_client_read_string("model_reasoning_effort", value, sizeof(value));
   return value;
}

int config_set_model_reasoning_effort(const char *value)
{
   return config_client_set_string("model_reasoning_effort", value);
}

size_t config_model_reasoning_effort_copy(char *out, size_t n)
{
   char value[32];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("model_reasoning_effort", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_rerank_mode(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("memory_rerank_mode", value, sizeof(value));
   return value;
}

int config_set_memory_rerank_mode(const char *value)
{
   return config_client_set_string("memory_rerank_mode", value);
}

size_t config_memory_rerank_mode_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_rerank_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_cognify_model(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("memory_cognify_model", value, sizeof(value));
   return value;
}

int config_set_memory_cognify_model(const char *value)
{
   return config_client_set_string("memory_cognify_model", value);
}

size_t config_memory_cognify_model_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_cognify_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_tsr_command(void)
{
   static _Thread_local char value[1024];
   (void)config_client_read_string("tsr_command", value, sizeof(value));
   return value;
}

int config_set_tsr_command(const char *value)
{
   return config_client_set_string("tsr_command", value);
}

size_t config_tsr_command_copy(char *out, size_t n)
{
   char value[1024];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("tsr_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_memory_recall_lanes_fact_kinds(void)
{
   static _Thread_local char value[256];
   (void)config_client_read_string("memory_recall_lanes_fact_kinds", value, sizeof(value));
   return value;
}

int config_set_memory_recall_lanes_fact_kinds(const char *value)
{
   return config_client_set_string("memory_recall_lanes_fact_kinds", value);
}

size_t config_memory_recall_lanes_fact_kinds_copy(char *out, size_t n)
{
   char value[256];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("memory_recall_lanes_fact_kinds", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_verify_prompt(void)
{
   static _Thread_local char value[2048];
   (void)config_client_read_string("verify_prompt", value, sizeof(value));
   return value;
}

int config_set_verify_prompt(const char *value)
{
   return config_client_set_string("verify_prompt", value);
}

size_t config_verify_prompt_copy(char *out, size_t n)
{
   char value[2048];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("verify_prompt", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_delegate_prompt_tier(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("delegate_prompt_tier", value, sizeof(value));
   return value;
}

int config_set_delegate_prompt_tier(const char *value)
{
   return config_client_set_string("delegate_prompt_tier", value);
}

size_t config_delegate_prompt_tier_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("delegate_prompt_tier", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_guardrails_semantic_mode(void)
{
   static _Thread_local char value[16];
   (void)config_client_read_string("guardrails_semantic_mode", value, sizeof(value));
   return value;
}

int config_set_guardrails_semantic_mode(const char *value)
{
   return config_client_set_string("guardrails_semantic_mode", value);
}

size_t config_guardrails_semantic_mode_copy(char *out, size_t n)
{
   char value[16];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("guardrails_semantic_mode", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_aimee_synthesis_model(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("aimee_synthesis_model", value, sizeof(value));
   return value;
}

int config_set_aimee_synthesis_model(const char *value)
{
   return config_client_set_string("aimee_synthesis_model", value);
}

size_t config_aimee_synthesis_model_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("aimee_synthesis_model", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_calibration_prompt_version(void)
{
   static _Thread_local char value[64];
   (void)config_client_read_string("calibration_prompt_version", value, sizeof(value));
   return value;
}

int config_set_calibration_prompt_version(const char *value)
{
   return config_client_set_string("calibration_prompt_version", value);
}

size_t config_calibration_prompt_version_copy(char *out, size_t n)
{
   char value[64];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("calibration_prompt_version", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_bandit_optimize_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("bandit_optimize_command", value, sizeof(value));
   return value;
}

int config_set_bandit_optimize_command(const char *value)
{
   return config_client_set_string("bandit_optimize_command", value);
}

size_t config_bandit_optimize_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("bandit_optimize_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_invalidation_notify_socket(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("kb_curator_invalidation_notify_socket", value, sizeof(value));
   return value;
}

int config_set_kb_curator_invalidation_notify_socket(const char *value)
{
   return config_client_set_string("kb_curator_invalidation_notify_socket", value);
}

size_t config_kb_curator_invalidation_notify_socket_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_invalidation_notify_socket", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_kb_curator_judge_command(void)
{
   static _Thread_local char value[512];
   (void)config_client_read_string("kb_curator_judge_command", value, sizeof(value));
   return value;
}

int config_set_kb_curator_judge_command(const char *value)
{
   return config_client_set_string("kb_curator_judge_command", value);
}

size_t config_kb_curator_judge_command_copy(char *out, size_t n)
{
   char value[512];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("kb_curator_judge_command", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_roundtable_pipeline_done_bar(void)
{
   static _Thread_local char value[40];
   (void)config_client_read_string("roundtable_pipeline_done_bar", value, sizeof(value));
   return value;
}

int config_set_roundtable_pipeline_done_bar(const char *value)
{
   return config_client_set_string("roundtable_pipeline_done_bar", value);
}

size_t config_roundtable_pipeline_done_bar_copy(char *out, size_t n)
{
   char value[40];
   if (!out || n == 0)
      return 0;
   (void)config_client_read_string("roundtable_pipeline_done_bar", value, sizeof(value));
   snprintf(out, n, "%s", value);
   return sizeof(value);
}

const char *config_charter_hard_constraints(int index)
{
   static _Thread_local char value[CONFIG_CHARTER_ENTRY_LEN];
   (void)config_client_read_indexed_string("charter_hard_constraints", index, NULL, value,
                                           sizeof(value));
   return value;
}

const char *config_ensemble_reference_models(int index)
{
   static _Thread_local char value[128];
   (void)config_client_read_indexed_string("ensemble_reference_models", index, NULL, value,
                                           sizeof(value));
   return value;
}

const char *config_cron_job_schedule(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_SCHEDULE];
   (void)config_client_read_indexed_string("cron_jobs", index, "schedule", value, sizeof(value));
   return value;
}

const char *config_cron_job_deliver_target(int index)
{
   static _Thread_local char value[CRON_JOB_MAX_DELIVER_TARGET];
   (void)config_client_read_indexed_string("cron_jobs", index, "deliver_target", value,
                                           sizeof(value));
   return value;
}

const char *config_trigger_rule_pipeline_template(int index)
{
   static _Thread_local char value[TRIGGER_RULE_MAX_TEMPLATE];
   (void)config_client_read_indexed_string("trigger_rules", index, "pipeline_template", value,
                                           sizeof(value));
   return value;
}
