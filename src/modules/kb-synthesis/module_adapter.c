#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/kb-synthesis/module_api.h>

#include "cJSON.h"
#include "kb_curator_grounding.h"

static cJSON *claim_payload(const aimee_kb_synthesis_grounding_request_t *request)
{
   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return NULL;
   cJSON *value = NULL;
   switch (request->claim_kind)
   {
   case AIMEE_KB_SYNTHESIS_CLAIM_NONE:
      return payload;
   case AIMEE_KB_SYNTHESIS_CLAIM_STRING:
      value = cJSON_CreateString(request->claims[0]);
      break;
   case AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY:
      value = cJSON_CreateArray();
      if (value)
         for (uint32_t i = 0; i < request->claim_count; ++i)
         {
            cJSON *item = cJSON_CreateString(request->claims[i]);
            if (!item || !cJSON_AddItemToArray(value, item))
            {
               cJSON_Delete(item);
               cJSON_Delete(value);
               cJSON_Delete(payload);
               return NULL;
            }
         }
      break;
   case AIMEE_KB_SYNTHESIS_CLAIM_NONSTRING:
      value = cJSON_CreateBool(1);
      break;
   }
   if (!value || !cJSON_AddItemToObject(payload, "side_effects", value))
   {
      cJSON_Delete(value);
      cJSON_Delete(payload);
      return NULL;
   }
   return payload;
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   aimee_kb_synthesis_grounding_request_t request;
   if (!invocation || !response_len ||
       invocation->stage_id != AIMEE_KB_SYNTHESIS_STAGE_GROUNDING ||
       response_capacity < AIMEE_KB_SYNTHESIS_RESPONSE_LEN ||
       aimee_kb_synthesis_request_decode(request_body, request_len, &request) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   cJSON *payload = claim_payload(&request);
   if (!payload)
      return AIMEE_MODULE_STATUS_INTERNAL;
   const char *callees[AIMEE_KB_SYNTHESIS_CALLEE_COUNT_MAX];
   for (uint32_t i = 0; i < request.callee_count; ++i)
      callees[i] = request.callees[i];
   char reason[AIMEE_KB_SYNTHESIS_TEXT_MAX + 1u];
   int contradicts = kb_curator_grounding_contradicts(
       payload, callees, (int)request.callee_count, reason, sizeof(reason));
   cJSON_Delete(payload);
   if (aimee_kb_synthesis_response_encode(contradicts, reason, response_body,
                                           response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_KB_SYNTHESIS_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
