/* anthropic_shape.c: Anthropic request-shaping helpers (§3 cache-aware shaping).
 * Pure cJSON, no provider/config dependency, so it is unit-tested directly.
 * See agent_anthropic_set_system in agent_protocol.h. */
#include "aimee.h" /* MAX_PATH_LEN, pulled in before agent_types.h */
#include "agent_protocol.h"
#include "cJSON.h"

void agent_anthropic_set_system(cJSON *req, const char *system_prompt, int cache_marking)
{
   if (!req || !system_prompt || !system_prompt[0])
      return;
   if (!cache_marking)
   {
      cJSON_AddStringToObject(req, "system", system_prompt);
      return;
   }
   /* Content-block array with cache_control so the provider caches the stable
    * system prefix across calls, cutting cost for repeated system prompts. */
   cJSON *sys_arr = cJSON_CreateArray();
   cJSON *block = cJSON_CreateObject();
   cJSON_AddStringToObject(block, "type", "text");
   cJSON_AddStringToObject(block, "text", system_prompt);
   cJSON *cc = cJSON_CreateObject();
   cJSON_AddStringToObject(cc, "type", "ephemeral");
   cJSON_AddItemToObject(block, "cache_control", cc);
   cJSON_AddItemToArray(sys_arr, block);
   cJSON_AddItemToObject(req, "system", sys_arr);
}
