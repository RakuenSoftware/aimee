/* gateway_policy.c: per-call gateway request/response policy. See gateway_policy.h.
 * CORE layer: cJSON + config + guardrails only; no DB, network, or agent state. */
#include "aimee.h" /* size macros for agent_types.h (MAX_PATH_LEN) */
#include "gateway_policy.h"
#include "cJSON.h"
#include "config.h"
#include "json_fluent.h" /* jo_cstr */
#include <stdlib.h>      /* free — for the dropped-entry arguments cleanup in P2c */
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

static int pin_model_enabled(void)
{
   config_t cfg;
   config_load(&cfg);
   return cfg.gateway_pin_model ? 1 : 0;
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

int gateway_policy_strip_tools(cJSON *tools, int tools_openai_shape)
{
   cJSON *t;
   int stripped = 0;

   if (!cJSON_IsArray(tools) || !prevent_subagents_enabled())
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
   return stripped;
}

int gateway_policy_apply_request(cJSON *req, int tools_openai_shape)
{
   cJSON *tools;
   int stripped;

   if (!req)
      return 0;

   tools = cJSON_GetObjectItemCaseSensitive(req, "tools");
   stripped = gateway_policy_strip_tools(tools, tools_openai_shape);
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

int gateway_policy_pin_model(cJSON *req, const char *agent_model)
{
   const char *cur;

   if (!req || !agent_model || !agent_model[0] || !pin_model_enabled())
      return 0;
   cur = jo_cstr(req, "model");
   if (cur && strcmp(cur, agent_model) == 0)
      return 0; /* already the pinned model — nothing to change */
   cJSON_DeleteItemFromObjectCaseSensitive(req, "model");
   cJSON_AddStringToObject(req, "model", agent_model);
   return 1;
}

/* Response-side (P2c). Same canonical mapping and config gate as the request
 * side, exposed as a predicate so the buffered/streamed paths can gate cheaply
 * (the predicate does the config_load + canonical-name lookup, so the call
 * site stays one line). */
int gateway_policy_is_denied_tool(const char *name)
{
   if (!name || !name[0])
      return 0;
   if (!prevent_subagents_enabled())
      return 0;
   return is_subagent_tool_name(name);
}

/* Response-side tool policing: memmove-compact denied entries out of `p->calls[]`
 * and rewrite `p->stop_reason` to mirror what `anthropic_response_from_parsed`
 * would render from the surviving `call_count` (line 435 of
 * src/server/anthropic_ingress.c: `n_calls > 0 ? "tool_use" : "end_turn"`). See
 * gateway_policy.h for the full set of fields the function does NOT touch and
 * the rationale for the in-place memmove compaction. */
int gateway_policy_police_parsed_response(parsed_response_t *p)
{
   int drops = 0;
   int n, i, w;

   if (!p)
      return 0;
   if (!prevent_subagents_enabled())
      return 0;

   n = p->call_count;
   if (n <= 0)
      return 0;

   /* Single pass: read index `i`, write index `w`. Surviving entries are
    * shifted down to the front of the array in their original order. Dropped
    * entries' `arguments` strings are freed eagerly — the slot is then
    * overwritten by the next survivor, so `agent_free_parsed_response`'s
    * 0..call_count-1 sweep would otherwise leak the dropped strings. */
   w = 0;
   for (i = 0; i < n; i++)
   {
      const parsed_tool_call_t *src = &p->calls[i];
      if (is_subagent_tool_name(src->name))
      {
         free(src->arguments);
         p->calls[i].arguments = NULL;
         drops++;
         continue;
      }
      if (w != i)
         p->calls[w] = *src;
      w++;
   }
   p->call_count = w;
   /* Mirror the renderer's own stop_reason mapping so the audit row at
    * messages_buffered line 483 (which reads parsed.stop_reason) agrees with
    * the wire emitted by anthropic_response_from_parsed (which re-derives
    * from call_count). */
   snprintf(p->stop_reason, sizeof(p->stop_reason), "%s", w > 0 ? "tool_use" : "end_turn");
   return drops;
}
