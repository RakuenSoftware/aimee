#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include "wm.h"

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
   assert(aimee_db1_get_u32(resp + 4) == 1u); /* one value in the reply */
   uint32_t value_len = aimee_db1_get_u32(resp + 8);
   assert(value_len == strlen("sess-abc123"));
   assert(memcmp(resp + 12, "sess-abc123", value_len) == 0);

   const char *by_session[] = {"/repo/one", "sess-abc123"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION, by_session, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);
   assert(aimee_db1_get_u32(resp + 4) == 1u);
   value_len = aimee_db1_get_u32(resp + 8);
   assert(value_len == strlen("feature"));
   assert(memcmp(resp + 12, "feature", value_len) == 0);

   /* A row nobody wrote is MISSING, not a failure: no owner is a real answer. */
   const char *absent[] = {"/repo/one", "other"};
   len = fields_frame(req, AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, absent, 2);
   assert(call_stage(AIMEE_DB1_STAGE_GIT_OWNERSHIP, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_MISSING);
   /* A read answers with its value slot present and empty, not absent. */
   assert(aimee_db1_get_u32(resp + 4) == 1u);
   assert(aimee_db1_get_u32(resp + 8) == 0u);

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
   /* Sized from the constant rather than written out: FIELDS_MAX is derived
      from the widest request in the catalog, so a literal list silently stops
      covering this case the moment a wider operation is declared. */
   const char *too_many[AIMEE_DB1_FIELDS_MAX + 1u];
   for (uint32_t i = 0; i < AIMEE_DB1_FIELDS_MAX + 1u; ++i)
      too_many[i] = "x";
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

/* Read one counted value out of a reply, by index. */
static const char *reply_value(const uint8_t *resp, uint32_t resp_len, uint32_t index,
                               uint32_t *len_out)
{
   uint32_t at = 8u;
   for (uint32_t i = 0; i < aimee_db1_get_u32(resp + 4u); ++i)
   {
      assert(at + 4u <= resp_len);
      uint32_t n = aimee_db1_get_u32(resp + at);
      at += 4u;
      assert(at + n <= resp_len);
      if (i == index)
      {
         *len_out = n;
         return (const char *)resp + at;
      }
      at += n;
   }
   assert(0 && "reply has no such value");
   return NULL;
}

/* A list crosses as its rows flattened member by member, and the reply's own
   value count is what tells the caller how many rows arrived -- an operation
   knows how wide its rows are, so the width is never sent. */
static void test_wm_list_returns_every_member_of_every_row(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-wmlist-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   assert(db1_wm_set("sess-1", "alpha", "first", "notes", 0) == 0);
   assert(db1_wm_set("sess-1", "beta", "second", "notes", 0) == 0);
   /* Another session's entry must not appear in this session's list. */
   assert(db1_wm_set("sess-2", "gamma", "third", "notes", 0) == 0);

   uint8_t req[1024], resp[8192];
   uint32_t resp_len = 0;
   const char *list[] = {"sess-1", "", "16"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_WM_LIST, list, 3);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);
   /* Two rows of eight members: the count is rows times width, not a row
      count, because the width is the operation's own contract. */
   assert(aimee_db1_get_u32(resp + 4u) == 16u);

   /* The key member of each row, at member offset 2 of 8. */
   uint32_t n = 0;
   const char *first = reply_value(resp, resp_len, 2u, &n);
   assert(n == strlen("alpha") && memcmp(first, "alpha", n) == 0);
   const char *second = reply_value(resp, resp_len, 8u + 2u, &n);
   assert(n == strlen("beta") && memcmp(second, "beta", n) == 0);
   /* The int64 id member travels as decimal text like every other integer. */
   const char *id = reply_value(resp, resp_len, 0u, &n);
   assert(n > 0 && id[0] >= '0' && id[0] <= '9');

   db1_shutdown();
   remove(path);
   printf("  PASS: test_wm_list_returns_every_member_of_every_row\n");
}

/* An empty list is an answer, not an absence. The stage must not borrow the
   read convention here: a read with nothing to say returns one empty value and
   MISSING, and a list saying "there are none" would then be indistinguishable
   from a row whose every member is blank. */
static void test_wm_list_finds_nothing_and_says_so(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-wmnone-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   uint8_t req[1024], resp[8192];
   uint32_t resp_len = 0;
   const char *list[] = {"sess-empty", "", "16"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_WM_LIST, list, 3);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);
   assert(aimee_db1_get_u32(resp + 4u) == 0u);

   db1_shutdown();
   remove(path);
   printf("  PASS: test_wm_list_finds_nothing_and_says_so\n");
}

/* The bound is an allocation. A stage that took the caller's word for it would
   size an array from the wire, so it is checked against the ceiling the catalog
   declares before anything is allocated from it. */
static void test_wm_list_refuses_a_bound_it_will_not_allocate(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-wmcap-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   uint8_t req[1024], resp[8192];
   uint32_t resp_len = 0;
   const char *too_many[] = {"sess-1", "", "65"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_WM_LIST, too_many, 3);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   const char *none[] = {"sess-1", "", "0"};
   len = fields_frame(req, AIMEE_DB1_OP_WM_LIST, none, 3);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   const char *negative[] = {"sess-1", "", "-4"};
   len = fields_frame(req, AIMEE_DB1_OP_WM_LIST, negative, 3);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* Not a number at all is refused by the same parse every integer uses. */
   const char *nonsense[] = {"sess-1", "", "sixteen"};
   len = fields_frame(req, AIMEE_DB1_OP_WM_LIST, nonsense, 3);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_INVALID_REQUEST);

   db1_shutdown();
   remove(path);
   printf("  PASS: test_wm_list_refuses_a_bound_it_will_not_allocate\n");
}

/* A bound smaller than what is stored is honoured: the caller's array is the
   limit, and returning more rows than it asked for would write past its end. */
static void test_wm_list_stops_at_the_bound_it_was_given(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-wmbound-%d.db", platform_tmpdir(),
            (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   for (int i = 0; i < 5; ++i)
   {
      char key[32];
      snprintf(key, sizeof key, "key-%d", i);
      assert(db1_wm_set("sess-1", key, "value", "notes", 0) == 0);
   }

   uint8_t req[1024], resp[8192];
   uint32_t resp_len = 0;
   const char *list[] = {"sess-1", "", "2"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_WM_LIST, list, 3);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);
   assert(aimee_db1_get_u32(resp + 4u) == 16u); /* two rows, not five */

   db1_shutdown();
   remove(path);
   printf("  PASS: test_wm_list_stops_at_the_bound_it_was_given\n");
}

/* The domain returns memory. The stage hands that straight to the reply rather
   than copying it through the stack buffer -- these carry an assembled context,
   not an identifier -- and frees it after the reply is written. Whether it is
   freed is ASAN's business; whether it ARRIVES is this test's. */
static void test_assembled_context_crosses_and_is_released(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-wmctx-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   assert(db1_wm_set("sess-ctx", "alpha", "the first value", "notes", 0) == 0);

   uint8_t req[1024], resp[4096];
   uint32_t resp_len = 0;
   const char *args[] = {"sess-ctx"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_WM_ASSEMBLE_CONTEXT, args, 1);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_OK);
   assert(aimee_db1_get_u32(resp + 4u) == 1u);
   uint32_t n = aimee_db1_get_u32(resp + 8u);
   /* The value the domain built, not an empty placeholder: a stage that
      dropped the returned pointer would answer with nothing and read as a
      session that has no working memory. */
   assert(n > 0);
   assert(memmem(resp + 12u, n, "the first value", strlen("the first value")) != NULL);

   db1_shutdown();
   remove(path);
   printf("  PASS: test_assembled_context_crosses_and_is_released\n");
}

/* A session with nothing assembles to NULL, which must reach the caller as a
   miss rather than as an empty string it might render. */
static void test_an_unassembled_context_is_a_miss(void)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-db1-wmctx0-%d.db", platform_tmpdir(), (int)getpid());
   remove(path);
   db1_shutdown();
   assert(db1_init(path) == 0);

   uint8_t req[1024], resp[4096];
   uint32_t resp_len = 0;
   const char *args[] = {"sess-nothing"};
   uint32_t len = fields_frame(req, AIMEE_DB1_OP_WM_ASSEMBLE_CONTEXT, args, 1);
   assert(call_stage(AIMEE_DB1_STAGE_CONVERSATION, req, len, resp, &resp_len) ==
          AIMEE_MODULE_STATUS_OK);
   assert(aimee_db1_get_u32(resp) == AIMEE_DB1_STATUS_MISSING);

   db1_shutdown();
   remove(path);
   printf("  PASS: test_an_unassembled_context_is_a_miss\n");
}

int main(void)
{
   printf("db1_module_stage:\n");
   test_assembled_context_crosses_and_is_released();
   test_an_unassembled_context_is_a_miss();
   test_malformed_frames_are_refused();
   test_save_then_load_round_trips();
   test_over_long_state_is_refused_not_truncated();
   test_unknown_stage_is_refused();
   test_git_ownership_round_trips();
   test_git_ownership_malformed_frames_are_refused();
   test_git_ownership_store_failure_is_not_a_miss();
   test_stage_carries_a_large_field();
   test_wm_list_returns_every_member_of_every_row();
   test_wm_list_finds_nothing_and_says_so();
   test_wm_list_refuses_a_bound_it_will_not_allocate();
   test_wm_list_stops_at_the_bound_it_was_given();
   printf("db1_module_stage: ok\n");
   return 0;
}
