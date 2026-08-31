/* test_db1_agent_work_client.c: the agent_work family's generated bus client.
 *
 * The new shape here is an operation that takes NOTHING. "What is the queue's
 * status" names no row, so its request is a header and no fields at all, and
 * the frame had refused that on both sides -- a zero count read as malformed.
 *
 * The bus is stubbed; the module's half is covered by unit-test-db1-module-stage.
 * What matters here is that a zero-field frame is built and sized correctly, and
 * that a struct reply still lands member by member when nothing was sent. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/core/event_bus/module_client.h>
#include "db1_client/cognify_jobs.h"
#include "db1_client/db1_module_api.h"

/* --- the stubbed bus ------------------------------------------------------ */

static int stub_available = 1;
static aimee_module_call_result_t stub_result = AIMEE_MODULE_CALL_OK;
static uint32_t stub_status = AIMEE_DB1_STATUS_OK;
static int stub_calls;
static uint8_t stub_request[1024];
static uint32_t stub_request_len;
/* The reply's values, in order. */
static const char *stub_values[16];
static uint32_t stub_value_count;

int obs_bus_module_available(uint32_t event_kind)
{
   assert(event_kind == AIMEE_DB1_EVENT_AGENT_WORK);
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
   assert(event_kind == AIMEE_DB1_EVENT_AGENT_WORK);
   assert(stage_id == AIMEE_DB1_STAGE_AGENT_WORK);
   assert(deadline_ns != 0);

   stub_calls++;
   stub_request_len = request_len < sizeof stub_request ? request_len : 0;
   if (stub_request_len)
      memcpy(stub_request, request_body, stub_request_len);
   if (stub_result != AIMEE_MODULE_CALL_OK)
      return stub_result;

   uint8_t *out = (uint8_t *)response_body;
   if (response_capacity < 8u)
      return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
   aimee_db1_put_u32(out, stub_status);
   aimee_db1_put_u32(out + 4u, stub_value_count);
   uint32_t at = 8u;
   for (uint32_t i = 0; i < stub_value_count; ++i)
   {
      uint32_t n = (uint32_t)strlen(stub_values[i]);
      if (response_capacity < at + 4u + n)
         return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, stub_values[i], n);
      at += n;
   }
   *response_len = at;
   return AIMEE_MODULE_CALL_OK;
}

static void reset(void)
{
   stub_available = 1;
   stub_result = AIMEE_MODULE_CALL_OK;
   stub_status = AIMEE_DB1_STATUS_OK;
   stub_calls = 0;
   stub_request_len = 0;
   stub_value_count = 0;
}

static void answer(const char *const *values, uint32_t count)
{
   for (uint32_t i = 0; i < count; ++i)
      stub_values[i] = values[i];
   stub_value_count = count;
}

/* --- the tests ------------------------------------------------------------ */

/* An operation that takes nothing sends a header and nothing else: the opcode
   and a field count of zero, eight bytes. The frame used to refuse that, so a
   queue-wide question could not be asked at all. */
static void test_an_operation_with_no_arguments_sends_only_a_header(void)
{
   reset();
   const char *values[] = {"3", "1", "9", "2", "15"};
   answer(values, 5);

   db1_cognify_job_stats_t stats;
   assert(db1_cognify_job_status(&stats) == 0);
   assert(stub_calls == 1);
   assert(stub_request_len == 8u);
   assert(aimee_db1_get_u32(stub_request) == AIMEE_DB1_OP_COGNIFY_STATUS);
   assert(aimee_db1_get_u32(stub_request + 4u) == 0u);

   /* And the struct reply still lands member by member. */
   assert(stats.pending == 3 && stats.running == 1 && stats.done == 9);
   assert(stats.failed == 2 && stats.total == 15);
   printf("  PASS: test_an_operation_with_no_arguments_sends_only_a_header\n");
}

/* This domain answers 1 claimed / 0 nothing there / -1 broken, and all three
   have to survive the crossing. Folding "nothing there" into "broken" would
   make a worker back off from an idle queue as though the store were down;
   folding it the other way would hand out a job whose id is zero. */
static void test_an_empty_queue_is_nothing_not_a_failure(void)
{
   reset();
   const char *claimed[] = {"12",      "4294967297", "1",          "3", "cognify",
                            "running", "host-a",     "2026-08-16", ""};
   answer(claimed, 9);
   db1_cognify_job_t job;
   assert(db1_cognify_job_claim_next(&job) == 1);
   assert(job.id == 12 && job.memory_id == 4294967297LL);
   assert(strcmp(job.kind, "cognify") == 0);

   /* Nothing to claim: zero, and the caller keeps polling. */
   reset();
   stub_status = AIMEE_DB1_STATUS_MISSING;
   memset(&job, 0xAB, sizeof job);
   assert(db1_cognify_job_claim_next(&job) == 0);

   /* Broken: negative, and the caller does not read that as an idle queue. */
   reset();
   stub_status = AIMEE_DB1_STATUS_FAILED;
   assert(db1_cognify_job_claim_next(&job) == -1);

   reset();
   stub_available = 0;
   assert(db1_cognify_job_claim_next(&job) == -1);
   assert(stub_calls == 0);
   printf("  PASS: test_an_empty_queue_is_nothing_not_a_failure\n");
}

/* An int64 argument travels as decimal text and is a distinct C type: as a
   struct member its width came from the struct, but as a parameter "int" is a
   different function and the caller's memory id would be truncated. */
static void test_an_int64_argument_crosses_whole(void)
{
   reset();
   int64_t big = 4294967297LL; /* 2^32 + 1: truncating to int loses it */
   assert(db1_cognify_job_enqueue(big) == 0);
   assert(stub_request_len > 8u);
   uint32_t n = aimee_db1_get_u32(stub_request + 8u);
   assert(n == strlen("4294967297"));
   assert(memcmp(stub_request + 12u, "4294967297", n) == 0);
   printf("  PASS: test_an_int64_argument_crosses_whole\n");
}

/* The optional error text is allowed to be absent and travels as empty, while
   the status it accompanies is required. */
static void test_a_mark_carries_its_optional_error(void)
{
   reset();
   assert(db1_cognify_job_mark(7, "failed", NULL) == 0);
   assert(aimee_db1_get_u32(stub_request + 4u) == 3u);

   reset();
   assert(db1_cognify_job_mark(7, "", "boom") == -1);
   assert(db1_cognify_job_mark(7, NULL, "boom") == -1);
   assert(stub_calls == 0);
   printf("  PASS: test_a_mark_carries_its_optional_error\n");
}

int main(void)
{
   printf("db1_agent_work_client:\n");
   test_an_operation_with_no_arguments_sends_only_a_header();
   test_an_empty_queue_is_nothing_not_a_failure();
   test_an_int64_argument_crosses_whole();
   test_a_mark_carries_its_optional_error();
   printf("db1_agent_work_client: ok\n");
   return 0;
}
