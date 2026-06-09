/* config_save.c: YAML writer + small accessors (guardrail mode, conversation
 * dirs). Load and schema validation stay in config.c. */
#include "aimee.h"
#include "config_internal.h"
#include "db1_optional.h"
#include "maintenance.h"
#include "platform_process.h"
#include "platform_path.h"
#include "sandbox.h"
#include "cJSON.h"
#include "server.h" /* SERVER_REMOTE_WRITES_* */
#include "yaml.h"
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/* Defined in config_kb_curator.c (inverse of config_parse_kb_curator). */
void config_save_kb_curator(const config_t *cfg, cJSON *root);
/* Defined in config_kb_maintenance.c (inverse of config_parse_kb_maintenance). */
void config_save_kb_maintenance(const config_t *cfg, cJSON *root);
/* Defined in config_charter.c (inverse of config_parse_charter). */
void config_save_charter(const config_t *cfg, cJSON *root);
/* Defined in config_learning.c (inverse of the intelligence.* parsers). */
void config_save_intelligence(const config_t *cfg, cJSON *root);
/* Defined in config_trigger.c (inverse of config_parse_trigger + cron_jobs). */
void config_save_trigger(const config_t *cfg, cJSON *root);

/* Inverse of the config.c inline parsers for dogfood/ensemble/integrity/identity
 * (each its own top-level object), so config_save does not drop them on save. */
static void config_save_misc_sections(const config_t *cfg, cJSON *root)
{
   if (!cfg || !root)
      return;

   /* dogfood.* (default enabled=1) */
   if (!cfg->dogfood_enabled || cfg->dogfood_log_dir[0] || cfg->dogfood_commit_raw ||
       cfg->dogfood_inline_tagging)
   {
      cJSON *d = cJSON_AddObjectToObject(root, "dogfood");
      if (d)
      {
         cJSON_AddBoolToObject(d, "enabled", cfg->dogfood_enabled ? 1 : 0);
         if (cfg->dogfood_log_dir[0])
            cJSON_AddStringToObject(d, "log_dir", cfg->dogfood_log_dir);
         cJSON_AddBoolToObject(d, "commit_raw", cfg->dogfood_commit_raw ? 1 : 0);
         cJSON_AddBoolToObject(d, "inline_tagging", cfg->dogfood_inline_tagging ? 1 : 0);
      }
   }

   /* integrity.* (defaults enabled=0, dry_run=1) */
   if (cfg->integrity_enabled || !cfg->integrity_dry_run)
   {
      cJSON *ig = cJSON_AddObjectToObject(root, "integrity");
      if (ig)
      {
         cJSON_AddBoolToObject(ig, "enabled", cfg->integrity_enabled ? 1 : 0);
         cJSON_AddBoolToObject(ig, "dry_run", cfg->integrity_dry_run ? 1 : 0);
      }
   }

   /* ensemble.* */
   if (cfg->ensemble_enabled || cfg->ensemble_aggregator[0] || cfg->ensemble_min_successful != 2 ||
       cfg->ensemble_max_cost_usd != 1.0 || cfg->ensemble_reference_count > 0)
   {
      cJSON *e = cJSON_AddObjectToObject(root, "ensemble");
      if (e)
      {
         cJSON_AddBoolToObject(e, "enabled", cfg->ensemble_enabled ? 1 : 0);
         if (cfg->ensemble_aggregator[0])
            cJSON_AddStringToObject(e, "aggregator", cfg->ensemble_aggregator);
         cJSON_AddNumberToObject(e, "min_successful", cfg->ensemble_min_successful);
         cJSON_AddNumberToObject(e, "max_cost_usd", cfg->ensemble_max_cost_usd);
         if (cfg->ensemble_reference_count > 0)
         {
            cJSON *refs = cJSON_AddArrayToObject(e, "reference_models");
            for (int i = 0; refs && i < cfg->ensemble_reference_count && i < 8; i++)
               cJSON_AddItemToArray(refs, cJSON_CreateString(cfg->ensemble_reference_models[i]));
         }
      }
   }

   /* roundtable.* */
   if (cfg->roundtable_max_rounds != 3 || cfg->roundtable_converge_threshold != 10 ||
       cfg->roundtable_deadline_ms != 600000 || strcmp(cfg->roundtable_turns, "parallel") != 0)
   {
      cJSON *rt = cJSON_AddObjectToObject(root, "roundtable");
      if (rt)
      {
         cJSON_AddNumberToObject(rt, "max_rounds", cfg->roundtable_max_rounds);
         cJSON_AddNumberToObject(rt, "converge_threshold", cfg->roundtable_converge_threshold);
         cJSON_AddNumberToObject(rt, "deadline_ms", cfg->roundtable_deadline_ms);
         cJSON_AddStringToObject(rt, "turns", cfg->roundtable_turns);
      }
   }

   /* identity.working_profile_injection.* */
   if (cfg->identity_working_profile_injection_enabled ||
       cfg->identity_working_profile_injection_fields_count > 0)
   {
      cJSON *id = cJSON_AddObjectToObject(root, "identity");
      cJSON *inj = id ? cJSON_AddObjectToObject(id, "working_profile_injection") : NULL;
      if (inj)
      {
         cJSON_AddBoolToObject(inj, "enabled",
                               cfg->identity_working_profile_injection_enabled ? 1 : 0);
         if (cfg->identity_working_profile_injection_fields_count > 0)
         {
            cJSON *f = cJSON_AddArrayToObject(inj, "fields");
            for (int i = 0; f && i < cfg->identity_working_profile_injection_fields_count; i++)
               cJSON_AddItemToArray(
                   f, cJSON_CreateString(cfg->identity_working_profile_injection_fields[i]));
         }
      }
   }

   /* proxy_url / proxy_token (top-level strings) */
   if (cfg->proxy_url[0])
      cJSON_AddStringToObject(root, "proxy_url", cfg->proxy_url);
   if (cfg->proxy_token[0])
      cJSON_AddStringToObject(root, "proxy_token", cfg->proxy_token);

   /* max_background_processes (top-level int; 0 = auto/unset) */
   if (cfg->max_background_processes > 0)
      cJSON_AddNumberToObject(root, "max_background_processes", cfg->max_background_processes);

   /* model_meta.* (defaults refresh_minutes=60, capability_routing=0) */
   if (cfg->model_meta_refresh_minutes != 60 || cfg->model_meta_capability_routing)
   {
      cJSON *mm = cJSON_AddObjectToObject(root, "model_meta");
      if (mm)
      {
         cJSON_AddNumberToObject(mm, "refresh_minutes", cfg->model_meta_refresh_minutes);
         cJSON_AddBoolToObject(mm, "capability_routing",
                               cfg->model_meta_capability_routing ? 1 : 0);
      }
   }

   /* search.* (web-search backend config) */
   if (cfg->search_backend[0] || cfg->search_max_results > 0 || cfg->search_searxng_url[0] ||
       cfg->search_tavily_api_key[0])
   {
      cJSON *sr = cJSON_AddObjectToObject(root, "search");
      if (sr)
      {
         if (cfg->search_backend[0])
            cJSON_AddStringToObject(sr, "backend", cfg->search_backend);
         if (cfg->search_max_results > 0)
            cJSON_AddNumberToObject(sr, "max_results", cfg->search_max_results);
         if (cfg->search_searxng_url[0])
            cJSON_AddStringToObject(sr, "searxng_url", cfg->search_searxng_url);
         if (cfg->search_tavily_api_key[0])
            cJSON_AddStringToObject(sr, "tavily_api_key", cfg->search_tavily_api_key);
      }
   }

   /* auxiliary.* (aux model routing + per-task overrides keyed by task name) */
   if (cfg->aux_enabled || cfg->aux_default_provider[0] || cfg->aux_default_model[0] ||
       cfg->aux_default_max_tokens > 0 || cfg->aux_task_count > 0)
   {
      cJSON *aux = cJSON_AddObjectToObject(root, "auxiliary");
      if (aux)
      {
         cJSON_AddBoolToObject(aux, "enabled", cfg->aux_enabled ? 1 : 0);
         if (cfg->aux_default_provider[0])
            cJSON_AddStringToObject(aux, "default_provider", cfg->aux_default_provider);
         if (cfg->aux_default_model[0])
            cJSON_AddStringToObject(aux, "default_model", cfg->aux_default_model);
         if (cfg->aux_default_max_tokens > 0)
            cJSON_AddNumberToObject(aux, "default_max_tokens", cfg->aux_default_max_tokens);
         if (cfg->aux_task_count > 0)
         {
            cJSON *tasks = cJSON_AddObjectToObject(aux, "tasks");
            for (int i = 0; tasks && i < cfg->aux_task_count && i < CONFIG_AUX_MAX_TASKS; i++)
            {
               if (!cfg->aux_tasks[i].task[0])
                  continue;
               cJSON *to = cJSON_AddObjectToObject(tasks, cfg->aux_tasks[i].task);
               if (!to)
                  continue;
               if (cfg->aux_tasks[i].provider[0])
                  cJSON_AddStringToObject(to, "provider", cfg->aux_tasks[i].provider);
               if (cfg->aux_tasks[i].model[0])
                  cJSON_AddStringToObject(to, "model", cfg->aux_tasks[i].model);
               if (cfg->aux_tasks[i].max_tokens > 0)
                  cJSON_AddNumberToObject(to, "max_tokens", cfg->aux_tasks[i].max_tokens);
            }
         }
      }
   }

   /* lsp_servers[] — {name, command, args[], extensions[]} */
   if (cfg->lsp_server_count > 0)
   {
      cJSON *arr = cJSON_AddArrayToObject(root, "lsp_servers");
      for (int i = 0; arr && i < cfg->lsp_server_count && i < CONFIG_LSP_MAX_SERVERS; i++)
      {
         const struct config_lsp_server *sv = &cfg->lsp_servers[i];
         cJSON *o = cJSON_CreateObject();
         if (!o)
            continue;
         if (sv->name[0])
            cJSON_AddStringToObject(o, "name", sv->name);
         if (sv->command[0])
            cJSON_AddStringToObject(o, "command", sv->command);
         if (sv->arg_count > 0)
         {
            cJSON *a = cJSON_AddArrayToObject(o, "args");
            for (int k = 0; a && k < sv->arg_count && k < CONFIG_LSP_MAX_ARGS; k++)
               cJSON_AddItemToArray(a, cJSON_CreateString(sv->args[k]));
         }
         if (sv->extension_count > 0)
         {
            cJSON *e = cJSON_AddArrayToObject(o, "extensions");
            for (int k = 0; e && k < sv->extension_count && k < CONFIG_LSP_MAX_EXTENSIONS; k++)
               cJSON_AddItemToArray(e, cJSON_CreateString(sv->extensions[k]));
         }
         cJSON_AddItemToArray(arr, o);
      }
   }
}

/* --- Save --- */

static void ensure_config_dir(void)
{
   const char *dir = config_default_dir();
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s", dir);

   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         platform_mkdir_p(tmp, 0700);
         *p = '/';
      }
   }
   platform_mkdir_p(tmp, 0700);
}

static const char *config_save_default_db1_path(void)
{
   if (db1_default_path)
      return db1_default_path();

   static char path[MAX_PATH_LEN];
   const char *dir = config_default_dir();
   snprintf(path, sizeof(path), "%s/aimee.db", dir ? dir : "/tmp");
   return path;
}

int config_save(const config_t *cfg)
{
   ensure_config_dir();

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   if (cfg->db1_path[0] && strcmp(cfg->db1_path, config_save_default_db1_path()) != 0)
      cJSON_AddStringToObject(root, "db1_path", cfg->db1_path);
   if (cfg->db2_url[0])
      cJSON_AddStringToObject(root, "db2_url", cfg->db2_url);
   if (cfg->kb_client_url[0])
      cJSON_AddStringToObject(root, "kb_client_url", cfg->kb_client_url);
   if (cfg->kb_client_bearer_token[0])
      cJSON_AddStringToObject(root, "kb_client_bearer_token", cfg->kb_client_bearer_token);
   if (cfg->db2_pool_size > 0 && cfg->db2_pool_size != 8)
      cJSON_AddNumberToObject(root, "db2_pool_size", cfg->db2_pool_size);
   cJSON_AddStringToObject(root, "guardrail_mode", cfg->guardrail_mode);
   cJSON_AddStringToObject(root, "provider", cfg->provider);

   if (cfg->autonomous)
      cJSON_AddTrueToObject(root, "autonomous");

   if (cfg->claude_model[0])
      cJSON_AddStringToObject(root, "claude_model", cfg->claude_model);
   if (cfg->codex_model[0])
      cJSON_AddStringToObject(root, "codex_model", cfg->codex_model);
   if (cfg->model_reasoning_effort[0])
      cJSON_AddStringToObject(root, "model_reasoning_effort", cfg->model_reasoning_effort);

   if (cfg->openai_endpoint[0])
      cJSON_AddStringToObject(root, "openai_endpoint", cfg->openai_endpoint);
   if (cfg->openai_model[0])
      cJSON_AddStringToObject(root, "openai_model", cfg->openai_model);
   if (cfg->openai_key_cmd[0])
      cJSON_AddStringToObject(root, "openai_key_cmd", cfg->openai_key_cmd);
   if (!cfg->learning_router_enabled || cfg->learning_proposal_ttl_days != 7 ||
       cfg->learning_max_commits_per_week != 25)
   {
      cJSON *learning = cJSON_AddObjectToObject(root, "learning");
      cJSON *router = cJSON_AddObjectToObject(learning, "router");
      cJSON_AddBoolToObject(router, "enabled", cfg->learning_router_enabled ? 1 : 0);
      cJSON_AddNumberToObject(router, "ttl_days", cfg->learning_proposal_ttl_days);
      cJSON_AddNumberToObject(router, "max_commits_per_week", cfg->learning_max_commits_per_week);
   }
   if (cfg->embedding_command[0])
      cJSON_AddStringToObject(root, "embedding_command", cfg->embedding_command);
   if (cfg->embedding_model[0])
      cJSON_AddStringToObject(root, "embedding_model", cfg->embedding_model);
   if (cfg->embedding_endpoint[0])
      cJSON_AddStringToObject(root, "embedding_endpoint", cfg->embedding_endpoint);
   if (cfg->embedding_dim > 0)
      cJSON_AddNumberToObject(root, "embedding_dim", cfg->embedding_dim);
   if (cfg->memory_weight_profile[0])
      cJSON_AddStringToObject(root, "memory_weight_profile", cfg->memory_weight_profile);
   if (cfg->memory_rerank_mode[0])
      cJSON_AddStringToObject(root, "memory_rerank_mode", cfg->memory_rerank_mode);
   if (cfg->memory_rerank_enabled || cfg->memory_rerank_command[0] ||
       cfg->memory_rerank_top_k > 0 || cfg->memory_rerank_mix != 0.0)
   {
      cJSON *mr = cJSON_AddObjectToObject(root, "memory_rerank");
      cJSON_AddBoolToObject(mr, "enabled", cfg->memory_rerank_enabled);
      if (cfg->memory_rerank_command[0])
         cJSON_AddStringToObject(mr, "command", cfg->memory_rerank_command);
      if (cfg->memory_rerank_top_k > 0)
         cJSON_AddNumberToObject(mr, "top_k", cfg->memory_rerank_top_k);
      if (cfg->memory_rerank_mix != 0.0)
         cJSON_AddNumberToObject(mr, "mix", cfg->memory_rerank_mix);
   }
   if (cfg->memory_query_expansion_mode[0] || cfg->memory_query_expansion_k > 0)
   {
      cJSON *qe = cJSON_AddObjectToObject(root, "memory_query_expansion");
      if (cfg->memory_query_expansion_mode[0])
         cJSON_AddStringToObject(qe, "mode", cfg->memory_query_expansion_mode);
      if (cfg->memory_query_expansion_k > 0)
         cJSON_AddNumberToObject(qe, "k", cfg->memory_query_expansion_k);
   }
   if (cfg->cache_aware_rewrite_enabled || cfg->cache_aware_rewrite_min_savings_tokens != 500 ||
       cfg->cache_aware_rewrite_hard_context_threshold != 0.85 ||
       cfg->cache_aware_rewrite_max_defer_turns != 20 ||
       cfg->cache_aware_rewrite_segment_check_turns != 5)
   {
      cJSON *transport = cJSON_AddObjectToObject(root, "transport");
      cJSON *cr = cJSON_AddObjectToObject(transport, "cache_aware_rewrite");
      cJSON_AddBoolToObject(cr, "enabled", cfg->cache_aware_rewrite_enabled ? 1 : 0);
      cJSON_AddNumberToObject(cr, "min_savings_tokens",
                              cfg->cache_aware_rewrite_min_savings_tokens);
      cJSON_AddNumberToObject(cr, "hard_context_threshold",
                              cfg->cache_aware_rewrite_hard_context_threshold);
      cJSON_AddNumberToObject(cr, "max_defer_turns", cfg->cache_aware_rewrite_max_defer_turns);
      cJSON_AddNumberToObject(cr, "segment_check_turns",
                              cfg->cache_aware_rewrite_segment_check_turns);
   }
   if (cfg->guardrails_semantic_enabled || !cfg->guardrails_semantic_dry_run ||
       !cfg->guardrails_semantic_advisory_only || cfg->guardrails_semantic_command[0] ||
       cfg->guardrails_semantic_warn_threshold != 0.40 ||
       cfg->guardrails_semantic_prompt_threshold != 0.70 ||
       cfg->guardrails_semantic_block_threshold != 0.90 ||
       cfg->guardrails_semantic_allow_ml_only_block)
   {
      cJSON *guardrails = cJSON_AddObjectToObject(root, "guardrails");
      cJSON *semantic = cJSON_AddObjectToObject(guardrails, "semantic");
      cJSON_AddBoolToObject(semantic, "enabled", cfg->guardrails_semantic_enabled ? 1 : 0);
      cJSON_AddBoolToObject(semantic, "dry_run", cfg->guardrails_semantic_dry_run ? 1 : 0);
      cJSON_AddBoolToObject(semantic, "advisory_only",
                            cfg->guardrails_semantic_advisory_only ? 1 : 0);
      if (cfg->guardrails_semantic_command[0])
         cJSON_AddStringToObject(semantic, "command", cfg->guardrails_semantic_command);
      cJSON_AddNumberToObject(semantic, "warn_threshold", cfg->guardrails_semantic_warn_threshold);
      cJSON_AddNumberToObject(semantic, "prompt_threshold",
                              cfg->guardrails_semantic_prompt_threshold);
      cJSON_AddNumberToObject(semantic, "block_threshold",
                              cfg->guardrails_semantic_block_threshold);
      cJSON_AddBoolToObject(semantic, "allow_ml_only_block",
                            cfg->guardrails_semantic_allow_ml_only_block ? 1 : 0);
   }
   if (cfg->memory_maintenance_trigger_inserts > 0 || cfg->memory_maintenance_trigger_secs > 0 ||
       cfg->memory_maintenance_enabled || cfg->memory_maintenance_interval_seconds > 0 ||
       cfg->memory_maintenance_summarize_enabled)
   {
      cJSON *mem_maint = cJSON_AddObjectToObject(root, "memory_maintenance");
      if (cfg->memory_maintenance_trigger_inserts > 0)
         cJSON_AddNumberToObject(mem_maint, "trigger_inserts",
                                 cfg->memory_maintenance_trigger_inserts);
      if (cfg->memory_maintenance_trigger_secs > 0)
         cJSON_AddNumberToObject(mem_maint, "trigger_secs", cfg->memory_maintenance_trigger_secs);
      if (cfg->memory_maintenance_enabled)
         cJSON_AddBoolToObject(mem_maint, "enabled", 1);
      if (cfg->memory_maintenance_interval_seconds > 0)
         cJSON_AddNumberToObject(mem_maint, "interval_seconds",
                                 cfg->memory_maintenance_interval_seconds);
      if (cfg->memory_maintenance_summarize_enabled)
         cJSON_AddBoolToObject(mem_maint, "summarize_enabled", 1);
   }
   if (cfg->skills_review_nudge_interval != 10 || cfg->skills_curator_interval_hours != 168 ||
       cfg->skills_stale_after_days != 30 || cfg->skills_archive_after_days != 90 ||
       cfg->skills_min_idle_minutes != 30 || cfg->skills_manage_enabled ||
       cfg->skills_review_enabled || cfg->skills_curator_enabled || !cfg->skills_dispatch_enabled ||
       cfg->skills_dispatch_max_index != 24 || cfg->skills_dispatch_advisory ||
       cfg->skills_capability_autostub || cfg->skills_eval_gate_enabled ||
       cfg->skills_eval_threshold != 0.01)
   {
      cJSON *skills = cJSON_AddObjectToObject(root, "skills");
      if (cfg->skills_review_enabled)
      {
         cJSON *review = cJSON_AddObjectToObject(skills, "review");
         cJSON_AddBoolToObject(review, "enabled", 1);
      }
      cJSON_AddNumberToObject(skills, "review_nudge_interval", cfg->skills_review_nudge_interval);
      if (cfg->skills_curator_enabled)
      {
         cJSON *curator = cJSON_AddObjectToObject(skills, "curator");
         cJSON_AddBoolToObject(curator, "enabled", 1);
      }
      cJSON_AddNumberToObject(skills, "curator_interval_hours", cfg->skills_curator_interval_hours);
      cJSON_AddNumberToObject(skills, "stale_after_days", cfg->skills_stale_after_days);
      cJSON_AddNumberToObject(skills, "archive_after_days", cfg->skills_archive_after_days);
      cJSON_AddNumberToObject(skills, "min_idle_minutes", cfg->skills_min_idle_minutes);
      cJSON *manage = cJSON_AddObjectToObject(skills, "manage");
      cJSON_AddBoolToObject(manage, "enabled", cfg->skills_manage_enabled ? 1 : 0);
      cJSON *dispatch = cJSON_AddObjectToObject(skills, "dispatch");
      cJSON_AddBoolToObject(dispatch, "enabled", cfg->skills_dispatch_enabled ? 1 : 0);
      cJSON_AddNumberToObject(dispatch, "max_index", cfg->skills_dispatch_max_index);
      cJSON_AddBoolToObject(dispatch, "advisory", cfg->skills_dispatch_advisory ? 1 : 0);
      cJSON *capability = cJSON_AddObjectToObject(skills, "capability");
      cJSON_AddBoolToObject(capability, "autostub", cfg->skills_capability_autostub ? 1 : 0);
      cJSON *eval = cJSON_AddObjectToObject(skills, "eval");
      cJSON_AddBoolToObject(eval, "gate_enabled", cfg->skills_eval_gate_enabled ? 1 : 0);
      cJSON_AddNumberToObject(eval, "threshold", cfg->skills_eval_threshold);
   }
   if (cfg->disposition_count > 0 || cfg->disposition_global_count > 0 ||
       cfg->disposition_workspace_count > 0 || cfg->disposition_project_count > 0 ||
       cfg->memory_salience_enabled || cfg->memory_salience_weight > 0.0 ||
       cfg->memory_salience_window_size > 0 || cfg->memory_surprise_enabled ||
       cfg->memory_surprise_weight > 0.0 || cfg->memory_coref_window > 0 ||
       (cfg->memory_coref_mode[0] && strcmp(cfg->memory_coref_mode, "off") != 0) ||
       cfg->memory_cognify_async_enabled || cfg->memory_pagerank_enabled ||
       cfg->memory_pagerank_iterations > 0 || cfg->memory_pagerank_weight > 0.0 ||
       cfg->memory_pagerank_relations[0] ||
       (cfg->memory_citations_mode[0] && strcmp(cfg->memory_citations_mode, "off") != 0) ||
       cfg->memory_citations_reprompt_on_miss || cfg->memory_citations_strip_unverified ||
       cfg->memory_profile_cards_enabled || cfg->memory_profile_cards_min_obs > 0 ||
       cfg->memory_profile_cards_stale_secs > 0 || cfg->memory_briefing_enabled ||
       cfg->memory_briefing_limit_tokens > 0 || cfg->memory_aggregation_enabled ||
       cfg->memory_aggregation_max_items > 0 || cfg->memory_prospective_enabled ||
       cfg->memory_prospective_max_matches > 0 || cfg->memory_lifecycle_enabled ||
       cfg->memory_lifecycle_hide_archived || cfg->memory_lifecycle_ttl_date_days > 0 ||
       cfg->memory_lifecycle_ttl_relative_days > 0 ||
       cfg->memory_lifecycle_ttl_open_ended_days > 0 || cfg->memory_recall_enabled ||
       cfg->memory_recall_limit_tokens_session > 0 || cfg->memory_recall_limit_tokens_turn > 0 ||
       cfg->memory_directives_enabled || cfg->memory_directives_failure_threshold > 0 ||
       cfg->memory_directives_max_matches > 0 || cfg->memory_rewrite_enabled ||
       cfg->memory_rewrite_command[0])
   {
      cJSON *memory = cJSON_AddObjectToObject(root, "memory");
      if (cfg->disposition_count > 0 || cfg->disposition_global_count > 0 ||
          cfg->disposition_workspace_count > 0 || cfg->disposition_project_count > 0)
      {
         cJSON *disp = cJSON_AddObjectToObject(memory, "dispositions");
         int global_count = cfg->disposition_global_count;
         const config_disposition_t *globals = cfg->disposition_globals;
         if (global_count == 0 && cfg->disposition_count > 0 &&
             cfg->disposition_workspace_count == 0 && cfg->disposition_project_count == 0)
         {
            global_count = cfg->disposition_count;
            globals = cfg->dispositions;
         }

         if (cfg->disposition_workspace_count == 0 && cfg->disposition_project_count == 0)
         {
            for (int i = 0; i < global_count; i++)
               cJSON_AddNumberToObject(disp, globals[i].name, globals[i].value);
         }
         else
         {
            if (global_count > 0)
            {
               cJSON *global = cJSON_AddObjectToObject(disp, "global");
               for (int i = 0; i < global_count; i++)
                  cJSON_AddNumberToObject(global, globals[i].name, globals[i].value);
            }
            if (cfg->disposition_workspace_count > 0)
            {
               cJSON *workspace = cJSON_AddObjectToObject(disp, "workspace");
               for (int i = 0; i < cfg->disposition_workspace_count; i++)
                  cJSON_AddNumberToObject(workspace, cfg->disposition_workspaces[i].name,
                                          cfg->disposition_workspaces[i].value);
            }
            if (cfg->disposition_project_count > 0)
            {
               cJSON *project = cJSON_AddObjectToObject(disp, "project");
               for (int i = 0; i < cfg->disposition_project_count; i++)
                  cJSON_AddNumberToObject(project, cfg->disposition_projects[i].name,
                                          cfg->disposition_projects[i].value);
            }
         }
      }
      if (cfg->memory_salience_enabled || cfg->memory_salience_weight > 0.0 ||
          cfg->memory_salience_window_size > 0 || cfg->memory_surprise_enabled ||
          cfg->memory_surprise_weight > 0.0)
      {
         cJSON *salience_cfg = cJSON_AddObjectToObject(memory, "salience");
         cJSON_AddBoolToObject(salience_cfg, "enabled", cfg->memory_salience_enabled ? 1 : 0);
         if (cfg->memory_salience_weight > 0.0)
            cJSON_AddNumberToObject(salience_cfg, "weight", cfg->memory_salience_weight);
         if (cfg->memory_salience_window_size > 0)
            cJSON_AddNumberToObject(salience_cfg, "window_size", cfg->memory_salience_window_size);
         cJSON_AddBoolToObject(salience_cfg, "surprise_enabled",
                               cfg->memory_surprise_enabled ? 1 : 0);
         if (cfg->memory_surprise_weight > 0.0)
            cJSON_AddNumberToObject(salience_cfg, "surprise_weight", cfg->memory_surprise_weight);
      }
      if (cfg->memory_cognify_async_enabled)
      {
         cJSON *cognify_cfg = cJSON_AddObjectToObject(memory, "cognify");
         cJSON *async_cfg = cJSON_AddObjectToObject(cognify_cfg, "async");
         cJSON_AddBoolToObject(async_cfg, "enabled", 1);
      }
      if (cfg->memory_pagerank_enabled || cfg->memory_pagerank_iterations > 0 ||
          cfg->memory_pagerank_weight > 0.0 || cfg->memory_pagerank_relations[0])
      {
         cJSON *pagerank_cfg = cJSON_AddObjectToObject(memory, "pagerank");
         cJSON_AddBoolToObject(pagerank_cfg, "enabled", cfg->memory_pagerank_enabled ? 1 : 0);
         if (cfg->memory_pagerank_iterations > 0)
            cJSON_AddNumberToObject(pagerank_cfg, "iterations", cfg->memory_pagerank_iterations);
         if (cfg->memory_pagerank_weight > 0.0)
            cJSON_AddNumberToObject(pagerank_cfg, "weight", cfg->memory_pagerank_weight);
         if (cfg->memory_pagerank_relations[0])
            cJSON_AddStringToObject(pagerank_cfg, "relations", cfg->memory_pagerank_relations);
      }
      if ((cfg->memory_coref_mode[0] && strcmp(cfg->memory_coref_mode, "off") != 0) ||
          cfg->memory_coref_window > 0)
      {
         cJSON *coref_cfg = cJSON_AddObjectToObject(memory, "coref");
         if (cfg->memory_coref_mode[0])
            cJSON_AddStringToObject(coref_cfg, "mode", cfg->memory_coref_mode);
         if (cfg->memory_coref_window > 0)
            cJSON_AddNumberToObject(coref_cfg, "window", cfg->memory_coref_window);
      }
      if ((cfg->memory_citations_mode[0] && strcmp(cfg->memory_citations_mode, "off") != 0) ||
          cfg->memory_citations_reprompt_on_miss || cfg->memory_citations_strip_unverified)
      {
         cJSON *citations_cfg = cJSON_AddObjectToObject(memory, "citations");
         if (cfg->memory_citations_mode[0])
            cJSON_AddStringToObject(citations_cfg, "mode", cfg->memory_citations_mode);
         cJSON_AddBoolToObject(citations_cfg, "reprompt_on_miss",
                               cfg->memory_citations_reprompt_on_miss ? 1 : 0);
         cJSON_AddBoolToObject(citations_cfg, "strip_unverified",
                               cfg->memory_citations_strip_unverified ? 1 : 0);
      }
      if (cfg->memory_profile_cards_enabled || cfg->memory_profile_cards_min_obs > 0 ||
          cfg->memory_profile_cards_stale_secs > 0)
      {
         cJSON *pc_cfg = cJSON_AddObjectToObject(memory, "profile_cards");
         cJSON_AddBoolToObject(pc_cfg, "enabled", cfg->memory_profile_cards_enabled ? 1 : 0);
         if (cfg->memory_profile_cards_min_obs > 0)
            cJSON_AddNumberToObject(pc_cfg, "min_observations", cfg->memory_profile_cards_min_obs);
         if (cfg->memory_profile_cards_stale_secs > 0)
            cJSON_AddNumberToObject(pc_cfg, "stale_secs", cfg->memory_profile_cards_stale_secs);
      }
      if (cfg->memory_briefing_enabled || cfg->memory_briefing_limit_tokens > 0)
      {
         cJSON *briefing = cJSON_AddObjectToObject(memory, "briefing");
         cJSON_AddBoolToObject(briefing, "enabled", cfg->memory_briefing_enabled ? 1 : 0);
         if (cfg->memory_briefing_limit_tokens > 0)
            cJSON_AddNumberToObject(briefing, "limit_tokens", cfg->memory_briefing_limit_tokens);
      }
      if (cfg->memory_aggregation_enabled || cfg->memory_aggregation_max_items > 0)
      {
         cJSON *agg = cJSON_AddObjectToObject(memory, "aggregation");
         cJSON_AddBoolToObject(agg, "enabled", cfg->memory_aggregation_enabled ? 1 : 0);
         if (cfg->memory_aggregation_max_items > 0)
            cJSON_AddNumberToObject(agg, "max_items", cfg->memory_aggregation_max_items);
      }
      if (cfg->memory_prospective_enabled || cfg->memory_prospective_max_matches > 0)
      {
         cJSON *prosp = cJSON_AddObjectToObject(memory, "prospective");
         cJSON_AddBoolToObject(prosp, "enabled", cfg->memory_prospective_enabled ? 1 : 0);
         if (cfg->memory_prospective_max_matches > 0)
            cJSON_AddNumberToObject(prosp, "max_matches", cfg->memory_prospective_max_matches);
      }
      if (cfg->memory_lifecycle_enabled || cfg->memory_lifecycle_hide_archived ||
          cfg->memory_lifecycle_ttl_date_days > 0 || cfg->memory_lifecycle_ttl_relative_days > 0 ||
          cfg->memory_lifecycle_ttl_open_ended_days > 0)
      {
         cJSON *lc = cJSON_AddObjectToObject(memory, "lifecycle");
         cJSON_AddBoolToObject(lc, "enabled", cfg->memory_lifecycle_enabled ? 1 : 0);
         cJSON_AddBoolToObject(lc, "hide_archived", cfg->memory_lifecycle_hide_archived ? 1 : 0);
         if (cfg->memory_lifecycle_ttl_date_days > 0)
            cJSON_AddNumberToObject(lc, "ttl_date_days", cfg->memory_lifecycle_ttl_date_days);
         if (cfg->memory_lifecycle_ttl_relative_days > 0)
            cJSON_AddNumberToObject(lc, "ttl_relative_days",
                                    cfg->memory_lifecycle_ttl_relative_days);
         if (cfg->memory_lifecycle_ttl_open_ended_days > 0)
            cJSON_AddNumberToObject(lc, "ttl_open_ended_days",
                                    cfg->memory_lifecycle_ttl_open_ended_days);
      }
      if (cfg->memory_recall_enabled || cfg->memory_recall_limit_tokens_session > 0 ||
          cfg->memory_recall_limit_tokens_turn > 0)
      {
         cJSON *rc = cJSON_AddObjectToObject(memory, "recall");
         cJSON_AddBoolToObject(rc, "enabled", cfg->memory_recall_enabled ? 1 : 0);
         if (cfg->memory_recall_limit_tokens_session > 0)
            cJSON_AddNumberToObject(rc, "limit_tokens_session",
                                    cfg->memory_recall_limit_tokens_session);
         if (cfg->memory_recall_limit_tokens_turn > 0)
            cJSON_AddNumberToObject(rc, "limit_tokens_turn", cfg->memory_recall_limit_tokens_turn);
      }
      if (cfg->memory_directives_enabled || cfg->memory_directives_failure_threshold > 0 ||
          cfg->memory_directives_max_matches > 0)
      {
         cJSON *dc = cJSON_AddObjectToObject(memory, "directives");
         cJSON_AddBoolToObject(dc, "enabled", cfg->memory_directives_enabled ? 1 : 0);
         if (cfg->memory_directives_failure_threshold > 0)
            cJSON_AddNumberToObject(dc, "failure_threshold",
                                    cfg->memory_directives_failure_threshold);
         if (cfg->memory_directives_max_matches > 0)
            cJSON_AddNumberToObject(dc, "max_matches", cfg->memory_directives_max_matches);
      }
      if (cfg->memory_rewrite_enabled || cfg->memory_rewrite_command[0])
      {
         cJSON *rw_cfg = cJSON_AddObjectToObject(memory, "rewrite");
         cJSON_AddBoolToObject(rw_cfg, "enabled", cfg->memory_rewrite_enabled ? 1 : 0);
         if (cfg->memory_rewrite_command[0])
            cJSON_AddStringToObject(rw_cfg, "command", cfg->memory_rewrite_command);
         cJSON_AddBoolToObject(rw_cfg, "hyde", cfg->memory_rewrite_hyde ? 1 : 0);
         cJSON_AddBoolToObject(rw_cfg, "decompose", cfg->memory_rewrite_decompose ? 1 : 0);
         if (cfg->memory_rewrite_max_subqueries > 0)
            cJSON_AddNumberToObject(rw_cfg, "max_subqueries", cfg->memory_rewrite_max_subqueries);
      }
   }

   /* workspaces: absolute paths. A non-default resource provider promotes the
    * entry from a bare string to a {path, provider} object (back-compatible:
    * shared/co-located workspaces stay plain strings). A `mirror` workspace also
    * carries its client vcs.remote + head so the session setup can drive the
    * mirror lifecycle (workspace-resource-plane §3). */
   cJSON *ws = cJSON_AddArrayToObject(root, "workspaces");
   for (int i = 0; i < cfg->workspace_count; i++)
   {
      const char *prov = cfg->workspace_providers[i];
      if (prov[0] && strcmp(prov, "shared") != 0)
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "path", cfg->workspaces[i]);
         cJSON_AddStringToObject(entry, "provider", prov);
         if (cfg->workspace_vcs_remote[i][0])
            cJSON_AddStringToObject(entry, "remote", cfg->workspace_vcs_remote[i]);
         if (cfg->workspace_vcs_head[i][0])
            cJSON_AddStringToObject(entry, "head", cfg->workspace_vcs_head[i]);
         cJSON_AddItemToArray(ws, entry);
      }
      else
         cJSON_AddItemToArray(ws, cJSON_CreateString(cfg->workspaces[i]));
   }

   /* Verify gating: only persist when enabled (default-off is the absence). */
   if (cfg->verify_enabled)
      cJSON_AddBoolToObject(root, "verify_enabled", 1);
   if (cfg->verify_cross_project)
      cJSON_AddBoolToObject(root, "verify_cross_project", 1);
   if (cfg->ingress_preinject_enabled)
      cJSON_AddBoolToObject(root, "ingress_preinject_enabled", 1);

   /* Cross-verification */
   if (cfg->cross_verify || cfg->verify_cmd[0] || cfg->verify_role[0])
   {
      cJSON *cv = cJSON_AddObjectToObject(root, "cross_verify");
      cJSON_AddBoolToObject(cv, "enabled", cfg->cross_verify);
      if (cfg->verify_cmd[0])
         cJSON_AddStringToObject(cv, "verify_cmd", cfg->verify_cmd);
      if (cfg->verify_role[0])
         cJSON_AddStringToObject(cv, "role", cfg->verify_role);
      if (cfg->verify_prompt[0])
         cJSON_AddStringToObject(cv, "prompt", cfg->verify_prompt);
   }

   /* Agent iteration limits (only save if non-default) */
   if (cfg->max_iterations)
      cJSON_AddNumberToObject(root, "max_iterations", cfg->max_iterations);
   if (cfg->max_iterations_delegate)
      cJSON_AddNumberToObject(root, "max_iterations_delegate", cfg->max_iterations_delegate);

   /* Delegation depth/spawn limits (only save if non-default) */
   if (cfg->max_delegation_depth)
      cJSON_AddNumberToObject(root, "max_delegation_depth", cfg->max_delegation_depth);
   if (cfg->max_delegation_spawns)
      cJSON_AddNumberToObject(root, "max_delegation_spawns", cfg->max_delegation_spawns);
   if (cfg->compute_threads)
      cJSON_AddNumberToObject(root, "background_threads", cfg->compute_threads);
   if (cfg->session_threads)
      cJSON_AddNumberToObject(root, "session_threads", cfg->session_threads);

   /* API retry settings (only save if non-default) */
   if (cfg->retry_max_attempts || cfg->retry_base_ms || cfg->retry_max_ms)
   {
      cJSON *retry = cJSON_AddObjectToObject(root, "retry");
      if (cfg->retry_max_attempts)
         cJSON_AddNumberToObject(retry, "max_attempts", cfg->retry_max_attempts);
      if (cfg->retry_base_ms)
         cJSON_AddNumberToObject(retry, "base_ms", cfg->retry_base_ms);
      if (cfg->retry_max_ms)
         cJSON_AddNumberToObject(retry, "max_ms", cfg->retry_max_ms);
   }

   int save_preempt_requeue =
       cfg->concurrency_preempt_requeue_max != CONFIG_DEFAULT_CONCURRENCY_PREEMPT_REQUEUE_MAX;

   /* Concurrency limits (only save if configured) */
   if (cfg->concurrency_default || cfg->concurrency_per_model_count ||
       cfg->concurrency_per_provider_count || cfg->concurrency_preempt_enabled ||
       !cfg->concurrency_preempt_single_slot_only ||
       (cfg->concurrency_preempt_enabled && save_preempt_requeue))
   {
      cJSON *conc = cJSON_AddObjectToObject(root, "concurrency");
      if (cfg->concurrency_default)
         cJSON_AddNumberToObject(conc, "default", cfg->concurrency_default);
      if (cfg->concurrency_per_model_count > 0)
      {
         cJSON *pm = cJSON_AddObjectToObject(conc, "per_model");
         for (int i = 0; i < cfg->concurrency_per_model_count; i++)
            cJSON_AddNumberToObject(pm, cfg->concurrency_per_model[i].key,
                                    cfg->concurrency_per_model[i].limit);
      }
      if (cfg->concurrency_per_provider_count > 0)
      {
         cJSON *pp = cJSON_AddObjectToObject(conc, "per_provider");
         for (int i = 0; i < cfg->concurrency_per_provider_count; i++)
            cJSON_AddNumberToObject(pp, cfg->concurrency_per_provider[i].key,
                                    cfg->concurrency_per_provider[i].limit);
      }
      if (cfg->concurrency_preempt_enabled || !cfg->concurrency_preempt_single_slot_only ||
          (cfg->concurrency_preempt_enabled && save_preempt_requeue))
      {
         cJSON *preempt = cJSON_AddObjectToObject(conc, "preempt");
         cJSON_AddBoolToObject(preempt, "enabled", cfg->concurrency_preempt_enabled);
         if (!cfg->concurrency_preempt_single_slot_only)
            cJSON_AddBoolToObject(preempt, "single_slot_only", 0);
         if (cfg->concurrency_preempt_enabled && save_preempt_requeue)
            cJSON_AddNumberToObject(preempt, "requeue_max", cfg->concurrency_preempt_requeue_max);
      }
   }

   /* Tool result compaction (only save non-default values) */
   if (!cfg->compact_enabled || cfg->compact_threshold || cfg->compact_head_bytes ||
       cfg->compact_tail_bytes || cfg->compact_per_tool_count)
   {
      cJSON *cmpct = cJSON_AddObjectToObject(root, "compact");
      cJSON_AddBoolToObject(cmpct, "enabled", cfg->compact_enabled);
      if (cfg->compact_threshold)
         cJSON_AddNumberToObject(cmpct, "threshold", cfg->compact_threshold);
      if (cfg->compact_head_bytes)
         cJSON_AddNumberToObject(cmpct, "head_bytes", cfg->compact_head_bytes);
      if (cfg->compact_tail_bytes)
         cJSON_AddNumberToObject(cmpct, "tail_bytes", cfg->compact_tail_bytes);
      if (cfg->compact_per_tool_count > 0)
      {
         cJSON *pt = cJSON_AddObjectToObject(cmpct, "per_tool");
         for (int i = 0; i < cfg->compact_per_tool_count; i++)
         {
            /* Entry format: "tool_name=threshold" */
            char tool[64];
            int thresh = 0;
            if (sscanf(cfg->compact_per_tool[i], "%63[^=]=%d", tool, &thresh) == 2)
               cJSON_AddNumberToObject(pt, tool, thresh);
         }
      }
   }

   /* Session/worktree cleanup policy (only save if non-default) */
   if (cfg->worktree_stale_secs || cfg->max_sessions || cfg->max_worktrees)
   {
      cJSON *sess = cJSON_AddObjectToObject(root, "sessions");
      if (cfg->worktree_stale_secs)
         cJSON_AddNumberToObject(sess, "stale_threshold_secs", cfg->worktree_stale_secs);
      if (cfg->max_sessions)
         cJSON_AddNumberToObject(sess, "max_sessions", cfg->max_sessions);
      if (cfg->max_worktrees)
         cJSON_AddNumberToObject(sess, "max_worktrees", cfg->max_worktrees);
   }

   /* Sandbox config (only save if non-default) */
   if (cfg->sandbox.mode != SANDBOX_MODE_OFF || cfg->sandbox.network_isolated ||
       cfg->sandbox.allow_path_count > 0)
   {
      cJSON *sbox = cJSON_AddObjectToObject(root, "sandbox");
      cJSON_AddStringToObject(sbox, "mode", sandbox_mode_to_string(cfg->sandbox.mode));
      if (cfg->sandbox.network_isolated)
         cJSON_AddTrueToObject(sbox, "network");
      if (cfg->sandbox.allow_path_count > 0)
      {
         cJSON *paths = cJSON_AddArrayToObject(sbox, "allow_paths");
         for (int i = 0; i < cfg->sandbox.allow_path_count; i++)
            cJSON_AddItemToArray(paths, cJSON_CreateString(cfg->sandbox.allow_paths[i]));
      }
   }

   /* Prompt tier config (only save non-default values) */
   if (cfg->prompt_tier[0])
      cJSON_AddStringToObject(root, "prompt_tier", cfg->prompt_tier);
   if (cfg->prompt_file[0])
      cJSON_AddStringToObject(root, "prompt_file", cfg->prompt_file);
   if (cfg->delegate_prompt_tier[0])
      cJSON_AddStringToObject(root, "delegate_prompt_tier", cfg->delegate_prompt_tier);

   /* Ecomode (only save when explicitly enabled) */
   if (cfg->ecomode)
      cJSON_AddBoolToObject(root, "ecomode", 1);

   /* Rewind settings (only save non-default values) */
   if (cfg->rewind_auto_snapshot)
   {
      cJSON *rw = cJSON_AddObjectToObject(root, "rewind");
      cJSON_AddBoolToObject(rw, "auto_snapshot", 1);
   }

   if (cfg->mcp_client_count > 0)
   {
      cJSON *mcp_arr = cJSON_AddArrayToObject(root, "mcp_clients");
      for (int i = 0; i < cfg->mcp_client_count; i++)
      {
         const config_mcp_client_t *client = &cfg->mcp_clients[i];
         cJSON *entry = cJSON_CreateObject();
         if (!entry)
            continue;

         cJSON_AddStringToObject(entry, "name", client->name);
         cJSON_AddStringToObject(entry, "transport",
                                 config_mcp_transport_to_string(client->transport));

         if (client->command_count > 0)
         {
            cJSON *cmd = cJSON_AddArrayToObject(entry, "command");
            for (int j = 0; j < client->command_count; j++)
               cJSON_AddItemToArray(cmd, cJSON_CreateString(client->command[j]));
         }
         if (client->cwd[0])
            cJSON_AddStringToObject(entry, "cwd", client->cwd);
         if (client->url[0])
            cJSON_AddStringToObject(entry, "url", client->url);
         if (client->bearer_token_env[0])
            cJSON_AddStringToObject(entry, "bearer_token_env", client->bearer_token_env);

         cJSON_AddItemToArray(mcp_arr, entry);
      }
   }

   if (!cfg->mcp_osv_enabled || cfg->mcp_osv_offline || !cfg->mcp_osv_enforce ||
       cfg->mcp_osv_cache_ttl_hours != 24 ||
       strcmp(cfg->mcp_osv_endpoint, "https://api.osv.dev/v1/query") != 0 ||
       cfg->mcp_osv_allow_count > 0)
   {
      cJSON *mcp = cJSON_AddObjectToObject(root, "mcp");
      cJSON *osv = cJSON_AddObjectToObject(mcp, "osv");
      cJSON_AddBoolToObject(osv, "enabled", cfg->mcp_osv_enabled ? 1 : 0);
      cJSON_AddBoolToObject(osv, "offline", cfg->mcp_osv_offline ? 1 : 0);
      cJSON_AddBoolToObject(osv, "enforce", cfg->mcp_osv_enforce ? 1 : 0);
      cJSON_AddNumberToObject(osv, "cache_ttl_hours", cfg->mcp_osv_cache_ttl_hours);
      cJSON_AddStringToObject(osv, "endpoint", cfg->mcp_osv_endpoint);
      if (cfg->mcp_osv_allow_count > 0)
      {
         cJSON *allow = cJSON_AddArrayToObject(osv, "allow");
         for (int i = 0; i < cfg->mcp_osv_allow_count; i++)
            cJSON_AddItemToArray(allow, cJSON_CreateString(cfg->mcp_osv_allow[i]));
      }
   }

   if (cfg->computer_use_enabled || strcmp(cfg->computer_use_default_navigation, "approve") != 0 ||
       !cfg->computer_use_redact_sensitive_screenshots ||
       cfg->computer_use_allowed_domain_count != 1 ||
       strcmp(cfg->computer_use_allowed_domains[0], "localhost") != 0)
   {
      cJSON *cu = cJSON_AddObjectToObject(root, "computer_use");
      cJSON_AddBoolToObject(cu, "enabled", cfg->computer_use_enabled ? 1 : 0);
      cJSON_AddStringToObject(cu, "default_navigation", cfg->computer_use_default_navigation);
      cJSON_AddBoolToObject(cu, "redact_sensitive_screenshots",
                            cfg->computer_use_redact_sensitive_screenshots ? 1 : 0);
      cJSON *domains = cJSON_AddArrayToObject(cu, "allowed_domains");
      for (int i = 0; i < cfg->computer_use_allowed_domain_count; i++)
         cJSON_AddItemToArray(domains, cJSON_CreateString(cfg->computer_use_allowed_domains[i]));
   }

   /* OpenTelemetry export (only save when endpoint is set) */
   if (cfg->otel_endpoint[0])
   {
      cJSON *otel_obj = cJSON_AddObjectToObject(root, "otel");
      cJSON_AddStringToObject(otel_obj, "endpoint", cfg->otel_endpoint);
      if (cfg->otel_service_name[0])
         cJSON_AddStringToObject(otel_obj, "service_name", cfg->otel_service_name);
   }

   /* Public HTTP API (aimee.api.*): round-trip the optional loopback /v1
    * listener config so `aimee api enable` persists and a hand-edited block
    * is not silently dropped on the next save. config_server_api.c parses
    * these back from the same nested mapping. */
   if (cfg->server_api_http_port > 0 || cfg->server_api_bearer_token[0] ||
       cfg->server_api_rate_limit_per_min > 0 || cfg->server_api_client_transport[0] ||
       cfg->server_api_remote_writes > SERVER_REMOTE_WRITES_OFF ||
       cfg->server_api_max_event_streams > 0)
   {
      cJSON *aimee_obj = cJSON_AddObjectToObject(root, "aimee");
      cJSON *api_obj = cJSON_AddObjectToObject(aimee_obj, "api");
      if (cfg->server_api_http_port > 0)
         cJSON_AddNumberToObject(api_obj, "http_port", cfg->server_api_http_port);
      if (cfg->server_api_bearer_token[0])
         cJSON_AddStringToObject(api_obj, "bearer_token", cfg->server_api_bearer_token);
      if (cfg->server_api_rate_limit_per_min > 0)
         cJSON_AddNumberToObject(api_obj, "rate_limit_per_min", cfg->server_api_rate_limit_per_min);
      if (cfg->server_api_max_event_streams > 0)
         cJSON_AddNumberToObject(api_obj, "max_event_streams", cfg->server_api_max_event_streams);
      if (cfg->server_api_client_transport[0])
         cJSON_AddStringToObject(api_obj, "client_transport", cfg->server_api_client_transport);
      if (cfg->server_api_remote_writes >= SERVER_REMOTE_WRITES_FULL)
         cJSON_AddStringToObject(api_obj, "remote_writes", "full");
      else if (cfg->server_api_remote_writes >= SERVER_REMOTE_WRITES_DATA)
         cJSON_AddStringToObject(api_obj, "remote_writes", "data");
   }

   /* kb.curator.* + kb.evidence.embed.* — serialized by config_kb_curator.c,
    * the inverse of config_parse_kb_curator (keeps the curator gates from being
    * dropped on save, e.g. by --bootstrap-db2). */
   config_save_kb_curator(cfg, root);
   config_save_kb_maintenance(cfg, root);
   config_save_charter(cfg, root);
   config_save_intelligence(cfg, root);
   config_save_trigger(cfg, root);
   config_save_misc_sections(cfg, root);

   char *yaml_str = yaml_emit(root);
   cJSON_Delete(root);
   if (!yaml_str)
      return -1;

   const char *path = config_default_path();

   /* Atomic write: write to temp file, then rename */
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid());
   FILE *fp = fopen(tmp, "w");
   if (!fp)
   {
      free(yaml_str);
      return -1;
   }

   fputs(yaml_str, fp);
   fclose(fp);
   free(yaml_str);

   /* Restrict permissions (may reference sensitive config) */
   chmod(tmp, 0600);

   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }

   /* Refresh the in-process cache so subsequent loads in the same second
    * (mtime resolution can collide on some filesystems) don't return the
    * stale pre-save snapshot. */
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         memcpy(&g_config_cache, cfg, sizeof(g_config_cache));
         g_config_mtime = AIMEE_STAT_MTIM(st);
         snprintf(g_config_cache_path, sizeof(g_config_cache_path), "%s", path);
         g_config_cached = 1;
      }
   }

   return 0;
}

/* --- Guardrail mode --- */

const char *config_guardrail_mode(const config_t *cfg)
{
   if (cfg->guardrail_mode[0])
      return cfg->guardrail_mode;
   return MODE_APPROVE;
}

/* --- Conversation directories --- */

int config_conversation_dirs(const config_t *cfg, char dirs[][MAX_PATH_LEN], int max_dirs)
{
   const char *home = platform_home_dir();
   if (!home)
      home = "/tmp";

   const char *provider = cfg->provider[0] ? cfg->provider : "claude";
   int count = 0;

   if (strcmp(provider, "claude") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.claude/projects", home);
         count++;
      }
   }
   else if (strcmp(provider, "gemini") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.gemini/tmp", home);
         count++;
      }
   }
   else if (strcmp(provider, "codex") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.codex/sessions", home);
         count++;
      }
   }
   else if (strcmp(provider, "mistral-plan") == 0 || strcmp(provider, "vibe") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.vibe/logs/session", home);
         count++;
      }
   }
   else if (strcmp(provider, "copilot") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.copilot", home);
         count++;
      }
   }

   return count;
}
