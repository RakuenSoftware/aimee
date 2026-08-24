/* ensemble_render.c: an ensemble row as a document.
 *
 * This reads nothing. It takes what db1_ensemble_get or _list already returned
 * and shapes it for an MCP or HTTP reply, so it belongs on the caller's side of
 * the module boundary rather than in the store -- the same split as
 * diagnose_render.c and clarify_render.c.
 *
 * Leaving it in the module would have put a JSON writer behind the bus to serve
 * a rendering nobody persists, and forced the rendered document back across the
 * wire to reach the caller that asked for it.
 */
#include "cJSON.h"
#include "ensemble.h"

cJSON *db1_ensemble_info_to_json(const ensemble_info_t *info, const char *prompt_text,
                                 const char *context_text)
{
   if (!info)
      return NULL;

   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;

   cJSON_AddNumberToObject(obj, "id", info->id);
   cJSON_AddStringToObject(obj, "template", info->template_name);
   cJSON_AddStringToObject(obj, "channel", info->channel);
   cJSON_AddStringToObject(obj, "status", info->status);
   cJSON_AddNumberToObject(obj, "current_phase", info->current_phase);
   cJSON_AddNumberToObject(obj, "current_turn", info->current_turn);
   cJSON_AddNumberToObject(obj, "phase_count", info->phase_count);
   cJSON_AddNumberToObject(obj, "turns_in_phase", info->turns_in_phase);
   cJSON_AddStringToObject(obj, "phase_name", info->phase_name);
   cJSON_AddStringToObject(obj, "expected_agent", info->expected_agent);
   cJSON_AddStringToObject(obj, "expected_role", info->expected_role);
   cJSON_AddStringToObject(obj, "paused_reason", info->paused_reason);
   cJSON_AddStringToObject(obj, "created_at", info->created_at);
   cJSON_AddStringToObject(obj, "updated_at", info->updated_at);
   if (prompt_text)
      cJSON_AddStringToObject(obj, "next_prompt", prompt_text);
   if (context_text)
      cJSON_AddStringToObject(obj, "recent_context", context_text);

   return obj;
}
