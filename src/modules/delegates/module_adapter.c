#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/delegates/module_api.h>

#include <string.h>

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   char role[AIMEE_DELEGATES_ROLE_MAX + 1];
   if (!invocation || !response_len || invocation->stage_id != AIMEE_DELEGATES_STAGE_INVOKE ||
       response_capacity < AIMEE_DELEGATES_MESSAGE_LEN ||
       aimee_delegates_message_decode(request_body, request_len, AIMEE_DELEGATES_REQUEST_MAGIC,
                                      role, sizeof(role)) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (aimee_delegates_message_encode(AIMEE_DELEGATES_RESPONSE_MAGIC,
                                      aimee_delegates_role_canonical(role), response_body,
                                      response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_DELEGATES_MESSAGE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
