#define _POSIX_C_SOURCE 200809L

#include "kb_module_stage_adapters.h"

#include "kb_curator_grounding.h"
#include "kb_route_acl.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/control-web/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/kb-synthesis/module_api.h>

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#define KB_MODULE_STAGE_DEADLINE_NS (500ULL * 1000000ULL)

static atomic_uint_fast64_t next_trace = 1;

static uint64_t monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int call_module(uint32_t event_kind, uint32_t stage_id, const void *request,
                       uint32_t request_len, void *response, uint32_t response_capacity,
                       uint32_t *response_len)
{
   uint64_t now = monotonic_ns();
   if (!now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   return obs_bus_module_call(event_kind, stage_id, trace, now + KB_MODULE_STAGE_DEADLINE_NS,
                              request, request_len, response, response_capacity, response_len, NULL,
                              NULL) == AIMEE_MODULE_CALL_OK
              ? 0
              : -1;
}

static int grounding_decide(aimee_kb_synthesis_claim_kind_t claim_kind, const char *const *claims,
                            uint32_t claim_count, const char *const *callees, uint32_t callee_count,
                            aimee_kb_synthesis_grounding_decision_t *decision)
{
   uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN];
   uint8_t response[AIMEE_KB_SYNTHESIS_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!decision || aimee_kb_synthesis_request_encode(claim_kind, claims, claim_count, callees,
                                                      callee_count, request, sizeof(request)) != 0)
      return -1;
   if (call_module(AIMEE_KB_SYNTHESIS_EVENT_GROUNDING, AIMEE_KB_SYNTHESIS_STAGE_GROUNDING, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_kb_synthesis_response_decode(response, response_len, decision);
}

static int control_web_authorize(const char *method, const char *path, int *allowed)
{
   uint8_t request[AIMEE_CONTROL_WEB_REQUEST_LEN];
   uint8_t response[AIMEE_CONTROL_WEB_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!allowed ||
       aimee_control_web_request_encode(AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, method, path,
                                        request, sizeof(request)) != 0 ||
       call_module(AIMEE_CONTROL_WEB_EVENT_AUTHORIZE, AIMEE_CONTROL_WEB_STAGE_AUTHORIZE, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_control_web_response_decode(response, response_len, allowed);
}

void kb_module_stage_adapters_configure(void)
{
   kb_curator_grounding_register_provider(grounding_decide);
   kb_route_acl_register_authorization_provider(control_web_authorize);
}
