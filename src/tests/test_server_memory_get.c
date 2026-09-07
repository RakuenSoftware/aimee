#include "aimee.h"
#include "cJSON.h"
#include "kb_client.h"
#include "server.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern cJSON *memory_get_command(cJSON *request);
extern cJSON *memory_store_command(const cJSON *request, memory_authority_t authority);

static int calls, clears, result;
static kb_valid_at_t answer;
static const char *expected_time;

int workspace_repo_identity(const char *cwd, char *project, size_t project_cap, char *workspace,
                            size_t workspace_cap)
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

static int user_calls, user_result, store_calls;
static double expected_confidence;

cJSON *server_module_memory_data(const cJSON *request)
{
   const char *operation = cJSON_GetObjectItem(request, "operation")->valuestring;
   if (strcmp(operation, "store") == 0)
   {
      store_calls++;
      assert(cJSON_GetObjectItem(request, "confidence")->valuedouble == expected_confidence);
      return cJSON_Parse("{\"records\":[{\"id\":42}]}");
   }
   user_calls++;
   assert(strcmp(cJSON_GetObjectItem(request, "operation")->valuestring, "get") == 0);
   assert(cJSON_GetObjectItem(request, "id")->valuedouble == 42);
   if (user_result == -1)
      return NULL;
   if (user_result == -2)
      return cJSON_Parse("{}");
   if (user_result == 1)
      return cJSON_Parse("{\"records\":[]}");
   return cJSON_Parse("{\"records\":[{\"id\":42,\"content\":\"private local memory\"}]}");
}

static void test_user_namespace(void)
{
   expected_time = "";
   cJSON *request = cJSON_Parse("{\"id\":42,\"project\":\"some-project\"}");
   cJSON *response = memory_get_command(request);
   cJSON *memory = cJSON_GetObjectItem(response, "memory");
   assert(cJSON_IsObject(memory));
   assert(strcmp(cJSON_GetObjectItem(memory, "content")->valuestring, "private local memory") == 0);
   assert(strcmp(cJSON_GetObjectItem(response, "store")->valuestring, "user") == 0);
   assert(calls == 0 && user_calls == 1);
   cJSON_Delete(response);
   for (user_result = -2; user_result <= 1; user_result++)
   {
      if (user_result == 0)
         continue;
      response = memory_get_command(request);
      assert(strcmp(cJSON_GetObjectItem(response, "kind")->valuestring,
                    user_result == 1 ? SERVER_ERR_NOT_FOUND : SERVER_ERR_UNAVAILABLE) == 0);
      assert(calls == 0); /* A miss or outage never falls back to a colliding KB ID. */
      cJSON_Delete(response);
   }
   const char *invalid[] = {"both", "", "USER"};
   for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
   {
      cJSON_DeleteItemFromObjectCaseSensitive(request, "store");
      cJSON_AddStringToObject(request, "store", invalid[i]);
      response = memory_get_command(request);
      assert(strcmp(cJSON_GetObjectItem(response, "kind")->valuestring,
                    SERVER_ERR_INVALID_ARGUMENT) == 0);
      cJSON_Delete(response);
   }
   cJSON_DeleteItemFromObjectCaseSensitive(request, "store");
   cJSON_AddStringToObject(request, "as_of", "2020-01-01T00:00:00Z");
   response = memory_get_command(request);
   assert(strcmp(cJSON_GetObjectItem(response, "kind")->valuestring, SERVER_ERR_INVALID_ARGUMENT) ==
          0);
   assert(calls == 0 && user_calls == 4);
   cJSON_Delete(response);
   cJSON_Delete(request);
}

int kb_client_memory_insert_as(const char *tier, const char *kind, const char *key,
                               const char *content, const char *use_cases, double confidence,
                               const char *session_id, memory_authority_t authority, memory_t *out)
{
   (void)tier;
   (void)kind;
   (void)key;
   (void)content;
   (void)use_cases;
   (void)session_id;
   (void)authority;
   store_calls++;
   assert(confidence == expected_confidence);
   out->id = 42;
   return 0;
}

static void test_store_confidence(void)
{
   const char *invalid[] = {"-1", "1.01", "false", "null", "\"invalid\"", "[]", "{}", "1e999"};
   const char *valid[] = {"0", "0.25", "1"};
   for (int shared = 0; shared < 2; shared++)
   {
      for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
      {
         cJSON *request = cJSON_Parse("{\"key\":\"fixture\",\"content\":\"synthetic\"}");
         cJSON_AddStringToObject(request, "store", shared ? "kb" : "user");
         cJSON_AddItemToObject(request, "confidence", cJSON_Parse(invalid[i]));
         int before = store_calls;
         cJSON *reply = memory_store_command(request, MEMORY_AUTHORITY_MODEL);
         cJSON *kind = cJSON_GetObjectItem(reply, "kind");
         assert(cJSON_IsString(kind) &&
                strcmp(kind->valuestring, SERVER_ERR_INVALID_ARGUMENT) == 0);
         assert(store_calls == before);
         cJSON_Delete(reply);
         cJSON_Delete(request);
      }
      for (unsigned i = 0; i <= sizeof(valid) / sizeof(valid[0]); i++)
      {
         cJSON *request = cJSON_Parse("{\"key\":\"fixture\",\"content\":\"synthetic\"}");
         cJSON_AddStringToObject(request, "store", shared ? "kb" : "user");
         expected_confidence = 1.0;
         if (i < sizeof(valid) / sizeof(valid[0]))
         {
            cJSON *value = cJSON_Parse(valid[i]);
            expected_confidence = value->valuedouble;
            cJSON_AddItemToObject(request, "confidence", value);
         }
         cJSON *reply = memory_store_command(request, MEMORY_AUTHORITY_MODEL);
         assert(cJSON_GetObjectItem(reply, "id")->valuedouble == 42);
         cJSON_Delete(reply);
         cJSON_Delete(request);
      }
   }
}

int main(void)
{
   test_user_namespace();
   expected_time = "2020-01-01T00:00:00Z";
   cJSON *request = cJSON_Parse("{\"store\":\"kb\",\"id\":42,\"as_of\":\"2020-01-01T00:00:00Z\"}");
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
   test_store_confidence();
   puts("server memory get: local privacy, explicit KB routing, temporal verdicts, and failures "
        "passed");
   return 0;
}
