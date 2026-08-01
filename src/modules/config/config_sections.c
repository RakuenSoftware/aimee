/* config_sections.c: aimee.yaml section parsers extracted from config_load
 * (see config_sections.h). Behavior-preserving verbatim moves. */
#include "aimee.h"
#include "json_fluent.h"
#include "config_internal.h"
#include "config_sections.h"
#include "economizer.h"
#include "sandbox.h"
#include "toolset.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

void config_parse_memory_rewrite_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *mem_rewrite = cJSON_GetObjectItemCaseSensitive(root, "memory_rewrite");
   if (cJSON_IsObject(mem_rewrite))
   {
      item = cJSON_GetObjectItemCaseSensitive(mem_rewrite, "enabled");
      if (cJSON_IsBool(item))
         cfg->memory_rewrite_enabled = cJSON_IsTrue(item);
      item = cJSON_GetObjectItemCaseSensitive(mem_rewrite, "hyde");
      if (cJSON_IsBool(item))
         cfg->memory_rewrite_hyde = cJSON_IsTrue(item);
      item = cJSON_GetObjectItemCaseSensitive(mem_rewrite, "decompose");
      if (cJSON_IsBool(item))
         cfg->memory_rewrite_decompose = cJSON_IsTrue(item);
      item = cJSON_GetObjectItemCaseSensitive(mem_rewrite, "max_subqueries");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->memory_rewrite_max_subqueries = (int)item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(mem_rewrite, "command");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->memory_rewrite_command, sizeof(cfg->memory_rewrite_command), "%s",
                  item->valuestring);
   }
}
void config_parse_memory_negation_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *mem_negation = cJSON_GetObjectItemCaseSensitive(root, "memory_negation");
   if (cJSON_IsObject(mem_negation))
   {
      item = cJSON_GetObjectItemCaseSensitive(mem_negation, "enabled");
      if (cJSON_IsBool(item))
         cfg->memory_negation_enabled = cJSON_IsTrue(item) ? 1 : 0;
   }
}
void config_parse_memory_query_expansion_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *mem_qexp = cJSON_GetObjectItemCaseSensitive(root, "memory_query_expansion");
   if (cJSON_IsObject(mem_qexp))
   {
      item = cJSON_GetObjectItemCaseSensitive(mem_qexp, "mode");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->memory_query_expansion_mode, sizeof(cfg->memory_query_expansion_mode), "%s",
                  item->valuestring);
      item = cJSON_GetObjectItemCaseSensitive(mem_qexp, "k");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->memory_query_expansion_k = (int)item->valuedouble;
   }
}
void config_parse_memory_recall_lanes_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *mem_lanes = cJSON_GetObjectItemCaseSensitive(root, "memory_recall_lanes");
   if (cJSON_IsObject(mem_lanes))
   {
      item = cJSON_GetObjectItemCaseSensitive(mem_lanes, "enabled");
      if (cJSON_IsBool(item))
         cfg->memory_recall_lanes_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(mem_lanes, "summary_kinds");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->memory_recall_lanes_summary_kinds,
                  sizeof(cfg->memory_recall_lanes_summary_kinds), "%s", item->valuestring);
      item = cJSON_GetObjectItemCaseSensitive(mem_lanes, "fact_kinds");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->memory_recall_lanes_fact_kinds, sizeof(cfg->memory_recall_lanes_fact_kinds),
                  "%s", item->valuestring);
      item = cJSON_GetObjectItemCaseSensitive(mem_lanes, "k_summary");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->memory_recall_lanes_k_summary = (int)item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(mem_lanes, "k_fact");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->memory_recall_lanes_k_fact = (int)item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(mem_lanes, "floor_summary");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0)
         cfg->memory_recall_lanes_floor_summary = (int)item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(mem_lanes, "floor_fact");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0)
         cfg->memory_recall_lanes_floor_fact = (int)item->valuedouble;
   }
}
void config_parse_memory_window_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *mem_window = cJSON_GetObjectItemCaseSensitive(root, "memory_window");
   if (cJSON_IsObject(mem_window))
   {
      item = cJSON_GetObjectItemCaseSensitive(mem_window, "radius");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0)
         cfg->memory_window_radius = (int)item->valuedouble;
   }
}
void config_parse_kb_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *kb_cfg = cJSON_GetObjectItemCaseSensitive(root, "kb");
   if (cJSON_IsObject(kb_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(kb_cfg, "search_max_results");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->kb_search_max_results = (int)item->valuedouble;
   }
}
void config_parse_memory_maintenance_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *mem_maint = cJSON_GetObjectItemCaseSensitive(root, "memory_maintenance");
   if (cJSON_IsObject(mem_maint))
   {
      item = cJSON_GetObjectItemCaseSensitive(mem_maint, "trigger_inserts");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0)
         cfg->memory_maintenance_trigger_inserts = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(mem_maint, "trigger_secs");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0)
         cfg->memory_maintenance_trigger_secs = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(mem_maint, "enabled");
      if (cJSON_IsBool(item))
         cfg->memory_maintenance_enabled = cJSON_IsTrue(item) ? 1 : 0;

      item = cJSON_GetObjectItemCaseSensitive(mem_maint, "interval_seconds");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0)
         cfg->memory_maintenance_interval_seconds = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(mem_maint, "summarize_enabled");
      if (cJSON_IsBool(item))
         cfg->memory_maintenance_summarize_enabled = cJSON_IsTrue(item) ? 1 : 0;
   }
}
void config_parse_worktree_gc_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *wt = cJSON_GetObjectItemCaseSensitive(root, "worktree_gc");
   if (cJSON_IsObject(wt))
   {
      item = cJSON_GetObjectItemCaseSensitive(wt, "enabled");
      if (cJSON_IsBool(item))
         cfg->worktree_gc_enabled = cJSON_IsTrue(item) ? 1 : 0;

      item = cJSON_GetObjectItemCaseSensitive(wt, "max_age_days");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->worktree_gc_max_age_days = (int)item->valuedouble;
   }
}
void config_parse_memory_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *memory_cfg = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (cJSON_IsObject(memory_cfg))
   {
      cJSON *salience_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "salience");
      if (cJSON_IsObject(salience_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(salience_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_salience_enabled = cJSON_IsTrue(item) ? 1 : 0;

         item = cJSON_GetObjectItemCaseSensitive(salience_cfg, "weight");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
            cfg->memory_salience_weight = item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(salience_cfg, "window_size");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->memory_salience_window_size = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(salience_cfg, "surprise_enabled");
         if (cJSON_IsBool(item))
            cfg->memory_surprise_enabled = cJSON_IsTrue(item) ? 1 : 0;

         item = cJSON_GetObjectItemCaseSensitive(salience_cfg, "surprise_weight");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
            cfg->memory_surprise_weight = item->valuedouble;
      }

      cJSON *pagerank_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "pagerank");
      if (cJSON_IsObject(pagerank_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(pagerank_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_pagerank_enabled = cJSON_IsTrue(item) ? 1 : 0;

         item = cJSON_GetObjectItemCaseSensitive(pagerank_cfg, "iterations");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->memory_pagerank_iterations = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(pagerank_cfg, "weight");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
            cfg->memory_pagerank_weight = item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(pagerank_cfg, "relations");
         if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
            snprintf(cfg->memory_pagerank_relations, sizeof(cfg->memory_pagerank_relations), "%s",
                     item->valuestring);
      }

      cJSON *coref_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "coref");
      if (cJSON_IsObject(coref_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(coref_cfg, "mode");
         if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
            snprintf(cfg->memory_coref_mode, sizeof(cfg->memory_coref_mode), "%s",
                     item->valuestring);

         item = cJSON_GetObjectItemCaseSensitive(coref_cfg, "window");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->memory_coref_window = (int)item->valuedouble;
      }

      cJSON *citations_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "citations");
      if (cJSON_IsObject(citations_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(citations_cfg, "mode");
         if (cJSON_IsString(item) && item->valuestring[0])
            snprintf(cfg->memory_citations_mode, sizeof(cfg->memory_citations_mode), "%s",
                     item->valuestring);

         item = cJSON_GetObjectItemCaseSensitive(citations_cfg, "reprompt_on_miss");
         if (cJSON_IsBool(item))
            cfg->memory_citations_reprompt_on_miss = cJSON_IsTrue(item) ? 1 : 0;

         item = cJSON_GetObjectItemCaseSensitive(citations_cfg, "strip_unverified");
         if (cJSON_IsBool(item))
            cfg->memory_citations_strip_unverified = cJSON_IsTrue(item) ? 1 : 0;
      }

      cJSON *cognify_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "cognify");
      if (cJSON_IsObject(cognify_cfg))
      {
         cJSON *async_cfg = cJSON_GetObjectItemCaseSensitive(cognify_cfg, "async");
         if (cJSON_IsObject(async_cfg))
         {
            item = cJSON_GetObjectItemCaseSensitive(async_cfg, "enabled");
            if (cJSON_IsBool(item))
               cfg->memory_cognify_async_enabled = cJSON_IsTrue(item) ? 1 : 0;
         }
         item = cJSON_GetObjectItemCaseSensitive(cognify_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_cognify_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(cognify_cfg, "model");
         if (cJSON_IsString(item) && item->valuestring[0])
            snprintf(cfg->memory_cognify_model, sizeof(cfg->memory_cognify_model), "%s",
                     item->valuestring);
         item = cJSON_GetObjectItemCaseSensitive(cognify_cfg, "command");
         if (cJSON_IsString(item) && item->valuestring[0])
            snprintf(cfg->memory_cognify_command, sizeof(cfg->memory_cognify_command), "%s",
                     item->valuestring);
      }

      cJSON *improve_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "improve");
      if (cJSON_IsObject(improve_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(improve_cfg, "dedupe_enabled");
         if (cJSON_IsBool(item))
            cfg->memory_improve_dedupe_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(improve_cfg, "summarise_enabled");
         if (cJSON_IsBool(item))
            cfg->memory_improve_summarise_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(improve_cfg, "min_cluster");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->memory_improve_min_cluster = (int)item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(improve_cfg, "max_confidence");
         if (cJSON_IsNumber(item))
            cfg->memory_improve_max_confidence = item->valuedouble;
      }

      cJSON *scenes_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "scenes");
      if (cJSON_IsObject(scenes_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(scenes_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_scenes_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(scenes_cfg, "global_escape_ratio");
         if (cJSON_IsNumber(item))
            cfg->memory_scenes_global_escape_ratio = item->valuedouble;
      }

      cJSON *episode_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "episode_summaries");
      if (cJSON_IsObject(episode_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(episode_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_episode_summaries_enabled = cJSON_IsTrue(item) ? 1 : 0;
      }

      cJSON *derive_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "derive_facts");
      if (cJSON_IsObject(derive_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(derive_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_derive_facts_enabled = cJSON_IsTrue(item) ? 1 : 0;
      }

      cJSON *failure_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "failure_detection");
      if (cJSON_IsObject(failure_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(failure_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_failure_detection_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(failure_cfg, "threshold");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
            cfg->memory_failure_detection_threshold = item->valuedouble;
      }

      cJSON *abstain_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "abstain");
      if (cJSON_IsObject(abstain_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(abstain_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_abstain_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(abstain_cfg, "gate");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= 1.0)
            cfg->memory_abstain_gate = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(abstain_cfg, "chunk_min_confidence");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= 1.0)
            cfg->memory_chunk_min_confidence = item->valuedouble;
      }

      cJSON *profile_cards_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "profile_cards");
      if (cJSON_IsObject(profile_cards_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(profile_cards_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_profile_cards_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(profile_cards_cfg, "min_observations");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->memory_profile_cards_min_obs = (int)item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(profile_cards_cfg, "stale_secs");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->memory_profile_cards_stale_secs = (int)item->valuedouble;
      }

      cJSON *budget_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "context_budget");
      if (cJSON_IsObject(budget_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(budget_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_context_budget_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(budget_cfg, "tokens");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->memory_context_budget_tokens = (int)item->valuedouble;
      }

      cJSON *routing_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "routing");
      if (cJSON_IsObject(routing_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(routing_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_routing_enabled = cJSON_IsTrue(item) ? 1 : 0;
      }

      /* Inline BM25 / semantic weight overrides */
      item = cJSON_GetObjectItemCaseSensitive(memory_cfg, "bm25_weight");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
         cfg->memory_bm25_weight = item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(memory_cfg, "semantic_weight");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
         cfg->memory_semantic_weight = item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(memory_cfg, "semantic_floor_scale");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
         cfg->memory_semantic_floor_scale = item->valuedouble;

      /* Dynamic fetch budget */
      cJSON *fetch_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "fetch_budget");
      if (cJSON_IsObject(fetch_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(fetch_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_fetch_budget_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(fetch_cfg, "base");
         if (cJSON_IsNumber(item) && item->valuedouble >= 32 && item->valuedouble <= 512)
            cfg->memory_fetch_budget_base = (int)item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(fetch_cfg, "shape_aware");
         if (cJSON_IsBool(item))
            cfg->memory_fetch_budget_shape_aware = cJSON_IsTrue(item) ? 1 : 0;
      }

      /* Hard-negative logging */
      item = cJSON_GetObjectItemCaseSensitive(memory_cfg, "hard_negative_log");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->memory_hard_negative_log, sizeof(cfg->memory_hard_negative_log), "%s",
                  item->valuestring);

      cJSON *rewrite_cfg = cJSON_GetObjectItemCaseSensitive(memory_cfg, "rewrite");
      if (cJSON_IsObject(rewrite_cfg))
      {
         item = cJSON_GetObjectItemCaseSensitive(rewrite_cfg, "enabled");
         if (cJSON_IsBool(item))
            cfg->memory_rewrite_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(rewrite_cfg, "command");
         if (cJSON_IsString(item) && item->valuestring[0])
            snprintf(cfg->memory_rewrite_command, sizeof(cfg->memory_rewrite_command), "%s",
                     item->valuestring);
         item = cJSON_GetObjectItemCaseSensitive(rewrite_cfg, "hyde");
         if (cJSON_IsBool(item))
            cfg->memory_rewrite_hyde = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(rewrite_cfg, "decompose");
         if (cJSON_IsBool(item))
            cfg->memory_rewrite_decompose = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(rewrite_cfg, "max_subqueries");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->memory_rewrite_max_subqueries = (int)item->valuedouble;
      }
   }
}
void config_parse_cross_verify_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *cv = cJSON_GetObjectItemCaseSensitive(root, "cross_verify");
   if (cJSON_IsObject(cv))
   {
      item = cJSON_GetObjectItemCaseSensitive(cv, "enabled");
      if (cJSON_IsBool(item))
         cfg->cross_verify = cJSON_IsTrue(item);

      item = cJSON_GetObjectItemCaseSensitive(cv, "verify_cmd");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->verify_cmd, sizeof(cfg->verify_cmd), "%s", item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(cv, "role");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->verify_role, sizeof(cfg->verify_role), "%s", item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(cv, "prompt");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->verify_prompt, sizeof(cfg->verify_prompt), "%s", item->valuestring);
   }
}
void config_parse_retry_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *retry = cJSON_GetObjectItemCaseSensitive(root, "retry");
   if (cJSON_IsObject(retry))
   {
      item = cJSON_GetObjectItemCaseSensitive(retry, "max_attempts");
      if (cJSON_IsNumber(item))
         cfg->retry_max_attempts = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(retry, "base_ms");
      if (cJSON_IsNumber(item))
         cfg->retry_base_ms = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(retry, "max_ms");
      if (cJSON_IsNumber(item))
         cfg->retry_max_ms = (int)item->valuedouble;
   }
}
void config_parse_concurrency_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *conc = cJSON_GetObjectItemCaseSensitive(root, "concurrency");
   if (cJSON_IsObject(conc))
   {
      item = cJSON_GetObjectItemCaseSensitive(conc, "default");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->concurrency_default = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(conc, "maximum_total_concurrent_agent_sessions");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->maximum_total_concurrent_agent_sessions = (int)item->valuedouble;

      cJSON *pm = cJSON_GetObjectItemCaseSensitive(conc, "per_model");
      if (cJSON_IsObject(pm))
      {
         const cJSON *entry;
         cJSON_ArrayForEach(entry, pm)
         {
            if (cfg->concurrency_per_model_count >= CONFIG_CONCURRENCY_MAX_ENTRIES)
               break;
            if (!cJSON_IsNumber(entry) || !entry->string)
               continue;
            config_concurrency_entry_t *e =
                &cfg->concurrency_per_model[cfg->concurrency_per_model_count++];
            snprintf(e->key, sizeof(e->key), "%s", entry->string);
            e->limit = (int)entry->valuedouble;
         }
      }

      cJSON *pp = cJSON_GetObjectItemCaseSensitive(conc, "per_provider");
      if (cJSON_IsObject(pp))
      {
         const cJSON *entry;
         cJSON_ArrayForEach(entry, pp)
         {
            if (cfg->concurrency_per_provider_count >= CONFIG_CONCURRENCY_MAX_ENTRIES)
               break;
            if (!cJSON_IsNumber(entry) || !entry->string)
               continue;
            config_concurrency_entry_t *e =
                &cfg->concurrency_per_provider[cfg->concurrency_per_provider_count++];
            snprintf(e->key, sizeof(e->key), "%s", entry->string);
            e->limit = (int)entry->valuedouble;
         }
      }

      cJSON *preempt = cJSON_GetObjectItemCaseSensitive(conc, "preempt");
      if (cJSON_IsObject(preempt))
      {
         item = cJSON_GetObjectItemCaseSensitive(preempt, "enabled");
         if (cJSON_IsBool(item))
            cfg->concurrency_preempt_enabled = cJSON_IsTrue(item) ? 1 : 0;

         item = cJSON_GetObjectItemCaseSensitive(preempt, "single_slot_only");
         if (cJSON_IsBool(item))
            cfg->concurrency_preempt_single_slot_only = cJSON_IsTrue(item) ? 1 : 0;

         item = cJSON_GetObjectItemCaseSensitive(preempt, "requeue_max");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0)
            cfg->concurrency_preempt_requeue_max = (int)item->valuedouble;
      }
   }
}
void config_parse_search_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *srch = cJSON_GetObjectItemCaseSensitive(root, "search");
   if (cJSON_IsObject(srch))
   {
      item = cJSON_GetObjectItemCaseSensitive(srch, "backend");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->search_backend, sizeof(cfg->search_backend), "%s", item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(srch, "max_results");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->search_max_results = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(srch, "searxng_url");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->search_searxng_url, sizeof(cfg->search_searxng_url), "%s",
                  item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(srch, "backends");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->search_backends, sizeof(cfg->search_backends), "%s", item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(srch, "fetch_pages");
      if (cJSON_IsBool(item))
         cfg->search_fetch_pages = cJSON_IsTrue(item) ? 1 : 0;

      item = cJSON_GetObjectItemCaseSensitive(srch, "tavily_api_key");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->search_tavily_api_key, sizeof(cfg->search_tavily_api_key), "%s",
                  item->valuestring);
   }
}
void config_parse_dogfood_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *dog_cfg = cJSON_GetObjectItemCaseSensitive(root, "dogfood");
   if (cJSON_IsObject(dog_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(dog_cfg, "enabled");
      if (cJSON_IsBool(item))
         cfg->dogfood_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(dog_cfg, "log_dir");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->dogfood_log_dir, sizeof(cfg->dogfood_log_dir), "%s", item->valuestring);
      item = cJSON_GetObjectItemCaseSensitive(dog_cfg, "commit_raw");
      if (cJSON_IsBool(item))
         cfg->dogfood_commit_raw = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(dog_cfg, "inline_tagging");
      if (cJSON_IsBool(item))
         cfg->dogfood_inline_tagging = cJSON_IsTrue(item) ? 1 : 0;
      /* autolabel toggles live in the flat keys exposed via cmd_data.c
       * (dogfood_autolabel_{repair,continuation,repeat_question}) so
       * the config-schema surface stays small. */
   }
}
void config_parse_identity_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *id_cfg = cJSON_GetObjectItemCaseSensitive(root, "identity");
   if (cJSON_IsObject(id_cfg))
   {
      cJSON *inject = cJSON_GetObjectItemCaseSensitive(id_cfg, "working_profile_injection");
      if (cJSON_IsObject(inject))
      {
         item = cJSON_GetObjectItemCaseSensitive(inject, "enabled");
         if (cJSON_IsBool(item))
            cfg->identity_working_profile_injection_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(inject, "fields");
         if (cJSON_IsArray(item))
         {
            int n = 0;
            cJSON *entry;
            cJSON_ArrayForEach(entry, item)
            {
               if (n >= CONFIG_WORKING_PROFILE_ALLOW_MAX)
                  break;
               if (!cJSON_IsString(entry) || !entry->valuestring[0])
                  continue;
               snprintf(cfg->identity_working_profile_injection_fields[n],
                        CONFIG_WORKING_PROFILE_FIELD_LEN, "%s", entry->valuestring);
               n++;
            }
            cfg->identity_working_profile_injection_fields_count = n;
         }
      }
   }
}
void config_parse_compact_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *cmpct = cJSON_GetObjectItemCaseSensitive(root, "compact");
   if (cJSON_IsObject(cmpct))
   {
      item = cJSON_GetObjectItemCaseSensitive(cmpct, "enabled");
      if (cJSON_IsBool(item))
         cfg->compact_enabled = cJSON_IsTrue(item) ? 1 : 0;

      item = cJSON_GetObjectItemCaseSensitive(cmpct, "threshold");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->compact_threshold = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(cmpct, "head_bytes");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->compact_head_bytes = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(cmpct, "tail_bytes");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->compact_tail_bytes = (int)item->valuedouble;

      cJSON *per_tool = cJSON_GetObjectItemCaseSensitive(cmpct, "per_tool");
      if (cJSON_IsObject(per_tool))
      {
         const cJSON *entry;
         cJSON_ArrayForEach(entry, per_tool)
         {
            if (cfg->compact_per_tool_count >= CONFIG_COMPACT_MAX_PER_TOOL)
               break;
            if (!entry->string || !cJSON_IsNumber(entry))
               continue;
            snprintf(cfg->compact_per_tool[cfg->compact_per_tool_count],
                     sizeof(cfg->compact_per_tool[0]), "%s=%d", entry->string,
                     (int)entry->valuedouble);
            cfg->compact_per_tool_count++;
         }
      }

      /* Coordinate Closet (fold §2), nested under "compact". Default-off. */
      cJSON *closet = cJSON_GetObjectItemCaseSensitive(cmpct, "coord_closet");
      if (cJSON_IsObject(closet))
      {
         item = cJSON_GetObjectItemCaseSensitive(closet, "enabled");
         if (cJSON_IsBool(item))
            cfg->coord_closet_enabled = cJSON_IsTrue(item) ? 1 : 0;

         item = cJSON_GetObjectItemCaseSensitive(closet, "budget_bytes");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->coord_closet_budget_bytes = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(closet, "max_ratio_pct");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->coord_closet_max_ratio_pct = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(closet, "denylist");
         if (cJSON_IsString(item) && item->valuestring)
            snprintf(cfg->coord_closet_denylist, sizeof(cfg->coord_closet_denylist), "%s",
                     item->valuestring);
      }
   }
}

void config_parse_fold_section(config_t *cfg, cJSON *root)
{
   cJSON *fold = cJSON_GetObjectItemCaseSensitive(root, "fold");
   if (!cJSON_IsObject(fold))
      return;
   cJSON *item = cJSON_GetObjectItemCaseSensitive(fold, "enabled");
   if (cJSON_IsBool(item))
      cfg->fold_enabled = cJSON_IsTrue(item) ? 1 : 0;
   item = cJSON_GetObjectItemCaseSensitive(fold, "retained_msgs");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->fold_retained_msgs = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(fold, "min_fold_msgs");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->fold_min_fold_msgs = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(fold, "excerpt_bytes");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->fold_excerpt_bytes = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(fold, "register_enabled");
   if (cJSON_IsBool(item))
      cfg->fold_register_enabled = cJSON_IsTrue(item) ? 1 : 0;
   cJSON *freeze = cJSON_GetObjectItemCaseSensitive(fold, "freeze");
   if (cJSON_IsObject(freeze))
   {
      item = cJSON_GetObjectItemCaseSensitive(freeze, "enabled");
      if (cJSON_IsBool(item))
         cfg->fold_freeze_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(freeze, "tail_cap_msgs");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->fold_freeze_tail_cap_msgs = (int)item->valuedouble;
   }
   cJSON *recall = cJSON_GetObjectItemCaseSensitive(fold, "recall");
   if (cJSON_IsObject(recall))
   {
      item = cJSON_GetObjectItemCaseSensitive(recall, "enabled");
      if (cJSON_IsBool(item))
         cfg->fold_recall_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(recall, "ttl_turns");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->fold_recall_ttl_turns = (int)item->valuedouble;
   }
}

/* Pluggable-module toggles (`modules:` block) — the canonical user surface for enabling or
 * disabling the pipeline modules. Each key is a bool; when present it sets the tristate to 0/1,
 * else the field stays -1 (unspecified) and the resolver falls back to env/default. See
 * config_module_enabled() and config.h. */
void config_parse_modules_section(config_t *cfg, cJSON *root)
{
   cJSON *mods = cJSON_GetObjectItemCaseSensitive(root, "modules");
   if (!cJSON_IsObject(mods))
      return;
   struct
   {
      const char *key;
      int *field;
   } toggles[] = {
       {"memory", &cfg->module_memory},         {"governance", &cfg->module_governance},
       {"delegates", &cfg->module_delegates},   {"workflows", &cfg->module_workflows},
       {"roundtable", &cfg->module_roundtable}, {"economizer", &cfg->module_economizer},
   };
   for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); i++)
   {
      cJSON *b = cJSON_GetObjectItemCaseSensitive(mods, toggles[i].key);
      if (cJSON_IsBool(b))
         *toggles[i].field = cJSON_IsTrue(b) ? 1 : 0;
   }
}

/* Parse the public shape: economizer: {mode: off|safe|aggressive}. */
int config_parse_economizer_section(config_t *cfg, cJSON *root)
{
   cJSON *econ = cJSON_GetObjectItemCaseSensitive(root, "economizer");
   if (!econ)
      return 0;
   if (!cJSON_IsObject(econ))
   {
      fprintf(stderr, "aimee: config error: economizer must be an object; use "
                      "economizer: {mode: off|safe|aggressive}\n");
      return -1;
   }

   cJSON *mode = NULL;
   int fields = 0;
   cJSON *field;
   cJSON_ArrayForEach(field, econ)
   {
      fields++;
      if (field->string && strcmp(field->string, "mode") == 0 && !mode)
         mode = field;
      else
      {
         fprintf(stderr,
                 "aimee: config error: invalid economizer field \"%s\"; only mode is allowed\n",
                 field->string ? field->string : "");
         return -1;
      }
   }
   if (fields != 1 || !cJSON_IsString(mode) || !mode->valuestring)
   {
      fprintf(stderr, "aimee: config error: economizer.mode must be off, safe, or aggressive\n");
      return -1;
   }
   int parsed = econ_mode_parse(mode->valuestring);
   if (parsed < 0)
   {
      fprintf(stderr,
              "aimee: config error: economizer mode \"%s\" is unsupported; choose "
              "off, safe, or aggressive\n",
              mode->valuestring);
      return -1;
   }
   cfg->economizer_mode = parsed;
   return 0;
}

/* Clamp an integer to [lo, hi]. */
static int clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

/* Autonomous-development pipeline knobs (Phase-C). Historically env-only
 * (AIMEE_AUTONOMY_*); parsed here so they live in the typed config + web Settings, then
 * bridged to the env vars at startup (autonomy_config_to_env) for the wfe library.
 * Out-of-range values are CLAMPED to sane bounds (not silently dropped) — this runs at
 * config_load, so it also bounds a value a raw config.set persisted, before the startup
 * bridge reads it: a fat-fingered skeptics=1000 can't fan out 1000 judge dispatches. The
 * wfe consumers additionally fail-safe on <=0 (treat as off/default), so the low end is
 * doubly safe. */
void config_parse_autonomy_section(config_t *cfg, cJSON *root)
{
   cJSON *autonomy = cJSON_GetObjectItemCaseSensitive(root, "autonomy");
   if (!cJSON_IsObject(autonomy))
      return;
   cJSON *item = cJSON_GetObjectItemCaseSensitive(autonomy, "skeptics");
   if (cJSON_IsNumber(item))
      cfg->autonomy_skeptics = clampi(item->valueint, 0, 32);
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "fanout");
   if (cJSON_IsBool(item))
      cfg->autonomy_fanout = cJSON_IsTrue(item) ? 1 : 0;
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "unit_retry");
   if (cJSON_IsNumber(item))
      cfg->autonomy_unit_retry = clampi(item->valueint, 0, 10);
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "unit_max");
   if (cJSON_IsNumber(item)) /* unit_max must be positive — 0 units is meaningless */
      cfg->autonomy_unit_max = clampi(item->valueint, 1, 256);
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "ci_retry_max");
   if (cJSON_IsNumber(item))
      cfg->autonomy_ci_retry_max = clampi(item->valueint, 0, 20);
   /* Run safety caps + auto-resume policy. Clamped to sane bounds; the wfe readers
    * additionally fall back to their historical default on a non-positive value. */
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "max_turns");
   if (cJSON_IsNumber(item)) /* must be positive — a run needs at least a few turns */
      cfg->autonomy_max_turns = clampi(item->valueint, 1, 100000);
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "max_wall_secs");
   if (cJSON_IsNumber(item)) /* >=30s: below that a run can't make useful progress per window */
      cfg->autonomy_max_wall_secs = clampi(item->valueint, 30, 86400);
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "stale_abandon_secs");
   if (cJSON_IsNumber(item)) /* 0 disables the stale-park reaper entirely */
      cfg->autonomy_stale_abandon_secs = clampi(item->valueint, 0, 604800);
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "concurrency");
   if (cJSON_IsNumber(item))
      cfg->autonomy_concurrency = clampi(item->valueint, 1, 128);
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "auto_resume_cap_parks");
   if (cJSON_IsBool(item))
      cfg->autonomy_auto_resume_cap_parks = cJSON_IsTrue(item) ? 1 : 0;
   item = cJSON_GetObjectItemCaseSensitive(autonomy, "max_resumes");
   if (cJSON_IsNumber(item)) /* 0 = never auto-resume (equivalent to the policy being off) */
      cfg->autonomy_max_resumes = clampi(item->valueint, 0, 100000);
}

/* Bridge the autonomy config knobs to the AIMEE_AUTONOMY_* env vars the wfe library
 * reads via getenv (it cannot see config_t across the module boundary). setenv's third
 * arg is 0 — it sets the var ONLY when unset, so an explicitly-exported env var (a
 * deployment override) always wins over the config value. Call once at server startup;
 * a Settings change to these knobs therefore applies on the next server start. */
/* NOTE: autonomy_config_to_env (the startup setenv bridge) was REMOVED — wfe now reads
 * autonomy.* live from the config snapshot via config_autonomy_lookup (thread-safe), so there
 * is no cross-thread setenv. An operator-exported AIMEE_AUTONOMY_* still overrides. */

void config_parse_sessions_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *sess_cfg = cJSON_GetObjectItemCaseSensitive(root, "sessions");
   if (cJSON_IsObject(sess_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(sess_cfg, "stale_threshold_secs");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->worktree_stale_secs = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(sess_cfg, "max_sessions");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->max_sessions = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(sess_cfg, "max_worktrees");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->max_worktrees = (int)item->valuedouble;
   }
}
void config_parse_sandbox_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *sbox = cJSON_GetObjectItemCaseSensitive(root, "sandbox");
   if (cJSON_IsObject(sbox))
   {
      item = cJSON_GetObjectItemCaseSensitive(sbox, "mode");
      if (cJSON_IsString(item) && item->valuestring[0])
         cfg->sandbox.mode = sandbox_mode_from_string(item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(sbox, "network");
      if (cJSON_IsBool(item))
         cfg->sandbox.network_isolated = cJSON_IsTrue(item);

      cJSON *paths = cJSON_GetObjectItemCaseSensitive(sbox, "allow_paths");
      if (cJSON_IsArray(paths))
      {
         int i = 0;
         cJSON *el;
         cJSON_ArrayForEach(el, paths)
         {
            if (i >= SANDBOX_MAX_ALLOW_PATHS)
               break;
            if (cJSON_IsString(el) && el->valuestring[0])
            {
               snprintf(cfg->sandbox.allow_paths[i], SANDBOX_MAX_PATH_LEN, "%s", el->valuestring);
               i++;
            }
         }
         cfg->sandbox.allow_path_count = i;
      }
   }
}
void config_parse_lsp_servers_section(config_t *cfg, cJSON *root)
{
   cJSON *lsp_arr = cJSON_GetObjectItemCaseSensitive(root, "lsp_servers");
   if (cJSON_IsArray(lsp_arr))
   {
      const cJSON *lsp_entry;
      cJSON_ArrayForEach(lsp_entry, lsp_arr)
      {
         if (cfg->lsp_server_count >= CONFIG_LSP_MAX_SERVERS)
            break;
         if (!cJSON_IsObject(lsp_entry))
            continue;

         struct config_lsp_server *s = &cfg->lsp_servers[cfg->lsp_server_count];
         memset(s, 0, sizeof(*s));

         cJSON *lsp_name = cJSON_GetObjectItemCaseSensitive(lsp_entry, "name");
         if (cJSON_IsString(lsp_name) && lsp_name->valuestring[0])
            snprintf(s->name, sizeof(s->name), "%s", lsp_name->valuestring);

         cJSON *lsp_cmd = cJSON_GetObjectItemCaseSensitive(lsp_entry, "command");
         if (cJSON_IsString(lsp_cmd) && lsp_cmd->valuestring[0])
            snprintf(s->command, sizeof(s->command), "%s", lsp_cmd->valuestring);
         else
            continue; /* command is required */

         cJSON *lsp_args = cJSON_GetObjectItemCaseSensitive(lsp_entry, "args");
         if (cJSON_IsArray(lsp_args))
         {
            const cJSON *arg;
            cJSON_ArrayForEach(arg, lsp_args)
            {
               if (s->arg_count >= CONFIG_LSP_MAX_ARGS)
                  break;
               if (cJSON_IsString(arg) && arg->valuestring[0])
               {
                  snprintf(s->args[s->arg_count], sizeof(s->args[0]), "%s", arg->valuestring);
                  s->arg_count++;
               }
            }
         }

         cJSON *lsp_exts = cJSON_GetObjectItemCaseSensitive(lsp_entry, "extensions");
         if (cJSON_IsArray(lsp_exts))
         {
            const cJSON *ext;
            cJSON_ArrayForEach(ext, lsp_exts)
            {
               if (s->extension_count >= CONFIG_LSP_MAX_EXTENSIONS)
                  break;
               if (cJSON_IsString(ext) && ext->valuestring[0])
               {
                  snprintf(s->extensions[s->extension_count], sizeof(s->extensions[0]), "%s",
                           ext->valuestring);
                  s->extension_count++;
               }
            }
         }

         if (s->extension_count == 0)
            continue; /* must map at least one extension */

         cfg->lsp_server_count++;
      }
   }
}
void config_parse_mcp_clients_section(config_t *cfg, cJSON *root)
{
   cJSON *mcp_arr = cJSON_GetObjectItemCaseSensitive(root, "mcp_clients");
   if (cJSON_IsArray(mcp_arr))
   {
      const cJSON *mcp_entry;
      cJSON_ArrayForEach(mcp_entry, mcp_arr)
      {
         if (cfg->mcp_client_count >= CONFIG_MCP_MAX_CLIENTS)
            break;
         if (!cJSON_IsObject(mcp_entry))
            continue;

         config_mcp_client_t *client = &cfg->mcp_clients[cfg->mcp_client_count];
         memset(client, 0, sizeof(*client));

         cJSON *name = cJSON_GetObjectItemCaseSensitive(mcp_entry, "name");
         if (!cJSON_IsString(name) || !name->valuestring[0])
            continue;
         snprintf(client->name, sizeof(client->name), "%s", name->valuestring);

         cJSON *transport = cJSON_GetObjectItemCaseSensitive(mcp_entry, "transport");
         if (cJSON_IsString(transport) && transport->valuestring[0])
            client->transport = config_mcp_transport_from_string(transport->valuestring);
         if (client->transport == CONFIG_MCP_TRANSPORT_NONE)
            continue;

         cJSON *command = cJSON_GetObjectItemCaseSensitive(mcp_entry, "command");
         if (cJSON_IsArray(command))
         {
            const cJSON *arg;
            cJSON_ArrayForEach(arg, command)
            {
               if (client->command_count >= CONFIG_MCP_MAX_COMMAND_ARGS)
                  break;
               if (!cJSON_IsString(arg) || !arg->valuestring[0])
                  continue;
               snprintf(client->command[client->command_count], sizeof(client->command[0]), "%s",
                        arg->valuestring);
               client->command_count++;
            }
         }

         cJSON *cwd = cJSON_GetObjectItemCaseSensitive(mcp_entry, "cwd");
         if (cJSON_IsString(cwd) && cwd->valuestring[0])
            snprintf(client->cwd, sizeof(client->cwd), "%s", cwd->valuestring);

         cJSON *url = cJSON_GetObjectItemCaseSensitive(mcp_entry, "url");
         if (cJSON_IsString(url) && url->valuestring[0])
            snprintf(client->url, sizeof(client->url), "%s", url->valuestring);

         cJSON *bearer = cJSON_GetObjectItemCaseSensitive(mcp_entry, "bearer_token_env");
         if (cJSON_IsString(bearer) && bearer->valuestring[0])
            snprintf(client->bearer_token_env, sizeof(client->bearer_token_env), "%s",
                     bearer->valuestring);

         /* install: "server" (default) exposes the plugin only to this server's
          * sessions; "kb" runs it on aimee-kb, shared to everything on that kb. */
         cJSON *install = cJSON_GetObjectItemCaseSensitive(mcp_entry, "install");
         if (cJSON_IsString(install) && strcasecmp(install->valuestring, "kb") == 0)
            client->install = CONFIG_MCP_INSTALL_KB;

         if (client->transport == CONFIG_MCP_TRANSPORT_STDIO && client->command_count == 0)
            continue;
         if (client->transport == CONFIG_MCP_TRANSPORT_SSE && client->url[0] == '\0')
            continue;

         cfg->mcp_client_count++;
      }
   }
}
void config_parse_mcp_section(config_t *cfg, cJSON *root)
{
   cJSON *mcp_obj = cJSON_GetObjectItemCaseSensitive(root, "mcp");
   if (cJSON_IsObject(mcp_obj))
   {
      cJSON *osv = cJSON_GetObjectItemCaseSensitive(mcp_obj, "osv");
      if (cJSON_IsObject(osv))
      {
         cJSON *item = cJSON_GetObjectItemCaseSensitive(osv, "enabled");
         if (cJSON_IsBool(item))
            cfg->mcp_osv_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(osv, "offline");
         if (cJSON_IsBool(item))
            cfg->mcp_osv_offline = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(osv, "enforce");
         if (cJSON_IsBool(item))
            cfg->mcp_osv_enforce = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(osv, "cache_ttl_hours");
         if (cJSON_IsNumber(item) && item->valueint > 0)
            cfg->mcp_osv_cache_ttl_hours = item->valueint;
         item = cJSON_GetObjectItemCaseSensitive(osv, "endpoint");
         if (cJSON_IsString(item) && item->valuestring[0])
            snprintf(cfg->mcp_osv_endpoint, sizeof(cfg->mcp_osv_endpoint), "%s", item->valuestring);

         cJSON *allow = cJSON_GetObjectItemCaseSensitive(osv, "allow");
         if (cJSON_IsArray(allow))
         {
            cfg->mcp_osv_allow_count = 0;
            cJSON *entry;
            cJSON_ArrayForEach(entry, allow)
            {
               if (cfg->mcp_osv_allow_count >= CONFIG_MCP_OSV_MAX_ALLOW)
                  break;
               if (!cJSON_IsString(entry) || !entry->valuestring[0])
                  continue;
               snprintf(cfg->mcp_osv_allow[cfg->mcp_osv_allow_count], sizeof(cfg->mcp_osv_allow[0]),
                        "%s", entry->valuestring);
               cfg->mcp_osv_allow_count++;
            }
         }
      }
   }
}
void config_parse_rewind_section(config_t *cfg, cJSON *root)
{
   cJSON *rw = cJSON_GetObjectItemCaseSensitive(root, "rewind");
   if (cJSON_IsObject(rw))
   {
      cJSON *as = cJSON_GetObjectItemCaseSensitive(rw, "auto_snapshot");
      if (cJSON_IsBool(as))
         cfg->rewind_auto_snapshot = cJSON_IsTrue(as) ? 1 : 0;
   }
}
void config_parse_otel_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *otel_obj = cJSON_GetObjectItemCaseSensitive(root, "otel");
   if (cJSON_IsObject(otel_obj))
   {
      item = cJSON_GetObjectItemCaseSensitive(otel_obj, "endpoint");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->otel_endpoint, sizeof(cfg->otel_endpoint), "%s", item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(otel_obj, "service_name");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->otel_service_name, sizeof(cfg->otel_service_name), "%s", item->valuestring);
   }
}
void config_parse_integrity_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *integ_cfg = cJSON_GetObjectItemCaseSensitive(root, "integrity");
   if (cJSON_IsObject(integ_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(integ_cfg, "enabled");
      if (cJSON_IsBool(item))
         cfg->integrity_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(integ_cfg, "dry_run");
      if (cJSON_IsBool(item))
         cfg->integrity_dry_run = cJSON_IsTrue(item) ? 1 : 0;
   }
}
void config_parse_session_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *sess_cfg = cJSON_GetObjectItemCaseSensitive(root, "session");
   if (cJSON_IsObject(sess_cfg))
   {
      cJSON *vc = cJSON_GetObjectItemCaseSensitive(sess_cfg, "virtual_context");
      if (cJSON_IsObject(vc))
      {
         item = cJSON_GetObjectItemCaseSensitive(vc, "enabled");
         if (cJSON_IsBool(item))
            cfg->virtual_context_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(vc, "assembly_budget");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->virtual_context_assembly_budget = (int)item->valuedouble;
      }
   }
}
void config_parse_transport_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *tr = cJSON_GetObjectItemCaseSensitive(root, "transport");
   if (cJSON_IsObject(tr))
   {
      item = cJSON_GetObjectItemCaseSensitive(tr, "kb_pool_enabled");
      if (cJSON_IsBool(item))
         cfg->transport_kb_pool_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(tr, "server_keepalive_enabled");
      if (cJSON_IsBool(item))
         cfg->transport_server_keepalive_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(tr, "thinclient_gzip_enabled");
      if (cJSON_IsBool(item))
         cfg->transport_thinclient_gzip_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(tr, "kb_gzip_enabled");
      if (cJSON_IsBool(item))
         cfg->transport_kb_gzip_enabled = cJSON_IsTrue(item) ? 1 : 0;
      cJSON *cr = cJSON_GetObjectItemCaseSensitive(tr, "cache_aware_rewrite");
      if (cJSON_IsObject(cr))
      {
         item = cJSON_GetObjectItemCaseSensitive(cr, "enabled");
         if (cJSON_IsBool(item))
            cfg->cache_aware_rewrite_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(cr, "min_savings_tokens");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0)
            cfg->cache_aware_rewrite_min_savings_tokens = (int)item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(cr, "hard_context_threshold");
         if (cJSON_IsNumber(item) && item->valuedouble > 0.0)
            cfg->cache_aware_rewrite_hard_context_threshold = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(cr, "max_defer_turns");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0)
            cfg->cache_aware_rewrite_max_defer_turns = (int)item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(cr, "segment_check_turns");
         if (cJSON_IsNumber(item) && item->valuedouble >= 0)
            cfg->cache_aware_rewrite_segment_check_turns = (int)item->valuedouble;
      }
   }

   cJSON *cre = cJSON_GetObjectItemCaseSensitive(root, "cost_reward");
   if (cJSON_IsObject(cre))
   {
      item = cJSON_GetObjectItemCaseSensitive(cre, "enabled");
      if (cJSON_IsBool(item))
         cfg->cost_reward_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(cre, "lambda_pct");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0)
         cfg->cost_reward_lambda_pct = (int)item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(cre, "ref_usd_milli");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->cost_reward_ref_usd_milli = (int)item->valuedouble;
   }

   cJSON *rcap = cJSON_GetObjectItemCaseSensitive(root, "reasoning_cap");
   if (cJSON_IsObject(rcap))
   {
      item = cJSON_GetObjectItemCaseSensitive(rcap, "enabled");
      if (cJSON_IsBool(item))
         cfg->reasoning_cap_enabled = cJSON_IsTrue(item) ? 1 : 0;
   }

   cJSON *ddp = cJSON_GetObjectItemCaseSensitive(root, "dedup");
   if (cJSON_IsObject(ddp))
   {
      item = cJSON_GetObjectItemCaseSensitive(ddp, "enabled");
      if (cJSON_IsBool(item))
         cfg->dedup_enabled = cJSON_IsTrue(item) ? 1 : 0;
   }

   cJSON *csh = cJSON_GetObjectItemCaseSensitive(root, "cache_shaping");
   if (cJSON_IsObject(csh))
   {
      item = cJSON_GetObjectItemCaseSensitive(csh, "enabled");
      if (cJSON_IsBool(item))
         cfg->cache_shaping_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(csh, "min_chars");
      if (cJSON_IsNumber(item) && item->valuedouble >= 0)
         cfg->cache_min_chars = (int)item->valuedouble;
   }

   cJSON *ing = cJSON_GetObjectItemCaseSensitive(root, "ingress");
   if (cJSON_IsObject(ing))
   {
      item = cJSON_GetObjectItemCaseSensitive(ing, "usage_accounting_enabled");
      if (cJSON_IsBool(item))
         cfg->ingress_usage_accounting_enabled = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(ing, "audit_async");
      if (cJSON_IsBool(item))
         cfg->ingress_audit_async = cJSON_IsTrue(item) ? 1 : 0;
      item = cJSON_GetObjectItemCaseSensitive(ing, "trusted_proxy_secret");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->ingress_trusted_proxy_secret, sizeof(cfg->ingress_trusted_proxy_secret),
                  "%s", item->valuestring);
   }

   cJSON *ddw = cJSON_GetObjectItemCaseSensitive(root, "dedup");
   if (cJSON_IsObject(ddw))
   {
      item = cJSON_GetObjectItemCaseSensitive(ddw, "window_seconds");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->dedup_window_seconds = (int)item->valuedouble;
   }
}
void config_parse_guardrails_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *gr = cJSON_GetObjectItemCaseSensitive(root, "guardrails");
   if (cJSON_IsObject(gr))
   {
      cJSON *sem = cJSON_GetObjectItemCaseSensitive(gr, "semantic");
      if (cJSON_IsObject(sem))
      {
         /* Canonical form: `mode: off|dry_run|advisory|enforce`. If absent, fall back to the
          * deprecated {enabled,dry_run,advisory_only,allow_ml_only_block} quad and map it onto
          * the ladder (using the historical defaults for any missing sibling). */
         item = cJSON_GetObjectItemCaseSensitive(sem, "mode");
         if (cJSON_IsString(item) && item->valuestring)
            snprintf(
                cfg->guardrails_semantic_mode, sizeof(cfg->guardrails_semantic_mode), "%s",
                guardrails_semantic_mode_name(guardrails_semantic_mode_parse(item->valuestring)));
         else if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(sem, "enabled")))
         {
            int enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sem, "enabled"));
            int dry = 1, adv = 1, allow = 0;
            cJSON *t;
            if (cJSON_IsBool(t = cJSON_GetObjectItemCaseSensitive(sem, "dry_run")))
               dry = cJSON_IsTrue(t);
            if (cJSON_IsBool(t = cJSON_GetObjectItemCaseSensitive(sem, "advisory_only")))
               adv = cJSON_IsTrue(t);
            if (cJSON_IsBool(t = cJSON_GetObjectItemCaseSensitive(sem, "allow_ml_only_block")))
               allow = cJSON_IsTrue(t);
            const char *m = !enabled          ? "off"
                            : dry             ? "dry_run"
                            : (adv || !allow) ? "advisory"
                                              : "enforce";
            snprintf(cfg->guardrails_semantic_mode, sizeof(cfg->guardrails_semantic_mode), "%s", m);
         }
         item = cJSON_GetObjectItemCaseSensitive(sem, "command");
         if (cJSON_IsString(item) && item->valuestring)
            snprintf(cfg->guardrails_semantic_command, sizeof(cfg->guardrails_semantic_command),
                     "%s", item->valuestring);
         item = cJSON_GetObjectItemCaseSensitive(sem, "warn_threshold");
         if (cJSON_IsNumber(item))
            cfg->guardrails_semantic_warn_threshold = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(sem, "prompt_threshold");
         if (cJSON_IsNumber(item))
            cfg->guardrails_semantic_prompt_threshold = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(sem, "block_threshold");
         if (cJSON_IsNumber(item))
            cfg->guardrails_semantic_block_threshold = item->valuedouble;
      }
      cJSON *br = cJSON_GetObjectItemCaseSensitive(gr, "blast_radius");
      if (cJSON_IsObject(br))
      {
         item = cJSON_GetObjectItemCaseSensitive(br, "advisory_enabled");
         if (cJSON_IsBool(item))
            cfg->guardrails_blast_radius_advisory_enabled = cJSON_IsTrue(item) ? 1 : 0;
      }
   }
}
void config_parse_auxiliary_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *aux_cfg = cJSON_GetObjectItemCaseSensitive(root, "auxiliary");
   if (cJSON_IsObject(aux_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(aux_cfg, "enabled");
      if (cJSON_IsBool(item))
         cfg->aux_enabled = cJSON_IsTrue(item) ? 1 : 0;

      item = cJSON_GetObjectItemCaseSensitive(aux_cfg, "default_provider");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->aux_default_provider, sizeof(cfg->aux_default_provider), "%s",
                  item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(aux_cfg, "default_model");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->aux_default_model, sizeof(cfg->aux_default_model), "%s", item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(aux_cfg, "default_max_tokens");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->aux_default_max_tokens = (int)item->valuedouble;

      cJSON *tasks = cJSON_GetObjectItemCaseSensitive(aux_cfg, "tasks");
      if (cJSON_IsObject(tasks))
      {
         const cJSON *t;
         cJSON_ArrayForEach(t, tasks)
         {
            if (cfg->aux_task_count >= CONFIG_AUX_MAX_TASKS)
               break;
            if (!t->string || !cJSON_IsObject(t))
               continue;
            int idx = cfg->aux_task_count;
            snprintf(cfg->aux_tasks[idx].task, CONFIG_AUX_TASK_NAME_LEN, "%s", t->string);
            cJSON *tf = cJSON_GetObjectItemCaseSensitive(t, "provider");
            if (cJSON_IsString(tf) && tf->valuestring)
               snprintf(cfg->aux_tasks[idx].provider, sizeof(cfg->aux_tasks[idx].provider), "%s",
                        tf->valuestring);
            tf = cJSON_GetObjectItemCaseSensitive(t, "model");
            if (cJSON_IsString(tf) && tf->valuestring)
               snprintf(cfg->aux_tasks[idx].model, sizeof(cfg->aux_tasks[idx].model), "%s",
                        tf->valuestring);
            tf = cJSON_GetObjectItemCaseSensitive(t, "max_tokens");
            if (cJSON_IsNumber(tf) && tf->valuedouble > 0)
               cfg->aux_tasks[idx].max_tokens = (int)tf->valuedouble;
            cfg->aux_task_count++;
         }
      }
   }
}
void config_parse_model_meta_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *model_meta_cfg = cJSON_GetObjectItemCaseSensitive(root, "model_meta");
   if (cJSON_IsObject(model_meta_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(model_meta_cfg, "refresh_minutes");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->model_meta_refresh_minutes = (int)item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(model_meta_cfg, "capability_routing");
      if (cJSON_IsBool(item))
         cfg->model_meta_capability_routing = cJSON_IsTrue(item) ? 1 : 0;
   }
   cJSON *routing_cfg = cJSON_GetObjectItemCaseSensitive(root, "routing");
   if (cJSON_IsObject(routing_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(routing_cfg, "prefer_local");
      if (cJSON_IsBool(item))
         cfg->prefer_local_agents = cJSON_IsTrue(item) ? 1 : 0;
   }
}
void config_parse_db2_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *db2_obj = cJSON_GetObjectItemCaseSensitive(root, "db2");
   cJSON *db2_vec =
       cJSON_IsObject(db2_obj) ? cJSON_GetObjectItemCaseSensitive(db2_obj, "vector") : NULL;
   if (cJSON_IsObject(db2_vec))
   {
      item = cJSON_GetObjectItemCaseSensitive(db2_vec, "corpus_index");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->db2_vector_corpus_index, sizeof(cfg->db2_vector_corpus_index), "%s",
                  item->valuestring);
      item = cJSON_GetObjectItemCaseSensitive(db2_vec, "corpus_diskann_threshold");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->db2_vector_corpus_diskann_threshold = (int64_t)item->valuedouble;
   }
}
void config_parse_ensemble_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *ensemble_cfg = cJSON_GetObjectItemCaseSensitive(root, "ensemble");
   if (cJSON_IsObject(ensemble_cfg))
   {
      item = cJSON_GetObjectItemCaseSensitive(ensemble_cfg, "aggregator");
      if (cJSON_IsString(item) && item->valuestring)
         snprintf(cfg->ensemble_aggregator, sizeof(cfg->ensemble_aggregator), "%s",
                  item->valuestring);
      item = cJSON_GetObjectItemCaseSensitive(ensemble_cfg, "min_successful");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->ensemble_min_successful = (int)item->valuedouble;
      item = cJSON_GetObjectItemCaseSensitive(ensemble_cfg, "max_cost_usd");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->ensemble_max_cost_usd = item->valuedouble;
      cJSON *refs = cJSON_GetObjectItemCaseSensitive(ensemble_cfg, "reference_models");
      if (cJSON_IsArray(refs))
      {
         cJSON *ref;
         cJSON_ArrayForEach(ref, refs)
         {
            /* 32 = ENSEMBLE_MAX_REFS (delegate_ensemble.h); literal to avoid
             * config -> ensemble-engine header coupling. */
            if (cJSON_IsString(ref) && ref->valuestring && cfg->ensemble_reference_count < 32)
            {
               snprintf(cfg->ensemble_reference_models[cfg->ensemble_reference_count],
                        sizeof(cfg->ensemble_reference_models[0]), "%s", ref->valuestring);
               cfg->ensemble_reference_count++;
            }
         }
      }
      /* Optional per-participant review personas, paired by index with
       * reference_models. Empty/missing entries fall back to the diverse default
       * lineup in the engine. */
      cJSON *personas = cJSON_GetObjectItemCaseSensitive(ensemble_cfg, "reference_personas");
      if (cJSON_IsArray(personas))
      {
         cJSON *p;
         cJSON_ArrayForEach(p, personas)
         {
            if (cJSON_IsString(p) && p->valuestring && cfg->ensemble_reference_persona_count < 32)
            {
               snprintf(cfg->ensemble_reference_personas[cfg->ensemble_reference_persona_count],
                        sizeof(cfg->ensemble_reference_personas[0]), "%s", p->valuestring);
               cfg->ensemble_reference_persona_count++;
            }
         }
      }
   }
}

void config_parse_roundtable_section(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;
   cJSON *roundtable_cfg = cJSON_GetObjectItemCaseSensitive(root, "roundtable");
   if (!cJSON_IsObject(roundtable_cfg))
      return;

   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "max_rounds");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->roundtable_max_rounds = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "converge_threshold");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 100)
      cfg->roundtable_converge_threshold = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "deadline_ms");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0)
      cfg->roundtable_deadline_ms = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "turns");
   if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
      snprintf(cfg->roundtable_turns, sizeof(cfg->roundtable_turns), "%s", item->valuestring);
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "default");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->roundtable_default, sizeof(cfg->roundtable_default), "%s", item->valuestring);

   /* Authoring pipeline (roundtable.pipeline_*). */
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "pipeline_done_bar");
   if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
      snprintf(cfg->roundtable_pipeline_done_bar, sizeof(cfg->roundtable_pipeline_done_bar), "%s",
               item->valuestring);
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "pipeline_max_passes");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0)
      cfg->roundtable_pipeline_max_passes = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "pipeline_max_attempts_per_pass");
   if (cJSON_IsNumber(item) && item->valuedouble >= 1)
      cfg->roundtable_pipeline_max_attempts_per_pass = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "pipeline_max_cost_usd");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0)
      cfg->roundtable_pipeline_max_cost_usd = item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "pipeline_max_total_cost_usd");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0)
      cfg->roundtable_pipeline_max_total_cost_usd = item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "pipeline_gate_ttl_h");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0)
      cfg->roundtable_pipeline_gate_ttl_h = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "pipeline_parked_releases_slot");
   if (cJSON_IsBool(item))
      cfg->roundtable_pipeline_parked_releases_slot = cJSON_IsTrue(item) ? 1 : 0;
   item = cJSON_GetObjectItemCaseSensitive(roundtable_cfg, "pipeline_unknown_context_tokens");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->roundtable_pipeline_unknown_context_tokens = (int)item->valuedouble;
}

void config_parse_kb_section2(config_t *cfg, cJSON *root)
{
   cJSON *item = NULL;

   /* Legacy telemetry.metrics_token is parsed only for the boot migration. Its
    * SHA-256 verifier is credential material and runtime custody is Vault-only. */
   cJSON *telemetry = cJSON_GetObjectItemCaseSensitive(root, "telemetry");
   if (cJSON_IsObject(telemetry))
   {
      item = cJSON_GetObjectItemCaseSensitive(telemetry, "metrics_token");
      if (cJSON_IsString(item) && item->valuestring)
         strncpy(cfg->telemetry_metrics_token, item->valuestring,
                 sizeof(cfg->telemetry_metrics_token) - 1);
   }

   cJSON *kb = cJSON_GetObjectItemCaseSensitive(root, "kb");
   if (cJSON_IsObject(kb))
   {
      cJSON *api = cJSON_GetObjectItemCaseSensitive(kb, "api");
      if (cJSON_IsObject(api))
      {
         item = cJSON_GetObjectItemCaseSensitive(api, "http_port");
         if (cJSON_IsNumber(item))
            cfg->kb_api_http_port = (int)item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(api, "bearer_token");
         if (cJSON_IsString(item) && item->valuestring)
            strncpy(cfg->kb_api_bearer_token, item->valuestring,
                    sizeof(cfg->kb_api_bearer_token) - 1);
      }

      item = cJSON_GetObjectItemCaseSensitive(kb, "worker_count");
      if (cJSON_IsNumber(item))
      {
         int wc = (int)item->valuedouble;
         /* Cap lifted 8 -> 32: DB2 connections are now bounded by the pool, not
          * by the worker count. */
         cfg->kb_worker_count = (wc < 0) ? 0 : (wc > 32) ? 32 : wc;
      }

      item = cJSON_GetObjectItemCaseSensitive(kb, "connection_pool_size");
      if (cJSON_IsNumber(item))
      {
         int ps = (int)item->valuedouble;
         cfg->db2_connection_pool_size = (ps < 1) ? 0 : (ps > 256) ? 256 : ps;
      }

      item = cJSON_GetObjectItemCaseSensitive(kb, "connection_workers");
      if (cJSON_IsNumber(item))
      {
         int cw = (int)item->valuedouble;
         cfg->kb_connection_workers = (cw < 1) ? 1 : (cw > 8) ? 8 : cw;
      }
      /* §2c: gate that makes the attended dim-change reset path available. */
      item = cJSON_GetObjectItemCaseSensitive(kb, "reembed_on_dim_change");
      if (cJSON_IsBool(item))
         cfg->kb_reembed_on_dim_change = cJSON_IsTrue(item) ? 1 : 0;

      /* Project-purge generation-fence TTL (webchat-project-lifecycle slice 2). */
      item = cJSON_GetObjectItemCaseSensitive(kb, "purge_fence_ttl_s");
      if (cJSON_IsNumber(item) && item->valuedouble > 0)
         cfg->kb_purge_fence_ttl_s = (int)item->valuedouble;

      cJSON *bg = cJSON_GetObjectItemCaseSensitive(kb, "background_ingest");
      if (cJSON_IsObject(bg))
      {
         item = cJSON_GetObjectItemCaseSensitive(bg, "enabled");
         if (cJSON_IsBool(item))
            cfg->kb_bg_ingest_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(bg, "interval_hours");
         if (cJSON_IsNumber(item))
            cfg->kb_bg_ingest_interval_hours = (int)item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(bg, "watch_enabled");
         if (cJSON_IsBool(item))
            cfg->kb_bg_watch_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(bg, "watch_debounce_secs");
         if (cJSON_IsNumber(item))
            cfg->kb_bg_watch_debounce_secs = (int)item->valuedouble;
      }

      cJSON *mining = cJSON_GetObjectItemCaseSensitive(kb, "mining");
      if (cJSON_IsObject(mining))
      {
         item = cJSON_GetObjectItemCaseSensitive(mining, "enabled");
         if (cJSON_IsBool(item))
            cfg->kb_mining_enabled = cJSON_IsTrue(item) ? 1 : 0;
         item = cJSON_GetObjectItemCaseSensitive(mining, "min_poll_s");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->kb_mining_min_poll_s = (int)item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(mining, "failure_learning_enabled");
         if (cJSON_IsBool(item))
            cfg->kb_mining_failure_learning_enabled = cJSON_IsTrue(item) ? 1 : 0;
      }

      /* §5 hybrid retrieval per-signal RRF weights + rank constant. */
      cJSON *ch = cJSON_GetObjectItemCaseSensitive(kb, "code_hybrid");
      if (cJSON_IsObject(ch))
      {
         item = cJSON_GetObjectItemCaseSensitive(ch, "weight_code");
         if (cJSON_IsNumber(item))
            cfg->code_hybrid_weight_code = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(ch, "weight_graph");
         if (cJSON_IsNumber(item))
            cfg->code_hybrid_weight_graph = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(ch, "weight_vector");
         if (cJSON_IsNumber(item))
            cfg->code_hybrid_weight_vector = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(ch, "weight_memory");
         if (cJSON_IsNumber(item))
            cfg->code_hybrid_weight_memory = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(ch, "surprising_precision_floor");
         if (cJSON_IsNumber(item))
            cfg->code_surprising_precision_floor = item->valuedouble;
         item = cJSON_GetObjectItemCaseSensitive(ch, "rrf_k");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->code_hybrid_rrf_k = item->valuedouble;
      }
   }
}

/* context.engine: name — re-homed from the deleted config_plugin.c so that a
 * single config_load() populates cfg->context_engine directly from the already
 * parsed aimee.yaml root (the old code re-opened and re-parsed the file). The
 * value drives context_engine_set_active() in server_main. */
void config_parse_context_engine(config_t *cfg, cJSON *root)
{
   cJSON *ctx = cJSON_GetObjectItemCaseSensitive(root, "context");
   if (!cJSON_IsObject(ctx))
      return;
   cJSON *eng = cJSON_GetObjectItemCaseSensitive(ctx, "engine");
   if (cJSON_IsString(eng) && eng->valuestring && eng->valuestring[0])
      snprintf(cfg->context_engine, sizeof(cfg->context_engine), "%s", eng->valuestring);
}
