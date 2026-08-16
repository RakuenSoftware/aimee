/* test_db1_module_stage.c: the DB1 module's stage handler.
 *
 * The handler is the whole boundary: it is what every caller will reach once
 * they stop linking db1, so a request it mis-parses is a request no caller can
 * see go wrong. These pin that a malformed frame is REFUSED rather than guessed
 * at, and that a save is readable by a load. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <aimee/core/event_bus/module_runtime.h>
#include "db1_module_api.h"

#include "checkpoints.h"
#include "db1.h"
#include "platform_test_util.h"

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data);

static aimee_module_status_t call(const uint8_t *req, uint32_t req_len, uint8_t *resp,
                                  uint32_t *resp_len)
{
   aimee_module_invocation_t invocation;
   memset(&invocation, 0, sizeof invocation);
   invocation.stage_id = AIMEE_DB1_STAGE_ECONOMIZER_STATE;
   *resp_len = 0;
   return aimee_module_handler(&invocation, req, req_len, resp, 4096u, resp_len, NULL);
}

/* op | key_len | key | json_len | json */
static uint32_t frame(uint8_t *out, uint32_t op, const char *key, const char *json)
{
   uint32_t n = 0, key_len = (uint32_t)strlen(key), json_len = (uint32_t)strlen(json);
   aimee_db1_put_u32(out + n, op);
   n += 4;
   aimee_db1_put_u32(out + n, key_len);
   n += 4;
   memcpy(out + n, key, key_len);
   n += key_len;
   aimee_db1_put_u32(out + n, json_len);
   n += 4;
   memcpy(out + n, json, json_len);
   n += json_len;
   return n;
}

static void test_malformed_frames_are_refused(void)
{
   uint8_t req[8192], resp[4096];
   uint32_t resp_len = 0;

   /* Nothing at all, and a frame carrying only an opcode: neither names a key,
      so neither can be acted on. */
   assert(call(req, 0, resp, &resp_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   aimee_db1_put_u32(req, AIMEE_DB1_OP_STATE_LOAD);
   assert(call(req, 4, resp, &resp_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* A length that runs past the end of the frame. Truncation must be refused
      rather than read, or the handler reads whatever follows in memory. */
   uint32_t n = frame(req, AIMEE_DB1_OP_STATE_LOAD, "gw:abc:0", "");
   assert(call(req, n - 1, resp, &resp_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* An empty key is not a key: it would name every session at once. */
   n = frame(req, AIMEE_DB1_OP_STATE_LOAD, "", "");
   assert(call(req, n, resp, &resp_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* An unknown opcode is refused rather than defaulted to a read. */
   n = frame(req, 99u, "gw:abc:0", "");
   assert(call(req, n, resp, &resp_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   printf("  PASS: test_malformed_frames_are_refused\n");
}

static void test_save_then_load_round_trips(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-stage-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   uint8_t req[8192], resp[4096];
   uint32_t resp_len = 0;

   /* A key with no state yet reads as MISSING, not as an error: the first turn
      of a conversation has none and the caller starts cold. */
   uint32_t n = frame(req, AIMEE_DB1_OP_STATE_LOAD, "gw:fresh:0", "");
   assert(call(req, n, resp, &resp_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_MISSING);

   const char *state = "{\"turn\":7,\"epoch\":2}";
   n = frame(req, AIMEE_DB1_OP_STATE_SAVE, "gw:abc:0", state);
   assert(call(req, n, resp, &resp_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);

   n = frame(req, AIMEE_DB1_OP_STATE_LOAD, "gw:abc:0", "");
   assert(call(req, n, resp, &resp_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);
   uint32_t len = aimee_db1_get_u32(resp + 4);
   assert(len == strlen(state));
   assert(memcmp(resp + 8, state, len) == 0);

   /* One session's state is not another's. */
   n = frame(req, AIMEE_DB1_OP_STATE_LOAD, "gw:other:0", "");
   assert(call(req, n, resp, &resp_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_MISSING);

   db1_shutdown();
   remove(path);
   printf("  PASS: test_save_then_load_round_trips\n");
}

static void test_over_long_state_is_refused_not_truncated(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-long-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   static uint8_t req[AIMEE_DB1_STATE_MAX * 2];
   uint8_t resp[4096];
   uint32_t resp_len = 0;

   char big[AIMEE_DB1_STATE_MAX + 16];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';

   /* Truncating a state blob would persist something that is not JSON, and the
      next load would hand a caller a value it cannot parse. Refuse instead. */
   uint32_t n = frame(req, AIMEE_DB1_OP_STATE_SAVE, "gw:big:0", big);
   assert(call(req, n, resp, &resp_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_TOO_LONG);

   /* And nothing was written. */
   n = frame(req, AIMEE_DB1_OP_STATE_LOAD, "gw:big:0", "");
   assert(call(req, n, resp, &resp_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_MISSING);

   db1_shutdown();
   remove(path);
   printf("  PASS: test_over_long_state_is_refused_not_truncated\n");
}

int main(void)
{
   printf("db1_module_stage:\n");
   test_malformed_frames_are_refused();
   test_save_then_load_round_trips();
   test_over_long_state_is_refused_not_truncated();
   printf("db1_module_stage: ok\n");
   return 0;
}
