#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/routing/module_api.h>

#include <stdatomic.h>

static atomic_uint balanced_cursor;

static uint64_t mix64(uint64_t value)
{
   value ^= value >> 30;
   value *= 0xbf58476d1ce4e5b9ULL;
   value ^= value >> 27;
   value *= 0x94d049bb133111ebULL;
   return value ^ (value >> 31);
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body, uint32_t request_len,
    uint8_t *response_body, uint32_t response_capacity, uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len || invocation->stage_id != AIMEE_ROUTING_STAGE_SELECT)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   aimee_routing_select_mode_t mode;
   uint32_t candidate_count = 0;
   if (aimee_routing_request_decode(request_body, request_len, &mode, &candidate_count) != 0 ||
       response_capacity < AIMEE_ROUTING_RESPONSE_LEN)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   uint32_t selected = 0;
   if (mode == AIMEE_ROUTING_SELECT_BALANCED)
      selected = atomic_fetch_add_explicit(&balanced_cursor, 1u, memory_order_relaxed) %
                 candidate_count;
   else
      selected = (uint32_t)(mix64(invocation->trace_id) % candidate_count);

   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (aimee_routing_response_encode(selected, response_body, response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_ROUTING_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
