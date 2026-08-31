/* gw_stage_completion.c -- keep an ordinary client-owned tool loop alive when
 * the model's own final answer proves that it stopped before completing a
 * confirmed, in-scope repair.  This is model/protocol neutral: it operates on
 * the parsed response and on the tools the API client already registered. */
#include "gw_stage_completion.h"

#include "aimee.h" /* size macros used by agent_protocol.h */
#include "agent_protocol.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_ulong g_completion_call_seq = 1;

static int text_has_ci(const char *haystack, const char *needle)
{
   if (!haystack || !needle || !needle[0])
      return 0;
   size_t nn = strlen(needle);
   for (const char *h = haystack; *h; h++)
   {
      size_t i = 0;
      while (i < nn && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
         i++;
      if (i == nn)
         return 1;
   }
   return 0;
}

static int json_has_text_ci(const cJSON *node, const char *needle)
{
   if (!node)
      return 0;
   if (cJSON_IsString(node) && text_has_ci(node->valuestring, needle))
      return 1;
   const cJSON *child;
   cJSON_ArrayForEach(child, node) if (json_has_text_ci(child, needle)) return 1;
   return 0;
}

static const char *tool_name(const cJSON *tool)
{
   const cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
   if (cJSON_IsString(name) && name->valuestring && name->valuestring[0])
      return name->valuestring;
   const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
   name = cJSON_IsObject(fn) ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
   return cJSON_IsString(name) ? name->valuestring : NULL;
}

static int name_is_edit(const char *name)
{
   return text_has_ci(name, "apply_patch") || text_has_ci(name, "edit_file") ||
          text_has_ci(name, "write_file") || text_has_ci(name, "str_replace");
}

static int arguments_are_edit(const cJSON *call)
{
   const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
   const cJSON *args = cJSON_GetObjectItemCaseSensitive(call, "arguments");
   if (!args && cJSON_IsObject(fn))
      args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
   if (!args)
      args = cJSON_GetObjectItemCaseSensitive(call, "input");
   return json_has_text_ci(args, "apply_patch") || json_has_text_ci(args, "sed -i") ||
          json_has_text_ci(args, "perl -pi") || json_has_text_ci(args, "cat >") ||
          json_has_text_ci(args, "tee ");
}

static int call_is_edit(const cJSON *call)
{
   return call && (name_is_edit(tool_name(call)) || arguments_are_edit(call));
}

static int call_is_completion(const cJSON *call)
{
   if (!call)
      return 0;
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
   if (!cJSON_IsString(id))
      id = cJSON_GetObjectItemCaseSensitive(call, "call_id");
   return cJSON_IsString(id) && id->valuestring &&
          strncmp(id->valuestring, AIMEE_COMPLETION_CALL_PREFIX,
                  strlen(AIMEE_COMPLETION_CALL_PREFIX)) == 0;
}

static int history_has_edit(const cJSON *messages)
{
   if (!cJSON_IsArray(messages))
      return 0;
   const cJSON *message;
   cJSON_ArrayForEach(message, messages)
   {
      const cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
      if (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0)
         continue;

      const cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
      const cJSON *call;
      if (cJSON_IsArray(calls))
         cJSON_ArrayForEach(call, calls) if (call_is_edit(call)) return 1;

      const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
      if (cJSON_IsArray(content))
      {
         cJSON_ArrayForEach(call, content)
         {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(call, "type");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "tool_use") == 0 &&
                call_is_edit(call))
               return 1;
         }
      }
   }
   return 0;
}

static int history_completion_call_count(const cJSON *messages)
{
   if (!cJSON_IsArray(messages))
      return 0;
   int count = 0;
   const cJSON *message;
   cJSON_ArrayForEach(message, messages)
   {
      const cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
      if (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0)
         continue;

      const cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
      const cJSON *call;
      if (cJSON_IsArray(calls))
         cJSON_ArrayForEach(call, calls) if (call_is_completion(call)) count++;

      const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
      if (cJSON_IsArray(content))
         cJSON_ArrayForEach(call, content) if (call_is_completion(call)) count++;
   }
   return count;
}

static int shell_tool(const cJSON *tools, const char **name_out, const char **arg_key_out)
{
   if (!cJSON_IsArray(tools))
      return 0;
   static const char *const preferred[] = {"exec_command", "Bash",        "bash",
                                           "shell",        "run_command", NULL};
   for (int p = 0; preferred[p]; p++)
   {
      const cJSON *tool;
      cJSON_ArrayForEach(tool, tools)
      {
         const char *name = tool_name(tool);
         if (!name || strcmp(name, preferred[p]) != 0)
            continue;
         const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
         const cJSON *params = cJSON_IsObject(fn)
                                   ? cJSON_GetObjectItemCaseSensitive(fn, "parameters")
                                   : cJSON_GetObjectItemCaseSensitive(tool, "parameters");
         const cJSON *props =
             cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "properties") : NULL;
         *name_out = name;
         *arg_key_out = cJSON_IsObject(props) && cJSON_GetObjectItemCaseSensitive(props, "cmd")
                            ? "cmd"
                            : "command";
         return 1;
      }
   }
   return 0;
}

int gw_response_completion_armed(const cJSON *messages, const cJSON *tools)
{
   const char *name = NULL, *key = NULL;
   return history_has_edit(messages) && shell_tool(tools, &name, &key) &&
          history_completion_call_count(messages) < 1;
}

char *gw_request_tool_system_prompt(const cJSON *tools, const char *base_system_prompt)
{
   const char *name = NULL, *key = NULL;
   if (!shell_tool(tools, &name, &key))
      return NULL;

   static const char policy[] =
       "<aimee-tool-policy>Aimee's CLI is the preferred registered repository-intelligence "
       "surface when a shell tool is available. REQUIRED FIRST STEP: before any repository read, "
       "search, edit, build, or test, run `aimee index investigate \"<plain-language summary "
       "of the task>\"`. Do not substitute an ordinary shell discovery command first. If client "
       "instructions name an exact Aimee executable, use that path; otherwise use `aimee`. If "
       "the command is unavailable, continue with ordinary tools and report the registration "
       "failure. Use targeted Aimee index and memory commands before broad shell searches. If "
       "investigate is unavailable or returns no evidence and the task concerns a repeated code "
       "shape, use `aimee index ast-grep` for that shape before ordinary shell search. MCP is the "
       "alternative only when no CLI is registered or a capability is explicitly MCP-only. For "
       "a requested code repair, scope the work to the defect's root cause, not merely the named "
       "location: before editing, search for the same defect pattern across production code. Any "
       "matching production instance you confirm is part of the repair unless the user explicitly "
       "excluded it. Fix and validate every confirmed matching instance while preserving the "
       "existing success-path contract; do not defer known matching defects as optional follow-up."
       "</aimee-tool-policy>";
   const char *base = base_system_prompt ? base_system_prompt : "";
   size_t base_len = strlen(base);
   size_t policy_len = strlen(policy);
   char *merged = malloc(base_len + policy_len + 2);
   if (!merged)
      return NULL;
   memcpy(merged, base, base_len);
   if (base_len)
      merged[base_len++] = '\n';
   memcpy(merged + base_len, policy, policy_len + 1);
   return merged;
}

char *gw_request_completion_system_prompt(const cJSON *messages, const char *base_system_prompt)
{
   if (history_completion_call_count(messages) == 0)
      return NULL;

   static const char policy[] =
       "<aimee-completion-policy>The preceding response confirmed matching production defects "
       "during an active repair but deferred them. Treat a production instance you independently "
       "confirmed as the same defect as part of the repair unless the user explicitly excluded it. "
       "Your next response must use the available tools to complete every such instance and "
       "validate the full repair before responding. Preserve the existing success-path contract "
       "and avoid unrelated behavior changes. Do not ask permission and do not present known "
       "matching defects as optional follow-up.</aimee-completion-policy>";
   const char *base = base_system_prompt ? base_system_prompt : "";
   size_t base_len = strlen(base);
   size_t policy_len = strlen(policy);
   char *merged = malloc(base_len + policy_len + 2);
   if (!merged)
      return NULL;
   memcpy(merged, base, base_len);
   if (base_len)
      merged[base_len++] = '\n';
   memcpy(merged + base_len, policy, policy_len + 1);
   return merged;
}

static int response_confirms_related_defect(const char *text)
{
   return text_has_ci(text, "related finding") || text_has_ci(text, "related issue") ||
          text_has_ci(text, "same issue") || text_has_ci(text, "same defect") ||
          text_has_ci(text, "same bug") || text_has_ci(text, "same vulnerability") ||
          (text_has_ci(text, "same") && text_has_ci(text, "pattern")) ||
          text_has_ci(text, "also vulnerable") || text_has_ci(text, "other vulnerable");
}

static int response_defers_repair(const char *text)
{
   return text_has_ci(text, "not changed") || text_has_ci(text, "not touched") ||
          text_has_ci(text, "left unchanged") || text_has_ci(text, "left unresolved") ||
          text_has_ci(text, "want me to") || text_has_ci(text, "if you want") ||
          text_has_ci(text, "if you'd like") || text_has_ci(text, "optional follow-up") ||
          text_has_ci(text, "follow-up") || text_has_ci(text, "follow up");
}

int gw_response_run_completion(parsed_response_t *parsed, const cJSON *messages, const cJSON *tools,
                               const char *tool_stop_reason)
{
   const char *name = NULL, *key = NULL;
   if (!parsed || parsed->is_tool_call || parsed->call_count != 0 || !parsed->content ||
       !response_confirms_related_defect(parsed->content) ||
       !response_defers_repair(parsed->content) || !gw_response_completion_armed(messages, tools) ||
       !shell_tool(tools, &name, &key))
      return 0;

   static const char command[] = "true";
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return 0;
   cJSON_AddStringToObject(args, key, command);
   char *arguments = cJSON_PrintUnformatted(args);
   cJSON_Delete(args);
   if (!arguments)
      return 0;

   parsed_tool_call_t *call = &parsed->calls[0];
   memset(call, 0, sizeof(*call));
   unsigned long seq = atomic_fetch_add(&g_completion_call_seq, 1);
   snprintf(call->id, sizeof(call->id), AIMEE_COMPLETION_CALL_PREFIX "%lu", seq);
   snprintf(call->name, sizeof(call->name), "%s", name);
   call->arguments = arguments;
   parsed->is_tool_call = 1;
   parsed->call_count = 1;
   snprintf(parsed->stop_reason, sizeof(parsed->stop_reason), "%s",
            tool_stop_reason && tool_stop_reason[0] ? tool_stop_reason : "tool_calls");
   return 1;
}
