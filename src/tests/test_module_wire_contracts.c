/* test_module_wire_contracts.c: the C core's side of module wire contracts.
 *
 * Every module process is Go; the C core is only ever their caller. These are
 * the caller-side codecs it encodes requests and decodes replies with, held to
 * the frame layout the module processes answer in. The handlers' own tests live
 * with the Go modules.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <aimee/postgres/module_api.h>
#include <aimee/workspace/module_api.h>

static void test_postgres_health_contract(void)
{
   uint8_t response[AIMEE_POSTGRES_RESPONSE_LEN] = {0};
   int schema_ok = 0, have_pg_trgm = 0, kb_tables_ok = 0;
   aimee_postgres_put_u32(response, AIMEE_POSTGRES_RESPONSE_MAGIC);
   aimee_postgres_put_u32(response + 4, AIMEE_POSTGRES_WIRE_VERSION);
   aimee_postgres_put_u32(response + 8, AIMEE_POSTGRES_FLAG_SCHEMA | AIMEE_POSTGRES_FLAG_PG_TRGM |
                                            AIMEE_POSTGRES_FLAG_KB_TABLES);
   assert(aimee_postgres_health_response_decode(response, sizeof(response), &schema_ok,
                                                &have_pg_trgm, &kb_tables_ok) == 0);
   assert(schema_ok && have_pg_trgm && kb_tables_ok);

   /* Unknown evidence bits and non-zero reserved bytes are wire errors. */
   aimee_postgres_put_u32(response + 8, 0x8u);
   assert(aimee_postgres_health_response_decode(response, sizeof(response), &schema_ok,
                                                &have_pg_trgm, &kb_tables_ok) == -1);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);
   aimee_postgres_put_u32(response + 8, AIMEE_POSTGRES_FLAG_KB_TABLES);
   aimee_postgres_put_u32(response + 12, 1u);
   assert(aimee_postgres_health_response_decode(response, sizeof(response), &schema_ok,
                                                &have_pg_trgm, &kb_tables_ok) == -1);
}

static void test_workspace_runner_frames(void)
{
   uint8_t request[AIMEE_WS_RUNNER_REQUEST_LEN];
   assert(aimee_ws_runner_request_encode(AIMEE_WS_RUNNER_OP_RESOLVE, "/srv/repo/src", 13, request,
                                         sizeof(request)) == 0);
   assert(aimee_workspace_get_u32(request) == AIMEE_WS_RUNNER_REQUEST_MAGIC);
   assert(request[4] == AIMEE_WORKSPACE_WIRE_VERSION);
   assert(request[5] == AIMEE_WS_RUNNER_OP_RESOLVE);
   assert(aimee_workspace_get_u16(request + 6) == 13);
   assert(memcmp(request + 8, "/srv/repo/src", 13) == 0);

   /* A registered id is the key the handoff is looked up by, so it is bounded to
    * that key even though a path being asked about is not. */
   char long_id[AIMEE_WS_RUNNER_ID_MAX + 2];
   memset(long_id, 'a', sizeof(long_id) - 1);
   long_id[sizeof(long_id) - 1] = '\0';
   assert(aimee_ws_runner_request_encode(AIMEE_WS_RUNNER_OP_REGISTER, long_id,
                                         AIMEE_WS_RUNNER_ID_MAX + 1, request, sizeof(request)) < 0);
   assert(aimee_ws_runner_request_encode(AIMEE_WS_RUNNER_OP_RESOLVE, long_id,
                                         AIMEE_WS_RUNNER_ID_MAX + 1, request,
                                         sizeof(request)) == 0);

   /* Nobody serving the tree decodes as an empty id, not as a failure. */
   uint8_t reply[AIMEE_WS_RUNNER_RESPONSE_LEN];
   memset(reply, 0, sizeof(reply));
   aimee_workspace_put_u32(reply, AIMEE_WS_RUNNER_RESPONSE_MAGIC);
   char id[AIMEE_WS_RUNNER_ID_MAX + 1] = "stale";
   assert(aimee_ws_runner_response_decode(reply, sizeof(reply), id, sizeof(id)) == 0);
   assert(id[0] == '\0');

   aimee_workspace_put_u32(reply + 4, 9);
   memcpy(reply + 8, "/srv/repo", 9);
   assert(aimee_ws_runner_response_decode(reply, sizeof(reply), id, sizeof(id)) == 0);
   assert(strcmp(id, "/srv/repo") == 0);

   /* An id longer than the frame allows is refused rather than truncated. */
   aimee_workspace_put_u32(reply + 4, AIMEE_WS_RUNNER_ID_MAX + 1);
   assert(aimee_ws_runner_response_decode(reply, sizeof(reply), id, sizeof(id)) < 0);
}

static void test_workspace_runner_io_frames(void)
{
   uint8_t request[AIMEE_WS_IO_HEADER_LEN + AIMEE_WS_RUNNER_ID_MAX + 32];
   const char *body = "{\"op\":\"read\"}";
   size_t n = aimee_ws_io_request_encode(AIMEE_WS_IO_OP_SUBMIT, "/srv/repo", body, strlen(body),
                                         request, sizeof(request));
   assert(n == AIMEE_WS_IO_HEADER_LEN + 9 + strlen(body));
   assert(aimee_workspace_get_u32(request) == AIMEE_WS_IO_REQUEST_MAGIC);
   assert(request[5] == AIMEE_WS_IO_OP_SUBMIT);
   assert(aimee_workspace_get_u16(request + 6) == 9);
   assert(aimee_workspace_get_u32(request + 8) == strlen(body));
   assert(memcmp(request + AIMEE_WS_IO_HEADER_LEN, "/srv/repo", 9) == 0);
   assert(memcmp(request + AIMEE_WS_IO_HEADER_LEN + 9, body, strlen(body)) == 0);

   /* Too small a buffer reports 0 rather than writing past it. */
   assert(aimee_ws_io_request_encode(AIMEE_WS_IO_OP_SUBMIT, "/srv/repo", body, strlen(body),
                                     request, AIMEE_WS_IO_HEADER_LEN) == 0);

   /* "another chunk follows" is what tells a streaming caller to drain again;
    * reading it off the wrong word would end a stream early. */
   uint8_t reply[AIMEE_WS_IO_RESP_HEADER_LEN + 4];
   aimee_workspace_put_u32(reply, AIMEE_WS_IO_RESPONSE_MAGIC);
   aimee_workspace_put_u32(reply + 4, AIMEE_WS_IO_MORE);
   aimee_workspace_put_u32(reply + 8, 4);
   memcpy(reply + AIMEE_WS_IO_RESP_HEADER_LEN, "tok1", 4);
   const uint8_t *chunk = NULL;
   size_t chunk_len = 0;
   int more = 0;
   assert(aimee_ws_io_response_decode(reply, sizeof(reply), &chunk, &chunk_len, &more) == 0);
   assert(more && chunk_len == 4 && memcmp(chunk, "tok1", 4) == 0);

   aimee_workspace_put_u32(reply + 4, 0);
   assert(aimee_ws_io_response_decode(reply, sizeof(reply), &chunk, &chunk_len, &more) == 0);
   assert(!more);

   /* A length that disagrees with the frame is refused, not trusted. */
   aimee_workspace_put_u32(reply + 8, 64);
   assert(aimee_ws_io_response_decode(reply, sizeof(reply), &chunk, &chunk_len, &more) < 0);
}

int main(void)
{
   test_postgres_health_contract();
   test_workspace_runner_frames();
   test_workspace_runner_io_frames();
   puts("module wire contracts: PASS");
   return 0;
}
