/* Exercise the shipping coordinator across its two stores and owner receipt. */
#include "server.h"
#include "cJSON.h"
#include "kb_client.h"
#include "db1_client/server_sessions.h"
#include "platform_random.h"
#include "modules/kb_client/kb_client_cache.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

extern int handle_kb_erase_subject(server_ctx_t *, server_conn_t *, cJSON *);
static int step, mode;
static cJSON *response;
static const char *request_id = "erase-native-protocol-0001";
static const char *digest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000001";
int server_send_response(server_conn_t *conn, cJSON *body)
{
   (void)conn;
   cJSON_Delete(response);
   response = cJSON_Duplicate(body, 1);
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *id)
{
   (void)conn;
   (void)id;
   assert(strstr(message, request_id));
   cJSON_Delete(response);
   response = cJSON_CreateObject();
   cJSON_AddStringToObject(response, "error", message);
   return -1;
}
int platform_random_bytes(void *out, size_t size)
{
   (void)out;
   (void)size;
   assert(!"explicit retry ID must be retained");
   return -1;
}
int db1_server_session_list_by_subject(const char *subject, char (*ids)[DB1_SS_ID_LEN], int max)
{
   assert(step++ == 0 && max == 4096 && strcmp(subject, "person@example.test") == 0);
   strcpy(ids[0], "initial-session");
   return 1;
}
char *kb_client_subject_erasure_begin(const char *id, const char *subject, cJSON *sessions,
                                      int *status)
{
   assert(strcmp(id, request_id) == 0 && strcmp(subject, "person@example.test") == 0);
   assert(cJSON_GetArraySize(sessions) == 1);
   const char *session = cJSON_GetArrayItem(sessions, 0)->valuestring;
   if (step == 1)
   {
      step++;
      assert(strcmp(session, "initial-session") == 0);
      *status = 200;
      return strdup("{\"memory_count\":2,\"document_count\":0}");
   }
   assert(step++ == 4 && strcmp(session, digest) == 0);
   *status = mode == 2 ? 500 : 200;
   return mode == 2 ? NULL : strdup("{\"memory_count\":3,\"document_count\":0}");
}
int db1_server_session_erase_subject(const char *id, const char *subject)
{
   assert(step++ == 2 && strcmp(id, request_id) == 0 &&
          strcmp(subject, "person@example.test") == 0);
   return 1;
}
char *db1_server_session_erasure_receipt(const char *id, const char *subject)
{
   assert(step++ == 3 && strcmp(id, request_id) == 0 &&
          strcmp(subject, "person@example.test") == 0);
   if (mode == 1)
      return NULL;
   cJSON *array = cJSON_CreateArray();
   cJSON_AddItemToArray(array, cJSON_CreateString(digest));
   char *text = cJSON_PrintUnformatted(array);
   cJSON_Delete(array);
   return text;
}
void kb_cache_invalidate_all(void)
{
   assert(step++ == 5);
}
char *kb_client_subject_erasure_complete(const char *id, int64_t count, int *status)
{
   assert(step++ == 6 && strcmp(id, request_id) == 0 && count == 1);
   *status = 200;
   return strdup(mode == 3
                     ? "{\"coverage_complete\":false,\"pending_owners\":1,\"event_created\":false}"
                     : "{\"coverage_complete\":true,\"pending_owners\":0,\"event_created\":true}");
}
int main(void)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "subject", "person@example.test");
   cJSON_AddStringToObject(request, "request_id", request_id);
   for (mode = 0; mode < 4; mode++)
   {
      step = 0;
      int result = handle_kb_erase_subject(NULL, NULL, request);
      if (mode == 1 || mode == 2)
      {
         assert(result == -1 && step == (mode == 1 ? 4 : 5));
         assert(!cJSON_GetObjectItemCaseSensitive(response, "coverage_complete"));
      }
      else
      {
         assert(result == 0 && step == 7);
         assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "coverage_complete")) ==
                (mode == 0));
         assert(cJSON_GetObjectItemCaseSensitive(response, "memory_count")->valueint == 3);
         assert(strcmp(cJSON_GetObjectItemCaseSensitive(response, "erasure_state")->valuestring,
                       mode == 0 ? "completed" : "pending_owners") == 0);
      }
   }
   cJSON_Delete(response);
   cJSON_Delete(request);
   return 0;
}
