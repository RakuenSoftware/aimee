#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/governance/module_api.h>

#include <stdio.h>
#include <string.h>

static int denied_tool(const char *name)
{
   return strcmp(name, "Agent") == 0 || strcmp(name, "spawn_agent") == 0 ||
          strcmp(name, "RemoteTrigger") == 0 || strcmp(name, "Task") == 0;
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   int policy_active = 0;
   char tool_names[AIMEE_GOVERNANCE_TOOL_COUNT_MAX][AIMEE_GOVERNANCE_TOOL_NAME_MAX + 1u];
   uint32_t call_count = 0;
   char original_reason[AIMEE_GOVERNANCE_STOP_REASON_MAX + 1u];
   if (!invocation || !response_len ||
       invocation->stage_id != AIMEE_GOVERNANCE_STAGE_EVALUATE ||
       response_capacity < AIMEE_GOVERNANCE_RESPONSE_LEN ||
       aimee_governance_request_decode(request_body, request_len, &policy_active, tool_names,
                                       &call_count, original_reason) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   uint32_t keep_mask = aimee_governance_mask_for_count(call_count);
   uint32_t drops = 0;
   char final_reason[AIMEE_GOVERNANCE_STOP_REASON_MAX + 1u];
   snprintf(final_reason, sizeof(final_reason), "%s", original_reason);
   if (policy_active && call_count > 0)
   {
      keep_mask = 0;
      for (uint32_t i = 0; i < call_count; ++i)
      {
         if (denied_tool(tool_names[i]))
            drops++;
         else
            keep_mask |= 1u << i;
      }
      uint32_t kept = call_count - drops;
      if (!original_reason[0] || kept == 0)
         snprintf(final_reason, sizeof(final_reason), "%s", kept > 0 ? "tool_use" : "end_turn");
   }

   if (aimee_governance_response_encode(keep_mask, drops, final_reason, response_body,
                                        response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_GOVERNANCE_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
