#define _POSIX_C_SOURCE 200809L

#include "module_routing_adapter.h"

#include "agent_config.h"
#include "log.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/routing/module_api.h>

#include <stdatomic.h>
#include <time.h>

#define ROUTING_DEADLINE_NS (500ULL * 1000000ULL)

static atomic_uint_fast64_t next_trace = 1;

static uint64_t monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int select_via_module(int randomized, uint32_t candidate_count, uint32_t *selected_index)
{
   uint8_t request[AIMEE_ROUTING_REQUEST_LEN];
   uint8_t response[AIMEE_ROUTING_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_routing_select_mode_t mode =
       randomized ? AIMEE_ROUTING_SELECT_RANDOMIZED : AIMEE_ROUTING_SELECT_BALANCED;
   if (!selected_index ||
       aimee_routing_request_encode(mode, candidate_count, request, sizeof(request)) != 0)
      return -1;
   uint64_t now = monotonic_ns();
   if (!now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   aimee_module_call_result_t result = obs_bus_module_call(
       AIMEE_ROUTING_EVENT_KIND, AIMEE_ROUTING_STAGE_SELECT, trace, now + ROUTING_DEADLINE_NS,
       request, sizeof(request), response, sizeof(response), &response_len, NULL, NULL);
   if (result != AIMEE_MODULE_CALL_OK ||
       aimee_routing_response_decode(response, response_len, candidate_count, selected_index) != 0)
   {
      /* ERROR, not WARN: the caller turns this into a NULL agent, which reads
       * downstream as "no eligible agent" -- the wrong diagnosis entirely. The
       * module being absent or slow is a deployment fault and has to say so in
       * its own words, or an operator hunts the agent roster while the routing
       * module is what is down. */
      aimee_log(LOG_ERROR, "routing",
                "routing module call failed (%s): refusing to select among %u candidates; this is "
                "a routing-module fault, not an empty agent roster",
                aimee_module_call_result_name(result), candidate_count);
      return -1;
   }
   return 0;
}

void server_module_routing_configure(void)
{
   agent_set_route_selection_provider(select_via_module);
}
