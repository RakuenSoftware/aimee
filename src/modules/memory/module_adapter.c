#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/memory/module_api.h>

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len || invocation->stage_id != AIMEE_MEMORY_STAGE_RERANK ||
       !request_body || request_len != AIMEE_MEMORY_REQUEST_LEN ||
       response_capacity < AIMEE_MEMORY_RESPONSE_LEN ||
       aimee_memory_get_u32(request_body) != AIMEE_MEMORY_REQUEST_MAGIC ||
       aimee_memory_get_u32(request_body + 4) != AIMEE_MEMORY_WIRE_VERSION)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   int64_t score = aimee_memory_get_i64(request_body + 8);
   aimee_memory_confidence_t confidence = score >= 660000 ? AIMEE_MEMORY_CONFIDENCE_HIGH
                                          : score >= 330000
                                              ? AIMEE_MEMORY_CONFIDENCE_MEDIUM
                                              : AIMEE_MEMORY_CONFIDENCE_LOW;
   aimee_memory_put_u32(response_body, AIMEE_MEMORY_RESPONSE_MAGIC);
   aimee_memory_put_u32(response_body + 4, (uint32_t)confidence);
   *response_len = AIMEE_MEMORY_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
