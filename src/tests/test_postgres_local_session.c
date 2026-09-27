/* Compiled by the real-PostgreSQL Go test with its private provider executable. */
#include <aimee/postgres/client.h>
#include "db_postgres.h"
#include <aimee/audit/obs_bus.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t aimee_module_call_deadline_ns(int ms)
{
   (void)ms;
   return 0;
}
aimee_module_call_result_t obs_bus_module_call(uint32_t event, uint32_t stage, uint64_t trace,
                                               uint64_t deadline, const void *body, uint32_t size,
                                               void *reply, uint32_t capacity, uint32_t *length,
                                               aimee_module_cancelled_fn cancelled, void *context)
{
   (void)event;
   (void)stage;
   (void)trace;
   (void)deadline;
   (void)body;
   (void)size;
   (void)reply;
   (void)capacity;
   (void)length;
   (void)cancelled;
   (void)context;
   assert(!"private authority must never use runtime bus");
   return AIMEE_MODULE_CALL_INTERNAL;
}
static int64_t now_ms(void)
{
   struct timespec now;
   assert(!clock_gettime(CLOCK_MONOTONIC, &now));
   return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
int main(void)
{
   char error[256], state[6], tx = 0;
   const char *dsn = getenv("AIMEE_DB_TEST_URL");
   assert(dsn && *dsn);
   /* Fixed runtime authority must neither inherit nor fall back to migration
    * credentials. Equal roles deliberately make the migration profile invalid. */
   assert(setenv("AIMEE_STORE_URL", dsn, 1) == 0);
   assert(setenv("AIMEE_STORE_MIGRATION_URL", dsn, 1) == 0);
   aimee_postgres_session_t *configured =
       aimee_postgres_session_open_configured(error, sizeof(error));
   assert(configured);
   assert(aimee_postgres_session_exec(configured, "SELECT 1", NULL, 0, NULL, state, error,
                                      sizeof(error)) == 0);
   assert(aimee_postgres_session_close(configured) == 0);
   assert(!aimee_postgres_session_open_migration(error, sizeof(error)));
   aimee_postgres_session_t *s = aimee_postgres_session_open_local(dsn, error, sizeof(error));
   assert(s);
   /* A native schema batch is larger than one result cell. Keep the full
    * statement inside the bounded transport without truncating it to 1 MiB. */
   size_t schema_length = 2u * 1024u * 1024u;
   char *schema_batch = malloc(schema_length + 1);
   assert(schema_batch);
   memset(schema_batch, ' ', schema_length);
   memcpy(schema_batch, "SELECT 1;", 9);
   schema_batch[schema_length] = 0;
   assert(aimee_postgres_session_exec(s, schema_batch, NULL, 0, NULL, state, error,
                                      sizeof(error)) == 0);
   free(schema_batch);
   assert(aimee_postgres_session_exec(s, "BEGIN", NULL, 0, NULL, state, error, sizeof(error)) == 0);
   assert(aimee_postgres_session_state(s, &tx) == 0 && tx == 'T');
   const unsigned char blob[] = {0, 1, 255};
   aimee_postgres_value_t args[] = {
       {.kind = AIMEE_POSTGRES_INT, .integer = INT64_C(9007199254740993)},
       {.kind = AIMEE_POSTGRES_BYTES, .data = blob, .length = 3}};
   aimee_postgres_result_t *r =
       aimee_postgres_session_query(s, "SELECT $1::bigint,$2::bytea,NULL::text,''::text,true", args,
                                    2, state, error, sizeof(error));
   assert(r && aimee_postgres_result_rows(r) == 1 && aimee_postgres_result_columns(r) == 5);
   size_t n;
   assert(!strcmp(aimee_postgres_result_cell(r, 0, 0, &n), "9007199254740993"));
   assert(!strcmp(aimee_postgres_result_cell(r, 0, 1, &n), "\\x0001ff"));
   assert(!aimee_postgres_result_cell(r, 0, 2, &n));
   assert(aimee_postgres_result_cell(r, 0, 3, &n) && n == 0);
   assert(!strcmp(aimee_postgres_result_cell(r, 0, 4, &n), "t"));
   const unsigned char *binary = aimee_postgres_result_binary(r, 0, 0, &n);
   static const unsigned char large_binary[] = {0, 32, 0, 0, 0, 0, 0, 1};
   assert(binary && n == 8 && !memcmp(binary, large_binary, 8));
   binary = aimee_postgres_result_binary(r, 0, 1, &n);
   assert(binary && n == 3 && !memcmp(binary, blob, 3));
   binary = aimee_postgres_result_binary(r, 0, 4, &n);
   assert(binary && n == 1 && *binary == 1);
   aimee_postgres_result_free(r);
   assert(aimee_postgres_session_exec(s, "SELECT 1/0", NULL, 0, NULL, state, error,
                                      sizeof(error)) == -1);
   assert(!strcmp(state, "22012"));
   assert(aimee_postgres_session_state(s, &tx) == 0 && tx == 'E');
   assert(aimee_postgres_session_reconnect(s, error, sizeof(error)) == 0);
   assert(aimee_postgres_session_state(s, &tx) == 0 && tx == 'I');
   assert(aimee_postgres_session_exec(s, "LISTEN aimee_postgres_session_test", NULL, 0, NULL, state,
                                      error, sizeof(error)) == 0);
   assert(aimee_postgres_session_wait(s, 20) == 0);
   aimee_postgres_session_t *other = aimee_postgres_session_open_local(dsn, error, sizeof(error));
   assert(other);
   assert(aimee_postgres_session_exec(other, "NOTIFY aimee_postgres_session_test", NULL, 0, NULL,
                                      state, error, sizeof(error)) == 0);
   assert(aimee_postgres_session_wait(s, 1000) == 1);
   assert(aimee_postgres_session_close(other) == 0);
   int64_t start = now_ms();
   assert(!aimee_postgres_session_deadline(s, start + 200));
   assert(aimee_postgres_session_exec(s, "SELECT pg_sleep(5)", NULL, 0, NULL, state, error,
                                      sizeof(error)) == -1);
   assert(now_ms() - start < 1500);
   assert(!aimee_postgres_session_deadline(s, 0));
   assert(!aimee_postgres_session_reconnect(s, error, sizeof(error)));
   assert(aimee_postgres_session_exec(s, "SELECT 1", NULL, 0, NULL, state, error, sizeof(error)) ==
          0);
   assert(aimee_postgres_session_close(s) == 0);
   s = aimee_postgres_session_open_local(
       "host=/nonexistent-aimee-test password=private-test-sentinel connect_timeout=1", error,
       sizeof(error));
   assert(!s && !strstr(error, "private-test-sentinel"));
   /* Exercise the actual knowledge statement adapter, including its legacy
    * named parameters and bytea decoding, against the new provider. */
   void *connection = aimee_pg_open(dsn, error, sizeof(error));
   assert(connection);
   aimee_pg_stmt_t *statement = aimee_pg_prepare(
       connection,
       "SELECT :large::bigint, :data::bytea, :empty::text, :nothing::text, true, :large::bigint",
       error, sizeof(error));
   assert(statement);
   assert(!aimee_pg_bind_int64(statement, "large", INT64_C(9007199254740993)));
   assert(!aimee_pg_bind_blob(statement, "data", blob, 3));
   assert(!aimee_pg_bind_text(statement, "empty", ""));
   assert(!aimee_pg_bind_null(statement, "nothing"));
   assert(aimee_pg_step(statement, error, sizeof(error)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int64(statement, 0) == INT64_C(9007199254740993));
   assert(aimee_pg_column_int64(statement, 5) == INT64_C(9007199254740993));
   assert(aimee_pg_column_type(statement, 1) == AIMEE_PG_VALUE_BLOB);
   assert(aimee_pg_column_bytes(statement, 1) == 3);
   assert(!memcmp(aimee_pg_column_blob(statement, 1), blob, 3));
   assert(!strcmp(aimee_pg_column_text(statement, 2), ""));
   assert(aimee_pg_column_is_null(statement, 3));
   assert(aimee_pg_column_int(statement, 4) == 1);
   assert(aimee_pg_step(statement, error, sizeof(error)) == AIMEE_PG_DONE);
   assert(!aimee_pg_reset(statement));
   assert(!aimee_pg_bind_int64(statement, "large", INT64_MIN));
   assert(!aimee_pg_bind_blob(statement, "data", blob, 0));
   assert(aimee_pg_step(statement, error, sizeof(error)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int64(statement, 0) == INT64_MIN);
   assert(aimee_pg_column_blob(statement, 1) && !aimee_pg_column_bytes(statement, 1));
   aimee_pg_finalize(statement);
   statement =
       aimee_pg_prepare(connection, "SELECT generate_series(1,10001)", error, sizeof(error));
   assert(statement);
   for (int i = 1; i <= 10001; i++)
   {
      assert(aimee_pg_step(statement, error, sizeof(error)) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int(statement, 0) == i);
   }
   assert(aimee_pg_step(statement, error, sizeof(error)) == AIMEE_PG_DONE);
   aimee_pg_finalize(statement);
   statement = aimee_pg_prepare(connection, "SELECT repeat('x',900000) FROM generate_series(1,25)",
                                error, sizeof(error));
   assert(statement);
   for (int i = 0; i < 25; i++)
   {
      assert(aimee_pg_step(statement, error, sizeof(error)) == AIMEE_PG_ROW);
      const char *text = aimee_pg_column_text(statement, 0);
      assert(text && strlen(text) == 900000 && text[899999] == 'x');
   }
   assert(aimee_pg_step(statement, error, sizeof(error)) == AIMEE_PG_DONE);
   aimee_pg_finalize(statement);
   aimee_pg_close(connection);
   return 0;
}
