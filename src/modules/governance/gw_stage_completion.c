/* gw_stage_completion.c -- keep an ordinary client-owned tool loop alive when
 * the model's own final answer proves that it stopped before completing a
 * confirmed, in-scope repair.  This is model/protocol neutral: it operates on
 * canonical responses and on the tools the API client already registered. */
#include "gw_stage_completion.h"

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

static int name_is_edit(const char *name)
{
   return text_has_ci(name, "apply_patch") || text_has_ci(name, "edit_file") ||
          text_has_ci(name, "write_file") || text_has_ci(name, "str_replace");
}

static const char tool_policy[] =
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

static const char completion_policy[] =
    "<aimee-completion-policy>The preceding response confirmed matching production defects "
    "during an active repair but deferred them. Treat a production instance you independently "
    "confirmed as the same defect as part of the repair unless the user explicitly excluded it. "
    "Your next response must use the available tools to complete every such instance and "
    "validate the full repair before responding. Preserve the existing success-path contract "
    "and avoid unrelated behavior changes. Do not ask permission and do not present known "
    "matching defects as optional follow-up.</aimee-completion-policy>";

static char *append_policy(const char *base, const char *policy)
{
   if (!base)
      base = "";
   size_t size = strlen(base), added = strlen(policy);
   char *merged = malloc(size + added + 2);
   if (!merged)
      return NULL;
   memcpy(merged, base, size);
   if (size)
      merged[size++] = '\n';
   memcpy(merged + size, policy, added + 1);
   return merged;
}

static int ir_history_signal(const aimee_request_t *request, int completion)
{
   int count = 0;
   if (!request)
      return 0;
   for (int i = 0; i < request->n_messages; i++)
   {
      const aimee_message_t *message = &request->messages[i];
      if (!message->role || strcmp(message->role, "assistant"))
         continue;
      for (int j = 0; j < message->n_blocks; j++)
      {
         const aimee_block_t *block = &message->blocks[j];
         if (block->type != AIMEE_BLK_TOOL_USE)
            continue;
         if (completion)
            count += block->tool_id && !strncmp(block->tool_id, AIMEE_COMPLETION_CALL_PREFIX,
                                                strlen(AIMEE_COMPLETION_CALL_PREFIX));
         else if (name_is_edit(block->tool_name) ||
                  json_has_text_ci(block->tool_input, "apply_patch") ||
                  json_has_text_ci(block->tool_input, "sed -i") ||
                  json_has_text_ci(block->tool_input, "perl -pi") ||
                  json_has_text_ci(block->tool_input, "cat >") ||
                  json_has_text_ci(block->tool_input, "tee "))
            count++;
      }
   }
   return count;
}

static int ir_shell_tool(const aimee_request_t *request, const char **name, const char **key)
{
   if (!request)
      return 0;
   static const char *const preferred[] = {"exec_command", "Bash",        "bash",
                                           "shell",        "run_command", NULL};
   for (int p = 0; preferred[p]; p++)
      for (int i = 0; i < request->n_tools; i++)
      {
         const aimee_tool_t *tool = &request->tools[i];
         if (!tool->name || strcmp(tool->name, preferred[p]))
            continue;
         const cJSON *properties = cJSON_GetObjectItemCaseSensitive(tool->schema, "properties");
         *name = tool->name;
         *key = cJSON_GetObjectItemCaseSensitive(properties, "cmd") ? "cmd" : "command";
         return 1;
      }
   return 0;
}

int gw_ir_response_completion_armed(const aimee_request_t *request)
{
   const char *name, *key;
   return ir_history_signal(request, 0) && !ir_history_signal(request, 1) &&
          ir_shell_tool(request, &name, &key);
}
char *gw_ir_request_tool_system_prompt(const aimee_request_t *request, const char *base)
{
   const char *name, *key;
   return ir_shell_tool(request, &name, &key) ? append_policy(base, tool_policy) : NULL;
}
char *gw_ir_request_completion_system_prompt(const aimee_request_t *request, const char *base)
{
   return ir_history_signal(request, 1) ? append_policy(base, completion_policy) : NULL;
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

int gw_ir_response_run_completion(aimee_response_t *response, const aimee_request_t *request)
{
   const char *name = NULL, *key = NULL;
   if (!response || aimee_ir_response_has_tool_use(response) || response->error_message ||
       response->n_content >= AIMEE_IR_MAX_BLOCKS || !gw_ir_response_completion_armed(request) ||
       !ir_shell_tool(request, &name, &key))
      return 0;
   char *text = aimee_ir_response_text_alloc(response);
   if (!text)
      return -1;
   int intervene = response_confirms_related_defect(text) && response_defers_repair(text);
   free(text);
   if (!intervene)
      return 0;
   aimee_block_t tool = {.type = AIMEE_BLK_TOOL_USE};
   char id[80];
   unsigned long seq = atomic_fetch_add(&g_completion_call_seq, 1);
   snprintf(id, sizeof id, AIMEE_COMPLETION_CALL_PREFIX "%lu", seq);
   tool.tool_id = strdup(id);
   tool.tool_name = strdup(name);
   tool.tool_input = cJSON_CreateObject();
   if (!tool.tool_id || !tool.tool_name || !tool.tool_input ||
       !cJSON_AddStringToObject(tool.tool_input, key, "true"))
   {
      aimee_block_free_contents(&tool);
      return -1;
   }
   aimee_block_t *grown =
       realloc(response->content, (size_t)(response->n_content + 1) * sizeof *grown);
   if (!grown)
   {
      aimee_block_free_contents(&tool);
      return -1;
   }
   aimee_ir_block_set_context(&tool, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_PUBLIC, 1, NULL, NULL, 0);
   response->content = grown;
   response->content[response->n_content++] = tool;
   response->stop_reason = AIMEE_STOP_TOOL_USE;
   free(response->raw_stop_reason);
   response->raw_stop_reason = NULL;
   return 1;
}
