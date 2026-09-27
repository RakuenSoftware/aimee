/* Wire tests for the credential-free native PostgreSQL client. */
#include <aimee/postgres/client.h>
#include <aimee/audit/obs_bus.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned char response[4096], request[4096];
static size_t used, request_length;
static unsigned calls;
static aimee_module_call_result_t transport = AIMEE_MODULE_CALL_OK;
static void put32(uint32_t n)
{
   for (unsigned i = 0; i < 4; i++)
      response[used++] = (unsigned char)(n >> (8 * i));
}
static void put64(uint64_t n)
{
   for (unsigned i = 0; i < 8; i++)
      response[used++] = (unsigned char)(n >> (8 * i));
}
static void cell(const void *p, size_t n)
{
   put32((uint32_t)n);
   memcpy(response + used, p, n);
   used += n;
}
static void header(uint32_t status, const char *state, const char *message)
{
   used = 0;
   put32(status);
   cell(state, strlen(state));
   cell(message, strlen(message));
}
uint64_t aimee_module_call_deadline_ns(int ms)
{
   assert(ms == 60000);
   return 123;
}
aimee_module_call_result_t obs_bus_module_call(uint32_t event, uint32_t stage, uint64_t trace,
                                               uint64_t deadline, const void *body, uint32_t size,
                                               void *reply, uint32_t capacity, uint32_t *length,
                                               aimee_module_cancelled_fn cancelled, void *context)
{
   assert(event == 11267 && stage == 3 && trace == 0 && deadline > 0);
   assert(!cancelled && !context && size <= sizeof(request) && used <= capacity);
   calls++;
   memcpy(request, body, size);
   request_length = size;
   memcpy(reply, response, used);
   *length = (uint32_t)used;
   return transport;
}
static void query_reply(void)
{
   header(0, "", "");
   response[used++] = 'T';
   response[used++] = 0;
   put64(1);
   put32(4);
   put32(1);
   cell("large", 5);
   put32(20);
   cell("nothing", 7);
   put32(25);
   cell("empty", 5);
   put32(25);
   cell("bytes", 5);
   put32(17);
   cell("9007199254740993", 16);
   put32(UINT32_MAX);
   cell("", 0);
   cell("\\x0001ff", 8);
}
int main(void)
{
   char error[256], state[6];
   header(0, "", "");
   put64(UINT64_C(0x0102030405060708));
   aimee_postgres_session_t *s = aimee_postgres_session_open(error, sizeof(error));
   assert(s && request_length == 12);
   static const unsigned char acquire[] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   assert(!memcmp(request, acquire, sizeof(acquire)));
   query_reply();
   aimee_postgres_result_t *r =
       aimee_postgres_session_query(s, "SELECT 1", NULL, 0, state, error, sizeof(error));
   assert(r && aimee_postgres_result_rows(r) == 1 && aimee_postgres_result_columns(r) == 4);
   assert(aimee_postgres_result_changes(r) == 1 &&
          !strcmp(aimee_postgres_result_name(r, 0), "large"));
   assert(aimee_postgres_result_oid(r, 3) == 17);
   size_t n = 999;
   assert(!strcmp(aimee_postgres_result_cell(r, 0, 0, &n), "9007199254740993") && n == 16);
   assert(!aimee_postgres_result_cell(r, 0, 1, &n) && n == 0);
   assert(aimee_postgres_result_cell(r, 0, 2, &n) && n == 0);
   assert(!strcmp(aimee_postgres_result_cell(r, 0, 3, &n), "\\x0001ff") && n == 8);
   assert(!aimee_postgres_result_cell(r, 1, 0, &n));
   aimee_postgres_result_free(r);
   header(2, "23505", "duplicate");
   assert(aimee_postgres_session_exec(s, "INSERT", NULL, 0, NULL, state, error, sizeof(error)) ==
          -1);
   assert(!strcmp(state, "23505") && !strcmp(error, "duplicate"));
   header(0, "", "");
   response[used++] = 'E';
   char tx = 0;
   assert(aimee_postgres_session_state(s, &tx) == 0 && tx == 'E');
   unsigned before = calls;
   assert(aimee_postgres_session_exec(s, "SELECT 1", NULL, 1, NULL, state, error, sizeof(error)) ==
              -1 &&
          calls == before);
   /* Typed input keeps int64 and binary values intact, including embedded NUL. */
   const unsigned char blob[] = {0, 1, 255};
   aimee_postgres_value_t args[] = {{.kind = AIMEE_POSTGRES_INT, .integer = INT64_MIN},
                                    {.kind = AIMEE_POSTGRES_BYTES, .data = blob, .length = 3},
                                    {.kind = AIMEE_POSTGRES_NULL}};
   header(0, "", "");
   response[used++] = 'I';
   put64(7);
   uint64_t changed = 0;
   assert(aimee_postgres_session_exec(s, "x", args, 3, &changed, state, error, sizeof(error)) ==
              0 &&
          changed == 7);
   static const unsigned char expected[] = {3, 0, 0, 0,   8, 7, 6, 5, 4, 3, 2, 1,   1,
                                            0, 0, 0, 'x', 3, 0, 0, 0, 2, 0, 0, 0,   0,
                                            0, 0, 0, 128, 6, 3, 0, 0, 0, 0, 1, 255, 0};
   assert(request_length == sizeof(expected) && !memcmp(request, expected, sizeof(expected)));
   header(0, "", "");
   assert(aimee_postgres_session_close(s) == 0 && request[0] == 2);
   header(0, "", "");
   put64(42);
   s = aimee_postgres_session_open(error, sizeof(error));
   assert(s);
   /* Every truncated reply fails, and malformed metadata cannot hide a suffix. */
   query_reply();
   size_t full = used;
   for (size_t i = 0; i < full; i++)
   {
      used = i;
      assert(!aimee_postgres_session_query(s, "SELECT 1", NULL, 0, state, error, sizeof(error)));
   }
   query_reply();
   response[34] = 0; /* embedded NUL in first column name */
   assert(!aimee_postgres_session_query(s, "SELECT 1", NULL, 0, state, error, sizeof(error)));
   query_reply();
   response[used++] = 0;
   assert(!aimee_postgres_session_query(s, "SELECT 1", NULL, 0, state, error, sizeof(error)));
   query_reply();
   response[22] = 0xff;
   response[23] = 0xff; /* impossible column count */
   assert(!aimee_postgres_session_query(s, "SELECT 1", NULL, 0, state, error, sizeof(error)));
   /* Release can itself be refused after poisoning; close still frees it. */
   header(0, "", "");
   (void)aimee_postgres_session_close(s);
   puts("PostgreSQL session transport: pass");
   return 0;
}
