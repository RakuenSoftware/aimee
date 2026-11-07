#include "db1_client/primary_sessions.h"
#include "db1_client/server_sessions.h"
#include "db1_client/transport.h"
#include "db1_client/db1_module_api.h"
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
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
uint64_t aimee_module_call_deadline_ns(uint64_t ms)
{
   return ms * 1000000;
}
aimee_module_call_result_t obs_bus_module_call(uint32_t event, uint32_t stage, uint64_t trace,
                                               uint64_t deadline, const void *request,
                                               uint32_t request_len, void *response, uint32_t cap,
                                               uint32_t *response_len,
                                               aimee_module_cancelled_fn cancel, void *context)
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
   return length > cap ? AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE : AIMEE_MODULE_CALL_OK;
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

static int delivery_begin(db1_original_input_receipt_t *r, uint64_t *id)
{
   return db1_session_original_input_begin_delivery("s", "12345678-1234-4234-8234-123456789abc",
                                                    "new-turn", "alice", "{}", "{}", r, id);
}
static void unknown_delivery(void)
{
   db1_original_input_receipt_t r;
   memset(&r, 0xa5, sizeof r);
   uint64_t id = 999;
   int before = calls;
   assert(delivery_begin(&r, &id) == -1 && calls == before + 1 && id == 0 &&
          r.outcome == DB1_ORIGINAL_INPUT_UNKNOWN && !r.begin_sequence && !r.input_sequence &&
          !r.original_turn[0]);
}
int main(void)
{
   const char *v[] = {"9007199254740993", "9007199254740994", "new-turn", "1",
                      "created",          "9007199254740995"};
   db1_original_input_receipt_t r;
   uint64_t id = 0;
   reply(6, v);
   assert(!delivery_begin(&r, &id) && r.begin_sequence == 9007199254740993ULL &&
          r.input_sequence == 9007199254740994ULL && id == 9007199254740995ULL);
   v[2] = "original-turn";
   v[4] = "replay";
   reply(6, v);
   assert(delivery_begin(&r, &id) == 1 && !strcmp(r.original_turn, "original-turn"));
   v[2] = "new-turn";
   v[4] = "created";
   for (unsigned field = 0; field < 6; field++)
   {
      const char *bad[] = {"0", "01", "", "2", "unknown", "0"};
      const char *x[6];
      memcpy(x, v, sizeof x);
      x[field] = bad[field];
      reply(6, x);
      unknown_delivery();
   }
   reply(5, v);
   unknown_delivery();
   reply(6, v);
   frame[length++] = 42;
   unknown_delivery();
   reply(6, v);
   frame[12] = 0;
   unknown_delivery();
   reply(6, v);
   override_length = UINT32_MAX;
   unknown_delivery();
   reply(1, (const char *[]){"{\"schema_version\":1}"});
   char *out = NULL;
   assert(!db1_session_original_input_delivery_get("s", "op", "alice", "receiver", &out) && out);
   free(out);
   reply(1, (const char *[]){"axb"});
   frame[13] = 0;
   out = NULL;
   assert(db1_session_original_input_delivery_get("s", "op", "alice", "receiver", &out) == -1 &&
          !out);
   reply(0, NULL);
   put(0, AIMEE_DB1_STATUS_MISSING);
   assert(db1_session_original_input_delivery_get("s", "op", "alice", "receiver", &out) == 1 &&
          !out);
   reply(1, (const char *[]){"1"});
   assert(!db1_session_original_input_delivery_ack("1", "receiver", "alice", "key", "delivered",
                                                   "{}"));
   reply(1, (const char *[]){"2"});
   assert(db1_session_original_input_delivery_ack("1", "receiver", "alice", "key", "delivered",
                                                  "{}") == -1);
   reply(0, NULL);
   put(0, AIMEE_DB1_STATUS_CONFLICT);
   assert(db1_server_session_delete("s") == DB1_SESSION_DELETE_CONFLICT);
   reply(1, (const char *[]){"forged conflict payload"});
   put(0, AIMEE_DB1_STATUS_CONFLICT);
   assert(db1_server_session_delete("s") == -1);
   reply(0, NULL);
   put(0, 6);
   assert(db1_server_session_delete("s") == -1);
   reply(0, NULL);
   put(0, UINT32_MAX);
   assert(db1_server_session_delete("s") == -1);
   db1_family_t legacy = {"legacy.missing", "fixture", 1, 1, 1000, 0};
   char a[2] = {0}, b[2] = {0};
   char *dest[] = {a, b};
   const size_t caps[] = {2, 2};
   reply(2, (const char *[]){"", ""});
   put(0, AIMEE_DB1_STATUS_MISSING);
   assert(db1_call(&legacy, 1, NULL, 0, dest, caps, 2, NULL) == AIMEE_DB1_STATUS_MISSING);
   puts("PASS typed58 six-cell receipt and wide uint64, replay, malformed/NUL/tail/overflow/no "
        "retry, get missing/owned bytes, exact ACKversion");
   return 0;
}
