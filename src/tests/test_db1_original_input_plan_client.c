#include "module_json_call.h"
#include "db1_client/original_input_plan.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include "db1_client/server_sessions.h"
#include "db1_client/transport.h"
#include "db1_client/db1_module_api.h"
#include <ouroboros/module_client.h>
#include <ouroboros/module_protocol.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "log.h"
void aimee_log(log_level_t l, const char *m, const char *fmt, ...)
{
   (void)l;
   (void)m;
   (void)fmt;
}
static unsigned char frame[8192];
static uint32_t length, override_length;
static int calls;
int obs_bus_module_available(uint32_t event)
{
   (void)event;
   return 1;
}
uint64_t aimee_module_call_deadline_ns(int ms)
{
   return ms * 1000000;
}
ouro_module_call_result_t obs_bus_module_call(uint32_t event, uint32_t stage, uint64_t trace,
                                              uint64_t deadline, const void *request,
                                              uint32_t request_len, void *response, uint32_t cap,
                                              uint32_t *response_len,
                                              ouro_module_cancelled_fn cancel, void *context)
{
   (void)event;
   (void)stage;
   (void)trace;
   (void)deadline;
   (void)request;
   (void)request_len;
   (void)cancel;
   (void)context;
   calls++;
   memcpy(response, frame, length < cap ? length : cap);
   *response_len = override_length ? override_length : length;
   return length > cap ? OURO_MODULE_CALL_RESPONSE_TOO_LARGE : OURO_MODULE_CALL_OK;
}
static void put(uint32_t at, uint32_t n)
{
   for (int i = 0; i < 4; i++)
      frame[at + i] = (unsigned char)(n >> (8 * i));
}
static void reply(unsigned n, const char **values)
{
   memset(frame, 0, sizeof frame);
   put(0, AIMEE_DB1_STATUS_OK);
   put(4, n);
   length = 8;
   override_length = 0;
   for (unsigned i = 0; i < n; i++)
   {
      size_t l = strlen(values[i]);
      put(length, l);
      length += 4;
      memcpy(frame + length, values[i], l);
      length += (uint32_t)l;
   }
}

static const char *op = "12345678-1234-4234-8234-123456789abc";
static char plan[8192], hash[65];
static void make_plan(void)
{
   assert(!aimee_sha256_hex("{}", 2, hash));
   snprintf(plan, sizeof plan,
            "{\"schema_version\":1,\"plan_id\":\"9007199254740993\",\"session_id\":\"s\","
            "\"operation_uuid\":\"%s\",\"actor\":\"alice\",\"receiver_tag\":\"receiver\",\"safe_"
            "text_sha256\":\"%s\",\"plan_sha256\":\"%s\",\"opaque_plan\":\"{}\",\"state\":"
            "\"reserved\",\"begin_sequence\":\"\",\"input_sequence\":\"\",\"original_turn\":\"\","
            "\"delivery_id\":\"\",\"terminal_result\":\"\"}",
            op, hash, hash);
}
static int reserve(db1_original_input_plan_receipt_t *p)
{
   return db1_session_original_input_plan_reserve("s", op, "alice", "receiver", hash, hash, "{}",
                                                  p);
}
static int probe(const char *body, db1_original_input_probe_t *p)
{
   reply(1, (const char *[]){body});
   return db1_session_original_input_probe("s", op, "alice", p);
}
static char *changed(const char *key, const char *value)
{
   cJSON *o = cJSON_Parse(plan);
   assert(o);
   cJSON_ReplaceItemInObjectCaseSensitive(o, key, cJSON_CreateString(value));
   char *wire = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   assert(wire);
   return wire;
}
int main(void)
{
   make_plan();
   db1_original_input_plan_receipt_t p = {0};
   reply(1, (const char *[]){plan});
   assert(!reserve(&p) && p.plan_id == 9007199254740993ULL && p.state == DB1_INPUT_PLAN_RESERVED);
   const char *keys[] = {"plan_id",      "session_id",       "operation_uuid", "actor",
                         "receiver_tag", "safe_text_sha256", "plan_sha256",    "opaque_plan",
                         "state",        "begin_sequence",   "input_sequence", "original_turn",
                         "delivery_id",  "terminal_result"};
   const char *bad[] = {"01",      "other",    "bad", "bob", "other", "bad", "bad",
                        "changed", "terminal", "1",   "2",   "turn",  "3",   "{}"};
   for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
   {
      char *wire = changed(keys[i], bad[i]);
      reply(1, (const char *[]){wire});
      memset(&p, 0xa5, sizeof p);
      assert(reserve(&p) == -1 && !p.plan_id && !p.contract_version);
      free(wire);
   }
   char *dup = malloc(strlen(plan) + 32);
   assert(dup);
   sprintf(dup, "{\"schema_version\":1,%s", plan + 1);
   reply(1, (const char *[]){dup});
   assert(reserve(&p) == -1);
   free(dup);
   char body[12000];
   snprintf(
       body, sizeof body,
       "{\"schema_version\":1,\"presence\":\"reserved\",\"session_id\":\"s\",\"operation_uuid\":\"%"
       "s\",\"actor\":\"alice\",\"canonical\":null,\"delivery_id\":\"\",\"plan\":%s}",
       op, plan);
   db1_original_input_probe_t q = {0};
   assert(!probe(body, &q) && q.presence == DB1_INPUT_PROBE_RESERVED && q.opaque_plan_len == 2 &&
          !strcmp(q.opaque_plan, "{}"));
   db1_original_input_probe_destroy(&q);
   assert(!q.opaque_plan && !q.contract_version);
   cJSON *o = cJSON_Parse(body);
   assert(o);
   cJSON_ReplaceItemInObjectCaseSensitive(o, "actor", cJSON_CreateString("bob"));
   char *wire = cJSON_PrintUnformatted(o);
   assert(probe(wire, &q) == -1 && !q.opaque_plan);
   free(wire);
   cJSON_Delete(o);
   const char *malformed[] = {"{}", "{\"schema_version\":1,\"schema_version\":1}",
                              "{\"x\":\"a\\u0000b\"}", "{} trailing"};
   for (size_t i = 0; i < sizeof malformed / sizeof *malformed; i++)
      assert(probe(malformed[i], &q) == -1 && !q.opaque_plan);
   const char *v[] = {"9007199254740993", "9007199254740994", "turn", "1",
                      "created",          "9007199254740995"};
   db1_original_input_receipt_t r = {0};
   uint64_t id = 0;
   reply(6, v);
   assert(!db1_session_original_input_begin_planned_delivery("s", op, "turn", "alice", "{}", "{}",
                                                             "1", hash, &r, &id) &&
          id == 9007199254740995ULL);
   v[2] = "old";
   v[4] = "replay";
   reply(6, v);
   assert(db1_session_original_input_begin_planned_delivery("s", op, "turn", "alice", "{}", "{}",
                                                            "1", hash, &r, &id) == 1 &&
          !strcmp(r.original_turn, "old"));
   reply(5, v);
   assert(db1_session_original_input_begin_planned_delivery("s", op, "turn", "alice", "{}", "{}",
                                                            "1", hash, &r, &id) == -1 &&
          !id && !r.contract_version);
   reply(1, (const char *[]){"1"});
   assert(!db1_session_original_input_plan_terminal("1", "s", op, "alice", "receiver", hash, "{}"));
   reply(1, (const char *[]){"2"});
   assert(db1_session_original_input_plan_terminal("1", "s", op, "alice", "receiver", hash, "{}") ==
          -1);
   reply(0, NULL);
   put(0, AIMEE_DB1_STATUS_CONFLICT);
   assert(db1_session_original_input_plan_terminal("1", "s", op, "alice", "receiver", hash, "{}") ==
          DB1_INPUT_PLAN_CONFLICT);
   reply(1, (const char *[]){"1"});
   put(0, AIMEE_DB1_STATUS_CONFLICT);
   assert(db1_session_original_input_plan_terminal("1", "s", op, "alice", "receiver", hash, "{}") ==
          -1);
   puts("PASS typed62/63/64/65: exact row bindings, wide IDs, owned probe cleanup, "
        "malformed/duplicate/NUL/trailing/unknown reply refusal, created/replay and closed "
        "conflict");
   return 0;
}
