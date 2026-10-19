#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/tools/module_api.h>

#include <string.h>

#include "tool_catalog_data.h"

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   char name[AIMEE_TOOLS_METADATA_NAME_MAX + 1];
   if (!invocation || !response_len || !request_body || !response_body ||
       invocation->stage_id != AIMEE_TOOLS_STAGE_DISPATCH || request_len < 8)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   *response_len = 0;
   int enriched = request_body[4] == AIMEE_TOOLS_METADATA_VERSION;
   if (response_capacity < (enriched ? AIMEE_TOOLS_METADATA_LEN : AIMEE_TOOLS_RESPONSE_LEN) ||
       (enriched ? aimee_tools_metadata_request_decode(request_body, request_len, name, sizeof name)
                 : aimee_tools_request_decode(request_body, request_len, name, sizeof name)) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   aimee_tool_metadata_t result = aimee_tools_catalog_metadata(name);
   aimee_tools_put_u32(response_body,
                       enriched ? AIMEE_TOOLS_METADATA_MAGIC : AIMEE_TOOLS_RESPONSE_MAGIC);
   aimee_tools_put_u32(response_body + 4, (uint32_t)result.classification);
   if (enriched)
   {
      aimee_tools_put_u32(response_body + 8, result.primary);
      aimee_tools_put_u32(response_body + 12, result.categories);
   }
   *response_len = enriched ? AIMEE_TOOLS_METADATA_LEN : AIMEE_TOOLS_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
