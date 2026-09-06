#include "aimee.h"
#include "cJSON.h"
#include "kb_client.h"
#include "server.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern cJSON *memory_get_command(cJSON *request);

static int calls, clears, result;
static kb_valid_at_t answer;
static const char *expected_time;

int workspace_repo_identity(const char *cwd, char *project, size_t project_cap,
                            char *workspace, size_t workspace_cap)
{
   (void)cwd;
   (void)project;
   (void)project_cap;
   (void)workspace;
   (void)workspace_cap;
   return -1;
}

void kb_client_memory_scope_context_set(const char *workspace, const char *project, int all)
{
   assert(workspace && project && !all);
}

void kb_client_memory_scope_context_clear(void)
{
   clears++;
}

int kb_client_memory_get_json_as_of(int64_t id, const char *as_of, cJSON **out,
                                   kb_valid_at_t *verdict)
{
   calls++;
   assert(id == 42);
   assert(strcmp(as_of ? as_of : "", expected_time) == 0);
   *verdict = answer;
   *out = result == 0 ? cJSON_Parse("{\"id\":42,\"content\":\"KB-owned memory\"}") : NULL;
   return result;
}

cJSON *server_error_kind_json(const char *kind, const char *message, const char *request_id)
{
   (void)request_id;
   cJSON *response = cJSON_CreateObject();
   cJSON_AddStringToObject(response, "status", "error");
   cJSON_AddStringToObject(response, "kind", kind);
   cJSON_AddStringToObject(response, "message", message);
   return response;
}

int main(void)
{
   expected_time = "2020-01-01T00:00:00Z";
   cJSON *request = cJSON_Parse("{\"id\":42,\"as_of\":\"2020-01-01T00:00:00Z\"}");
   const kb_valid_at_t verdicts[] = {KB_VALID_AT_YES, KB_VALID_AT_NO, KB_VALID_AT_UNKNOWN};
   for (unsigned i = 0; i < sizeof(verdicts) / sizeof(verdicts[0]); i++)
   {
      answer = verdicts[i];
      cJSON *response = memory_get_command(request);
      assert(cJSON_IsObject(cJSON_GetObjectItem(response, "memory")));
      assert(strcmp(cJSON_GetObjectItem(response, "as_of")->valuestring, expected_time) == 0);
      cJSON *valid = cJSON_GetObjectItem(response, "valid_at");
      if (answer == KB_VALID_AT_UNKNOWN)
         assert(cJSON_IsString(valid) && strcmp(valid->valuestring, "unknown") == 0);
      else
         assert(cJSON_IsBool(valid) && cJSON_IsTrue(valid) == (answer == KB_VALID_AT_YES));
      cJSON_Delete(response);
   }
   cJSON_DeleteItemFromObjectCaseSensitive(request, "as_of");
   expected_time = "";
   cJSON *response = memory_get_command(request);
   assert(!cJSON_HasObjectItem(response, "valid_at"));
   assert(!cJSON_HasObjectItem(response, "as_of"));
   cJSON_Delete(response);
   for (result = -1; result <= 1; result += 2)
   {
      response = memory_get_command(request);
      const char *kind = cJSON_GetObjectItem(response, "kind")->valuestring;
      assert(strcmp(kind, result < 0 ? SERVER_ERR_UNAVAILABLE : SERVER_ERR_NOT_FOUND) == 0);
      cJSON_Delete(response);
   }
   assert(calls == 6 && clears == calls);
   cJSON_Delete(request);
   puts("server memory get: KB routing, temporal verdicts, and failures passed");
   return 0;
}
