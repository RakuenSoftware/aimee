/* Session creation uses ingress-attested accounts across shared process UIDs. */
#include "aimee.h"
#include "server.h"
#include "db1_client/server_sessions.h"
#include "json_fluent.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char recorded[256];
static int writes;
void now_utc(char *out, size_t cap)
{
   snprintf(out, cap, "2026-10-20T00:00:00Z");
}
cJSON *jo_ok(void)
{
   cJSON *value = cJSON_CreateObject();
   cJSON_AddStringToObject(value, "status", "ok");
   return value;
}
int db1_server_session_create(const char *id, const char *client, const char *principal)
{
   assert(strlen(id) == 36 && !strcmp(client, "fixture"));
   snprintf(recorded, sizeof recorded, "%s", principal);
   writes++;
   return 0;
}
int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "session_id"));
   assert(id && strlen(id) == 36);
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *request)
{
   (void)conn;
   (void)message;
   (void)request;
   assert(0 && "unexpected session creation failure");
   return -1;
}
int main(void)
{
   const struct
   {
      int transport, uid;
      const char *attested, *expected;
   } cases[] = {{ATTEST_NONE, 42, "webuser:forged", "uid:42"},
                {ATTEST_UDS_PEERCRED, 42, "uid:42", "uid:42"},
                {ATTEST_WEBCHAT_TRUSTED, 0, "webuser:alice", "webuser:alice"},
                {ATTEST_WEBCHAT_TRUSTED, 0, "webuser:bob", "webuser:bob"},
                {ATTEST_WEBCHAT_TRUSTED, 0,
                 "webuser:long-browser-account-012345678901234567890123456789",
                 "webuser:long-browser-account-012345678901234567890123456789"},
                {ATTEST_MTLS_CLIENT, -1, "cert:client-a", "cert:client-a"},
                {ATTEST_MTLS_CLIENT, -1, "cert:client-b", "cert:client-b"}};
   cJSON *request = cJSON_Parse("{\"client_type\":\"fixture\",\"principal\":\"webuser:spoof\"}");
   assert(request);
   for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
   {
      server_conn_t conn = {0};
      conn.peer_uid = cases[i].uid;
      conn.attested_transport = cases[i].transport;
      snprintf(conn.vault_principal, sizeof conn.vault_principal, "%s", cases[i].attested);
      int rc = handle_session_create(NULL, &conn, request);
      assert(!rc && !strcmp(recorded, cases[i].expected));
   }
   assert(writes == (int)(sizeof cases / sizeof cases[0]));
   cJSON_Delete(request);
   puts("session ownership: attested browser/certificate/UID identities and spoof refusal passed");
   return 0;
}
