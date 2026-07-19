#include "aimee.h"
#include "config.h"
#include "json_fluent.h"
#include "config_memory.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int config_parse_disposition_entries(const cJSON *obj, const char *path,
                                            config_disposition_t *out, int *count,
                                            config_disposition_source_t source)
{
   int issues = 0;
   const cJSON *entry;
   if (!cJSON_IsObject(obj))
      return config_issue("\"%s\" expected object, got %s", path, jo_type_name(obj));

   cJSON_ArrayForEach(entry, obj)
   {
      char *end = NULL;
      double value = 0.0;
      if (!entry->string || !entry->string[0])
         continue;
      if (cJSON_IsNumber(entry))
         value = entry->valuedouble;
      else if (cJSON_IsString(entry) && entry->valuestring && entry->valuestring[0])
      {
         value = strtod(entry->valuestring, &end);
         if (!end || *end != '\0')
         {
            issues += config_issue("\"%s.%s\" expected number, got %s", path, entry->string,
                                   jo_type_name(entry));
            continue;
         }
      }
      else
      {
         issues += config_issue("\"%s.%s\" expected number, got %s", path, entry->string,
                                jo_type_name(entry));
         continue;
      }
      if (value < 0.0 || value > 1.0)
      {
         issues += config_issue("\"%s.%s\" must be in [0,1], got %.3f", path, entry->string, value);
         continue;
      }
      if (*count >= CONFIG_MAX_DISPOSITIONS)
      {
         issues += config_issue("too many disposition traits in %s (max %d)", path,
                                CONFIG_MAX_DISPOSITIONS);
         break;
      }

      config_disposition_t *slot = &out[(*count)++];
      memset(slot, 0, sizeof(*slot));
      snprintf(slot->name, sizeof(slot->name), "%s", entry->string);
      slot->value = value;
      slot->source = source;
   }

   return issues;
}

static void config_merge_disposition_scope(config_t *cfg, const config_disposition_t *scope_entries,
                                           int scope_count, config_disposition_source_t source)
{
   for (int i = 0; i < scope_count; i++)
   {
      int found = -1;
      for (int j = 0; j < cfg->disposition_count; j++)
      {
         if (strcmp(cfg->dispositions[j].name, scope_entries[i].name) == 0)
         {
            found = j;
            break;
         }
      }

      if (found >= 0)
      {
         cfg->dispositions[found].value = scope_entries[i].value;
         cfg->dispositions[found].source = source;
         continue;
      }

      if (cfg->disposition_count >= CONFIG_MAX_DISPOSITIONS)
         break;

      cfg->dispositions[cfg->disposition_count] = scope_entries[i];
      cfg->dispositions[cfg->disposition_count].source = source;
      cfg->disposition_count++;
   }
}

int config_parse_dispositions(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *disp = cJSON_GetObjectItemCaseSensitive(memory, "dispositions");
   if (!disp)
      return 0;
   if (!cJSON_IsObject(disp))
      return config_issue("\"memory.dispositions\" expected object, got %s", jo_type_name(disp));

   int issues = 0;
   int scoped = 0;
   const cJSON *global = cJSON_GetObjectItemCaseSensitive(disp, "global");
   const cJSON *workspace = cJSON_GetObjectItemCaseSensitive(disp, "workspace");
   const cJSON *project = cJSON_GetObjectItemCaseSensitive(disp, "project");
   if ((global && cJSON_IsObject(global)) || (workspace && cJSON_IsObject(workspace)) ||
       (project && cJSON_IsObject(project)))
   {
      scoped = 1;
      const cJSON *entry;
      cJSON_ArrayForEach(entry, disp)
      {
         if (!entry->string || !entry->string[0])
            continue;
         if (strcmp(entry->string, "global") == 0 || strcmp(entry->string, "workspace") == 0 ||
             strcmp(entry->string, "project") == 0)
            continue;
         issues +=
             config_issue("\"memory.dispositions\" cannot mix scoped blocks with flat trait \"%s\"",
                          entry->string);
      }
   }

   if (scoped)
   {
      if (global)
         issues += config_parse_disposition_entries(
             global, "memory.dispositions.global", cfg->disposition_globals,
             &cfg->disposition_global_count, CONFIG_DISPOSITION_SOURCE_GLOBAL);
      if (workspace)
         issues += config_parse_disposition_entries(
             workspace, "memory.dispositions.workspace", cfg->disposition_workspaces,
             &cfg->disposition_workspace_count, CONFIG_DISPOSITION_SOURCE_WORKSPACE);
      if (project)
         issues += config_parse_disposition_entries(
             project, "memory.dispositions.project", cfg->disposition_projects,
             &cfg->disposition_project_count, CONFIG_DISPOSITION_SOURCE_PROJECT);
   }
   else
   {
      issues += config_parse_disposition_entries(
          disp, "memory.dispositions", cfg->disposition_globals, &cfg->disposition_global_count,
          CONFIG_DISPOSITION_SOURCE_GLOBAL);
   }

   config_merge_disposition_scope(cfg, cfg->disposition_globals, cfg->disposition_global_count,
                                  CONFIG_DISPOSITION_SOURCE_GLOBAL);
   config_merge_disposition_scope(cfg, cfg->disposition_workspaces,
                                  cfg->disposition_workspace_count,
                                  CONFIG_DISPOSITION_SOURCE_WORKSPACE);
   config_merge_disposition_scope(cfg, cfg->disposition_projects, cfg->disposition_project_count,
                                  CONFIG_DISPOSITION_SOURCE_PROJECT);

   return issues;
}

int config_parse_memory_citations(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *citations = cJSON_GetObjectItemCaseSensitive(memory, "citations");
   if (!citations)
      return 0;
   if (!cJSON_IsObject(citations))
      return config_issue("\"memory.citations\" expected object, got %s", jo_type_name(citations));

   int issues = 0;
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(citations, "mode");
   if (item)
   {
      if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
         issues +=
             config_issue("\"memory.citations.mode\" expected string, got %s", jo_type_name(item));
      else if (strcmp(item->valuestring, "off") != 0 &&
               strcmp(item->valuestring, "required") != 0 &&
               strcmp(item->valuestring, "verify") != 0)
         issues += config_issue(
             "\"memory.citations.mode\" must be one of off|required|verify, got \"%s\"",
             item->valuestring);
      else
         snprintf(cfg->memory_citations_mode, sizeof(cfg->memory_citations_mode), "%s",
                  item->valuestring);
   }

   item = cJSON_GetObjectItemCaseSensitive(citations, "reprompt_on_miss");
   if (item)
   {
      if (!cJSON_IsBool(item))
         issues += config_issue("\"memory.citations.reprompt_on_miss\" expected boolean, got %s",
                                jo_type_name(item));
      else
         cfg->memory_citations_reprompt_on_miss = cJSON_IsTrue(item) ? 1 : 0;
   }

   item = cJSON_GetObjectItemCaseSensitive(citations, "strip_unverified");
   if (item)
   {
      if (!cJSON_IsBool(item))
         issues += config_issue("\"memory.citations.strip_unverified\" expected boolean, got %s",
                                jo_type_name(item));
      else
         cfg->memory_citations_strip_unverified = cJSON_IsTrue(item) ? 1 : 0;
   }

   return issues;
}

int config_parse_memory_briefing(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *briefing = cJSON_GetObjectItemCaseSensitive(memory, "briefing");
   if (!briefing)
      return 0;
   if (!cJSON_IsObject(briefing))
      return config_issue("\"memory.briefing\" expected object, got %s", jo_type_name(briefing));

   int issues = 0;
   const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(briefing, "enabled");
   if (enabled)
   {
      if (!cJSON_IsBool(enabled))
         issues += config_issue("\"memory.briefing.enabled\" expected boolean, got %s",
                                jo_type_name(enabled));
      else
         cfg->memory_briefing_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }

   const cJSON *limit = cJSON_GetObjectItemCaseSensitive(briefing, "limit_tokens");
   if (limit)
   {
      if (!cJSON_IsNumber(limit))
         issues += config_issue("\"memory.briefing.limit_tokens\" expected number, got %s",
                                jo_type_name(limit));
      else
         cfg->memory_briefing_limit_tokens = (int)limit->valuedouble;
   }

   return issues;
}

int config_parse_memory_aggregation(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *agg = cJSON_GetObjectItemCaseSensitive(memory, "aggregation");
   if (!agg)
      return 0;
   if (!cJSON_IsObject(agg))
      return config_issue("\"memory.aggregation\" expected object, got %s", jo_type_name(agg));

   int issues = 0;
   const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(agg, "enabled");
   if (enabled)
   {
      if (!cJSON_IsBool(enabled))
         issues += config_issue("\"memory.aggregation.enabled\" expected boolean, got %s",
                                jo_type_name(enabled));
      else
         cfg->memory_aggregation_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }
   const cJSON *max_items = cJSON_GetObjectItemCaseSensitive(agg, "max_items");
   if (max_items)
   {
      if (!cJSON_IsNumber(max_items))
         issues += config_issue("\"memory.aggregation.max_items\" expected number, got %s",
                                jo_type_name(max_items));
      else
         cfg->memory_aggregation_max_items = (int)max_items->valuedouble;
   }
   return issues;
}

int config_parse_memory_prospective(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *prosp = cJSON_GetObjectItemCaseSensitive(memory, "prospective");
   if (!prosp)
      return 0;
   if (!cJSON_IsObject(prosp))
      return config_issue("\"memory.prospective\" expected object, got %s", jo_type_name(prosp));

   int issues = 0;
   const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(prosp, "enabled");
   if (enabled)
   {
      if (!cJSON_IsBool(enabled))
         issues += config_issue("\"memory.prospective.enabled\" expected boolean, got %s",
                                jo_type_name(enabled));
      else
         cfg->memory_prospective_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }
   const cJSON *max_matches = cJSON_GetObjectItemCaseSensitive(prosp, "max_matches");
   if (max_matches)
   {
      if (!cJSON_IsNumber(max_matches))
         issues += config_issue("\"memory.prospective.max_matches\" expected number, got %s",
                                jo_type_name(max_matches));
      else
         cfg->memory_prospective_max_matches = (int)max_matches->valuedouble;
   }
   return issues;
}

int config_parse_memory_lifecycle(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *lc = cJSON_GetObjectItemCaseSensitive(memory, "lifecycle");
   if (!lc)
      return 0;
   if (!cJSON_IsObject(lc))
      return config_issue("\"memory.lifecycle\" expected object, got %s", jo_type_name(lc));

   int issues = 0;
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(lc, "enabled");
   if (item)
   {
      if (!cJSON_IsBool(item))
         issues += config_issue("\"memory.lifecycle.enabled\" expected boolean, got %s",
                                jo_type_name(item));
      else
         cfg->memory_lifecycle_enabled = cJSON_IsTrue(item) ? 1 : 0;
   }
   item = cJSON_GetObjectItemCaseSensitive(lc, "hide_archived");
   if (item)
   {
      if (!cJSON_IsBool(item))
         issues += config_issue("\"memory.lifecycle.hide_archived\" expected boolean, got %s",
                                jo_type_name(item));
      else
         cfg->memory_lifecycle_hide_archived = cJSON_IsTrue(item) ? 1 : 0;
   }
   item = cJSON_GetObjectItemCaseSensitive(lc, "ttl_date_days");
   if (item)
   {
      if (!cJSON_IsNumber(item))
         issues += config_issue("\"memory.lifecycle.ttl_date_days\" expected number, got %s",
                                jo_type_name(item));
      else
         cfg->memory_lifecycle_ttl_date_days = (int)item->valuedouble;
   }
   item = cJSON_GetObjectItemCaseSensitive(lc, "ttl_relative_days");
   if (item)
   {
      if (!cJSON_IsNumber(item))
         issues += config_issue("\"memory.lifecycle.ttl_relative_days\" expected number, got %s",
                                jo_type_name(item));
      else
         cfg->memory_lifecycle_ttl_relative_days = (int)item->valuedouble;
   }
   item = cJSON_GetObjectItemCaseSensitive(lc, "ttl_open_ended_days");
   if (item)
   {
      if (!cJSON_IsNumber(item))
         issues += config_issue("\"memory.lifecycle.ttl_open_ended_days\" expected number, got %s",
                                jo_type_name(item));
      else
         cfg->memory_lifecycle_ttl_open_ended_days = (int)item->valuedouble;
   }
   return issues;
}

int config_parse_memory_recall(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *rc = cJSON_GetObjectItemCaseSensitive(memory, "recall");
   if (!rc)
      return 0;
   if (!cJSON_IsObject(rc))
      return config_issue("\"memory.recall\" expected object, got %s", jo_type_name(rc));

   int issues = 0;
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(rc, "enabled");
   if (item)
   {
      if (!cJSON_IsBool(item))
         issues +=
             config_issue("\"memory.recall.enabled\" expected boolean, got %s", jo_type_name(item));
      else
         cfg->memory_recall_enabled = cJSON_IsTrue(item) ? 1 : 0;
   }
   item = cJSON_GetObjectItemCaseSensitive(rc, "limit_tokens_session");
   if (item)
   {
      if (!cJSON_IsNumber(item))
         issues += config_issue("\"memory.recall.limit_tokens_session\" expected number, got %s",
                                jo_type_name(item));
      else
         cfg->memory_recall_limit_tokens_session = (int)item->valuedouble;
   }
   item = cJSON_GetObjectItemCaseSensitive(rc, "limit_tokens_turn");
   if (item)
   {
      if (!cJSON_IsNumber(item))
         issues += config_issue("\"memory.recall.limit_tokens_turn\" expected number, got %s",
                                jo_type_name(item));
      else
         cfg->memory_recall_limit_tokens_turn = (int)item->valuedouble;
   }
   return issues;
}

int config_parse_memory_directives(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *dc = cJSON_GetObjectItemCaseSensitive(memory, "directives");
   if (!dc)
      return 0;
   if (!cJSON_IsObject(dc))
      return config_issue("\"memory.directives\" expected object, got %s", jo_type_name(dc));

   int issues = 0;
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(dc, "enabled");
   if (item)
   {
      if (!cJSON_IsBool(item))
         issues += config_issue("\"memory.directives.enabled\" expected boolean, got %s",
                                jo_type_name(item));
      else
         cfg->memory_directives_enabled = cJSON_IsTrue(item) ? 1 : 0;
   }
   item = cJSON_GetObjectItemCaseSensitive(dc, "failure_threshold");
   if (item)
   {
      if (!cJSON_IsNumber(item))
         issues += config_issue("\"memory.directives.failure_threshold\" expected number, got %s",
                                jo_type_name(item));
      else
         cfg->memory_directives_failure_threshold = (int)item->valuedouble;
   }
   item = cJSON_GetObjectItemCaseSensitive(dc, "max_matches");
   if (item)
   {
      if (!cJSON_IsNumber(item))
         issues += config_issue("\"memory.directives.max_matches\" expected number, got %s",
                                jo_type_name(item));
      else
         cfg->memory_directives_max_matches = (int)item->valuedouble;
   }
   return issues;
}

int config_parse_memory_cognify(config_t *cfg, const cJSON *root)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(root, "memory");
   if (!memory)
      return 0;
   if (!cJSON_IsObject(memory))
      return config_issue("\"memory\" expected object, got %s", jo_type_name(memory));

   const cJSON *cognify = cJSON_GetObjectItemCaseSensitive(memory, "cognify");
   if (!cognify)
      return 0;
   if (!cJSON_IsObject(cognify))
      return config_issue("\"memory.cognify\" expected object, got %s", jo_type_name(cognify));

   const cJSON *async_cfg = cJSON_GetObjectItemCaseSensitive(cognify, "async");
   if (!async_cfg)
      return 0;
   if (!cJSON_IsObject(async_cfg))
      return config_issue("\"memory.cognify.async\" expected object, got %s",
                          jo_type_name(async_cfg));

   const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(async_cfg, "enabled");
   if (!enabled)
      return 0;
   if (!cJSON_IsBool(enabled))
      return config_issue("\"memory.cognify.async.enabled\" expected boolean, got %s",
                          jo_type_name(enabled));
   cfg->memory_cognify_async_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   return 0;
}
