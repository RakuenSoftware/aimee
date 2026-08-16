/* test_db1_module_stage.c: the DB1 module's stage handler.
 *
 * The handler is the whole boundary: it is what every caller will reach once
 * they stop linking db1, so a request it mis-parses is a request no caller can
 * see go wrong. These pin that a malformed frame is REFUSED rather than guessed
 * at, and that a save is readable by a load. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <aimee/core/event_bus/module_runtime.h>
#include "db1_module_api.h"

#include "checkpoints.h"
#include "db1.h"
#include "git_ownership.h"
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

static aimee_module_status_t call_stage(uint32_t stage, const uint8_t *req, uint32_t req_len,
                                        uint8_t *resp, uint32_t *resp_len)
{
   aimee_module_invocation_t invocation;
   memset(&invocation, 0, sizeof invocation);
   invocation.stage_id = stage;
   *resp_len = 0;
   return aimee_module_handler(&invocation, req, req_len, resp, 4096u, resp_len, NULL);
}

/* op | field_count | (len | bytes) * count */
static uint32_t fields_frame(uint8_t *out, uint32_t op, const char *const *values, uint32_t count)
{
   uint32_t n = 0;
   aimee_db1_put_u32(out + n, op);
   n += 4;
   aimee_db1_put_u32(out + n, count);
   n += 4;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint32_t len = (uint32_t)strlen(values[i]);
      aimee_db1_put_u32(out + n, len);
      n += 4;
      memcpy(out + n, values[i], len);
      n += len;
   }
   return n;
}

/* An unknown stage must not fall through to another family's decoder: the two
   frames differ, so a mis-dispatched request would be parsed as nonsense. */
static void test_unknown_stage_is_refused(void)
{
   uint8_t req[256], resp[4096];
   uint32_t resp_len = 0;
   const char *values[] = {"/repo", "main", "sess-1"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_UPSERT, values, 3);
   assert(call_stage(0u, req, len, resp, &resp_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(call_stage(99u, req, len, resp, &resp_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   printf("  PASS: test_unknown_stage_is_refused\n");
}

static void test_git_ownership_round_trips(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-owner-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   uint8_t req[1024], resp[4096];
   uint32_t resp_len = 0;
   const char *upsert[] = {"/repo/one", "feature", "sess-abc123"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_UPSERT, upsert, 3);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);

   /* The owner is readable back through the same boundary that wrote it. */
   const char *owner_get[] = {"/repo/one", "feature"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, owner_get, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);
   uint32_t value_len = aimee_db1_get_u32(resp + 4);
   assert(value_len == strlen("sess-abc123"));
   assert(memcmp(resp + 8, "sess-abc123", value_len) == 0);

   const char *by_session[] = {"/repo/one", "sess-abc123"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION, by_session, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);
   value_len = aimee_db1_get_u32(resp + 4);
   assert(value_len == strlen("feature"));
   assert(memcmp(resp + 8, "feature", value_len) == 0);

   /* A row nobody wrote is MISSING, not a failure: no owner is a real answer. */
   const char *absent[] = {"/repo/one", "other"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, absent, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_MISSING);
   assert(aimee_db1_get_u32(resp + 4) == 0u);

   const char *del[] = {"/repo/one", "feature"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_DELETE, del, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);

   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, owner_get, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_MISSING);

   db1_shutdown();
   remove(path);
   printf("  PASS: test_git_ownership_round_trips\n");
}

/* Wrong arity is a contract mismatch between caller and module, so it is
   refused rather than answered from whatever fields did arrive. */
static void test_git_ownership_malformed_frames_are_refused(void)
{
   uint8_t req[1024], resp[4096];
   uint32_t resp_len = 0;
   const char *two[] = {"/repo", "main"};
   const char *three[] = {"/repo", "main", "sess"};

   /* upsert wants three fields */
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_UPSERT, two, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);
   /* delete wants two */
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_DELETE, three, 3);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);
   /* an unknown op is not guessed at */
   len = fields_frame(req, 250u, two, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);
   /* a truncated header carries no op at all */
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, 7u, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);
   /* More fields than any op declares, and every one of them WELL FORMED. The
      decoder reads into a fixed array of AIMEE_DB1_FIELDS_MAX, so the count
      check is what stops a frame from writing past it -- a truncated frame
      would be refused by the field reader instead and prove nothing.

      A normal build cannot tell the two apart: with the bound removed the
      per-op arity check still refuses this frame, so the result is unchanged
      even though the write already happened. Built with -fsanitize=address it
      reports a stack-buffer-overflow, which is what this frame is really for. */
   const char *too_many[] = {"/repo", "main", "sess", "extra"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_UPSERT, too_many, AIMEE_DB1_FIELDS_MAX + 1u);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* a length that runs past the frame */
   aimee_db1_put_u32(req, AIMEE_DB1_OP_OWNERSHIP_DELETE);
   aimee_db1_put_u32(req + 4, 2u);
   aimee_db1_put_u32(req + 8, 4096u);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, 12u, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* an embedded NUL would shorten a path into a different row */
   uint32_t n = 0;
   aimee_db1_put_u32(req + n, AIMEE_DB1_OP_OWNERSHIP_DELETE);
   n += 4;
   aimee_db1_put_u32(req + n, 2u);
   n += 4;
   aimee_db1_put_u32(req + n, 6u);
   n += 4;
   memcpy(req + n, "/re\0po", 6);
   n += 6;
   aimee_db1_put_u32(req + n, 4u);
   n += 4;
   memcpy(req + n, "main", 4);
   n += 4;
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, n, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* trailing bytes mean the arities disagree */
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_DELETE, two, 2);
   req[len] = 0;
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len + 1u, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* an empty field is not a key */
   const char *empty[] = {"", "main"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_DELETE, empty, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   printf("  PASS: test_git_ownership_malformed_frames_are_refused\n");
}

/* A read distinguishes "no row" from "the store is broken". Flattening the two
   would report an unreadable database as "no owner recorded", and the caller
   would take a branch it does not own. */
static void test_git_ownership_store_failure_is_not_a_miss(void)
{
   uint8_t req[512], resp[4096];
   uint32_t resp_len = 0;

   /* No open database: every read fails rather than finding nothing. */
   db1_shutdown();
   const char *owner_get[] = {"/repo/one", "feature"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, owner_get, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_FAILED);

   /* A write says so too, rather than reporting a silent success. */
   const char *upsert[] = {"/repo/one", "feature", "sess"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_UPSERT, upsert, 3);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_FAILED);

   printf("  PASS: test_git_ownership_store_failure_is_not_a_miss\n");
}

/* The serving side carries a large field too, and frees what it allocated to do
   it. Built with -fsanitize=address this is also the leak check for the scratch
   buffer the decoder now owns. */
static void test_stage_carries_a_large_field(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-big-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   enum
   {
      BIG = 32u * 1024u
   };
   char *big = malloc(BIG + 1u);
   assert(big != NULL);
   memset(big, 'r', BIG);
   big[BIG] = '\0';

   uint8_t *req = malloc(BIG + 4096u);
   uint8_t resp[4096];
   uint32_t resp_len = 0;
   assert(req != NULL);
   const char *values[] = {big, "feature", "sess-big"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_UPSERT, values, 3);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);

   /* And it round-trips: the long key is what the row was written under. */
   const char *owner_get[] = {big, "feature"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, owner_get, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);

   free(req);
   free(big);
   db1_shutdown();
   remove(path);
   printf("  PASS: test_stage_carries_a_large_field\n");
}

int main(void)
{
   printf("db1_module_stage:\n");
   test_malformed_frames_are_refused();
   test_save_then_load_round_trips();
   test_over_long_state_is_refused_not_truncated();
   test_unknown_stage_is_refused();
   test_git_ownership_round_trips();
   test_git_ownership_malformed_frames_are_refused();
   test_git_ownership_store_failure_is_not_a_miss();
   test_stage_carries_a_large_field();
   printf("db1_module_stage: ok\n");
   return 0;
}
