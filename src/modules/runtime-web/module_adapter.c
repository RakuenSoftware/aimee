#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/runtime-web/module_api.h>

#include <string.h>

static uint32_t rpc_fault_http_status(const char *kind)
{
   if (strcmp(kind, "invalid_argument") == 0)
      return 400u;
   if (strcmp(kind, "not_found") == 0)
      return 404u;
   if (strcmp(kind, "permission_denied") == 0)
      return 403u;
   if (strcmp(kind, "unavailable") == 0)
      return 503u;
   return 502u;
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   char kind[AIMEE_RUNTIME_WEB_KIND_MAX + 1u];
   if (!invocation || !response_len ||
       invocation->stage_id != AIMEE_RUNTIME_WEB_STAGE_CLASSIFY ||
       response_capacity < AIMEE_RUNTIME_WEB_RESPONSE_LEN ||
       aimee_runtime_web_request_decode(request_body, request_len, kind) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (aimee_runtime_web_response_encode(rpc_fault_http_status(kind), response_body,
                                         response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_RUNTIME_WEB_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
