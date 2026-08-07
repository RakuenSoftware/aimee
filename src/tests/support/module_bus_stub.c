/* module_bus_stub.c: a controllable event-bus stub for minimal test binaries.
 *
 * Several unit tests link a module seam whose implementation now makes a bus
 * call, but do not link the bus runtime -- that is the whole point of those
 * binaries, which exist to test one translation unit without the daemon around
 * it. This supplies the two symbols such a seam needs.
 *
 * The default is "no module attached", because that is the honest default for a
 * process with no bus: a seam that must fail closed then gets to prove it does.
 * A test that needs a specific reply sets one with module_bus_stub_reply().
 */
#include "module_bus_stub.h"

#include <string.h>

static int g_available;
static const char *g_reply;
static aimee_module_call_result_t g_result = AIMEE_MODULE_CALL_OK;
static uint32_t g_last_event, g_last_stage;
static int g_calls;

void module_bus_stub_reply(const char *json)
{
   g_reply = json;
   g_available = json != NULL;
   g_result = AIMEE_MODULE_CALL_OK;
}

void module_bus_stub_absent(void)
{
   g_reply = NULL;
   g_available = 0;
}

void module_bus_stub_fail(aimee_module_call_result_t result)
{
   g_available = 1;
   g_reply = NULL;
   g_result = result;
}

int module_bus_stub_calls(void)
{
   return g_calls;
}

uint32_t module_bus_stub_last_event(void)
{
   return g_last_event;
}

uint32_t module_bus_stub_last_stage(void)
{
   return g_last_stage;
}

int obs_bus_module_available(uint32_t event_kind)
{
   (void)event_kind;
   return g_available;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id, (void)deadline_ns, (void)request_body, (void)request_len, (void)cancelled,
       (void)cancel_context;
   g_calls++;
   g_last_event = event_kind;
   g_last_stage = stage_id;
   if (g_result != AIMEE_MODULE_CALL_OK)
      return g_result;
   if (!g_reply)
      return AIMEE_MODULE_CALL_CAPABILITY_ABSENT;
   size_t n = strlen(g_reply);
   if (n > response_capacity)
      return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
   memcpy(response_body, g_reply, n);
   *response_len = (uint32_t)n;
   return AIMEE_MODULE_CALL_OK;
}
