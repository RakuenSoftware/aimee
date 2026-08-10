/* obs_bus_module_call stub for unit tests.
 *
 * The workspace runner now asks the module over the bus, so anything linking it
 * needs that symbol. A unit test has no bus and no module process, so this
 * stands in for one.
 *
 * The default answer is "no module available", which is the honest state for a
 * test binary: callers then behave exactly as they do in production when the
 * module is not reachable. A test that wants to exercise the answer installs a
 * responder with ws_test_set_module_responder(). */
#include <stdint.h>
#include <string.h>

#include <aimee/core/event_bus/module_client.h>

typedef int (*ws_test_module_responder_fn)(uint32_t event_kind, uint32_t stage_id,
                                           const void *request, uint32_t request_len,
                                           void *response, uint32_t response_capacity,
                                           uint32_t *response_len);

static ws_test_module_responder_fn g_responder;

void ws_test_set_module_responder(ws_test_module_responder_fn fn)
{
   g_responder = fn;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id;
   (void)deadline_ns;
   (void)cancelled;
   (void)cancel_context;
   if (response_len)
      *response_len = 0;
   if (!g_responder)
      return AIMEE_MODULE_CALL_CAPABILITY_ABSENT; /* no module: the caller must cope */
   return g_responder(event_kind, stage_id, request_body, request_len, response_body,
                      response_capacity, response_len)
              ? AIMEE_MODULE_CALL_INTERNAL
              : AIMEE_MODULE_CALL_OK;
}
