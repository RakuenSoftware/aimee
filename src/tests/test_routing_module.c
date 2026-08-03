#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/routing/module_api.h>

extern aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body, uint32_t request_len,
    uint8_t *response_body, uint32_t response_capacity, uint32_t *response_len, void *user_data);

static int test_cancelled;

/* The adapter depends on the core-owned cancellation query. This focused unit
 * test supplies the boundary directly; module-runtime integration is covered by
 * test_module_runtime. */
int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return test_cancelled;
}

static aimee_module_status_t select_route(aimee_routing_select_mode_t mode, uint32_t count,
                                           uint64_t trace_id, uint32_t *selected)
{
   uint8_t request[AIMEE_ROUTING_REQUEST_LEN];
   uint8_t response[AIMEE_ROUTING_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {
       .stage_id = AIMEE_ROUTING_STAGE_SELECT,
       .trace_id = trace_id,
   };
   assert(aimee_routing_request_encode(mode, count, request, sizeof(request)) == 0);
   aimee_module_status_t status =
       aimee_module_handler(&invocation, request, sizeof(request), response, sizeof(response),
                            &response_len, NULL);
   if (status == AIMEE_MODULE_STATUS_OK)
      assert(aimee_routing_response_decode(response, response_len, count, selected) == 0);
   return status;
}

int main(void)
{
   uint32_t first = 0;
   assert(select_route(AIMEE_ROUTING_SELECT_BALANCED, 3, 1, &first) == AIMEE_MODULE_STATUS_OK);
   for (uint32_t i = 1; i < 7; ++i)
   {
      uint32_t selected = 0;
      assert(select_route(AIMEE_ROUTING_SELECT_BALANCED, 3, i + 1, &selected) ==
             AIMEE_MODULE_STATUS_OK);
      assert(selected == (first + i) % 3);
   }

   for (uint64_t trace = 1; trace <= 32; ++trace)
   {
      uint32_t selected = 0;
      assert(select_route(AIMEE_ROUTING_SELECT_RANDOMIZED, 5, trace, &selected) ==
             AIMEE_MODULE_STATUS_OK);
      assert(selected < 5);
   }

   uint8_t request[AIMEE_ROUTING_REQUEST_LEN] = {0};
   uint8_t response[AIMEE_ROUTING_RESPONSE_LEN] = {0};
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_ROUTING_STAGE_SELECT};
   assert(aimee_module_handler(&invocation, request, sizeof(request), response, sizeof(response),
                               &response_len, NULL) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(aimee_routing_request_encode(AIMEE_ROUTING_SELECT_BALANCED, 2, request,
                                       sizeof(request)) == 0);
   assert(aimee_module_handler(&invocation, request, sizeof(request), response,
                               AIMEE_ROUTING_RESPONSE_LEN - 1, &response_len,
                               NULL) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   invocation.stage_id++;
   assert(aimee_module_handler(&invocation, request, sizeof(request), response, sizeof(response),
                               &response_len, NULL) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   invocation.stage_id = AIMEE_ROUTING_STAGE_SELECT;
   test_cancelled = 1;
   assert(aimee_module_handler(&invocation, request, sizeof(request), response, sizeof(response),
                               &response_len, NULL) == AIMEE_MODULE_STATUS_CANCELLED);

   puts("test_routing_module: PASS");
   return 0;
}
