/* gateway_policy.c: per-call gateway request/response policy. See gateway_policy.h.
 * CORE layer: cJSON + config + guardrails only; no DB, network, or agent state. */
#include "gateway_policy.h"
#include "cJSON.h"
#include "config.h"
#include "json_fluent.h" /* jo_cstr */
#include <string.h>

/* From guardrails (guardrails_orchestrator.c). Forward-declared rather than
 * #include "guardrails.h": that header needs stdint/severity types pre-included,
 * and this module only needs the canonical-tool-name mapping. */
const char *guardrails_canonical_tool_name(const char *tool_name);

/* A subagent-spawning tool, via the shared guardrails canonicalization (which maps
 * Task/Agent/spawn_agent/RemoteTrigger -> "Subagent"). No duplicate name list. */
static int is_subagent_tool_name(const char *name)
{
   return name && name[0] && strcmp(guardrails_canonical_tool_name(name), "Subagent") == 0;
}

static int prevent_subagents_enabled(void)
{
   config_t cfg;
   config_load(&cfg);
   return cfg.gateway_prevent_subagents ? 1 : 0;
}

/* A tool entry's name, regardless of API shape. */
static const char *tool_entry_name(const cJSON *tool, int openai_shape)
{
   if (openai_shape)
      return jo_cstr(cJSON_GetObjectItemCaseSensitive((cJSON *)tool, "function"), "name");
   return jo_cstr(tool, "name");
}

/* The name a tool_choice object forces, or "" if it does not force a named tool. */
static const char *tool_choice_name(const cJSON *tc, int openai_shape)
{
   if (!cJSON_IsObject(tc))
      return "";
   if (openai_shape)
      return jo_cstr(cJSON_GetObjectItemCaseSensitive((cJSON *)tc, "function"), "name");
   return jo_cstr(tc, "name");
}

int gateway_policy_apply_request(cJSON *req, int tools_openai_shape)
{
   cJSON *tools;
   cJSON *t;
   int stripped = 0;

   if (!req || !prevent_subagents_enabled())
      return 0;

   tools = cJSON_GetObjectItemCaseSensitive(req, "tools");
   if (!cJSON_IsArray(tools))
      return 0;

   for (t = tools->child; t;)
   {
      cJSON *next = t->next;
      const char *name = tool_entry_name(t, tools_openai_shape);
      if (name && name[0] && is_subagent_tool_name(name))
      {
         cJSON_DetachItemViaPointer(tools, t);
         cJSON_Delete(t);
         stripped++;
      }
      t = next;
   }

   if (!stripped)
      return 0;

   if (cJSON_GetArraySize(tools) == 0)
   {
      /* No tools left: drop the empty array (providers reject `tools: []`) AND any
       * tool_choice — a forced/`any`/`required` choice with no tools is a 400
       * upstream, so leaving it would break the request. */
      cJSON_DeleteItemFromObjectCaseSensitive(req, "tools");
      cJSON_DeleteItemFromObjectCaseSensitive(req, "tool_choice");
   }
   else
   {
      /* Tools remain: relax tool_choice only if it forced a removed tool (absent
       * = auto). `auto`/`any`/`required` and choices naming a surviving tool are
       * left untouched. */
      cJSON *tc = cJSON_GetObjectItemCaseSensitive(req, "tool_choice");
      const char *forced = tool_choice_name(tc, tools_openai_shape);
      if (forced && forced[0] && is_subagent_tool_name(forced))
         cJSON_DeleteItemFromObjectCaseSensitive(req, "tool_choice");
   }

   return stripped;
}
