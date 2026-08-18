#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <aimee/db2/module_api.h>
#include <aimee/db2/client.h>

#include "module_adapter.h"

static int cancelled;
static int cancel_after;
static int cancel_checks;
static int health_result;
static int kb_health_result;
static int health_calls;
static int kb_health_calls;
static int embedding_dimension_value;
static int embedding_dimension_calls;
static int pool_status_result;
static long long refused_count_value;
static int last_offered_value;
static int embedding_refusals_result;
static int postgres_status_result;
static int reembed_status_result;
static int reembed_clear_result;
static aimee_module_call_result_t transport_result;
static uint8_t transport_response[AIMEE_DB2_POOL_STATUS_RESPONSE_LEN];
static uint32_t transport_response_len;
static int transport_calls;
static int transport_expect_dimension;
static int transport_expect_pool;
static int transport_expect_refusals;
static int transport_expect_postgres;
static int transport_expect_reembed;
static int transport_expect_reembed_clear;

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   assert(invocation);
   cancel_checks++;
   return cancelled || (cancel_after > 0 && cancel_checks >= cancel_after);
}

static int health_probe(int *schema_ok, int *have_pg_trgm)
{
   health_calls++;
   if (schema_ok)
      *schema_ok = 1;
   if (have_pg_trgm)
      *have_pg_trgm = 1;
   return health_result;
}

static int kb_health_probe(int *kb_tables_ok)
{
   kb_health_calls++;
   if (kb_tables_ok)
      *kb_tables_ok = 1;
   return kb_health_result;
}

int db2_health_probe(int *schema_ok, int *have_pg_trgm)
{
   return health_probe(schema_ok, have_pg_trgm);
}

int db2_kb_health_probe(int *kb_tables_ok)
{
   return kb_health_probe(kb_tables_ok);
}

int db2_embedding_dim(void)
{
   embedding_dimension_calls++;
   return embedding_dimension_value;
}

static int embedding_dimension(void)
{
   embedding_dimension_calls++;
   return embedding_dimension_value;
}

void db2_pool_stats(int *size, int *in_use, int *waiters, long *lease_grants, long *lease_timeouts,
                    long *stuck, long *poisoned)
{
   if (size)
      *size = 16;
   if (in_use)
      *in_use = 2;
   if (waiters)
      *waiters = 1;
   if (lease_grants)
      *lease_grants = 10;
   if (lease_timeouts)
      *lease_timeouts = 3;
   if (stuck)
      *stuck = 4;
   if (poisoned)
      *poisoned = 5;
}

static int pool_status(aimee_db2_pool_status_t *status)
{
   *status = (aimee_db2_pool_status_t){16, 2, 1, 10, 3, 4, 5};
   return pool_status_result;
}

long long db2_embedding_dim_refused_count(void)
{
   return refused_count_value;
}

int db2_embedding_dim_last_offered(void)
{
   return last_offered_value;
}

static int embedding_refusals(aimee_db2_embedding_refusals_t *status)
{
   *status = (aimee_db2_embedding_refusals_t){7, 768};
   return embedding_refusals_result;
}

int db2_pg_stat_summary(int *active, int *maximum, int *replica, int64_t *lag)
{
   if (active)
      *active = 12;
   if (maximum)
      *maximum = 100;
   if (replica)
      *replica = 1;
   if (lag)
      *lag = 1048576;
   return 0;
}

static int postgres_status(aimee_db2_postgres_status_t *status)
{
   *status = (aimee_db2_postgres_status_t){15, 12, 100, 1, 1048576};
   return postgres_status_result;
}

int db2_reembed_in_progress_get(int *target, long *started)
{
   if (target)
      *target = 384;
   if (started)
      *started = 1700000000;
   return 1;
}

static int reembed_status(aimee_db2_reembed_status_t *status)
{
   *status = (aimee_db2_reembed_status_t){384, 1700000000};
   return reembed_status_result;
}

int db2_reembed_in_progress_clear(void)
{
   return reembed_clear_result;
}

static void reset(void)
{
   cancelled = 0;
   cancel_after = 0;
   cancel_checks = 0;
   health_result = 0;
   kb_health_result = 0;
   health_calls = 0;
   kb_health_calls = 0;
   embedding_dimension_value = 384;
   embedding_dimension_calls = 0;
   pool_status_result = 0;
   refused_count_value = 7;
   last_offered_value = 768;
   embedding_refusals_result = 0;
   postgres_status_result = 0;
   reembed_status_result = 1;
   reembed_clear_result = 0;
   transport_result = AIMEE_MODULE_CALL_OK;
   transport_response_len = AIMEE_DB2_RESPONSE_LEN;
   transport_calls = 0;
   transport_expect_dimension = 0;
   transport_expect_pool = 0;
   transport_expect_refusals = 0;
   transport_expect_postgres = 0;
   transport_expect_reembed = 0;
   transport_expect_reembed_clear = 0;
   assert(aimee_db2_health_response_encode(AIMEE_DB2_FLAG_SCHEMA | AIMEE_DB2_FLAG_KB_TABLES,
                                           transport_response, sizeof(transport_response)) == 0);
}

static aimee_module_call_result_t
transport(void *context, uint32_t event_kind, uint32_t stage_id, uint64_t trace_id,
          uint64_t deadline_ns, const void *request_body, uint32_t request_len, void *response_body,
          uint32_t response_capacity, uint32_t *response_len,
          aimee_module_cancelled_fn cancelled_fn, void *cancel_context)
{
   assert(context == (void *)0x1234);
   uint32_t expected_event = transport_expect_reembed_clear ? AIMEE_DB2_EVENT_REEMBED_CLEAR
                             : transport_expect_reembed     ? AIMEE_DB2_EVENT_REEMBED_STATUS
                             : transport_expect_postgres    ? AIMEE_DB2_EVENT_POSTGRES_STATUS
                             : transport_expect_refusals    ? AIMEE_DB2_EVENT_EMBEDDING_REFUSALS
                             : transport_expect_pool        ? AIMEE_DB2_EVENT_POOL_STATUS
                             : transport_expect_dimension   ? AIMEE_DB2_EVENT_EMBEDDING_DIMENSION
                                                            : AIMEE_DB2_EVENT_HEALTH;
   uint32_t expected_stage = transport_expect_reembed_clear ? AIMEE_DB2_STAGE_REEMBED_CLEAR
                             : transport_expect_reembed     ? AIMEE_DB2_STAGE_REEMBED_STATUS
                             : transport_expect_postgres    ? AIMEE_DB2_STAGE_POSTGRES_STATUS
                             : transport_expect_refusals    ? AIMEE_DB2_STAGE_EMBEDDING_REFUSALS
                             : transport_expect_pool        ? AIMEE_DB2_STAGE_POOL_STATUS
                             : transport_expect_dimension   ? AIMEE_DB2_STAGE_EMBEDDING_DIMENSION
                                                            : AIMEE_DB2_STAGE_HEALTH;
   assert(event_kind == expected_event);
   assert(stage_id == expected_stage);
   assert(trace_id == 77);
   assert(deadline_ns == 88);
   if (transport_expect_reembed_clear)
      assert(aimee_db2_reembed_clear_request_decode(request_body, request_len) == 0);
   else if (transport_expect_reembed)
      assert(aimee_db2_reembed_status_request_decode(request_body, request_len) == 0);
   else if (transport_expect_postgres)
      assert(aimee_db2_postgres_status_request_decode(request_body, request_len) == 0);
   else if (transport_expect_refusals)
      assert(aimee_db2_embedding_refusals_request_decode(request_body, request_len) == 0);
   else if (transport_expect_pool)
      assert(aimee_db2_pool_status_request_decode(request_body, request_len) == 0);
   else if (transport_expect_dimension)
      assert(aimee_db2_embedding_dimension_request_decode(request_body, request_len) == 0);
   else
      assert(aimee_db2_health_request_decode(request_body, request_len) == 0);
   assert(cancelled_fn == NULL && cancel_context == NULL);
   transport_calls++;
   if (transport_result != AIMEE_MODULE_CALL_OK)
      return transport_result;
   if (transport_response_len > response_capacity)
      return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
   memcpy(response_body, transport_response, transport_response_len);
   *response_len = transport_response_len;
   return AIMEE_MODULE_CALL_OK;
}

static void test_wire_contract(void)
{
   uint8_t request[AIMEE_DB2_REQUEST_LEN] = {0};
   assert(aimee_db2_health_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_health_request_decode(request, sizeof(request)) == 0);
   assert(aimee_db2_health_request_encode(NULL, sizeof(request)) == -1);
   assert(aimee_db2_health_request_encode(request, sizeof(request) - 1) == -1);
   assert(aimee_db2_health_request_decode(request, sizeof(request) - 1) == -1);
   request[0] ^= 1u;
   assert(aimee_db2_health_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_health_request_encode(request, sizeof(request)) == 0);
   request[4] ^= 1u;
   assert(aimee_db2_health_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_health_request_decode(NULL, sizeof(request)) == -1);

   for (uint32_t flags = 0; flags <= AIMEE_DB2_FLAG_ALL; ++flags)
   {
      uint8_t response[AIMEE_DB2_RESPONSE_LEN] = {0};
      int schema_ok = -1, have_pg_trgm = -1, kb_tables_ok = -1;
      assert(aimee_db2_health_response_encode(flags, response, sizeof(response)) == 0);
      assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                              &kb_tables_ok) == 0);
      assert(schema_ok == !!(flags & AIMEE_DB2_FLAG_SCHEMA));
      assert(have_pg_trgm == !!(flags & AIMEE_DB2_FLAG_PG_TRGM));
      assert(kb_tables_ok == !!(flags & AIMEE_DB2_FLAG_KB_TABLES));
   }

   uint8_t response[AIMEE_DB2_RESPONSE_LEN] = {0};
   int schema_ok = 1, have_pg_trgm = 1, kb_tables_ok = 1;
   assert(aimee_db2_health_response_encode(0, NULL, sizeof(response)) == -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response) - 1) == -1);
   assert(aimee_db2_health_response_encode(AIMEE_DB2_FLAG_ALL + 1u, response, sizeof(response)) ==
          -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response)) == 0);
   assert(aimee_db2_health_response_decode(NULL, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(aimee_db2_health_response_decode(response, sizeof(response) - 1, &schema_ok,
                                           &have_pg_trgm, &kb_tables_ok) == -1);
   response[0] ^= 1u;
   assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response)) == 0);
   response[4] ^= 1u;
   assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response)) == 0);
   aimee_db2_put_u32(response + 8, AIMEE_DB2_FLAG_ALL + 1u);
   assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response)) == 0);
   aimee_db2_put_u32(response + 12, 1u);
   assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);
}

static void test_body_envelope(void)
{
   uint8_t frame[AIMEE_DB2_ENVELOPE_HEADER_LEN + 3] = {0};
   const uint32_t operation = 0x01020304u;
   assert(aimee_db2_request_header_encode(operation, 5u, 3u, frame, sizeof(frame)) == 0);
   frame[AIMEE_DB2_ENVELOPE_HEADER_LEN] = 0xaa;
   frame[AIMEE_DB2_ENVELOPE_HEADER_LEN + 1] = 0xbb;
   frame[AIMEE_DB2_ENVELOPE_HEADER_LEN + 2] = 0xcc;
   aimee_db2_request_header_t request = {9, 9, 9};
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == 0);
   assert(request.operation == operation && request.flags == 5 && request.payload_len == 3);

   assert(aimee_db2_request_header_encode(0, 0, 0, frame, sizeof(frame)) == -1);
   assert(aimee_db2_request_header_encode(1, 0, 0, NULL, sizeof(frame)) == -1);
   assert(aimee_db2_request_header_encode(1, 0, 0, frame, AIMEE_DB2_ENVELOPE_HEADER_LEN - 1) == -1);
   assert(aimee_db2_request_header_decode(NULL, sizeof(frame), &request) == -1);
   assert(request.operation == 0 && request.flags == 0 && request.payload_len == 0);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), NULL) == -1);

   assert(aimee_db2_request_header_encode(operation, 5u, 3u, frame, sizeof(frame)) == 0);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame) - 1, &request) == -1);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame) + 1, &request) == -1);
   frame[0] ^= 1u;
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   frame[0] ^= 1u;
   frame[4] ^= 1u;
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   frame[4] ^= 1u;
   frame[6] ^= 1u;
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   frame[6] ^= 1u;
   aimee_db2_put_u32(frame + 8, 0);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   aimee_db2_put_u32(frame + 8, operation);
   aimee_db2_put_u32(frame + 20, 1);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   assert(aimee_db2_request_header_encode(operation, 5u, 3u, frame, sizeof(frame)) == 0);
   aimee_db2_put_u32(frame + 16, 4);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);

   for (uint32_t result = AIMEE_DB2_RESULT_OK; result <= AIMEE_DB2_RESULT_INVALID_STATE; ++result)
   {
      assert(aimee_db2_reply_header_encode(operation, result, 3u, frame, sizeof(frame)) == 0);
      aimee_db2_reply_header_t reply = {9, 9, 9};
      assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == 0);
      assert(reply.operation == operation && reply.result == result && reply.payload_len == 3);
   }
   assert(aimee_db2_reply_header_encode(0, 0, 0, frame, sizeof(frame)) == -1);
   assert(aimee_db2_reply_header_encode(operation, AIMEE_DB2_RESULT_INVALID_STATE + 1u, 0, frame,
                                        sizeof(frame)) == -1);
   assert(aimee_db2_reply_header_encode(operation, 0, 0, NULL, sizeof(frame)) == -1);
   assert(aimee_db2_reply_header_encode(operation, 0, 0, frame,
                                        AIMEE_DB2_ENVELOPE_HEADER_LEN - 1) == -1);
   assert(aimee_db2_reply_header_encode(operation, AIMEE_DB2_RESULT_OK, 3u, frame, sizeof(frame)) ==
          0);
   aimee_db2_put_u32(frame + 12, AIMEE_DB2_RESULT_INVALID_STATE + 1u);
   aimee_db2_reply_header_t reply = {9, 9, 9};
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   assert(reply.operation == 0 && reply.result == 0 && reply.payload_len == 0);
   assert(aimee_db2_reply_header_encode(operation, AIMEE_DB2_RESULT_OK, 3u, frame, sizeof(frame)) ==
          0);
   frame[0] ^= 1u;
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   frame[0] ^= 1u;
   frame[4] ^= 1u;
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   frame[4] ^= 1u;
   frame[6] ^= 1u;
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   frame[6] ^= 1u;
   aimee_db2_put_u32(frame + 8, 0);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   aimee_db2_put_u32(frame + 8, operation);
   aimee_db2_put_u32(frame + 16, 4);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   aimee_db2_put_u32(frame + 16, 3);
   aimee_db2_put_u32(frame + 20, 1);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   assert(aimee_db2_reply_header_encode(operation, AIMEE_DB2_RESULT_OK, 3u, frame, sizeof(frame)) ==
          0);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame) - 1, &reply) == -1);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame) + 1, &reply) == -1);
   assert(aimee_db2_reply_header_decode(NULL, sizeof(frame), &reply) == -1);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), NULL) == -1);
}

static void test_embedding_dimension_wire(void)
{
   uint8_t request[AIMEE_DB2_EMBEDDING_DIMENSION_REQUEST_LEN] = {0};
   assert(aimee_db2_embedding_dimension_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedding_dimension_request_decode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedding_dimension_request_encode(NULL, sizeof(request)) == -1);
   assert(aimee_db2_embedding_dimension_request_encode(request, sizeof(request) - 1) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_embedding_dimension_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_embedding_dimension_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedding_dimension_request_decode(request, sizeof(request) - 1) == -1);

   uint8_t reply[AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, result = 99, dimension = 99;
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply, sizeof(reply),
                                                     &reply_len) == 0);
   assert(reply_len == sizeof(reply));
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, &result, &dimension) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && dimension == 384);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 0, reply,
                                                     sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_EMBEDDING_DIMENSION_ERROR_LEN);
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, &result, &dimension) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && dimension == 0);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 0, reply, sizeof(reply),
                                                     &reply_len) == -1);
   assert(reply_len == 0);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK,
                                                     AIMEE_DB2_EMBEDDING_DIMENSION_MAX + 1u, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, 0, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 1, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, NULL, sizeof(reply),
                                                     &reply_len) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply, sizeof(reply),
                                                     NULL) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply,
                                                     sizeof(reply) - 1, &reply_len) == -1);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply, sizeof(reply),
                                                     &reply_len) == 0);
   aimee_db2_put_u32(reply + 8, AIMEE_DB2_OPERATION_HEALTH);
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, &result, &dimension) == -1);
   assert(result == 0 && dimension == 0);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply, sizeof(reply),
                                                     &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 0);
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, &result, &dimension) == -1);
   assert(aimee_db2_embedding_dimension_reply_decode(NULL, reply_len, &result, &dimension) == -1);
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, NULL, &dimension) == -1);
}

static void test_pool_status_wire(void)
{
   uint8_t request[AIMEE_DB2_POOL_STATUS_REQUEST_LEN] = {0};
   assert(aimee_db2_pool_status_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_pool_status_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_pool_status_request_decode(request, sizeof(request)) == -1);

   const aimee_db2_pool_status_t expected = {16, 2, 1, 10, 3, 4, 5};
   uint8_t reply[AIMEE_DB2_POOL_STATUS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, result = 99;
   aimee_db2_pool_status_t decoded = {0};
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply, sizeof(reply),
                                             &reply_len) == 0);
   assert(reply_len == sizeof(reply));
   assert(aimee_db2_pool_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.size == 16 && decoded.in_use == 2 &&
          decoded.waiters == 1 && decoded.lease_grants == 10 && decoded.lease_timeouts == 3 &&
          decoded.stuck == 4 && decoded.poisoned == 5);

   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                             sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_POOL_STATUS_ERROR_LEN);
   assert(aimee_db2_pool_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded.size == 0);

   aimee_db2_pool_status_t bad = expected;
   bad.size = 0;
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                             &reply_len) == -1);
   bad = expected;
   bad.in_use = bad.size + 1;
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                             &reply_len) == -1);
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, NULL, reply, sizeof(reply),
                                             &reply_len) == -1);
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, &expected, reply,
                                             sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply,
                                             sizeof(reply) - 1, &reply_len) == -1);

   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply, sizeof(reply),
                                             &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 0);
   assert(aimee_db2_pool_status_reply_decode(reply, reply_len, &result, &decoded) == -1);
   assert(result == 0 && decoded.size == 0);
   assert(aimee_db2_pool_status_reply_decode(NULL, reply_len, &result, &decoded) == -1);
}

static void test_embedding_refusals_wire(void)
{
   uint8_t request[AIMEE_DB2_EMBEDDING_REFUSALS_REQUEST_LEN] = {0};
   assert(aimee_db2_embedding_refusals_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedding_refusals_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1);
   assert(aimee_db2_embedding_refusals_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN] = {0};
   const aimee_db2_embedding_refusals_t expected = {7, 768};
   aimee_db2_embedding_refusals_t decoded = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply,
                                                    sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_embedding_refusals_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.refused_count == 7 &&
          decoded.last_offered == 768);
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                                    sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_embedding_refusals_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded.refused_count == 0);

   aimee_db2_embedding_refusals_t bad = {7, 0};
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                    &reply_len) == -1);
   bad = (aimee_db2_embedding_refusals_t){0, 768};
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                    &reply_len) == -1);
}

static void test_postgres_status_wire(void)
{
   uint8_t request[AIMEE_DB2_POSTGRES_STATUS_REQUEST_LEN] = {0};
   assert(aimee_db2_postgres_status_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_postgres_status_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1);
   assert(aimee_db2_postgres_status_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN] = {0};
   const aimee_db2_postgres_status_t expected = {15, 12, 100, 1, 1048576};
   aimee_db2_postgres_status_t decoded = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply,
                                                 sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_postgres_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.available == 15 &&
          decoded.active_connections == 12 && decoded.max_connections == 100 &&
          decoded.is_replica == 1 && decoded.replica_lag_bytes == 1048576);

   const aimee_db2_postgres_status_t partial = {
       AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE | AIMEE_DB2_POSTGRES_AVAILABLE_MAX, 12, 100, 0, 0};
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &partial, reply,
                                                 sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_postgres_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(decoded.available == 3 && decoded.is_replica == 0 && decoded.replica_lag_bytes == 0);

   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                                 sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_postgres_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded.available == 0);

   aimee_db2_postgres_status_t bad = expected;
   bad.available = 16;
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                 &reply_len) == -1);
   bad = expected;
   bad.available &= ~AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE;
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                 &reply_len) == -1);
   bad = expected;
   bad.is_replica = 0;
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                 &reply_len) == -1);
}

static void test_reembed_status_wire(void)
{
   uint8_t request[AIMEE_DB2_REEMBED_STATUS_REQUEST_LEN] = {0};
   assert(aimee_db2_reembed_status_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_reembed_status_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1);
   assert(aimee_db2_reembed_status_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN] = {0};
   const aimee_db2_reembed_status_t expected = {384, 1700000000};
   aimee_db2_reembed_status_t decoded = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply,
                                                sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.target_dimension == 384 &&
          decoded.started_epoch == 1700000000);
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, NULL, reply,
                                                sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND && decoded.target_dimension == 0);
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                                sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);

   aimee_db2_reembed_status_t bad = {0, 1700000000};
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                &reply_len) == -1);
   bad = (aimee_db2_reembed_status_t){384, 0};
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                &reply_len) == -1);
}

static void test_reembed_clear_wire(void)
{
   uint8_t request[AIMEE_DB2_REEMBED_CLEAR_REQUEST_LEN] = {0};
   assert(aimee_db2_reembed_clear_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_reembed_clear_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1);
   assert(aimee_db2_reembed_clear_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_reembed_clear_reply_encode(AIMEE_DB2_RESULT_OK, reply, sizeof(reply),
                                               &reply_len) == 0);
   assert(aimee_db2_reembed_clear_reply_decode(reply, reply_len, &result) == 0);
   assert(result == AIMEE_DB2_RESULT_OK);
   assert(aimee_db2_reembed_clear_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, reply, sizeof(reply),
                                               &reply_len) == 0);
   assert(aimee_db2_reembed_clear_reply_decode(reply, reply_len, &result) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_reembed_clear_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, reply, sizeof(reply),
                                               &reply_len) == -1);
}

static aimee_module_status_t invoke(const aimee_db2_module_backend_t *backend,
                                    aimee_module_invocation_t *invocation, uint8_t *request,
                                    uint32_t request_len, uint8_t *response,
                                    uint32_t response_capacity, uint32_t *response_len)
{
   return aimee_module_handler(invocation, request, request_len, response, response_capacity,
                               response_len, (void *)backend);
}

static void test_handler_success_and_failures(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .health_probe = health_probe,
       .kb_health_probe = kb_health_probe,
   };
   uint8_t request[AIMEE_DB2_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_HEALTH};
   assert(aimee_db2_health_request_encode(request, sizeof(request)) == 0);
   memset(response, 0xa5, sizeof(response));

   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(response_len == AIMEE_DB2_RESPONSE_LEN);
   assert(health_calls == 1 && kb_health_calls == 1);
   int schema_ok = 0, have_pg_trgm = 0, kb_tables_ok = 0;
   assert(aimee_db2_health_response_decode(response, response_len, &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == 0);
   assert(schema_ok && have_pg_trgm && kb_tables_ok);

   response_len = 99;
   invocation.stage_id++;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(response_len == 0);
   invocation.stage_id = AIMEE_DB2_STAGE_HEALTH;
   assert(invoke(&backend, &invocation, request, sizeof(request) - 1, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);

   health_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   health_result = 0;
   kb_health_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   kb_health_result = 0;
   cancelled = 1;
   int prior_health_calls = health_calls;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CANCELLED);
   assert(health_calls == prior_health_calls);

   cancelled = 0;
   cancel_after = 2;
   cancel_checks = 0;
   prior_health_calls = health_calls;
   int prior_kb_health_calls = kb_health_calls;
   response_len = 99;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CANCELLED);
   assert(response_len == 0);
   assert(health_calls == prior_health_calls + 1);
   assert(kb_health_calls == prior_kb_health_calls + 1);
}

static void test_embedding_dimension_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .health_probe = health_probe,
       .kb_health_probe = kb_health_probe,
       .embedding_dimension = embedding_dimension,
   };
   uint8_t request[AIMEE_DB2_EMBEDDING_DIMENSION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99, dimension = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EMBEDDING_DIMENSION};
   assert(aimee_db2_embedding_dimension_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(embedding_dimension_calls == 1);
   assert(aimee_db2_embedding_dimension_reply_decode(response, response_len, &result, &dimension) ==
          0);
   assert(result == AIMEE_DB2_RESULT_OK && dimension == 384);

   embedding_dimension_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedding_dimension_reply_decode(response, response_len, &result, &dimension) ==
          0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && dimension == 0);
   embedding_dimension_value = 4001;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedding_dimension_reply_decode(response, response_len, &result, &dimension) ==
          0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && dimension == 0);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   embedding_dimension_value = 384;
   cancel_after = 2;
   cancel_checks = 0;
   int prior_calls = embedding_dimension_calls;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CANCELLED);
   assert(response_len == 0 && embedding_dimension_calls == prior_calls + 1);
}

static void test_pool_status_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .pool_status = pool_status,
   };
   uint8_t request[AIMEE_DB2_POOL_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_POOL_STATUS_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_pool_status_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_POOL_STATUS};
   assert(aimee_db2_pool_status_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_pool_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.size == 16 && status.in_use == 2);

   pool_status_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_pool_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && status.size == 0);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_embedding_refusals_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.embedding_refusals = embedding_refusals};
   uint8_t request[AIMEE_DB2_EMBEDDING_REFUSALS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_embedding_refusals_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EMBEDDING_REFUSALS};
   assert(aimee_db2_embedding_refusals_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedding_refusals_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.refused_count == 7 && status.last_offered == 768);
   embedding_refusals_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedding_refusals_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
}

static void test_postgres_status_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.postgres_status = postgres_status};
   uint8_t request[AIMEE_DB2_POSTGRES_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_postgres_status_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_POSTGRES_STATUS};
   assert(aimee_db2_postgres_status_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_postgres_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.available == 15 &&
          status.replica_lag_bytes == 1048576);
   postgres_status_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_postgres_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
}

static void test_reembed_status_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.reembed_status = reembed_status};
   uint8_t request[AIMEE_DB2_REEMBED_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_reembed_status_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_REEMBED_STATUS};
   assert(aimee_db2_reembed_status_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.target_dimension == 384);
   reembed_status_result = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND);
   reembed_status_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
}

static void test_reembed_clear_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.reembed_clear = db2_reembed_in_progress_clear};
   uint8_t request[AIMEE_DB2_REEMBED_CLEAR_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_REEMBED_CLEAR};
   assert(aimee_db2_reembed_clear_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_clear_reply_decode(response, response_len, &result) == 0);
   assert(result == AIMEE_DB2_RESULT_OK);
   reembed_clear_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_clear_reply_decode(response, response_len, &result) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
}

static void test_typed_client(void)
{
   reset();
   int schema_ok = 9, have_pg_trgm = 9, kb_tables_ok = 9;
   assert(aimee_db2_health_call(NULL, NULL, 77, 88, &schema_ok, &have_pg_trgm, &kb_tables_ok, NULL,
                                NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);

   assert(aimee_db2_health_call(transport, (void *)0x1234, 77, 88, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(schema_ok && !have_pg_trgm && kb_tables_ok);
   assert(transport_calls == 1);

   transport_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   schema_ok = have_pg_trgm = kb_tables_ok = 9;
   assert(aimee_db2_health_call(transport, (void *)0x1234, 77, 88, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);

   transport_result = AIMEE_MODULE_CALL_OK;
   transport_response[0] ^= 1u;
   assert(aimee_db2_health_call(transport, (void *)0x1234, 77, 88, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_PROTOCOL);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);
   transport_response[0] ^= 1u;
   transport_response_len--;
   assert(aimee_db2_health_call(transport, (void *)0x1234, 77, 88, NULL, NULL, NULL, NULL, NULL) ==
          AIMEE_MODULE_CALL_PROTOCOL);
}

static void test_embedding_dimension_typed_client(void)
{
   reset();
   transport_expect_dimension = 1;
   uint32_t domain_result = 9, dimension = 9;
   assert(aimee_db2_embedding_dimension_call(NULL, NULL, 77, 88, &domain_result, &dimension, NULL,
                                             NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(domain_result == 0 && dimension == 0);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, transport_response,
                                                     sizeof(transport_response),
                                                     &transport_response_len) == 0);
   assert(aimee_db2_embedding_dimension_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && dimension == 384 && transport_calls == 1);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 0,
                                                     transport_response, sizeof(transport_response),
                                                     &transport_response_len) == 0);
   assert(aimee_db2_embedding_dimension_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_INVALID_STATE && dimension == 0);

   transport_result = AIMEE_MODULE_CALL_CANCELLED;
   domain_result = dimension = 9;
   assert(aimee_db2_embedding_dimension_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             &dimension, NULL,
                                             NULL) == AIMEE_MODULE_CALL_CANCELLED);
   assert(domain_result == 0 && dimension == 0);
   transport_result = AIMEE_MODULE_CALL_OK;
   transport_response[0] ^= 1u;
   assert(aimee_db2_embedding_dimension_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_PROTOCOL);
   assert(domain_result == 0 && dimension == 0);
}

static void test_pool_status_typed_client(void)
{
   reset();
   transport_expect_pool = 1;
   const aimee_db2_pool_status_t expected = {16, 2, 1, 10, 3, 4, 5};
   uint32_t domain_result = 9;
   aimee_db2_pool_status_t status = {.size = 9};
   assert(aimee_db2_pool_status_call(NULL, NULL, 77, 88, &domain_result, &status, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(domain_result == 0 && status.size == 0);

   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, transport_response,
                                             sizeof(transport_response),
                                             &transport_response_len) == 0);
   assert(aimee_db2_pool_status_call(transport, (void *)0x1234, 77, 88, &domain_result, &status,
                                     NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && status.size == 16 && status.poisoned == 5);

   transport_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   domain_result = 9;
   status.size = 9;
   assert(aimee_db2_pool_status_call(transport, (void *)0x1234, 77, 88, &domain_result, &status,
                                     NULL, NULL) == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(domain_result == 0 && status.size == 0);
}

static void test_embedding_refusals_typed_client(void)
{
   reset();
   transport_expect_refusals = 1;
   const aimee_db2_embedding_refusals_t expected = {7, 768};
   uint32_t domain_result = 9;
   aimee_db2_embedding_refusals_t status = {0};
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_OK, &expected,
                                                    transport_response, sizeof(transport_response),
                                                    &transport_response_len) == 0);
   assert(aimee_db2_embedding_refusals_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                            &status, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && status.refused_count == 7 &&
          status.last_offered == 768);
}

static void test_postgres_status_typed_client(void)
{
   reset();
   transport_expect_postgres = 1;
   const aimee_db2_postgres_status_t expected = {15, 12, 100, 1, 1048576};
   uint32_t domain_result = 9;
   aimee_db2_postgres_status_t status = {0};
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, transport_response,
                                                 sizeof(transport_response),
                                                 &transport_response_len) == 0);
   assert(aimee_db2_postgres_status_call(transport, (void *)0x1234, 77, 88, &domain_result, &status,
                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && status.available == 15 &&
          status.active_connections == 12 && status.replica_lag_bytes == 1048576);
}

static void test_reembed_status_typed_client(void)
{
   reset();
   transport_expect_reembed = 1;
   const aimee_db2_reembed_status_t expected = {384, 1700000000};
   uint32_t domain_result = 9;
   aimee_db2_reembed_status_t status = {0};
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, transport_response,
                                                sizeof(transport_response),
                                                &transport_response_len) == 0);
   assert(aimee_db2_reembed_status_call(transport, (void *)0x1234, 77, 88, &domain_result, &status,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && status.target_dimension == 384 &&
          status.started_epoch == 1700000000);
}

static void test_reembed_clear_typed_client(void)
{
   reset();
   transport_expect_reembed_clear = 1;
   uint32_t domain_result = 9;
   assert(aimee_db2_reembed_clear_reply_encode(AIMEE_DB2_RESULT_OK, transport_response,
                                               sizeof(transport_response),
                                               &transport_response_len) == 0);
   assert(aimee_db2_reembed_clear_call(transport, (void *)0x1234, 77, 88, &domain_result, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK);
}

int main(void)
{
   test_wire_contract();
   test_body_envelope();
   test_embedding_dimension_wire();
   test_pool_status_wire();
   test_embedding_refusals_wire();
   test_postgres_status_wire();
   test_reembed_status_wire();
   test_reembed_clear_wire();
   test_handler_success_and_failures();
   test_embedding_dimension_handler();
   test_pool_status_handler();
   test_embedding_refusals_handler();
   test_postgres_status_handler();
   test_reembed_status_handler();
   test_reembed_clear_handler();
   test_typed_client();
   test_embedding_dimension_typed_client();
   test_pool_status_typed_client();
   test_embedding_refusals_typed_client();
   test_postgres_status_typed_client();
   test_reembed_status_typed_client();
   test_reembed_clear_typed_client();
   puts("test_db2_module_contract: ok");
   return 0;
}
