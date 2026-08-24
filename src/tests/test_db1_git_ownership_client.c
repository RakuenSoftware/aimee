/* test_db1_git_ownership_client.c: the bus client the daemon now links.
 *
 * This is the production path for branch ownership, so the bytes it emits and
 * the way it reads a reply are the contract. The bus is stubbed rather than
 * run: what matters here is the framing and the result mapping, and the module
 * that answers these frames is covered by unit-test-db1-module-stage.
 *
 * The mapping is the part with teeth. A failure must surface as -1 and never as
 * "no owner": branch_own_check() treats a negative as "no enforcement" and
 * allows the operation, which is what it has always done without a database,
 * whereas a fabricated 0 asserts the branch is unowned and would let a caller
 * take one somebody else holds. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/core/event_bus/module_client.h>
#include "db1_client/db1_module_api.h"
#include "db1_client/git_ownership.h"

/* --- the stubbed bus ------------------------------------------------------ */

static int stub_available = 1;
static aimee_module_call_result_t stub_result = AIMEE_MODULE_CALL_OK;
static uint32_t stub_status = AIMEE_DB1_STATUS_OK;
static const char *stub_value = "";
static int stub_calls;
static uint8_t stub_request[1024];
static uint32_t stub_request_len;
static uint32_t stub_frame_len;
/* When set, the reply declares a length that disagrees with what it carries. */
static int stub_lie_about_length;
/* When set, the reply carries no values at all where the caller asked for one:
   what a module built against an older contract would send. */
static int stub_drop_the_value;

int obs_bus_module_available(uint32_t event_kind)
{
   assert(event_kind == AIMEE_DB1_EVENT_GIT_OWNERSHIP);
   return stub_available;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id;
   (void)cancelled;
   (void)cancel_context;
   assert(event_kind == AIMEE_DB1_EVENT_GIT_OWNERSHIP);
   assert(stage_id == AIMEE_DB1_STAGE_GIT_OWNERSHIP);
   assert(deadline_ns != 0);

   stub_calls++;
   /* Record the TRUE length even when the frame outgrows the capture buffer:
      the point of the large-field test is how many bytes were sent. */
   stub_frame_len = request_len;
   stub_request_len = request_len < sizeof stub_request ? request_len : 0;
   if (stub_request_len)
      memcpy(stub_request, request_body, stub_request_len);
   if (stub_result != AIMEE_MODULE_CALL_OK)
      return stub_result;

   /* status | field_count | (len | bytes) * count.

      A write answers with no values, exactly as the real stage does. The client
      sizes its reply buffer from what it asked for, so a stub that always sent
      a value would be asking for room the caller never requested. */
   uint32_t op = request_len >= 4u ? aimee_db1_get_u32((const uint8_t *)request_body) : 0u;
   int answers_with_a_value =
       (op == AIMEE_DB1_OP_OWNERSHIP_OWNER_GET || op == AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION ||
        op == AIMEE_DB1_OP_OWNERSHIP_SESSION_BY_PREFIX);
   uint32_t value_len = (uint32_t)strlen(stub_value);
   uint8_t *out = (uint8_t *)response_body;
   if (answers_with_a_value && stub_drop_the_value)
   {
      if (response_capacity < 8u)
         return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
      aimee_db1_put_u32(out, stub_status);
      aimee_db1_put_u32(out + 4u, 0u);
      *response_len = 8u;
      return AIMEE_MODULE_CALL_OK;
   }
   if (!answers_with_a_value)
   {
      if (response_capacity < 8u)
         return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
      aimee_db1_put_u32(out, stub_status);
      aimee_db1_put_u32(out + 4u, 0u);
      *response_len = 8u;
      return AIMEE_MODULE_CALL_OK;
   }
   if (response_capacity < 12u + value_len)
      return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
   aimee_db1_put_u32(out, stub_status);
   aimee_db1_put_u32(out + 4u, 1u);
   aimee_db1_put_u32(out + 8u, stub_lie_about_length ? value_len + 7u : value_len);
   memcpy(out + 12u, stub_value, value_len);
   *response_len = 12u + value_len;
   return AIMEE_MODULE_CALL_OK;
}

static void reset(void)
{
   stub_available = 1;
   stub_result = AIMEE_MODULE_CALL_OK;
   stub_status = AIMEE_DB1_STATUS_OK;
   stub_value = "";
   stub_calls = 0;
   stub_request_len = 0;
   stub_frame_len = 0;
   stub_lie_about_length = 0;
   stub_drop_the_value = 0;
}

/* --- reading back the frame the client emitted ---------------------------- */

static uint32_t frame_op(void)
{
   return aimee_db1_get_u32(stub_request);
}
static uint32_t frame_count(void)
{
   return aimee_db1_get_u32(stub_request + 4u);
}

static int frame_field_is(uint32_t index, const char *expected)
{
   uint32_t at = 8u;
   for (uint32_t i = 0; i < index; ++i)
   {
      uint32_t n = aimee_db1_get_u32(stub_request + at);
      at += 4u + n;
   }
   uint32_t len = aimee_db1_get_u32(stub_request + at);
   return len == strlen(expected) && memcmp(stub_request + at + 4u, expected, len) == 0;
}

static void test_upsert_emits_the_declared_frame(void)
{
   reset();
   assert(db1_git_ownership_upsert("/repo", "feature", "sess-1") == 0);
   assert(stub_calls == 1);
   assert(frame_op() == AIMEE_DB1_OP_OWNERSHIP_UPSERT);
   assert(frame_count() == 3u);
   assert(frame_field_is(0, "/repo"));
   assert(frame_field_is(1, "feature"));
   assert(frame_field_is(2, "sess-1"));
   /* The frame is exactly its fields: nothing padded, nothing trailing. */
   assert(stub_request_len == 8u + (4u + 5u) + (4u + 7u) + (4u + 6u));
   printf("  PASS: test_upsert_emits_the_declared_frame\n");
}

static void test_reads_report_found_missing_and_error_apart(void)
{
   char owner[128];

   reset();
   stub_status = AIMEE_DB1_STATUS_OK;
   stub_value = "sess-9";
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == 1);
   assert(strcmp(owner, "sess-9") == 0);

   reset();
   stub_status = AIMEE_DB1_STATUS_MISSING;
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == 0);
   assert(owner[0] == '\0');

   /* A store that failed is NOT an unowned branch. */
   reset();
   stub_status = AIMEE_DB1_STATUS_FAILED;
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == -1);

   printf("  PASS: test_reads_report_found_missing_and_error_apart\n");
}

static void test_an_unreachable_module_is_an_error_not_an_absence(void)
{
   char owner[128];

   /* Nothing serving the stage. */
   reset();
   stub_available = 0;
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == -1);
   assert(stub_calls == 0); /* and no call was attempted */

   /* Serving, but the call did not complete. */
   for (int i = 0; i < 3; ++i)
   {
      reset();
      stub_result = (i == 0)   ? AIMEE_MODULE_CALL_DEADLINE_EXCEEDED
                    : (i == 1) ? AIMEE_MODULE_CALL_CAPABILITY_DENIED
                               : AIMEE_MODULE_CALL_INTERNAL;
      assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == -1);
      assert(db1_git_ownership_upsert("/repo", "feature", "sess-1") == -1);
   }
   printf("  PASS: test_an_unreachable_module_is_an_error_not_an_absence\n");
}

static void test_a_malformed_reply_is_refused(void)
{
   char owner[128];
   reset();
   stub_lie_about_length = 1;
   stub_value = "sess-9";
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == -1);
   printf("  PASS: test_a_malformed_reply_is_refused\n");
}

/* A stage that answers with FEWER values than the caller asked for is the same
   contract mismatch as one that answers with more, read from the other side --
   and it used to pass. The unfilled slots keep the empty string the client
   clears them to, so the caller read a blank where a value should have been and
   could not tell that from a value that is genuinely blank.

   This is not hypothetical across a process boundary: the daemon and the module
   are two binaries deployed at two times, and a module built before a reply
   grew a value sends exactly this. */
static void test_a_reply_with_too_few_values_is_refused(void)
{
   char owner[128];
   reset();
   stub_drop_the_value = 1;
   stub_value = "sess-9";
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == -1);
   printf("  PASS: test_a_reply_with_too_few_values_is_refused\n");
}

static void test_bad_arguments_cost_no_round_trip(void)
{
   char owner[128];
   reset();
   assert(db1_git_ownership_upsert(NULL, "feature", "sess") == -1);
   assert(db1_git_ownership_upsert("/repo", "", "sess") == -1);
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, 0) == -1);
   assert(stub_calls == 0);
   printf("  PASS: test_bad_arguments_cost_no_round_trip\n");
}

static void test_every_operation_uses_its_own_op_and_arity(void)
{
   char out[128];

   reset();
   assert(db1_git_ownership_delete("/repo", "feature") == 0);
   assert(frame_op() == AIMEE_DB1_OP_OWNERSHIP_DELETE && frame_count() == 2u);

   reset();
   stub_value = "feature";
   assert(db1_git_ownership_get_branch_for_session("/repo", "sess-1", out, sizeof out) == 1);
   assert(frame_op() == AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION && frame_count() == 2u);
   assert(frame_field_is(1, "sess-1"));

   /* The global lookup carries only its prefix: there is no repository to scope
      a session-prefix search by. */
   reset();
   stub_value = "sess-abc";
   assert(db1_git_ownership_find_session_by_prefix("sess-a", out, sizeof out) == 1);
   assert(frame_op() == AIMEE_DB1_OP_OWNERSHIP_SESSION_BY_PREFIX && frame_count() == 1u);
   assert(frame_field_is(0, "sess-a"));

   printf("  PASS: test_every_operation_uses_its_own_op_and_arity\n");
}

/* A field far larger than any fixed cap is CARRIED, not refused. These fields
   hold prompts, results and documents elsewhere in DB1, and an in-process
   caller has always passed them whole -- refusing here would return the same -1
   as a broken store, and would do it only on production-sized data. */
static void test_a_large_field_is_carried_not_refused(void)
{
   enum
   {
      BIG = 64u * 1024u
   };
   char *big = malloc(BIG + 1u);
   assert(big != NULL);
   memset(big, 'x', BIG);
   big[BIG] = '\0';

   reset();
   assert(db1_git_ownership_upsert(big, "feature", "sess-1") == 0);
   assert(stub_calls == 1);
   /* 8 header bytes, then each field as a 4-byte length plus its bytes. */
   assert(stub_frame_len == 8u + (4u + BIG) + (4u + 7u) + (4u + 6u));
   free(big);
   printf("  PASS: test_a_large_field_is_carried_not_refused\n");
}

/* A value that exactly fills the caller's buffer leaves no room for the NUL, so
   it is refused rather than terminated one byte past the end. Built with
   -fsanitize=address this is the check that says so. */
static void test_a_value_that_fills_the_buffer_is_refused(void)
{
   char owner[8];
   reset();
   stub_status = AIMEE_DB1_STATUS_OK;
   stub_value = "12345678"; /* exactly sizeof owner, so the NUL would not fit */
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == -1);

   /* One shorter still fits, terminator included. */
   reset();
   stub_status = AIMEE_DB1_STATUS_OK;
   stub_value = "1234567";
   assert(db1_git_ownership_get_owner("/repo", "feature", owner, sizeof owner) == 1);
   assert(strcmp(owner, "1234567") == 0);
   printf("  PASS: test_a_value_that_fills_the_buffer_is_refused\n");
}

int main(void)
{
   printf("db1_git_ownership_client:\n");
   test_upsert_emits_the_declared_frame();
   test_reads_report_found_missing_and_error_apart();
   test_an_unreachable_module_is_an_error_not_an_absence();
   test_a_malformed_reply_is_refused();
   test_a_reply_with_too_few_values_is_refused();
   test_a_value_that_fills_the_buffer_is_refused();
   test_bad_arguments_cost_no_round_trip();
   test_a_large_field_is_carried_not_refused();
   test_every_operation_uses_its_own_op_and_arity();
   printf("db1_git_ownership_client: ok\n");
   return 0;
}
