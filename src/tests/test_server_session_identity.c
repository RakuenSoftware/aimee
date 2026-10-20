/* Session creation uses ingress-attested accounts across shared process UIDs. */
#include "aimee.h"
#include "server.h"
#include "db1_client/server_sessions.h"
#include "json_fluent.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char recorded[256];
static int writes, transcript_mode, expected_error, archive_writes, fail_archive;
static const char *archive_owner = "webuser:alice";
static char checkpoint_saved[65];
cJSON *jo_ok(void)
{
   cJSON *value = cJSON_CreateObject();
   cJSON_AddStringToObject(value, "status", "ok");
   return value;
}
int db1_server_session_create(const char *id, const char *client, const char *principal)
{
   assert(!transcript_mode && strlen(id) == 36 && !strcmp(client, "fixture"));
   snprintf(recorded, sizeof recorded, "%s", principal);
   writes++;
   return 0;
}
int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "session_id"));
   assert(id && (transcript_mode ? !strcmp(id, "transcript") : strlen(id) == 36));
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *request)
{
   (void)conn;
   (void)message;
   (void)request;
   assert(expected_error && "unexpected session failure");
   return -1;
}

int db1_server_session_get(const char *id, db1_server_session_t *row)
{
   assert(transcript_mode && !strcmp(id, "transcript"));
   memset(row, 0, sizeof *row);
   snprintf(row->principal, sizeof row->principal, "%s", archive_owner);
   return 0;
}
int db1_session_ir_history_save(const char *id, const char *checkpoint, const char *scope,
                                const char *actor, const char *messages)
{
   assert(transcript_mode && !strcmp(id, "transcript") && strlen(checkpoint) == 64);
   assert(!strcmp(scope, "external-sdk:claude-code") && !strcmp(actor, "webuser:alice"));
   cJSON *value = cJSON_Parse(messages);
   assert(value && cJSON_GetArraySize(value) == 1);
   cJSON *block = cJSON_GetArrayItem(
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(value, 0), "blocks"), 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "text")),
                  "external note"));
   assert(cJSON_GetObjectItemCaseSensitive(block, "context"));
   cJSON_Delete(value);
   snprintf(checkpoint_saved, sizeof checkpoint_saved, "%s", checkpoint);
   archive_writes++;
   return fail_archive ? -1 : 0;
}
static void test_external_archive(void)
{
   transcript_mode = 1;
   server_conn_t conn = {.attested_transport = ATTEST_WEBCHAT_TRUSTED};
   snprintf(conn.vault_principal, sizeof conn.vault_principal, "webuser:alice");
   cJSON *request =
       cJSON_Parse("{\"session_id\":\"transcript\",\"scope\":\"primary\",\"messages\":[{\"type\":"
                   "\"user\",\"sessionId\":\"transcript\",\"uuid\":\"first\",\"message\":{\"role\":"
                   "\"user\",\"content\":\"external note\"}}]}");
   assert(request);
   assert(!handle_session_record_transcript(NULL, &conn, request));
   assert(archive_writes == 1);
   char first[65];
   snprintf(first, sizeof first, "%s", checkpoint_saved);
   assert(!handle_session_record_transcript(NULL, &conn, request));
   assert(!strcmp(first, checkpoint_saved));
   cJSON *record = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(request, "messages"), 0);
   cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(record, "uuid"), "different-sdk-source");
   assert(!handle_session_record_transcript(NULL, &conn, request));
   assert(strcmp(first, checkpoint_saved)); /* Same text never conflates SDK source identity. */
   expected_error = 1;
   int before = archive_writes;
   archive_owner = "webuser:bob";
   assert(handle_session_record_transcript(NULL, &conn, request) == -1 && archive_writes == before);
   archive_owner = "";
   assert(handle_session_record_transcript(NULL, &conn, request) == -1 && archive_writes == before);
   archive_owner = "webuser:alice";
   cJSON_AddStringToObject(request, "provider", "openai");
   assert(handle_session_record_transcript(NULL, &conn, request) == -1 && archive_writes == before);
   cJSON_DeleteItemFromObjectCaseSensitive(request, "provider");
   fail_archive = 1;
   assert(handle_session_record_transcript(NULL, &conn, request) == -1 &&
          archive_writes == before + 1);
   cJSON_Delete(request);
   puts("external archive: authenticated scope, source identity, foreign/ownerless refusal and "
        "durable failure passed");
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
   test_external_archive();
   return 0;
}
