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
#include "module_stage_adapters.h"
#include "module_commands.h"
#include "log.h"
#include "config.h"
#include "config_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *g_saved_backend = "native";

/* Snapshot boundary only: use the production startup capture and restart policy. */
int config_client_read_string(const char *key, char *out, size_t n)
{
   assert(strcmp(key, "memory_backend_engine") == 0);
   snprintf(out, n, "%s", g_saved_backend);
   return 0;
}

static int g_available = 1;
static int g_calls;
static int g_required_budget_ms;
static uint64_t g_deadline;
static char g_body[8192];
static const char *g_reply = "{\"ok\":true}";
static const void *g_reply_frame;
static size_t g_reply_frame_len;
static uint32_t g_event, g_stage, g_request_len;
static ouro_module_call_result_t g_result = OURO_MODULE_CALL_OK;

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

int obs_bus_module_available(uint32_t event_kind)
{
   (void)event_kind;
   return g_available;
}

ouro_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    ouro_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)event_kind, (void)stage_id, (void)trace_id, (void)cancelled, (void)cancel_context;
   g_calls++;
   g_event = event_kind;
   g_stage = stage_id;
   g_request_len = request_len;
   g_deadline = deadline_ns;
   assert(request_len < sizeof g_body);
   memcpy(g_body, request_body, request_len);
   g_body[request_len] = '\0';
   if (g_required_budget_ms && deadline_ns < aimee_module_call_deadline_ns(g_required_budget_ms))
      return OURO_MODULE_CALL_DEADLINE_EXCEEDED;
   if (g_result != OURO_MODULE_CALL_OK)
      return g_result;
   size_t n = g_reply_frame ? g_reply_frame_len : strlen(g_reply);
   assert(n <= response_capacity);
   memcpy(response_body, g_reply_frame ? g_reply_frame : g_reply, n);
   *response_len = (uint32_t)n;
   return OURO_MODULE_CALL_OK;
}

static cJSON *req(void)
{
   cJSON *o = cJSON_CreateObject();
   assert(o);
   cJSON_AddStringToObject(o, "k", "v");
   return o;
}

static void test_memory_reset_backend(const char *engine, int from_snapshot)
{
   /* Each backend starts in a fresh process before pthread_once captures it. */
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      g_saved_backend = from_snapshot ? engine : "native";
      assert(unsetenv("AIMEE_MEMORY_BACKEND") == 0);
      if (!from_snapshot)
         assert(setenv("AIMEE_MEMORY_BACKEND", engine, 1) == 0);
      assert(strcmp(config_memory_backend_current(), engine) == 0);
      g_calls = 0;
      int native = strcmp(engine, "native") == 0;
      g_required_budget_ms = 6500;
      g_reply = "{\"records\":[],\"deleted\":true}";
      assert(server_module_memory_reset_derived() == 0);
      assert(g_calls == (native ? 0 : 1));
      if (!native)
      {
         assert(strstr(g_body, "\"operation\":\"reset-derived\""));
         assert(strstr(g_body, "\"scope\":{\"type\":\"user\"}"));
         g_reply = "{\"records\":[],\"deleted\":false}";
         assert(server_module_memory_reset_derived() == -1);
         g_result = OURO_MODULE_CALL_CAPABILITY_ABSENT;
         assert(server_module_memory_reset_derived() == -1);
      }
      /* Later saves and environment edits cannot reroute the running owner. */
      g_saved_backend = native ? "hillock" : "native";
      assert(setenv("AIMEE_MEMORY_BACKEND", g_saved_backend, 1) == 0);
      assert(strcmp(config_memory_backend_current(), engine) == 0);
      g_result = OURO_MODULE_CALL_OK;
      g_reply = "{\"records\":[],\"deleted\":true}";
      g_calls = 0;
      assert(server_module_memory_reset_derived() == 0);
      assert(g_calls == (native ? 0 : 1));
      exit(0);
   }
   int status;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
   test_memory_reset_backend("native", 0);
   test_memory_reset_backend("cognee", 0);
   test_memory_reset_backend("hillock", 0);
   test_memory_reset_backend("cognee", 1);
   printf("  ok    startup-selected erasure verifies cleanup and remains stable until restart\n");
   assert(unsetenv("AIMEE_MEMORY_BACKEND") == 0);

   ouro_module_call_result_t result;

   /* Deadline arithmetic: no timeout means no deadline, and a real timeout must
    * produce a future stamp -- a past one would fail every call instantly. */
   assert(aimee_module_call_deadline_ns(0) == 0);
   assert(aimee_module_call_deadline_ns(-5) == 0);
   assert(aimee_module_call_deadline_ns(1000) != 0);
   printf("  ok    deadline arithmetic\n");

   /* Happy path: the reply parses and the module saw our body. */
   g_available = 1;
   g_calls = 0;
   g_result = OURO_MODULE_CALL_OK;
   cJSON *reply = aimee_module_json_call(1, 1, req(), 4096, 1000, &result);
   assert(reply && result == OURO_MODULE_CALL_OK);
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
   assert(result == OURO_MODULE_CALL_CAPABILITY_ABSENT);
   assert(g_calls == 0);
   printf("  ok    unattached module short-circuits and is named\n");

   /* A transport failure is reported as itself, not flattened to "absent" --
    * that distinction is the whole reason callers get a result pointer. */
   g_available = 1;
   g_calls = 0;
   g_result = OURO_MODULE_CALL_DEADLINE_EXCEEDED;
   reply = aimee_module_json_call(1, 1, req(), 4096, 1000, &result);
   assert(!reply && result == OURO_MODULE_CALL_DEADLINE_EXCEEDED && g_calls == 1);
   printf("  ok    transport failure keeps its own outcome\n");

   /* A body over the cap fails without touching the bus. */
   g_result = OURO_MODULE_CALL_OK;
   g_calls = 0;
   reply = aimee_module_json_call(1, 1, req(), 4, 1000, &result);
   assert(!reply && result == OURO_MODULE_CALL_INVALID_REQUEST && g_calls == 0);
   printf("  ok    oversized request never reaches the bus\n");

   /* Bad arguments are refused, and a NULL tree must not crash. */
   g_calls = 0;
   reply = aimee_module_json_call(1, 1, NULL, 4096, 1000, &result);
   assert(!reply && result == OURO_MODULE_CALL_INVALID_ARGUMENT && g_calls == 0);
   reply = aimee_module_json_call(1, 1, req(), 0, 1000, &result);
   assert(!reply && result == OURO_MODULE_CALL_INVALID_ARGUMENT && g_calls == 0);
   printf("  ok    invalid arguments refused\n");

   /* An answering module with an unparseable payload is NOT a transport error:
    * result stays OK and the NULL return carries the distinction. */
   g_available = 1;
   g_calls = 0;
   g_reply = "not json";
   reply = aimee_module_json_call(1, 1, req(), 4096, 1000, &result);
   assert(!reply && result == OURO_MODULE_CALL_OK && g_calls == 1);
   g_reply = "{\"ok\":true}";
   printf("  ok    unparseable reply is distinguishable from a failed call\n");

   /* The raw variant borrows its body rather than owning it, and is what the two
    * consumers that already hold a serialized request use. */
   g_calls = 0;
   const char *body = "{\"a\":1}";
   reply = aimee_module_json_call_raw(1, 1, body, strlen(body), 4096, 1000, &result);
   assert(reply && result == OURO_MODULE_CALL_OK && g_calls == 1);
   assert(strcmp(g_body, body) == 0);
   assert(strcmp(body, "{\"a\":1}") == 0); /* still ours, untouched */
   cJSON_Delete(reply);
   printf("  ok    raw variant borrows the caller's body\n");

   /* A database-backed personal recall can take longer than a CPU-only stage.
    * Model 1.5 seconds of database work: the production caller must grant that
    * budget, preserve user placement, and keep the caller's request borrowed. */
   cJSON *memory_request = cJSON_CreateObject();
   cJSON_AddStringToObject(memory_request, "operation", "recall-bundle");
   cJSON_AddStringToObject(memory_request, "scope", "caller-supplied");
   g_required_budget_ms = 1500;
   g_calls = 0;
   g_reply = "{\"payload\":{\"active_context\":[]}}";
   uint64_t maximum_deadline = aimee_module_call_deadline_ns(6000);
   reply = server_module_memory_data(memory_request);
   assert(reply && g_calls == 1);
   assert(g_deadline < maximum_deadline);
   assert(strstr(g_body, "\"scope\":{\"type\":\"user\"}"));
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(memory_request, "scope")->valuestring,
                 "caller-supplied") == 0);
   cJSON_Delete(reply);
   printf("  ok    personal recall receives a bounded database budget and user scope\n");

   /* Work that exceeds the database budget remains an explicit failure. */
   g_required_budget_ms = 6000;
   reply = server_module_memory_data(memory_request);
   assert(!reply);
   g_required_budget_ms = 0;
   g_result = OURO_MODULE_CALL_CAPABILITY_ABSENT;
   reply = server_module_memory_data(memory_request);
   assert(!reply);
   cJSON_Delete(memory_request);
   printf("  ok    expired or unavailable memory does not fabricate success\n");

   g_result = OURO_MODULE_CALL_OK;
   memory_request = cJSON_CreateObject();
   cJSON_AddStringToObject(memory_request, "operation", "code-index");
   cJSON *index = cJSON_AddObjectToObject(memory_request, "code_index");
   cJSON_AddStringToObject(index, "route", "/v1/code/scan");
   g_required_budget_ms = 10000;
   reply = server_module_memory_data(memory_request);
   assert(reply && g_deadline < aimee_module_call_deadline_ns(121000));
   cJSON_Delete(reply);
   cJSON_ReplaceItemInObjectCaseSensitive(index, "route", cJSON_CreateString("/v1/code/hybrid"));
   reply = server_module_memory_data(memory_request);
   assert(reply && g_deadline < aimee_module_call_deadline_ns(31000));
   cJSON_Delete(reply);
   g_required_budget_ms = 31000;
   assert(!server_module_memory_data(memory_request));
   cJSON_Delete(memory_request);
   printf("  ok    code publication and retrieval have separate bounded budgets\n");

   /* The same command framing serves plugins and fixed Go modules. Stage IDs
    * must reach the owner unchanged, and malformed replies cannot become JSON. */
   const unsigned char command_reply[] = "CMPS\1\0\0\0\17\0\0\0{\"status\":\"ok\"}";
   const unsigned char command_request[] = "CMPQ\1\0\0\0\3\0\0\0\11\0\0\0get{\"id\":42}";
   g_reply_frame = command_reply;
   g_reply_frame_len = sizeof(command_reply) - 1;
   g_required_budget_ms = 0;
   cJSON *args = cJSON_Parse("{\"id\":42}");
   reply = aimee_module_command_call(5896, 8, "get", args);
   assert(reply && strcmp(cJSON_GetObjectItem(reply, "status")->valuestring, "ok") == 0);
   assert(g_event == 5896 && g_stage == 8);
   assert(g_request_len == sizeof(command_request) - 1);
   assert(memcmp(g_body, command_request, g_request_len) == 0);
   assert(cJSON_GetObjectItem(args, "id")->valueint == 42); /* borrowed */
   cJSON_Delete(reply);
   for (size_t n = 0; n < sizeof(command_reply) - 1; n++)
   {
      g_reply_frame_len = n;
      assert(!aimee_module_command_call(5896, 8, "get", args));
   }
   int before = g_calls;
   assert(!aimee_module_command_call(5896, 8, "", args));
   assert(g_calls == before);
   cJSON_Delete(args);
   printf("  ok    shared command framing preserves stages and rejects truncated replies\n");
   printf("module_json_call: all tests passed\n");
   return 0;
}
