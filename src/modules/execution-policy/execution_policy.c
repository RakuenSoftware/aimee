/* execution_policy.c: fail-closed tool-action policy decision — loads the
 * operator policy (.aimee-policy.json) and decides whether a tool call is
 * allowed (policy_check_tool). Extracted from server/agent_policy.c (core
 * modularization); the decision contract is declared in the shared
 * headers/agent_exec.h, which this module implements. Schema/argument
 * validation (tool_validate) and side-effect classification stay with the
 * server/tools surface and are reached through the same header; the execution
 * trace, metrics, and manifest half of agent_policy.c likewise stays and
 * consumes this decision. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "db1.h"
#include "db2/tool_registry.h"
#include "agent.h"
#include "kb_client.h"
#include "headers/memory.h"
#include "headers/agent_exec.h"
#include "compact.h"
#include "computer_use.h"
#include "config.h"
#include "aimee/protocols/mcp/mcp_client_registry.h"
#include "log.h"
#include "dstr.h"
#include "otel.h"
#include "platform_path.h"
#include "cJSON.h"
#include "headers/agent_policy_intercept.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Policy checking --- */

static char *g_policy_json = NULL;

int policy_load(void)
{
   const char *paths[] = {".aimee-policy.json", NULL};
   char global_path[MAX_PATH_LEN];
   snprintf(global_path, sizeof(global_path), "%s/policy.json", config_default_dir());
   paths[1] = global_path;

   for (int i = 0; i < 2; i++)
   {
      FILE *f = fopen(paths[i], "r");
      if (!f)
         continue;
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (sz > 0 && sz < 1024 * 1024)
      {
         free(g_policy_json);
         g_policy_json = malloc((size_t)sz + 1);
         if (!g_policy_json)
         {
            fclose(f);
            return -1;
         }
         size_t nread = fread(g_policy_json, 1, (size_t)sz, f);
         if (ferror(f) || (long)nread != sz)
         {
            free(g_policy_json);
            g_policy_json = NULL;
            fclose(f);
            return -1;
         }
         g_policy_json[nread] = '\0';
      }
      fclose(f);
      return 0;
   }
   return -1;
}

int policy_check_tool(const char *tool_name, const char *side_effect, const char *args_json,
                      char *reason_out, size_t reason_len)
{
   if (computer_use_is_tool_name(tool_name))
   {
      computer_use_policy_t cu_policy;
      computer_use_policy_from_config(&cu_policy);
      computer_use_decision_t decision;
      char cu_reason[256] = "";
      if (computer_use_classify(&cu_policy, tool_name, args_json, &decision, cu_reason,
                                sizeof(cu_reason)) &&
          decision != COMPUTER_USE_DECISION_ALLOW)
      {
         if (reason_out && reason_len > 0)
            snprintf(reason_out, reason_len, "%s",
                     cu_reason[0] ? cu_reason : "computer-use action requires approval");
         return -1;
      }
   }

   /* Hardcoded discovery-shell intercept — fires unconditionally, no policy file required. */
   if (strcmp(tool_name, "bash") == 0 && args_json)
   {
      cJSON *args = cJSON_Parse(args_json);
      cJSON *cmd = args ? cJSON_GetObjectItem(args, "command") : NULL;
      if (cmd && cJSON_IsString(cmd) && policy_is_source_discovery(cmd->valuestring))
      {
         snprintf(reason_out, reason_len,
                  "Use `aimee index find <symbol>` or `aimee index overview` for code "
                  "discovery. Fall back to shell search only if aimee returns nothing.");
         cJSON_Delete(args);
         return -1;
      }
      cJSON_Delete(args);
   }

   if (!g_policy_json)
   {
      policy_load();
      if (!g_policy_json)
         return 0;
   }

   cJSON *policy = cJSON_Parse(g_policy_json);
   if (!policy)
      return 0;

   if (strcmp(tool_name, "bash") == 0 && args_json)
   {
      cJSON *args = cJSON_Parse(args_json);
      cJSON *cmd = args ? cJSON_GetObjectItem(args, "command") : NULL;

      cJSON *forbidden = cJSON_GetObjectItem(policy, "forbidden_commands");
      if (forbidden && cJSON_IsArray(forbidden) && cmd && cJSON_IsString(cmd))
      {
         int n = cJSON_GetArraySize(forbidden);
         for (int i = 0; i < n; i++)
         {
            cJSON *pat = cJSON_GetArrayItem(forbidden, i);
            if (pat && cJSON_IsString(pat) && strstr(cmd->valuestring, pat->valuestring))
            {
               snprintf(reason_out, reason_len, "command matches forbidden pattern: %s",
                        pat->valuestring);
               cJSON_Delete(args);
               cJSON_Delete(policy);
               return -1;
            }
         }
      }

      cJSON_Delete(args);
   }

   /* Check tool_rules path-prefix restrictions (Feature 4) */
   cJSON *tool_rules = cJSON_GetObjectItem(policy, "tool_rules");
   if (tool_rules && cJSON_IsArray(tool_rules) && args_json)
   {
      /* Extract path from tool args */
      cJSON *args = cJSON_Parse(args_json);
      const char *target_path = NULL;
      if (args)
      {
         cJSON *p = cJSON_GetObjectItem(args, "path");
         if (p && cJSON_IsString(p))
            target_path = p->valuestring;
         else
         {
            cJSON *cmd = cJSON_GetObjectItem(args, "command");
            if (cmd && cJSON_IsString(cmd))
               target_path = cmd->valuestring;
         }
      }

      if (target_path)
      {
         int n = cJSON_GetArraySize(tool_rules);
         for (int i = 0; i < n; i++)
         {
            cJSON *rule = cJSON_GetArrayItem(tool_rules, i);
            if (!rule)
               continue;
            cJSON *prefix = cJSON_GetObjectItem(rule, "path_prefix");
            if (!prefix || !cJSON_IsString(prefix))
               continue;

            size_t plen = strlen(prefix->valuestring);
            if (strncmp(target_path, prefix->valuestring, plen) == 0 &&
                (target_path[plen] == '/' || target_path[plen] == '\0'))
            {
               /* Path matches this rule; check if tool is allowed */
               cJSON *allowed = cJSON_GetObjectItem(rule, "allowed_tools");
               if (allowed && cJSON_IsArray(allowed))
               {
                  int found = 0;
                  int an = cJSON_GetArraySize(allowed);
                  for (int j = 0; j < an; j++)
                  {
                     cJSON *at = cJSON_GetArrayItem(allowed, j);
                     if (at && cJSON_IsString(at) && strcmp(at->valuestring, tool_name) == 0)
                     {
                        found = 1;
                        break;
                     }
                  }
                  if (!found)
                  {
                     snprintf(reason_out, reason_len, "tool '%s' not allowed for path %s",
                              tool_name, prefix->valuestring);
                     cJSON_Delete(args);
                     cJSON_Delete(policy);
                     return -1;
                  }
               }
               break; /* First matching prefix wins */
            }
         }
      }
      cJSON_Delete(args);
   }

   cJSON *levels = cJSON_GetObjectItem(policy, "approval_levels");
   if (levels && side_effect)
   {
      cJSON *level = cJSON_GetObjectItem(levels, side_effect);
      if (level && cJSON_IsString(level) && strcmp(level->valuestring, "block") == 0)
      {
         snprintf(reason_out, reason_len, "policy blocks %s operations", side_effect);
         cJSON_Delete(policy);
         return -1;
      }
   }

   cJSON_Delete(policy);
   return 0;
}
