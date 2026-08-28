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
static const void *g_reply;
static uint32_t g_reply_len;
static aimee_module_call_result_t g_result = AIMEE_MODULE_CALL_OK;
static uint32_t g_last_event, g_last_stage;
static int g_calls;
static uint32_t g_event_reply_kind;
static const char *g_event_reply;
static uint32_t g_event_reply_len;
static char g_last_request[16384];

void module_bus_stub_reply(const char *json)
{
   g_reply = json;
   g_reply_len = json ? (uint32_t)strlen(json) : 0u;
   g_available = json != NULL;
   g_result = AIMEE_MODULE_CALL_OK;
}

void module_bus_stub_reply_for_event(uint32_t event_kind, const char *json)
{
   g_event_reply_kind = json ? event_kind : 0;
   g_event_reply = json;
   g_event_reply_len = json ? (uint32_t)strlen(json) : 0u;
}

void module_bus_stub_reply_bytes(const void *body, uint32_t len)
{
   g_reply = body;
   g_reply_len = len;
   g_available = body != NULL;
   g_result = AIMEE_MODULE_CALL_OK;
}

void module_bus_stub_absent(void)
{
   g_reply = NULL;
   g_reply_len = 0u;
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

const char *module_bus_stub_last_request(void)
{
   return g_last_request;
}

int obs_bus_module_available(uint32_t event_kind)
{
   return g_available || (g_event_reply && event_kind == g_event_reply_kind);
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id, (void)deadline_ns, (void)cancelled, (void)cancel_context;
   g_calls++;
   g_last_event = event_kind;
   g_last_stage = stage_id;
   size_t request_copy = request_len < sizeof(g_last_request) - 1 ? request_len
                                                                  : sizeof(g_last_request) - 1;
   if (request_body && request_copy)
      memcpy(g_last_request, request_body, request_copy);
   g_last_request[request_copy] = '\0';
   if (g_result != AIMEE_MODULE_CALL_OK)
      return g_result;
   const void *reply = g_reply;
   uint32_t reply_len = g_reply_len;
   if (g_event_reply && event_kind == g_event_reply_kind)
   {
      reply = g_event_reply;
      reply_len = g_event_reply_len;
   }
   if (!reply)
      return AIMEE_MODULE_CALL_CAPABILITY_ABSENT;
   size_t n = reply_len;
   if (n > response_capacity)
      return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
   memcpy(response_body, reply, n);
   *response_len = (uint32_t)n;
   return AIMEE_MODULE_CALL_OK;
}
