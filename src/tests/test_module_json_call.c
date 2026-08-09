/* test_module_json_call.c: the C core's shared JSON ingestion path.
 *
 * This is the seam every future module consumer is meant to use instead of
 * hand-rolling a round trip, so the contract that matters is ownership and
 * outcome reporting: the request tree is freed on EVERY path (an unattached
 * module is the ordinary case, not an exception), and the caller can always tell
 * WHY a call produced nothing, because the three existing consumers render that
 * outcome three different ways.
 *
 * The bus is stubbed: what is under test is the plumbing, not the transport.
 */
#include "headers/module_json_call.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_available = 1;
static int g_calls;
static uint64_t g_deadline;
static char g_body[8192];
static const char *g_reply = "{\"ok\":true}";
static aimee_module_call_result_t g_result = AIMEE_MODULE_CALL_OK;

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
   (void)event_kind, (void)stage_id, (void)trace_id, (void)cancelled, (void)cancel_context;
   g_calls++;
   g_deadline = deadline_ns;
   assert(request_len < sizeof g_body);
   memcpy(g_body, request_body, request_len);
   g_body[request_len] = '\0';
   if (g_result != AIMEE_MODULE_CALL_OK)
      return g_result;
   size_t n = strlen(g_reply);
   assert(n <= response_capacity);
   memcpy(response_body, g_reply, n);
   *response_len = (uint32_t)n;
   return AIMEE_MODULE_CALL_OK;
}

static cJSON *req(void)
{
   cJSON *o = cJSON_CreateObject();
   assert(o);
   cJSON_AddStringToObject(o, "k", "v");
   return o;
}

int main(void)
{
   aimee_module_call_result_t result;

   /* Deadline arithmetic: no timeout means no deadline, and a real timeout must
    * produce a future stamp -- a past one would fail every call instantly. */
   assert(aimee_module_call_deadline_ns(0) == 0);
   assert(aimee_module_call_deadline_ns(-5) == 0);
   assert(aimee_module_call_deadline_ns(1000) != 0);
   printf("  ok    deadline arithmetic\n");

   /* Happy path: the reply parses and the module saw our body. */
   g_available = 1;
   g_calls = 0;
   g_result = AIMEE_MODULE_CALL_OK;
   cJSON *reply = aimee_module_json_call(1, 1, req(), 4096, 1000, &result);
   assert(reply && result == AIMEE_MODULE_CALL_OK);
   assert(g_calls == 1);
   assert(strstr(g_body, "\"k\":\"v\""));
   assert(g_deadline != 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")));
   cJSON_Delete(reply);
   printf("  ok    round trip parses and forwards the body\n");

   /* An unattached module must not reach the bus at all, must report why, and
    * must still consume the request tree. Running this under ASAN/valgrind is
    * what proves the free; here we assert the observable half. */
   g_available = 0;
   g_calls = 0;
   reply = aimee_module_json_call(1, 1, req(), 4096, 1000, &result);
   assert(!reply);
   assert(result == AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
   assert(g_calls == 0);
   printf("  ok    unattached module short-circuits and is named\n");

   /* A transport failure is reported as itself, not flattened to "absent" --
    * that distinction is the whole reason callers get a result pointer. */
   g_available = 1;
   g_calls = 0;
   g_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   reply = aimee_module_json_call(1, 1, req(), 4096, 1000, &result);
   assert(!reply && result == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED && g_calls == 1);
   printf("  ok    transport failure keeps its own outcome\n");

   /* A body over the cap fails without touching the bus. */
   g_result = AIMEE_MODULE_CALL_OK;
   g_calls = 0;
   reply = aimee_module_json_call(1, 1, req(), 4, 1000, &result);
   assert(!reply && result == AIMEE_MODULE_CALL_INVALID_REQUEST && g_calls == 0);
   printf("  ok    oversized request never reaches the bus\n");

   /* Bad arguments are refused, and a NULL tree must not crash. */
   g_calls = 0;
   reply = aimee_module_json_call(1, 1, NULL, 4096, 1000, &result);
   assert(!reply && result == AIMEE_MODULE_CALL_INVALID_ARGUMENT && g_calls == 0);
   reply = aimee_module_json_call(1, 1, req(), 0, 1000, &result);
   assert(!reply && result == AIMEE_MODULE_CALL_INVALID_ARGUMENT && g_calls == 0);
   printf("  ok    invalid arguments refused\n");

   /* An answering module with an unparseable payload is NOT a transport error:
    * result stays OK and the NULL return carries the distinction. */
   g_available = 1;
   g_calls = 0;
   g_reply = "not json";
   reply = aimee_module_json_call(1, 1, req(), 4096, 1000, &result);
   assert(!reply && result == AIMEE_MODULE_CALL_OK && g_calls == 1);
   g_reply = "{\"ok\":true}";
   printf("  ok    unparseable reply is distinguishable from a failed call\n");

   /* The raw variant borrows its body rather than owning it, and is what the two
    * consumers that already hold a serialized request use. */
   g_calls = 0;
   const char *body = "{\"a\":1}";
   reply = aimee_module_json_call_raw(1, 1, body, strlen(body), 4096, 1000, &result);
   assert(reply && result == AIMEE_MODULE_CALL_OK && g_calls == 1);
   assert(strcmp(g_body, body) == 0);
   assert(strcmp(body, "{\"a\":1}") == 0); /* still ours, untouched */
   cJSON_Delete(reply);
   printf("  ok    raw variant borrows the caller's body\n");

   printf("module_json_call: all tests passed\n");
   return 0;
}
