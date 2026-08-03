#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/skills/module_api.h>

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len || invocation->stage_id != AIMEE_SKILLS_STAGE_CONTEXT ||
       !request_body || request_len != AIMEE_SKILLS_REQUEST_LEN ||
       response_capacity < AIMEE_SKILLS_RESPONSE_LEN ||
       aimee_skills_get_u32(request_body) != AIMEE_SKILLS_REQUEST_MAGIC ||
       aimee_skills_get_u32(request_body + 4) != AIMEE_SKILLS_WIRE_VERSION)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   int count = (int)(int32_t)aimee_skills_get_u32(request_body + 8);
   int interval = (int)(int32_t)aimee_skills_get_u32(request_body + 12);
   int fire = interval > 0 && count > 0 && (count % interval) == 0;
   aimee_skills_put_u32(response_body, AIMEE_SKILLS_RESPONSE_MAGIC);
   aimee_skills_put_u32(response_body + 4, (uint32_t)fire);
   *response_len = AIMEE_SKILLS_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
