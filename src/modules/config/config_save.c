/* config_save.c: YAML writer + small accessors (guardrail mode, conversation
 * dirs). Load and schema validation stay in config.c. */
#include "aimee.h"
#include "config_internal.h"
#include "config_fields.h"
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
#include <stddef.h> /* offsetof */
#include <stdlib.h> /* getenv — EMBEDDER_URL default */
#include "log.h"
#include <errno.h>
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

   /* modules.* (pluggable-module toggles; default -1 = unspecified). Persist only the keys the
    * operator actually set, so an untouched config never sprouts a modules: block and the
    * resolver's env/default fallback stays in effect for the rest. */
   if (cfg->module_memory != -1 || cfg->module_governance != -1 || cfg->module_delegates != -1 ||
       cfg->module_workflows != -1 || cfg->module_roundtable != -1 || cfg->module_economizer != -1)
   {
      cJSON *m = cJSON_AddObjectToObject(root, "modules");
      if (m)
      {
         if (cfg->module_memory != -1)
            cJSON_AddBoolToObject(m, "memory", cfg->module_memory ? 1 : 0);
         if (cfg->module_governance != -1)
            cJSON_AddBoolToObject(m, "governance", cfg->module_governance ? 1 : 0);
         if (cfg->module_delegates != -1)
            cJSON_AddBoolToObject(m, "delegates", cfg->module_delegates ? 1 : 0);
         if (cfg->module_workflows != -1)
            cJSON_AddBoolToObject(m, "workflows", cfg->module_workflows ? 1 : 0);
         if (cfg->module_roundtable != -1)
            cJSON_AddBoolToObject(m, "roundtable", cfg->module_roundtable ? 1 : 0);
         if (cfg->module_economizer != -1)
            cJSON_AddBoolToObject(m, "economizer", cfg->module_economizer ? 1 : 0);
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
   if (cfg->ensemble_aggregator[0] || cfg->ensemble_min_successful != 2 ||
       cfg->ensemble_max_cost_usd > 0.0 || cfg->ensemble_reference_count > 0 ||
       cfg->ensemble_reference_persona_count > 0)
   {
      cJSON *e = cJSON_AddObjectToObject(root, "ensemble");
      if (e)
      {
         if (cfg->ensemble_aggregator[0])
            cJSON_AddStringToObject(e, "aggregator", cfg->ensemble_aggregator);
         cJSON_AddNumberToObject(e, "min_successful", cfg->ensemble_min_successful);
         /* Only persist a cap when one is set; 0/unset = no limit (the default). */
         if (cfg->ensemble_max_cost_usd > 0.0)
            cJSON_AddNumberToObject(e, "max_cost_usd", cfg->ensemble_max_cost_usd);
         if (cfg->ensemble_reference_count > 0)
         {
            cJSON *refs = cJSON_AddArrayToObject(e, "reference_models");
            /* 32 = ENSEMBLE_MAX_REFS (delegate_ensemble.h). */
            for (int i = 0; refs && i < cfg->ensemble_reference_count && i < 32; i++)
               cJSON_AddItemToArray(refs, cJSON_CreateString(cfg->ensemble_reference_models[i]));
         }
         if (cfg->ensemble_reference_persona_count > 0)
         {
            cJSON *ps = cJSON_AddArrayToObject(e, "reference_personas");
            for (int i = 0; ps && i < cfg->ensemble_reference_persona_count && i < 32; i++)
               cJSON_AddItemToArray(ps, cJSON_CreateString(cfg->ensemble_reference_personas[i]));
         }
      }
   }

   /* roundtable.* (including the authoring-pipeline keys, roundtable.pipeline_*) */
   if (cfg->roundtable_max_rounds != 1 || cfg->roundtable_converge_threshold != 10 ||
       cfg->roundtable_deadline_ms != 600000 || strcmp(cfg->roundtable_turns, "parallel") != 0 ||
       strcmp(cfg->roundtable_pipeline_done_bar, "zero_blocking") != 0 ||
       cfg->roundtable_pipeline_max_passes != 0 ||
       cfg->roundtable_pipeline_max_attempts_per_pass != 2 ||
       cfg->roundtable_pipeline_max_cost_usd != 0.0 ||
       cfg->roundtable_pipeline_max_total_cost_usd != 0.0 ||
       cfg->roundtable_pipeline_gate_ttl_h != 0 ||
       cfg->roundtable_pipeline_parked_releases_slot != 1 ||
       cfg->roundtable_pipeline_unknown_context_tokens != 8000 || cfg->roundtable_default[0])
   {
      cJSON *rt = cJSON_AddObjectToObject(root, "roundtable");
      if (rt)
      {
         cJSON_AddNumberToObject(rt, "max_rounds", cfg->roundtable_max_rounds);
         cJSON_AddNumberToObject(rt, "converge_threshold", cfg->roundtable_converge_threshold);
         cJSON_AddNumberToObject(rt, "deadline_ms", cfg->roundtable_deadline_ms);
         cJSON_AddStringToObject(rt, "turns", cfg->roundtable_turns);
         if (cfg->roundtable_default[0])
            cJSON_AddStringToObject(rt, "default", cfg->roundtable_default);
         cJSON_AddStringToObject(rt, "pipeline_done_bar", cfg->roundtable_pipeline_done_bar);
         cJSON_AddNumberToObject(rt, "pipeline_max_passes", cfg->roundtable_pipeline_max_passes);
         cJSON_AddNumberToObject(rt, "pipeline_max_attempts_per_pass",
                                 cfg->roundtable_pipeline_max_attempts_per_pass);
         cJSON_AddNumberToObject(rt, "pipeline_max_cost_usd",
                                 cfg->roundtable_pipeline_max_cost_usd);
         cJSON_AddNumberToObject(rt, "pipeline_max_total_cost_usd",
                                 cfg->roundtable_pipeline_max_total_cost_usd);
         cJSON_AddNumberToObject(rt, "pipeline_gate_ttl_h", cfg->roundtable_pipeline_gate_ttl_h);
         cJSON_AddBoolToObject(rt, "pipeline_parked_releases_slot",
                               cfg->roundtable_pipeline_parked_releases_slot ? 1 : 0);
         cJSON_AddNumberToObject(rt, "pipeline_unknown_context_tokens",
                                 cfg->roundtable_pipeline_unknown_context_tokens);
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

   /* proxy_url is non-secret. proxy_token lives only in Vault. */
   if (cfg->proxy_url[0])
      cJSON_AddStringToObject(root, "proxy_url", cfg->proxy_url);

   /* max_background_processes (top-level int; 0 = auto/unset) */
   if (cfg->max_background_processes > 0)
      cJSON_AddNumberToObject(root, "max_background_processes", cfg->max_background_processes);

   if (cfg->prefer_local_agents)
   {
      cJSON *rt = cJSON_AddObjectToObject(root, "routing");
      if (rt)
         cJSON_AddBoolToObject(rt, "prefer_local", 1);
   }

   /* model_meta.* (defaults refresh_minutes=60, capability_routing=1) */
   if (cfg->model_meta_refresh_minutes != 60 || !cfg->model_meta_capability_routing)
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
       cfg->search_backends[0] || cfg->search_fetch_pages >= 0)
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
         if (cfg->search_backends[0])
            cJSON_AddStringToObject(sr, "backends", cfg->search_backends);
         if (cfg->search_fetch_pages >= 0)
            cJSON_AddBoolToObject(sr, "fetch_pages", cfg->search_fetch_pages ? 1 : 0);
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

static void config_save_concurrency(const config_t *cfg, cJSON *root)
{
   int save_preempt_requeue =
       cfg->concurrency_preempt_requeue_max != CONFIG_DEFAULT_CONCURRENCY_PREEMPT_REQUEUE_MAX;

   /* Concurrency limits (only save if configured) */
   if (cfg->concurrency_default || cfg->maximum_total_concurrent_agent_sessions ||
       cfg->concurrency_per_model_count || cfg->concurrency_per_provider_count ||
       cfg->concurrency_preempt_enabled || !cfg->concurrency_preempt_single_slot_only ||
       (cfg->concurrency_preempt_enabled && save_preempt_requeue))
   {
      cJSON *conc = cJSON_AddObjectToObject(root, "concurrency");
      if (cfg->concurrency_default)
         cJSON_AddNumberToObject(conc, "default", cfg->concurrency_default);
      if (cfg->maximum_total_concurrent_agent_sessions)
         cJSON_AddNumberToObject(conc, "maximum_total_concurrent_agent_sessions",
                                 cfg->maximum_total_concurrent_agent_sessions);
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
}

int config_save(const config_t *cfg)
{
   ensure_config_dir();

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   if (cfg->db1_path[0] && strcmp(cfg->db1_path, config_save_default_db1_path()) != 0)
      cJSON_AddStringToObject(root, "db1_path", cfg->db1_path);
   if (cfg->kb_client_url[0])
      cJSON_AddStringToObject(root, "kb_client_url", cfg->kb_client_url);

   /* Setup-wizard page-2 backend record: save each non-empty string field from a
    * compact table (mirrors the parse table in config.c). */
   {
      static const struct
      {
         const char *key;
         size_t off;
      } page2[] = {
          {"kb_mode", offsetof(config_t, kb_mode)},
          /* Must mirror the parse table in config.c: a key parsed but not saved
           * silently loses the operator's value on the next rewrite. */
          {"synthesis_endpoint", offsetof(config_t, synthesis_endpoint)},
          {"synthesis_model", offsetof(config_t, synthesis_model)},
      };
      for (size_t i = 0; i < sizeof(page2) / sizeof(page2[0]); i++)
      {
         const char *v = (const char *)cfg + page2[i].off;
         if (v[0])
            cJSON_AddStringToObject(root, page2[i].key, v);
      }
   }
   if (cfg->db2_pool_size > 0 && cfg->db2_pool_size != 8)
      cJSON_AddNumberToObject(root, "db2_pool_size", cfg->db2_pool_size);
   cJSON_AddStringToObject(root, "guardrail_mode", cfg->guardrail_mode);
   cJSON_AddStringToObject(root, "provider", cfg->provider);
   if (cfg->default_persona[0] && strcmp(cfg->default_persona, "engineer") != 0)
      cJSON_AddStringToObject(root, "default_persona", cfg->default_persona);

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
   {
      int router_any = !cfg->learning_router_enabled || cfg->learning_proposal_ttl_days != 7 ||
                       cfg->learning_max_commits_per_week != 25;
      /* citation_* default on, the 3 stateful heuristics default off — emit when
       * any differs so an override (incl. turning the citation detector off)
       * persists. */
      int implicit_any =
          !cfg->learning_implicit_citation_repair ||
          !cfg->learning_implicit_citation_continuation || cfg->learning_implicit_repeat_question ||
          cfg->learning_implicit_repeated_correction || cfg->learning_implicit_workflow_repetition;
      if (router_any || implicit_any)
      {
         cJSON *learning = cJSON_AddObjectToObject(root, "learning");
         if (router_any)
         {
            cJSON *router = cJSON_AddObjectToObject(learning, "router");
            cJSON_AddBoolToObject(router, "enabled", cfg->learning_router_enabled ? 1 : 0);
            cJSON_AddNumberToObject(router, "ttl_days", cfg->learning_proposal_ttl_days);
            cJSON_AddNumberToObject(router, "max_commits_per_week",
                                    cfg->learning_max_commits_per_week);
         }
         if (implicit_any)
         {
            cJSON *implicit = cJSON_AddObjectToObject(learning, "implicit");
            cJSON_AddBoolToObject(implicit, "citation_repair",
                                  cfg->learning_implicit_citation_repair ? 1 : 0);
            cJSON_AddBoolToObject(implicit, "citation_continuation",
                                  cfg->learning_implicit_citation_continuation ? 1 : 0);
            cJSON_AddBoolToObject(implicit, "repeat_question",
                                  cfg->learning_implicit_repeat_question ? 1 : 0);
            cJSON_AddBoolToObject(implicit, "repeated_correction",
                                  cfg->learning_implicit_repeated_correction ? 1 : 0);
            cJSON_AddBoolToObject(implicit, "workflow_repetition",
                                  cfg->learning_implicit_workflow_repetition ? 1 : 0);
         }
      }
   }
   if (cfg->embedder_command[0])
      cJSON_AddStringToObject(root, "embedder_command", cfg->embedder_command);
   if (cfg->embedder_model[0])
      cJSON_AddStringToObject(root, "embedder_model", cfg->embedder_model);
   if (cfg->embedder_url[0])
      cJSON_AddStringToObject(root, "embedder_url", cfg->embedder_url);
   if (cfg->embedder_dims > 0)
      cJSON_AddNumberToObject(root, "embedder_dims", cfg->embedder_dims);
   if (cfg->memory_weight_profile[0])
      cJSON_AddStringToObject(root, "memory_weight_profile", cfg->memory_weight_profile);
   if (cfg->memory_rerank_mode[0])
      cJSON_AddStringToObject(root, "memory_rerank_mode", cfg->memory_rerank_mode);
   if (cfg->memory_query_expansion_mode[0] || cfg->memory_query_expansion_k > 0)
   {
      cJSON *qe = cJSON_AddObjectToObject(root, "memory_query_expansion");
      if (cfg->memory_query_expansion_mode[0])
         cJSON_AddStringToObject(qe, "mode", cfg->memory_query_expansion_mode);
      if (cfg->memory_query_expansion_k > 0)
         cJSON_AddNumberToObject(qe, "k", cfg->memory_query_expansion_k);
   }
   if (cfg->transport_kb_pool_enabled || cfg->transport_server_keepalive_enabled ||
       cfg->transport_thinclient_gzip_enabled || cfg->transport_kb_gzip_enabled ||
       cfg->cache_aware_rewrite_enabled || cfg->cache_aware_rewrite_min_savings_tokens != 500 ||
       cfg->cache_aware_rewrite_hard_context_threshold != 0.85 ||
       cfg->cache_aware_rewrite_max_defer_turns != 20 ||
       cfg->cache_aware_rewrite_segment_check_turns != 5)
   {
      cJSON *transport = cJSON_AddObjectToObject(root, "transport");
      cJSON_AddBoolToObject(transport, "kb_pool_enabled", cfg->transport_kb_pool_enabled ? 1 : 0);
      cJSON_AddBoolToObject(transport, "server_keepalive_enabled",
                            cfg->transport_server_keepalive_enabled ? 1 : 0);
      cJSON_AddBoolToObject(transport, "thinclient_gzip_enabled",
                            cfg->transport_thinclient_gzip_enabled ? 1 : 0);
      cJSON_AddBoolToObject(transport, "kb_gzip_enabled", cfg->transport_kb_gzip_enabled ? 1 : 0);
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
   if (cfg->cost_reward_enabled || cfg->cost_reward_lambda_pct != 30 ||
       cfg->cost_reward_ref_usd_milli != 500)
   {
      cJSON *cre = cJSON_AddObjectToObject(root, "cost_reward");
      cJSON_AddBoolToObject(cre, "enabled", cfg->cost_reward_enabled ? 1 : 0);
      cJSON_AddNumberToObject(cre, "lambda_pct", cfg->cost_reward_lambda_pct);
      cJSON_AddNumberToObject(cre, "ref_usd_milli", cfg->cost_reward_ref_usd_milli);
   }
   if (cfg->reasoning_cap_enabled)
   {
      cJSON *rcap = cJSON_AddObjectToObject(root, "reasoning_cap");
      cJSON_AddBoolToObject(rcap, "enabled", cfg->reasoning_cap_enabled ? 1 : 0);
   }
   if (!cfg->dedup_enabled || cfg->dedup_window_seconds != 5) /* default-on: persist the opt-out */
   {
      cJSON *ddp = cJSON_AddObjectToObject(root, "dedup");
      cJSON_AddBoolToObject(ddp, "enabled", cfg->dedup_enabled ? 1 : 0);
      cJSON_AddNumberToObject(ddp, "window_seconds", cfg->dedup_window_seconds);
   }
   if (!cfg->cache_shaping_enabled || cfg->cache_min_chars != 0) /* default-on: persist opt-out */
   {
      cJSON *csh = cJSON_AddObjectToObject(root, "cache_shaping");
      cJSON_AddBoolToObject(csh, "enabled", cfg->cache_shaping_enabled ? 1 : 0);
      cJSON_AddNumberToObject(csh, "min_chars", cfg->cache_min_chars);
   }
   /* usage_accounting + audit_async are default-on: persist only an opt-out of either. */
   if (!cfg->ingress_usage_accounting_enabled || !cfg->ingress_audit_async)
   {
      cJSON *ing = cJSON_AddObjectToObject(root, "ingress");
      cJSON_AddBoolToObject(ing, "usage_accounting_enabled",
                            cfg->ingress_usage_accounting_enabled ? 1 : 0);
      cJSON_AddBoolToObject(ing, "audit_async", cfg->ingress_audit_async ? 1 : 0);
   }
   int gsem_mode = guardrails_semantic_mode_parse(cfg->guardrails_semantic_mode);
   if (gsem_mode != GSEM_MODE_OFF || cfg->guardrails_semantic_command[0] ||
       cfg->guardrails_semantic_warn_threshold != 0.40 ||
       cfg->guardrails_semantic_prompt_threshold != 0.70 ||
       cfg->guardrails_semantic_block_threshold != 0.90)
   {
      cJSON *guardrails = cJSON_AddObjectToObject(root, "guardrails");
      cJSON *semantic = cJSON_AddObjectToObject(guardrails, "semantic");
      cJSON_AddStringToObject(semantic, "mode", guardrails_semantic_mode_name(gsem_mode));
      if (cfg->guardrails_semantic_command[0])
         cJSON_AddStringToObject(semantic, "command", cfg->guardrails_semantic_command);
      cJSON_AddNumberToObject(semantic, "warn_threshold", cfg->guardrails_semantic_warn_threshold);
      cJSON_AddNumberToObject(semantic, "prompt_threshold",
                              cfg->guardrails_semantic_prompt_threshold);
      cJSON_AddNumberToObject(semantic, "block_threshold",
                              cfg->guardrails_semantic_block_threshold);
   }
   if (cfg->guardrails_blast_radius_advisory_enabled)
   {
      /* Reuse the guardrails object if the semantic block above created it. */
      cJSON *guardrails = cJSON_GetObjectItemCaseSensitive(root, "guardrails");
      if (!guardrails)
         guardrails = cJSON_AddObjectToObject(root, "guardrails");
      cJSON *br = cJSON_AddObjectToObject(guardrails, "blast_radius");
      cJSON_AddBoolToObject(br, "advisory_enabled", 1);
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
   if (cfg->skills_review_nudge_interval != 10 || cfg->skills_stale_after_days != 30 ||
       cfg->skills_archive_after_days != 90 || cfg->skills_review_enabled ||
       !cfg->skills_dispatch_enabled || cfg->skills_dispatch_max_index != 24 ||
       cfg->skills_dispatch_advisory || cfg->skills_capability_autostub ||
       cfg->skills_eval_gate_enabled || cfg->skills_eval_threshold != 0.01)
   {
      cJSON *skills = cJSON_AddObjectToObject(root, "skills");
      if (cfg->skills_review_enabled)
      {
         cJSON *review = cJSON_AddObjectToObject(skills, "review");
         cJSON_AddBoolToObject(review, "enabled", 1);
      }
      cJSON_AddNumberToObject(skills, "review_nudge_interval", cfg->skills_review_nudge_interval);
      cJSON_AddNumberToObject(skills, "stale_after_days", cfg->skills_stale_after_days);
      cJSON_AddNumberToObject(skills, "archive_after_days", cfg->skills_archive_after_days);
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
   /* worktree_gc defaults: enabled=1, max_age_days=14. Persist only when changed. */
   if (!cfg->worktree_gc_enabled || cfg->worktree_gc_max_age_days != 14)
   {
      cJSON *wt = cJSON_AddObjectToObject(root, "worktree_gc");
      cJSON_AddBoolToObject(wt, "enabled", cfg->worktree_gc_enabled ? 1 : 0);
      cJSON_AddNumberToObject(wt, "max_age_days", cfg->worktree_gc_max_age_days);
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
       !cfg->memory_profile_cards_enabled || cfg->memory_profile_cards_min_obs > 0 ||
       cfg->memory_profile_cards_stale_secs > 0 || cfg->memory_briefing_enabled ||
       cfg->memory_briefing_limit_tokens > 0 || cfg->memory_aggregation_enabled ||
       cfg->memory_aggregation_max_items > 0 || cfg->memory_prospective_enabled ||
       cfg->memory_prospective_max_matches > 0 || cfg->memory_lifecycle_enabled ||
       cfg->memory_lifecycle_hide_archived || cfg->memory_lifecycle_ttl_date_days > 0 ||
       cfg->memory_lifecycle_ttl_relative_days > 0 ||
       cfg->memory_lifecycle_ttl_open_ended_days > 0 || cfg->memory_recall_enabled ||
       cfg->memory_recall_limit_tokens_session > 0 || cfg->memory_recall_limit_tokens_turn > 0 ||
       !cfg->memory_directives_enabled || cfg->memory_directives_failure_threshold > 0 ||
       cfg->memory_directives_max_matches > 0 || cfg->memory_rewrite_enabled ||
       cfg->memory_rewrite_command[0] || !cfg->memory_improve_dedupe_enabled ||
       cfg->memory_improve_summarise_enabled || cfg->memory_improve_min_cluster > 0 ||
       cfg->memory_improve_max_confidence > 0.0)
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
      if (!cfg->memory_profile_cards_enabled || cfg->memory_profile_cards_min_obs > 0 ||
          cfg->memory_profile_cards_stale_secs > 0)
      {
         cJSON *pc_cfg = cJSON_AddObjectToObject(memory, "profile_cards");
         cJSON_AddBoolToObject(pc_cfg, "enabled", cfg->memory_profile_cards_enabled ? 1 : 0);
         if (cfg->memory_profile_cards_min_obs > 0)
            cJSON_AddNumberToObject(pc_cfg, "min_observations", cfg->memory_profile_cards_min_obs);
         if (cfg->memory_profile_cards_stale_secs > 0)
            cJSON_AddNumberToObject(pc_cfg, "stale_secs", cfg->memory_profile_cards_stale_secs);
      }
      if (!cfg->memory_improve_dedupe_enabled || cfg->memory_improve_summarise_enabled ||
          cfg->memory_improve_min_cluster > 0 || cfg->memory_improve_max_confidence > 0.0)
      {
         cJSON *improve = cJSON_AddObjectToObject(memory, "improve");
         cJSON_AddBoolToObject(improve, "dedupe_enabled",
                               cfg->memory_improve_dedupe_enabled ? 1 : 0);
         cJSON_AddBoolToObject(improve, "summarise_enabled",
                               cfg->memory_improve_summarise_enabled ? 1 : 0);
         if (cfg->memory_improve_min_cluster > 0)
            cJSON_AddNumberToObject(improve, "min_cluster", cfg->memory_improve_min_cluster);
         if (cfg->memory_improve_max_confidence > 0.0)
            cJSON_AddNumberToObject(improve, "max_confidence", cfg->memory_improve_max_confidence);
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
      if (!cfg->memory_directives_enabled || cfg->memory_directives_failure_threshold > 0 ||
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
      int prov_nondefault = prov[0] && strcmp(prov, "shared") != 0;
      /* Promote to a {path, ...} object for ANY non-default field, so a
       * sandbox_image override on an otherwise-shared workspace is not dropped. */
      if (prov_nondefault || cfg->workspace_sandbox_image[i][0])
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "path", cfg->workspaces[i]);
         if (prov_nondefault)
            cJSON_AddStringToObject(entry, "provider", prov);
         if (cfg->workspace_vcs_remote[i][0])
            cJSON_AddStringToObject(entry, "remote", cfg->workspace_vcs_remote[i]);
         if (cfg->workspace_vcs_head[i][0])
            cJSON_AddStringToObject(entry, "head", cfg->workspace_vcs_head[i]);
         if (cfg->workspace_sandbox_image[i][0])
            cJSON_AddStringToObject(entry, "sandbox_image", cfg->workspace_sandbox_image[i]);
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
   if (cfg->delegate_graph_context_enabled)
      cJSON_AddBoolToObject(root, "delegate_graph_context_enabled", 1);
   if (cfg->ingress_preinject_enabled)
      cJSON_AddBoolToObject(root, "ingress_preinject_enabled", 1);
   if (strcmp(cfg->code_context_mode, "on") != 0)
      cJSON_AddStringToObject(root, "code_context_mode", cfg->code_context_mode);
   if (cfg->ingress_preinject_anthropic_enabled)
      cJSON_AddBoolToObject(root, "ingress_preinject_anthropic_enabled", 1);
   if (cfg->ingress_compress_enabled)
      cJSON_AddBoolToObject(root, "ingress_compress_enabled", 1);
   if (cfg->ingress_cache_placement_enabled)
      cJSON_AddBoolToObject(root, "ingress_cache_placement_enabled", 1);
   if (cfg->ingress_compress_min_chars != 80)
      cJSON_AddNumberToObject(root, "ingress_compress_min_chars", cfg->ingress_compress_min_chars);
   /* Written when FALSE, not when true: these three default ON, and the
    * surrounding convention of "emit only if set" would silently drop the OFF
    * state on save -- turning a deliberate disable back on at the next load. */
   if (!cfg->delegates_enabled)
      cJSON_AddBoolToObject(root, "delegates_enabled", 0);
   if (!cfg->prompt_manager_block_enabled)
      cJSON_AddBoolToObject(root, "prompt_manager_block_enabled", 0);
   if (!cfg->prompt_manager_review_enabled)
      cJSON_AddBoolToObject(root, "prompt_manager_review_enabled", 0);
   if (cfg->gateway_prevent_subagents)
      cJSON_AddBoolToObject(root, "gateway_prevent_subagents", 1);
   if (cfg->gateway_pin_model)
      cJSON_AddBoolToObject(root, "gateway_pin_model", 1);
   if (cfg->ingress_preinject_assembly_budget != 6144)
      cJSON_AddNumberToObject(root, "ingress_preinject_assembly_budget",
                              cfg->ingress_preinject_assembly_budget);
   if (cfg->code_span_max_lines != 400)
      cJSON_AddNumberToObject(root, "code_span_max_lines", cfg->code_span_max_lines);
   if (cfg->tool_output_max_bytes != 0)
      cJSON_AddNumberToObject(root, "tool_output_max_bytes", cfg->tool_output_max_bytes);
   if (cfg->ingress_max_raw_scans != 0)
      cJSON_AddNumberToObject(root, "ingress_max_raw_scans", cfg->ingress_max_raw_scans);
   /* Default-on: persist only the non-default (disabled) state, writing the real
    * value so an explicit opt-out survives save+reload. */
   if (!cfg->require_session_worktree)
      cJSON_AddBoolToObject(root, "require_session_worktree", 0);
   if (!cfg->require_aimee_memory) /* default-on: persist only the opt-out */
      cJSON_AddBoolToObject(root, "require_aimee_memory", 0);
   if (!cfg->require_aimee_git) /* default-on: persist only the opt-out */
      cJSON_AddBoolToObject(root, "require_aimee_git", 0);
   if (!cfg->subagent_ban_enabled) /* default-on: persist only the opt-out */
      cJSON_AddBoolToObject(root, "subagent_ban_enabled", 0);
   if (cfg->delegate_sandbox_image[0])
      cJSON_AddStringToObject(root, "delegate_sandbox_image", cfg->delegate_sandbox_image);
   /* Persist only when non-default ("proxy"); absence means the default. */
   if (cfg->delegate_sandbox_package_access[0] &&
       strcmp(cfg->delegate_sandbox_package_access, "proxy") != 0)
      cJSON_AddStringToObject(root, "delegate_sandbox_package_access",
                              cfg->delegate_sandbox_package_access);
   if (cfg->delegate_sandbox_require_isolation) /* default off: persist only the opt-in */
      cJSON_AddBoolToObject(root, "delegate_sandbox_require_isolation", 1);
   if (!cfg->delegate_sandbox_learn_packages) /* default on: persist only the opt-out */
      cJSON_AddBoolToObject(root, "delegate_sandbox_learn_packages", 0);
   /* typed_facts_enabled is KB-owned: persisted as kb.typed_facts.enabled by
    * config_save_kb_curator (still parsed at root for backward compat). */
   /* structured-PDF preset: persist the tier when non-default ("off"). Parse
    * applies it before the per-stage gates, so on reload it re-derives them. */
   if (cfg->kb_pdf_tier[0] && strcmp(cfg->kb_pdf_tier, "off") != 0)
      cJSON_AddStringToObject(root, "kb_pdf_tier", cfg->kb_pdf_tier);
   if (cfg->kb_pdf_ingest_enabled) /* default-off: persist only when enabled */
      cJSON_AddBoolToObject(root, "kb_pdf_ingest_enabled", 1);
   if (cfg->kb_pdf_vector_enabled) /* default-off: persist only when enabled */
      cJSON_AddBoolToObject(root, "kb_pdf_vector_enabled", 1);
   if (cfg->kb_pdf_tsr_enabled) /* default-off: persist only when enabled */
      cJSON_AddBoolToObject(root, "kb_pdf_tsr_enabled", 1);
   if (cfg->tsr_command[0])
      cJSON_AddStringToObject(root, "tsr_command", cfg->tsr_command);
   if (cfg->kb_pdf_assets_enabled) /* default-off: persist only when enabled */
      cJSON_AddBoolToObject(root, "kb_pdf_assets_enabled", 1);
   if (cfg->kb_pdf_blob_dir[0])
      cJSON_AddStringToObject(root, "kb_pdf_blob_dir", cfg->kb_pdf_blob_dir);
   if (cfg->kb_pdf_blob_recon_secs != 3600) /* persist only a non-default */
      cJSON_AddNumberToObject(root, "kb_pdf_blob_recon_secs", cfg->kb_pdf_blob_recon_secs);
   if (cfg->kb_pdf_blob_orphan_alarm_mb != 1024)
      cJSON_AddNumberToObject(root, "kb_pdf_blob_orphan_alarm_mb",
                              cfg->kb_pdf_blob_orphan_alarm_mb);
   if (cfg->kb_pdf_ocr_enabled) /* default-off: persist only when enabled */
      cJSON_AddBoolToObject(root, "kb_pdf_ocr_enabled", 1);
   if (cfg->ocr_command[0])
      cJSON_AddStringToObject(root, "ocr_command", cfg->ocr_command);
   if (!cfg->css_style_graph_enabled) /* default-on: persist only the opt-out */
      cJSON_AddBoolToObject(root, "css_style_graph_enabled", 0);
   if (!cfg->code_cochange_git_enabled) /* default-on: persist only the opt-out */
      cJSON_AddBoolToObject(root, "code_cochange_git_enabled", 0);
   if (cfg->wfe_live_forge_enabled) /* default-off: persist only the opt-in (enable) */
      cJSON_AddBoolToObject(root, "wfe_live_forge_enabled", 1);
   if (cfg->wfe_proposals_autoscan_enabled) /* default-off: persist only the opt-in */
      cJSON_AddBoolToObject(root, "wfe_proposals_autoscan_enabled", 1);
   if (!cfg->client_integrations_enabled) /* default-on: persist only the opt-out (disable) */
      cJSON_AddBoolToObject(root, "client_integrations_enabled", 0);
   if (!cfg->audit_action_enabled) /* default-on: persist only the opt-out (disable) */
      cJSON_AddBoolToObject(root, "audit_action_enabled", 0);
   if (cfg->audit_worm_enabled) /* default-off: persist only the opt-in (enable) */
      cJSON_AddBoolToObject(root, "audit_worm_enabled", 1);
   /* default-on render backend: persist only a non-default value (a custom command
    * OR an empty string to disable) so both round-trip; the default isn't written. */
   if (strcmp(cfg->css_render_command, CONFIG_DEFAULT_CSS_RENDER_COMMAND) != 0)
      cJSON_AddStringToObject(root, "css_render_command", cfg->css_render_command);
   /* Vault custody: default "file" — persist only a non-default selection, under a
    * nested "vault" object so the key round-trips as vault.custody. */
   {
      int want_custody = cfg->vault_custody[0] && strcmp(cfg->vault_custody, "file") != 0;
      int want_blob = cfg->vault_tpm2_blob_path[0] != '\0';
      int want_tcti = cfg->vault_tpm2_tcti[0] &&
                      strcmp(cfg->vault_tpm2_tcti, CONFIG_DEFAULT_VAULT_TPM2_TCTI) != 0;
      int want_nvidx = cfg->vault_tpm2_nv_index[0] &&
                       strcmp(cfg->vault_tpm2_nv_index, CONFIG_DEFAULT_VAULT_TPM2_NV_INDEX) != 0;
      if (want_custody || want_blob || want_tcti || want_nvidx)
      {
         cJSON *vault = cJSON_AddObjectToObject(root, "vault");
         if (vault)
         {
            if (want_custody)
               cJSON_AddStringToObject(vault, "custody", cfg->vault_custody);
            if (want_blob || want_tcti || want_nvidx)
            {
               cJSON *tpm2 = cJSON_AddObjectToObject(vault, "tpm2");
               if (tpm2)
               {
                  if (want_blob)
                     cJSON_AddStringToObject(tpm2, "blob_path", cfg->vault_tpm2_blob_path);
                  if (want_tcti)
                     cJSON_AddStringToObject(tpm2, "tcti", cfg->vault_tpm2_tcti);
                  if (want_nvidx)
                     cJSON_AddStringToObject(tpm2, "nv_index", cfg->vault_tpm2_nv_index);
               }
            }
         }
      }
   }
   if (cfg->kb_evidence_emit_enabled)
      cJSON_AddBoolToObject(root, "kb_evidence_emit_enabled", 1);
   if (cfg->fidelity_check_enabled)
      cJSON_AddBoolToObject(root, "fidelity_check_enabled", 1);

   /* Cross-verification — flat top-level keys (was the cross_verify:{enabled,...}
    * object; config_parse_cross_verify_section still reads the old object form for
    * back-compat). */
   if (cfg->cross_verify)
      cJSON_AddBoolToObject(root, "cross_verify", 1);
   if (cfg->verify_cmd[0])
      cJSON_AddStringToObject(root, "verify_cmd", cfg->verify_cmd);
   if (cfg->verify_role[0])
      cJSON_AddStringToObject(root, "verify_role", cfg->verify_role);
   if (cfg->verify_prompt[0])
      cJSON_AddStringToObject(root, "verify_prompt", cfg->verify_prompt);

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
   if (cfg->delegate_max_inflight)
      cJSON_AddNumberToObject(root, "delegate_max_inflight", cfg->delegate_max_inflight);

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

   config_save_concurrency(cfg, root);

   /* Tool result compaction (only save non-default values) */
   if (!cfg->compact_enabled || cfg->compact_threshold || cfg->compact_head_bytes ||
       cfg->compact_tail_bytes || cfg->compact_per_tool_count || cfg->compact_from_record ||
       !cfg->coord_closet_enabled || cfg->coord_closet_budget_bytes ||
       cfg->coord_closet_max_ratio_pct || cfg->coord_closet_denylist[0])
   {
      cJSON *cmpct = cJSON_AddObjectToObject(root, "compact");
      cJSON_AddBoolToObject(cmpct, "enabled", cfg->compact_enabled);
      if (cfg->compact_from_record) /* default-off: persist only the opt-in */
         cJSON_AddBoolToObject(cmpct, "from_record", cfg->compact_from_record);
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
      /* Coordinate Closet (fold §2), nested under "compact". Default-ON now, so the
       * block is emitted only to record a non-default (disabled, or tuned budget). */
      if (!cfg->coord_closet_enabled || cfg->coord_closet_budget_bytes ||
          cfg->coord_closet_max_ratio_pct || cfg->coord_closet_denylist[0])
      {
         cJSON *closet = cJSON_AddObjectToObject(cmpct, "coord_closet");
         cJSON_AddBoolToObject(closet, "enabled", cfg->coord_closet_enabled);
         if (cfg->coord_closet_budget_bytes)
            cJSON_AddNumberToObject(closet, "budget_bytes", cfg->coord_closet_budget_bytes);
         if (cfg->coord_closet_max_ratio_pct)
            cJSON_AddNumberToObject(closet, "max_ratio_pct", cfg->coord_closet_max_ratio_pct);
         if (cfg->coord_closet_denylist[0])
            cJSON_AddStringToObject(closet, "denylist", cfg->coord_closet_denylist);
      }
   }

   /* Rolling context fold (fold §1/§3/§4/§6, only save if non-default) */
   if (cfg->fold_enabled || cfg->fold_retained_msgs || cfg->fold_min_fold_msgs ||
       cfg->fold_excerpt_bytes || cfg->fold_register_enabled || cfg->fold_freeze_enabled ||
       cfg->fold_freeze_tail_cap_msgs || cfg->fold_recall_enabled || cfg->fold_recall_ttl_turns)
   {
      cJSON *fold = cJSON_AddObjectToObject(root, "fold");
      cJSON_AddBoolToObject(fold, "enabled", cfg->fold_enabled);
      if (cfg->fold_retained_msgs)
         cJSON_AddNumberToObject(fold, "retained_msgs", cfg->fold_retained_msgs);
      if (cfg->fold_min_fold_msgs)
         cJSON_AddNumberToObject(fold, "min_fold_msgs", cfg->fold_min_fold_msgs);
      if (cfg->fold_excerpt_bytes)
         cJSON_AddNumberToObject(fold, "excerpt_bytes", cfg->fold_excerpt_bytes);
      if (cfg->fold_register_enabled)
         cJSON_AddBoolToObject(fold, "register_enabled", cfg->fold_register_enabled);
      if (cfg->fold_freeze_enabled || cfg->fold_freeze_tail_cap_msgs)
      {
         cJSON *freeze = cJSON_AddObjectToObject(fold, "freeze");
         cJSON_AddBoolToObject(freeze, "enabled", cfg->fold_freeze_enabled);
         if (cfg->fold_freeze_tail_cap_msgs)
            cJSON_AddNumberToObject(freeze, "tail_cap_msgs", cfg->fold_freeze_tail_cap_msgs);
      }
      if (cfg->fold_recall_enabled || cfg->fold_recall_ttl_turns)
      {
         cJSON *recall = cJSON_AddObjectToObject(fold, "recall");
         cJSON_AddBoolToObject(recall, "enabled", cfg->fold_recall_enabled);
         if (cfg->fold_recall_ttl_turns)
            cJSON_AddNumberToObject(recall, "ttl_turns", cfg->fold_recall_ttl_turns);
         if (cfg->fold_recall_inject) /* default-off: persist only the opt-in */
            cJSON_AddBoolToObject(recall, "inject", cfg->fold_recall_inject);
      }
   }

   /* SAFE is the default. Persist either non-default choice explicitly. */
   if (cfg->economizer_mode != ECON_MODE_SAFE)
   {
      cJSON *econ = cJSON_AddObjectToObject(root, "economizer");
      if (econ)
         cJSON_AddStringToObject(econ, "mode", econ_mode_name(cfg->economizer_mode));
   }

   /* Autonomous-dev knobs — persist only non-defaults (defaults: skeptics 0, fanout off,
    * unit_retry 2, unit_max 16, ci_retry_max 2; caps: max_turns 300, max_wall 1800,
    * stale_abandon 3600, concurrency 8, auto_resume ON, max_resumes 50). */
   if (cfg->autonomy_skeptics != 0 || cfg->autonomy_fanout != 0 || cfg->autonomy_unit_retry != 2 ||
       cfg->autonomy_unit_max != 16 || cfg->autonomy_ci_retry_max != 2 ||
       cfg->autonomy_max_turns != 300 || cfg->autonomy_max_wall_secs != 1800 ||
       cfg->autonomy_stale_abandon_secs != 3600 || cfg->autonomy_concurrency != 8 ||
       cfg->autonomy_auto_resume_cap_parks != 1 || cfg->autonomy_max_resumes != 50)
   {
      cJSON *autonomy = cJSON_AddObjectToObject(root, "autonomy");
      if (cfg->autonomy_skeptics != 0)
         cJSON_AddNumberToObject(autonomy, "skeptics", cfg->autonomy_skeptics);
      if (cfg->autonomy_fanout != 0)
         cJSON_AddBoolToObject(autonomy, "fanout", cfg->autonomy_fanout);
      if (cfg->autonomy_unit_retry != 2)
         cJSON_AddNumberToObject(autonomy, "unit_retry", cfg->autonomy_unit_retry);
      if (cfg->autonomy_unit_max != 16)
         cJSON_AddNumberToObject(autonomy, "unit_max", cfg->autonomy_unit_max);
      if (cfg->autonomy_ci_retry_max != 2)
         cJSON_AddNumberToObject(autonomy, "ci_retry_max", cfg->autonomy_ci_retry_max);
      if (cfg->autonomy_max_turns != 300)
         cJSON_AddNumberToObject(autonomy, "max_turns", cfg->autonomy_max_turns);
      if (cfg->autonomy_max_wall_secs != 1800)
         cJSON_AddNumberToObject(autonomy, "max_wall_secs", cfg->autonomy_max_wall_secs);
      if (cfg->autonomy_stale_abandon_secs != 3600)
         cJSON_AddNumberToObject(autonomy, "stale_abandon_secs", cfg->autonomy_stale_abandon_secs);
      if (cfg->autonomy_concurrency != 8)
         cJSON_AddNumberToObject(autonomy, "concurrency", cfg->autonomy_concurrency);
      if (cfg->autonomy_auto_resume_cap_parks != 1)
         cJSON_AddBoolToObject(autonomy, "auto_resume_cap_parks",
                               cfg->autonomy_auto_resume_cap_parks);
      if (cfg->autonomy_max_resumes != 50)
         cJSON_AddNumberToObject(autonomy, "max_resumes", cfg->autonomy_max_resumes);
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

   /* Sandbox config (only save if non-default). The default is now
    * SANDBOX_MODE_WORKSPACE_ONLY, so the value that MUST survive a save is the
    * explicit opt-out (`mode: "off"`) — mirroring require_aimee_git above, which
    * persists only its opt-out. Testing against SANDBOX_MODE_OFF here (the old
    * predicate) would drop an operator's "off" on the next save and silently
    * re-enable the sandbox from the default. */
   if (cfg->sandbox.mode != SANDBOX_MODE_WORKSPACE_ONLY || cfg->sandbox.network_isolated ||
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
         if (client->install == CONFIG_MCP_INSTALL_KB)
            cJSON_AddStringToObject(entry, "install", "kb"); /* omit "server" (the default) */

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
   if (cfg->server_api_http_port > 0 || cfg->server_api_tls_port > 0 || cfg->server_api_mtls > 0 ||
       cfg->server_api_mtls_client_ca[0] || cfg->server_api_rate_limit_per_min > 0 ||
       cfg->server_api_client_transport[0] ||
       cfg->server_api_remote_writes > SERVER_REMOTE_WRITES_OFF ||
       cfg->server_api_max_event_streams > 0 || cfg->server_api_cli_session_forwarding)
   {
      cJSON *aimee_obj = cJSON_AddObjectToObject(root, "aimee");
      cJSON *api_obj = cJSON_AddObjectToObject(aimee_obj, "api");
      if (cfg->server_api_http_port > 0)
         cJSON_AddNumberToObject(api_obj, "http_port", cfg->server_api_http_port);
      if (cfg->server_api_tls_port > 0)
         cJSON_AddNumberToObject(api_obj, "tls_port", cfg->server_api_tls_port);
      if (cfg->server_api_mtls > 0)
         cJSON_AddStringToObject(api_obj, "mtls",
                                 cfg->server_api_mtls >= 2 ? "required" : "optional");
      if (cfg->server_api_mtls_client_ca[0])
         cJSON_AddStringToObject(api_obj, "mtls_client_ca", cfg->server_api_mtls_client_ca);
      if (cfg->server_api_rate_limit_per_min > 0)
         cJSON_AddNumberToObject(api_obj, "rate_limit_per_min", cfg->server_api_rate_limit_per_min);
      if (cfg->server_api_max_event_streams > 0)
         cJSON_AddNumberToObject(api_obj, "max_event_streams", cfg->server_api_max_event_streams);
      if (cfg->server_api_cli_session_forwarding)
         cJSON_AddBoolToObject(api_obj, "cli_session_forwarding", 1);
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
         g_config_size = st.st_size;
         g_config_ino = st.st_ino;
         snprintf(g_config_cache_path, sizeof(g_config_cache_path), "%s", path);
         g_config_cached = 1;
      }
   }

   return 0;
}

/* --- Guardrail mode --- */

/* Defined in config.c; declared here the same way the generated accessor shards
 * declare it, since it has no public header (it is a config-module internal). */
int config_field_read(size_t offset, size_t size, void *dst);

/* Reads the field directly rather than through a generated accessor: the
 * generator skips a field whose accessor name is already taken, and this
 * function occupies config_guardrail_mode. That is fine here — this IS the
 * config module, which is the one place allowed to know config_t's shape. */
const char *config_guardrail_mode(void)
{
   static _Thread_local char buf[sizeof(((config_t *)0)->guardrail_mode)];
   buf[0] = 0;
   config_field_read(offsetof(config_t, guardrail_mode), sizeof(buf), buf);
   buf[sizeof(buf) - 1] = 0;
   return buf[0] ? buf : MODE_APPROVE;
}

const char *config_embedder_command(const config_t *cfg, const char *requested)
{
   if (requested && requested[0])
      return requested;
   /* This is the ONE place an embedder address is resolved. Precedence, and why:
    *
    * EMBEDDER_URL OUTRANKS the stored command, because the env var is how the
    * RUNNING embedder announces itself. The shipped config pre-selects nothing (first
    * boot leaves the choice to the wizard), and when the entrypoint does start the
    * bundled in-container model it exports EMBEDDER_URL pointing at it — so the
    * bundled model and an operator's external endpoint arrive by the same route and
    * obey one rule. Checking config first would make the variable dead and let the kb
    * embed locally while its schema was sized for the external endpoint.
    *
    * memory_embed_text speaks http:// directly, so this costs no fork and no python.
    *
    * Deliberately NOT falling back to SYNTHESIS_ENDPOINT: that knob is synthesis-only since
    * the aimee-llm container was retired, and letting it select an embedder would point
    * retrieval at a chat endpoint. */
   const char *env = getenv("EMBEDDER_URL");
   if (env && env[0])
      return env;
   if (cfg && cfg->embedder_command[0])
      return cfg->embedder_command;
   /* Nothing selected at all. The honest answer is the empty string, and callers read
    * it as "no embedder": memory_embed_text embeds nothing, the kb refuses to start,
    * and a deploy is rejected before a container runs.
    *
    * This used to return "builtin", naming a lexical feature hash that served whenever
    * nothing was configured. Returning a name for it now would be worse than the
    * fallback ever was: nothing implements it, so the string would reach the sidecar
    * exec path, fork `/bin/sh -c builtin` for every embed call, fail, and charge the
    * dependency breaker for an endpoint that was never configured. */
   return "";
}

/* --- Conversation directories --- */

int config_conversation_dirs(char dirs[][MAX_PATH_LEN], int max_dirs)
{
   const char *home = platform_home_dir();
   if (!home)
      home = "/tmp";

   const char *provider = config_provider()[0] ? config_provider() : "claude";
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

/* ---- Surgical, key-addressed config write (Proposal B write side) ----------
 * config_set is the single save path. It edits the config YAML as a *document*
 * — load, set one key, write back, republish — never re-serialising config_t.
 * There is no whole-file rebuild, so parse and save cannot drift, and a write
 * preserves every other key already in the file. */

static cJSON *config_set_value_node(const config_field_t *f, const char *value)
{
   if (f->is_bool || f->type == CFG_BOOL)
   {
      if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
         return cJSON_CreateBool(1);
      if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
         return cJSON_CreateBool(0);
      return NULL; /* not a boolean */
   }
   if (f->type == CFG_INT)
      return cJSON_CreateNumber(atoi(value));
   if (f->type == CFG_FLOAT)
      return cJSON_CreateNumber(atof(value));
   if (f->type == CFG_ECON_MODE)
   {
      if (econ_mode_parse(value) < 0)
         return NULL; /* not off|safe|aggressive */
      return cJSON_CreateString(value);
   }
   return cJSON_CreateString(value);
}

/* Set a possibly-dotted key (a.b.c) in the doc to node (takes ownership). */
static void config_doc_set(cJSON *root, const char *key, cJSON *node)
{
   const char *dot = strchr(key, '.');
   if (!dot)
   {
      cJSON_DeleteItemFromObjectCaseSensitive(root, key);
      cJSON_AddItemToObject(root, key, node);
      return;
   }
   char head[128];
   size_t n = (size_t)(dot - key);
   if (n >= sizeof(head))
      n = sizeof(head) - 1;
   memcpy(head, key, n);
   head[n] = '\0';
   cJSON *sub = cJSON_GetObjectItemCaseSensitive(root, head);
   if (!cJSON_IsObject(sub))
   {
      cJSON_DeleteItemFromObjectCaseSensitive(root, head);
      sub = cJSON_CreateObject();
      cJSON_AddItemToObject(root, head, sub);
   }
   config_doc_set(sub, dot + 1, node);
}

static cJSON *config_load_doc(void)
{
   const char *path = config_default_path();
   FILE *fp = fopen(path, "r");
   if (!fp)
      return cJSON_CreateObject();
   fseek(fp, 0, SEEK_END);
   long len = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (len <= 0 || len > 4 * 1024 * 1024)
   {
      fclose(fp);
      return cJSON_CreateObject();
   }
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(fp);
      return cJSON_CreateObject();
   }
   size_t rd = fread(buf, 1, (size_t)len, fp);
   fclose(fp);
   buf[rd] = '\0';
   cJSON *root = yaml_parse(buf);
   free(buf);
   return root ? root : cJSON_CreateObject();
}

static int config_write_doc(cJSON *root)
{
   char *yaml_str = yaml_emit(root);
   if (!yaml_str)
      return -1;
   const char *path = config_default_path();
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
   FILE *fp = fopen(tmp, "w");
   if (!fp)
   {
      free(yaml_str);
      return -1;
   }
   fputs(yaml_str, fp);
   fclose(fp);
   free(yaml_str);
   chmod(tmp, 0600);
   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

/* The embedder names an image can actually serve. Empty embedder_url means the
 * weights BAKED INTO the image variant, so the name has to be one of them; a
 * non-empty URL means an operator-run endpoint whose model may be called anything
 * (see config.h's embedder_model comment).
 *
 * Unvalidated, a typo produced a deployment that looked configured and searched
 * lexically forever: config_emit_deploy_env maps any unrecognised name to the a25m
 * variant, so the image arrives WITH bekko weights and EMBEDDER_MODEL=<typo>, the
 * entrypoint finds no match, logs "'<typo>' selected but this image has no bundled
 * embedder" once, and starts nothing. Everything downstream reports healthy. */
static int embedder_model_is_bundled(const char *v)
{
   return v && (strcmp(v, "bekko-a25m") == 0 || strcmp(v, "nomic-embed-text-v2-moe") == 0);
}

int config_set(const char *key, const char *value)
{
   if (!key || !value)
      return -1;
   const config_field_t *f = config_field_lookup(key);
   if (!f)
      return -1; /* unknown key */

   /* Refused rather than warned: the failure it prevents is silent and permanent
    * once a corpus has been embedded at the wrong width, and re-running the command
    * in the other order costs nothing. Clearing the field is allowed -- that is how
    * an operator moves to an external embedder. */
   if (strcmp(key, "embedder_model") == 0 && value[0] && !embedder_model_is_bundled(value))
   {
      char url[512] = "";
      config_embedder_url_copy(url, sizeof(url));
      if (!url[0])
      {
         fprintf(stderr,
                 "config: '%s' is not an embedder this image bakes (bekko-a25m or "
                 "nomic-embed-text-v2-moe).\n"
                 "  A name the image does not carry deploys with no embedder running and "
                 "searches lexically, reporting healthy throughout.\n"
                 "  If this is an external endpoint's model name, set embedder_url first.\n",
                 value);
         return -1;
      }
   }
   const char *secret_name = config_field_secret_name(f);
   if (secret_name)
   {
      /* Credential compatibility fields are process-memory views of Vault
       * records, never YAML settings. The injected writer updates Vault and the
       * locked runtime cache atomically; absent initialization fails closed. */
      return config_secret_store(secret_name, value);
   }
   cJSON *node = config_set_value_node(f, value);
   if (!node)
      return -1; /* invalid value for the field's type */
   ensure_config_dir();
   cJSON *root = config_load_doc();
   config_doc_set(root, key, node);
   int rc = config_write_doc(root);
   cJSON_Delete(root);
   if (rc == 0)
      (void)config_reload(); /* republish the snapshot so live readers see it */
   return rc;
}

/* Surgical write of a whole config *section*: rebuild just that section's subtree
 * in the document from cfg (via its serializer), preserving every other key.
 * The structured counterpart to config_set — for arrays / nested objects (concurrency,
 * workspaces, ...) that a flat scalar config_set cannot express. No whole-file rebuild. */
static int config_set_section(const char *key, void (*emit)(const config_t *, cJSON *),
                              const config_t *cfg)
{
   if (!key || !emit || !cfg)
      return -1;
   ensure_config_dir();
   cJSON *root = config_load_doc();
   cJSON_DeleteItemFromObjectCaseSensitive(root, key);
   emit(cfg, root);
   int rc = config_write_doc(root);
   cJSON_Delete(root);
   if (rc == 0)
      (void)config_reload();
   return rc;
}

/* Apply the KB typed-facts group in ONE document write. Each parameter is
 * "leave unchanged" when negative, so a caller can patch any subset. Grouped
 * rather than three config_set calls because the console applies them as one
 * request: three separate writes would be three reload publishes and could
 * leave the layer half-configured if one failed. */
int config_set_typed_facts(int enabled, int auto_promote, int promote_threshold)
{
   int rc = 0;
   if (enabled >= 0)
      rc = config_set("typed_facts_enabled", enabled ? "true" : "false");
   if (rc == 0 && auto_promote >= 0)
      rc = config_set("kb_typed_facts_auto_promote_enabled", auto_promote ? "true" : "false");
   if (rc == 0 && promote_threshold > 0)
   {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d", promote_threshold);
      rc = config_set("kb_typed_facts_promote_threshold", buf);
   }
   return rc;
}

/* Register a workspace. The caller supplies the data; the 64-entry cap, the
 * duplicate check and the four parallel arrays are config's business, not a
 * caller's. Returns 0 on success, -1 on save failure, -2 when `path` is already
 * registered, -3 when the table is full. `provider`/`remote`/`head` may be NULL
 * or "" for the defaults. */
int config_workspace_add(const char *path, const char *provider, const char *remote,
                         const char *head)
{
   if (!path || !path[0])
      return -1;
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = -1;
   if (config_load(cfg) == 0)
   {
      rc = 0;
      for (int i = 0; i < cfg->workspace_count; i++)
         if (strcmp(cfg->workspaces[i], path) == 0)
            rc = -2;
      int cap = (int)(sizeof(cfg->workspaces) / sizeof(cfg->workspaces[0]));
      /* PRUNE THE DEAD BEFORE REFUSING. A workspace whose directory no longer
       * exists cannot be used for anything: it is a corpse holding a slot. The
       * registry had no removal path other than an explicit `workspace remove`,
       * so any workflow that creates short-lived checkouts -- CI, benchmark
       * cells, ephemeral worktrees -- eventually filled all 64 and then every
       * `workspace add` failed with "maximum workspace count reached", on a
       * machine where none of the 64 still existed on disk.
       *
       * Only entries whose path is gone are dropped, so a workspace on an
       * unmounted volume is NOT collected -- stat failing for any reason other
       * than ENOENT leaves the entry alone. */
      if (rc == 0 && cfg->workspace_count >= cap)
      {
         int kept = 0;
         for (int i = 0; i < cfg->workspace_count; i++)
         {
            struct stat st;
            if (stat(cfg->workspaces[i], &st) != 0 && errno == ENOENT)
               continue; /* gone: drop it */
            if (kept != i)
            {
               memcpy(cfg->workspaces[kept], cfg->workspaces[i], sizeof(cfg->workspaces[0]));
               memcpy(cfg->workspace_providers[kept], cfg->workspace_providers[i],
                      sizeof(cfg->workspace_providers[0]));
               memcpy(cfg->workspace_vcs_remote[kept], cfg->workspace_vcs_remote[i],
                      sizeof(cfg->workspace_vcs_remote[0]));
               memcpy(cfg->workspace_vcs_head[kept], cfg->workspace_vcs_head[i],
                      sizeof(cfg->workspace_vcs_head[0]));
            }
            kept++;
         }
         if (kept < cfg->workspace_count)
            aimee_log(LOG_INFO, "workspace", "pruned %d registered workspace(s) whose path is gone",
                      cfg->workspace_count - kept);
         cfg->workspace_count = kept;
      }
      if (rc == 0 && cfg->workspace_count >= cap)
         rc = -3;
      if (rc == 0)
      {
         int idx = cfg->workspace_count++;
         snprintf(cfg->workspaces[idx], sizeof(cfg->workspaces[idx]), "%s", path);
         snprintf(cfg->workspace_providers[idx], sizeof(cfg->workspace_providers[idx]), "%s",
                  (provider && strcmp(provider, "shared") != 0) ? provider : "");
         snprintf(cfg->workspace_vcs_remote[idx], sizeof(cfg->workspace_vcs_remote[idx]), "%s",
                  remote ? remote : "");
         snprintf(cfg->workspace_vcs_head[idx], sizeof(cfg->workspace_vcs_head[idx]), "%s",
                  head ? head : "");
         rc = config_save(cfg);
      }
      else if (rc == -2)
      {
         /* Already registered: REFRESH the coordinates the caller supplied.
          *
          * These are not static properties of a path — a mirror workspace's head
          * is whichever commit the client's patch applies to, and it moves every
          * time the developer commits or pushes. Leaving the first value frozen
          * meant a re-attaching client shipped a patch against one commit while
          * the server still checked out another, and `git apply` failed on a
          * tree that had been fine at first attach.
          *
          * Only non-NULL arguments are written, so a caller that supplies just a
          * head (workspace.mirror-sync) does not erase the remote. Still returns
          * -2, which callers treat as idempotent success. */
         for (int i = 0; i < cfg->workspace_count; i++)
         {
            if (strcmp(cfg->workspaces[i], path) != 0)
               continue;
            if (provider)
               snprintf(cfg->workspace_providers[i], sizeof(cfg->workspace_providers[i]), "%s",
                        strcmp(provider, "shared") != 0 ? provider : "");
            if (remote)
               snprintf(cfg->workspace_vcs_remote[i], sizeof(cfg->workspace_vcs_remote[i]), "%s",
                        remote);
            if (head)
               snprintf(cfg->workspace_vcs_head[i], sizeof(cfg->workspace_vcs_head[i]), "%s", head);
            if (provider || remote || head)
               (void)config_save(cfg);
            break;
         }
      }
   }
   free(cfg);
   return rc;
}

/* Remove a workspace by path, closing the gap in the parallel arrays. Returns 0,
 * -1 on save failure, -2 when the path is not registered. Counterpart to
 * config_workspace_add: the array shuffle is config's business. */
int config_workspace_remove(const char *path)
{
   if (!path || !path[0])
      return -1;
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = -1;
   if (config_load(cfg) == 0)
   {
      int found = -1;
      for (int i = 0; i < cfg->workspace_count; i++)
         if (strcmp(cfg->workspaces[i], path) == 0)
         {
            found = i;
            break;
         }
      if (found < 0)
         rc = -2;
      else
      {
         for (int i = found; i < cfg->workspace_count - 1; i++)
         {
            snprintf(cfg->workspaces[i], sizeof(cfg->workspaces[i]), "%s", cfg->workspaces[i + 1]);
            snprintf(cfg->workspace_providers[i], sizeof(cfg->workspace_providers[i]), "%s",
                     cfg->workspace_providers[i + 1]);
            snprintf(cfg->workspace_vcs_remote[i], sizeof(cfg->workspace_vcs_remote[i]), "%s",
                     cfg->workspace_vcs_remote[i + 1]);
            snprintf(cfg->workspace_vcs_head[i], sizeof(cfg->workspace_vcs_head[i]), "%s",
                     cfg->workspace_vcs_head[i + 1]);
         }
         cfg->workspace_count--;
         rc = config_save(cfg);
      }
   }
   free(cfg);
   return rc;
}

/* Enable the /v1 HTTP listener and persist both settings atomically, reading the
 * FILE rather than the live snapshot.
 *
 * The generated setters go through config_load, which in the SERVER returns the
 * published snapshot. Calling the port and rate-limit setters back-to-back made
 * the second save start from the same stale snapshot and overwrite the port the
 * first save had just written. Reading the file once also preserves edits made
 * since the last publish. */
int config_set_api_http_listener(int http_port, int rate_limit_per_min)
{
   if (http_port <= 0 || rate_limit_per_min <= 0)
      return -1;
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = config_load_file(cfg);
   if (rc == 0)
   {
      cfg->server_api_http_port = http_port;
      cfg->server_api_rate_limit_per_min = rate_limit_per_min;
      rc = config_save(cfg);
      if (rc == 0 && config_reload() < 0)
         rc = -1;
   }
   free(cfg);
   return rc;
}

/* Disable the /v1 HTTP listener and persist, reading the FILE rather than the
 * live snapshot for the same stale-write reason as the enable path above. */
int config_disable_api_http_listener(void)
{
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = config_load_file(cfg);
   if (rc == 0)
   {
      cfg->server_api_http_port = 0;
      rc = config_save(cfg);
   }
   free(cfg);
   return rc;
}

/* Materialise the config file: load (defaults when absent) and write it back.
 * Idempotent, and the one thing `aimee init` / `aimee setup` actually wanted
 * from their load-then-save round trip. */
int config_persist_defaults(void)
{
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = config_load(cfg);
   if (rc == 0)
      rc = config_save(cfg);
   free(cfg);
   return rc;
}

int config_apply_roundtable_preset(const config_roundtable_preset_t *p)
{
   if (!p)
      return -1;
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = -1;
   /* From DISK, not the snapshot, so applying a preset never clobbers an
    * external edit made since the last reload (matches config_set). */
   if (config_load_file(cfg) == 0)
   {
      int n = p->seat_count;
      if (n > CONFIG_RT_PRESET_MAX_SEATS)
         n = CONFIG_RT_PRESET_MAX_SEATS;
      for (int i = 0; i < n; i++)
      {
         snprintf(cfg->ensemble_reference_models[i], sizeof(cfg->ensemble_reference_models[i]),
                  "%s", p->models[i]);
         snprintf(cfg->ensemble_reference_personas[i], sizeof(cfg->ensemble_reference_personas[i]),
                  "%s", p->personas[i]);
      }
      cfg->ensemble_reference_count = n;
      cfg->ensemble_reference_persona_count = n;
      cfg->ensemble_min_successful = p->min_successful;
      cfg->ensemble_max_cost_usd = p->max_cost_usd;
      cfg->roundtable_max_rounds = p->max_rounds;
      cfg->roundtable_converge_threshold = p->converge_threshold;
      cfg->roundtable_deadline_ms = p->deadline_ms;
      if (p->turns[0])
         snprintf(cfg->roundtable_turns, sizeof(cfg->roundtable_turns), "%s", p->turns);
      if (p->pipeline_done_bar[0])
         snprintf(cfg->roundtable_pipeline_done_bar, sizeof(cfg->roundtable_pipeline_done_bar),
                  "%s", p->pipeline_done_bar);
      cfg->roundtable_pipeline_max_passes = p->pipeline_max_passes;
      cfg->roundtable_pipeline_max_attempts_per_pass = p->pipeline_max_attempts_per_pass;
      cfg->roundtable_pipeline_max_cost_usd = p->pipeline_max_cost_usd;
      cfg->roundtable_pipeline_max_total_cost_usd = p->pipeline_max_total_cost_usd;
      cfg->roundtable_pipeline_gate_ttl_h = p->pipeline_gate_ttl_h;
      cfg->roundtable_pipeline_parked_releases_slot = p->pipeline_parked_releases_slot;
      cfg->roundtable_pipeline_unknown_context_tokens = p->pipeline_unknown_context_tokens;
      snprintf(cfg->roundtable_default, sizeof(cfg->roundtable_default), "%s", p->name);
      rc = config_save(cfg);
      if (rc == 0)
         (void)config_reload();
   }
   free(cfg);
   return rc;
}

/* Upsert a per-model concurrency limit and persist the concurrency section.
 * Returns 0, -1 on failure, -2 when the table is full. The table layout and its
 * cap are config's business, not a caller's. */
int config_set_model_concurrency(const char *model, int limit)
{
   if (!model || !model[0] || limit <= 0)
      return -1;
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = -1;
   if (config_load(cfg) == 0)
   {
      int found = -1;
      for (int i = 0; i < cfg->concurrency_per_model_count; i++)
         if (strcmp(cfg->concurrency_per_model[i].key, model) == 0)
         {
            found = i;
            break;
         }
      if (found >= 0)
         cfg->concurrency_per_model[found].limit = limit;
      else if (cfg->concurrency_per_model_count >= CONFIG_CONCURRENCY_MAX_ENTRIES)
         rc = -2;
      else
      {
         config_concurrency_entry_t *e =
             &cfg->concurrency_per_model[cfg->concurrency_per_model_count++];
         snprintf(e->key, sizeof(e->key), "%s", model);
         e->limit = limit;
      }
      if (rc != -2)
         rc = config_set_concurrency(cfg);
   }
   free(cfg);
   return rc;
}

/* Drop a per-model concurrency entry, closing the gap. 0 = removed or absent. */
int config_remove_model_concurrency(const char *model)
{
   if (!model || !model[0])
      return -1;
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = -1;
   if (config_load(cfg) == 0)
   {
      int found = -1;
      for (int i = 0; i < cfg->concurrency_per_model_count; i++)
         if (strcmp(cfg->concurrency_per_model[i].key, model) == 0)
         {
            found = i;
            break;
         }
      if (found < 0)
         rc = 0;
      else
      {
         for (int i = found; i < cfg->concurrency_per_model_count - 1; i++)
            cfg->concurrency_per_model[i] = cfg->concurrency_per_model[i + 1];
         cfg->concurrency_per_model_count--;
         rc = config_set_concurrency(cfg);
      }
   }
   free(cfg);
   return rc;
}

int config_set_concurrency(const config_t *cfg)
{
   return config_set_section("concurrency", config_save_concurrency, cfg);
}

/* Copy out the sandbox block whole. The one place a caller legitimately wants a
 * struct rather than a field: sandbox_config_t is a self-contained POD that
 * sandbox_* consumes as a unit, so per-field accessors would just be reassembled
 * at every call site. Leaves *out zeroed when the config cannot be read, which is
 * the all-defaults-off shape callers already used on load failure. */
void config_sandbox(sandbox_config_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   /* Read the LIVE config, like every generated accessor: config_field_read
    * prefers the pinned snapshot and heap-loads only when none is live. Loading a
    * whole config_t here instead made this the one read accessor that bypassed the
    * snapshot -- so a delegated shell could be gated on file state the live
    * snapshot had not adopted, and every such call paid a full config load on a
    * hot path. Leaves *out zeroed when the config cannot be read, which is the
    * all-defaults-off shape callers already relied on. */
   config_field_read(offsetof(config_t, sandbox), sizeof(*out), out);
}
