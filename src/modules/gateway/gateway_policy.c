/* gateway_policy.c: per-call gateway request/response policy. See gateway_policy.h.
 * CORE layer: cJSON + config + guardrails only; no DB, network, or agent state. */
#include "aimee.h" /* size macros for agent_types.h (MAX_PATH_LEN) */
#include <aimee/gateway/gateway_policy.h>
#include "config.h"
#include <stdlib.h> /* free — for the dropped-entry arguments cleanup in P2c */
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

/* Enforce-delegate-only: the server registers a provider that reports whether
 * usable aimee delegates exist (this CORE module must not read agent state, and
 * is linked into the client + tests without the server, so it can't call agent_*
 * directly). When the provider reports true, the gateway strips provider-native
 * sub-agent tools automatically, so a primary proxied through the gateway (Codex,
 * other OpenAI/Anthropic-shape providers) cannot spawn its own sub-agents. The
 * provider is expected to cache (it runs on the gateway hot path). */
static int (*g_delegates_available_provider)(void);

void gateway_policy_set_delegates_available_provider(int (*provider)(void))
{
   g_delegates_available_provider = provider;
}

int gateway_prevent_subagents_enabled(void)
{
   if (g_delegates_available_provider && g_delegates_available_provider())
      return 1;
   return config_gateway_prevent_subagents() ? 1 : 0;
}

static int police_ir_catalog(aimee_tool_t *tools, int *count)
{
   int removed = 0;
   for (int i = *count - 1; i >= 0; i--)
   {
      aimee_tool_t *tool = &tools[i];
      if (tool->kind == AIMEE_TOOL_NAMESPACE)
         removed += police_ir_catalog(tool->children, &tool->n_children);
      if ((tool->name && is_subagent_tool_name(tool->name)) ||
          (tool->kind == AIMEE_TOOL_NAMESPACE && !tool->n_children))
      {
         aimee_tool_free(tool);
         memmove(tool, tool + 1, (size_t)(*count - i - 1) * sizeof *tool);
         (*count)--;
         memset(&tools[*count], 0, sizeof *tool);
         removed++;
      }
   }
   return removed;
}
int gateway_policy_apply_ir(aimee_request_t *request)
{
   if (!request || !gateway_prevent_subagents_enabled())
      return 0;
   int removed = police_ir_catalog(request->tools, &request->n_tools);
   if (!removed)
      return 0;
   if (!request->n_tools ||
       (request->tool_choice == AIMEE_TOOL_CHOICE_TOOL && request->tool_choice_name &&
        is_subagent_tool_name(request->tool_choice_name)))
   {
      request->tool_choice = request->n_tools ? AIMEE_TOOL_CHOICE_AUTO : AIMEE_TOOL_CHOICE_UNSET;
      if (!request->n_tools)
         request->has_parallel_tool_calls = 0;
      free(request->tool_choice_name);
      request->tool_choice_name = NULL;
   }
   request->mutated = 1;
   return removed;
}

/* Response-side (P2c). Same canonical mapping and config gate as the request
 * side, exposed as a predicate so the buffered/streamed paths can gate cheaply
 * (the predicate does the legacy_config_read + canonical-name lookup, so the call
 * site stays one line). */
int gateway_policy_is_denied_tool(const char *name)
{
   if (!name || !name[0])
      return 0;
   if (!gateway_prevent_subagents_enabled())
      return 0;
   return is_subagent_tool_name(name);
}
