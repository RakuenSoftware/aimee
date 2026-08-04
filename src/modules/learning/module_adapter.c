#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/learning/module_api.h>
#include "learning_signal_policy.h"

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   char signal[AIMEE_LEARNING_SIGNAL_MAX + 1];
   if (!invocation || !response_len || invocation->stage_id != AIMEE_LEARNING_STAGE_OBSERVE ||
       response_capacity < AIMEE_LEARNING_RESPONSE_LEN ||
       aimee_learning_request_decode(request_body, request_len, signal, sizeof(signal)) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   uint32_t mask = 0;
   if (learning_signal_policy_sink_mask(signal, &mask) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   aimee_learning_put_u32(response_body, AIMEE_LEARNING_RESPONSE_MAGIC);
   aimee_learning_put_u32(response_body + 4, mask);
   *response_len = AIMEE_LEARNING_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
