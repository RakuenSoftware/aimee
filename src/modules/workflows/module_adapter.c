#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/workflows/module_api.h>

#include <string.h>

static aimee_workflows_advance_outcome_t decide(
    const aimee_workflows_advance_request_t *request)
{
   if (!request->work_item[0] || !request->observed_stage[0])
      return AIMEE_WORKFLOWS_ADVANCE_BADARGS;
   if (!request->bound_work_item[0] ||
       strcmp(request->bound_work_item, request->work_item) != 0)
      return AIMEE_WORKFLOWS_ADVANCE_UNBOUND;
   if (request->have_nonce && request->last_nonce[0] &&
       strcmp(request->nonce, request->last_nonce) == 0)
      return AIMEE_WORKFLOWS_ADVANCE_REPLAY;
   if (strcmp(request->actual_state, "accepted") == 0 ||
       strcmp(request->actual_state, "rejected") == 0 ||
       strcmp(request->actual_state, "abandoned") == 0)
      return AIMEE_WORKFLOWS_ADVANCE_TERMINAL;
   if (!request->actual_stage[0] ||
       strcmp(request->observed_stage, request->actual_stage) != 0)
      return AIMEE_WORKFLOWS_ADVANCE_STALE;
   return AIMEE_WORKFLOWS_ADVANCE_OK;
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   aimee_workflows_advance_request_t request;
   if (!invocation || !response_len ||
       invocation->stage_id != AIMEE_WORKFLOWS_STAGE_ADVANCE ||
       response_capacity < AIMEE_WORKFLOWS_RESPONSE_LEN ||
       aimee_workflows_request_decode(request_body, request_len, &request) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (aimee_workflows_response_encode(decide(&request), response_body, response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_WORKFLOWS_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
