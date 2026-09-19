#include "aimee.h"
#include "cJSON.h"
#include "kb_client.h"
#include "server.h"
#include "integrity.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern cJSON *memory_get_command(cJSON *request);
extern cJSON *memory_stats_command(const cJSON *request);
extern cJSON *memory_review_list_command(cJSON *request);
extern cJSON *memory_restore_command(cJSON *request);
extern cJSON *memory_list_command(const cJSON *request);
extern cJSON *memory_store_command(const cJSON *request, memory_authority_t authority);

extern int handle_memory_search(server_ctx_t *, server_conn_t *, cJSON *);
static char *search_wire_reply;
int send_and_free(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   free(search_wire_reply);
   search_wire_reply = cJSON_PrintUnformatted(response);
   cJSON_Delete(response);
   return 0;
}

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
static const char *personal_recall_reply;
static int personal_quarantine;
int integrity_ingress_decide(const char *text, integrity_source_t source, const char *boundary,
                             int autonomous, integrity_result_t *result_out)
{
   (void)result_out;
   assert(text && source == INTEGRITY_SOURCE_AGENT_MESSAGE && !strcmp(boundary, "recall") &&
          autonomous);
   return personal_quarantine;
}
static double expected_confidence;
static const char *store_reply, *get_reply;
extern cJSON *memory_delete_command(cJSON *, const char *);
memory_authority_t server_account_memory_authority(const char *account)
{
   return account && !strcmp(account, "user") ? MEMORY_AUTHORITY_USER : MEMORY_AUTHORITY_MODEL;
}
static int expected_store_authority;

/* Go validates and shapes these commands. This native test only verifies the
 * explicit user/KB routing boundary and propagation of complete module replies. */
cJSON *server_invoke_module_operation(const char *method, const char *operation,
                                      const cJSON *request, const char *unavailable_message)
{
   assert(strcmp(method, "memory.runtime") == 0);
   if (strcmp(operation, "user-review-list") == 0)
   {
      assert(strcmp(unavailable_message, "user memory review unavailable") == 0);
      cJSON *reply = cJSON_CreateObject();
      cJSON_AddStringToObject(reply, "status", "ok");
      cJSON_AddStringToObject(
          reply, "json",
          "{\"status\":\"ok\",\"store\":\"user\",\"memories\":[{\"id\":9007199254740993}]}");
      return reply;
   }
   assert(strcmp(unavailable_message, "user memory module unavailable") == 0);
   if (strcmp(operation, "personal-recall") == 0)
   {
      assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "task_hint")),
                    "private query") == 0);
      assert(cJSON_GetObjectItemCaseSensitive(request, "limit_tokens")->valuedouble == 8192);
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "session_start")));
      if (!personal_recall_reply)
         return server_error_kind_json(SERVER_ERR_UNAVAILABLE, unavailable_message, NULL);
      cJSON *reply = cJSON_CreateObject();
      cJSON_AddStringToObject(reply, "status", "ok");
      cJSON_AddStringToObject(reply, "json", personal_recall_reply);
      return reply;
   }

   if (strcmp(operation, "user-stats") == 0)
      return cJSON_Parse("{\"status\":\"ok\",\"store\":\"user\",\"stats\":{\"total\":3}}");
   if (strcmp(operation, "user-store") == 0)
   {
      store_calls++;
      cJSON *confidence = cJSON_GetObjectItemCaseSensitive(request, "confidence");
      assert((confidence ? confidence->valuedouble : 1.0) == expected_confidence);
      return cJSON_Parse("{\"status\":\"ok\",\"store\":\"user\",\"id\":42}");
   }
   user_calls++;
   assert(strcmp(operation, "user-get") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(request, "id")->valuedouble == 42);
   if (user_result < 0)
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, unavailable_message, NULL);
   if (user_result == 1)
      return server_error_kind_json(SERVER_ERR_NOT_FOUND, "user memory not found", NULL);
   return cJSON_Parse("{\"status\":\"ok\",\"store\":\"user\",\"memory\":{\"id\":42,\"content\":"
                      "\"private local memory\"}}");
}

static const char *review_reply, *review_method;
static int review_calls;
static const char *expected_search_keyword;
static int restore_audits, restore_successes;
void kb_client_memory_audit_note(const char *op, int64_t id, const char *tier, const char *kind,
                                 const char *key, double confidence, const char *session_id, int ok)
{
   assert(strcmp(op, "memory.restore") == 0 && id == 42);
   assert(!tier && !kind && !key && confidence == 0 && !session_id);
   restore_audits++;
   restore_successes += ok;
}
void kb_client_memory_scope_context_apply(cJSON *request)
{
   cJSON_AddBoolToObject(request, "scope_context", 1);
   cJSON_AddStringToObject(request, "project", "test-project");
}
char *kb_v1_action_request(const char *method, cJSON *request)
{
   if (!strcmp(method, "memory.get"))
   {
      calls++;
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "scope_context")));
      assert(!cJSON_HasObjectItem(request, "actor") && !cJSON_HasObjectItem(request, "authority"));
      if (get_reply)
      {
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "id")),
                        "9007199254740993"));
         cJSON_Delete(request);
         return strdup(get_reply);
      }
      assert(cJSON_GetObjectItemCaseSensitive(request, "id")->valuedouble == 42);
      const char *as_of = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "as_of"));
      assert(!strcmp(as_of ? as_of : "", expected_time));
      cJSON_Delete(request);
      if (result)
         return strdup(result < 0 ? "{\"status\":\"error\",\"kind\":\"unavailable\"}"
                                  : "{\"status\":\"error\",\"kind\":\"not_found\"}");
      cJSON *reply = cJSON_Parse("{\"status\":\"ok\",\"store\":\"kb\",\"memory\":{\"id\":42,"
                                 "\"content\":\"KB-owned memory\"}}");
      if (expected_time[0])
      {
         cJSON_AddStringToObject(reply, "as_of", expected_time);
         if (answer == KB_VALID_AT_UNKNOWN)
            cJSON_AddStringToObject(reply, "valid_at", "unknown");
         else
            cJSON_AddBoolToObject(reply, "valid_at", answer == KB_VALID_AT_YES);
      }
      char *raw = cJSON_PrintUnformatted(reply);
      cJSON_Delete(reply);
      return raw;
   }
   if (!strcmp(method, "memory.store") || !strcmp(method, "memory.delete"))
   {
      store_calls++;
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "scope_context")));
      assert(!cJSON_HasObjectItem(request, "include_all"));
      assert(!cJSON_HasObjectItem(request, "actor"));
      assert(!cJSON_HasObjectItem(request, "operation"));
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "view")->valuestring, "server"));
      const cJSON *authority = cJSON_GetObjectItemCaseSensitive(request, "authority");
      assert(expected_store_authority
                 ? cJSON_IsString(authority) && !strcmp(authority->valuestring, "user")
                 : authority == NULL);
      cJSON_Delete(request);
      return store_reply ? strdup(store_reply) : NULL;
   }

   assert(strcmp(method, review_method) == 0);
   if (!strcmp(method, "memory.search"))
   {
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "view")->valuestring, "server"));
      assert(!strcmp(
          cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(request, "keywords"), 0)->valuestring,
          expected_search_keyword));
   }

   if (strcmp(method, "memory.stats") == 0)
      assert(cJSON_GetArraySize(request) == 0);
   else
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "scope_context")));
   assert(!cJSON_HasObjectItem(request, "actor"));
   assert(!cJSON_HasObjectItem(request, "authority"));
   assert(!cJSON_HasObjectItem(request, "operation"));
   review_calls++;
   cJSON_Delete(request);
   return review_reply ? strdup(review_reply) : NULL;
}
void server_error_kind_apply(cJSON *response, const char *kind)
{
   cJSON_DeleteItemFromObjectCaseSensitive(response, "kind");
   cJSON_AddStringToObject(response, "kind", kind);
}
static void test_stats_transport(void)
{
   cJSON *request =
       cJSON_Parse("{\"operation\":\"delete\",\"authority\":\"user\",\"view\":\"console\"}");
   int previous_calls = review_calls;
   cJSON *reply = memory_stats_command(request);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(reply, "store")->valuestring, "user") == 0);
   assert(review_calls == previous_calls);
   cJSON_Delete(reply);
   cJSON_AddStringToObject(request, "store", "kb");
   review_method = "memory.stats";
   review_reply =
       "{\"status\":\"ok\",\"stats\":{\"total\":9007199254740993},\"receipt\":\"owner\"}";
   reply = memory_stats_command(request);
   char *rendered = cJSON_PrintUnformatted(reply);
   assert(strcmp(rendered, review_reply) == 0);
   free(rendered);
   cJSON_Delete(reply);
   const char *bad[] = {NULL,
                        "{}",
                        "[]",
                        "{\"status\":\"ok\"}",
                        "{\"status\":\"ok\",\"stats\":null}",
                        "{\"status\":\"ok\",\"stats\":{}} trailing"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      review_reply = bad[i];
      reply = memory_stats_command(request);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(reply, "kind")->valuestring,
                    SERVER_ERR_UNAVAILABLE) == 0);
      cJSON_Delete(reply);
   }
   review_reply = "{\"status\":\"error\",\"kind\":\"unauthorized\",\"message\":\"owner refusal\"}";
   reply = memory_stats_command(request);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(reply, "kind")->valuestring, "unauthorized") ==
          0);
   cJSON_Delete(reply);
   cJSON_ReplaceItemInObjectCaseSensitive(request, "store", cJSON_CreateString("invalid"));
   previous_calls = review_calls;
   reply = memory_stats_command(request);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(reply, "kind")->valuestring,
                 SERVER_ERR_INVALID_ARGUMENT) == 0 &&
          review_calls == previous_calls);
   cJSON_Delete(reply);
   cJSON_Delete(request);
}

static void test_review_transport(void)
{
   cJSON *request = cJSON_Parse("{\"project\":\"test-project\"}");
   cJSON *response = memory_review_list_command(request);
   char *rendered = cJSON_PrintUnformatted(response);
   assert(strstr(rendered, "9007199254740993") && strstr(rendered, "\"store\":\"user\""));
   free(rendered);
   assert(review_calls == 0);
   cJSON_Delete(response);
   cJSON_AddStringToObject(request, "store", "kb");
   review_method = "memory.review_list";
   review_reply =
       "{\"status\":\"ok\",\"memories\":[{\"id\":9007199254740993,\"content\":\"complete\"}]}";
   response = memory_review_list_command(request);
   rendered = cJSON_PrintUnformatted(response);
   assert(strcmp(rendered, review_reply) == 0);
   free(rendered);
   cJSON_Delete(response);
   const char *bad[] = {NULL,
                        "{}",
                        "[]",
                        "{\"status\":\"ok\"}",
                        "{\"status\":\"ok\",\"memories\":null}",
                        "{\"status\":\"ok\",\"memories\":[]} trailing"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      review_reply = bad[i];
      response = memory_review_list_command(request);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(response, "kind")->valuestring,
                    SERVER_ERR_UNAVAILABLE) == 0);
      cJSON_Delete(response);
   }
   review_method = "memory.restore";
   cJSON_AddNumberToObject(request, "id", 42);
   cJSON_AddStringToObject(request, "actor", "forged");
   cJSON_AddStringToObject(request, "authority", "user");
   cJSON_AddStringToObject(request, "operation", "delete");
   review_reply = "{\"status\":\"ok\",\"id\":42,\"restored\":true,\"receipt\":\"owner\"}";
   response = memory_restore_command(request);
   rendered = cJSON_PrintUnformatted(response);
   assert(strcmp(rendered, review_reply) == 0);
   free(rendered);
   cJSON_Delete(response);
   review_reply = "{\"status\":\"ok\",\"id\":43,\"restored\":true}";
   response = memory_restore_command(request);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(response, "kind")->valuestring,
                 SERVER_ERR_UNAVAILABLE) == 0);
   cJSON_Delete(response);
   review_reply = "{\"status\":\"error\",\"kind\":\"unauthorized\",\"message\":\"owner denied\"}";
   response = memory_restore_command(request);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(response, "kind")->valuestring, "unauthorized") ==
          0);
   cJSON_Delete(response);
   assert(restore_audits == 3 && restore_successes == 1);
   cJSON_Delete(request);
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
   cJSON_Delete(request);
}

static cJSON *materialize_reply(cJSON *reply)
{
   char *raw = cJSON_PrintUnformatted(reply);
   cJSON_Delete(reply);
   cJSON *parsed = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   assert(parsed);
   return parsed;
}

static void test_store_confidence(void)
{
   const char *invalid[] = {"-1", "1.01", "false", "null", "\"invalid\"", "[]", "{}", "1e999"};
   const char *valid[] = {"0", "0.25", "1"};
   for (int shared = 0; shared < 2; shared++)
   {
      for (unsigned i = 0; shared && i < sizeof(invalid) / sizeof(invalid[0]); i++)
      {
         cJSON *request = cJSON_Parse("{\"key\":\"fixture\",\"content\":\"synthetic\"}");
         cJSON_AddStringToObject(request, "store", shared ? "kb" : "user");
         cJSON_AddItemToObject(request, "confidence", cJSON_Parse(invalid[i]));
         int before = store_calls;
         store_reply = "{\"status\":\"error\",\"kind\":\"invalid_argument\",\"message\":\"Go owner "
                       "rejected confidence\"}";
         cJSON *reply = materialize_reply(memory_store_command(request, MEMORY_AUTHORITY_MODEL));
         cJSON *kind = cJSON_GetObjectItem(reply, "kind");
         assert(cJSON_IsString(kind) &&
                strcmp(kind->valuestring, SERVER_ERR_INVALID_ARGUMENT) == 0);
         assert(store_calls == before + 1);
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
         store_reply = "{\"status\":\"ok\",\"store\":\"kb\",\"id\":42}";
         cJSON *reply = materialize_reply(memory_store_command(request, MEMORY_AUTHORITY_MODEL));
         assert(cJSON_GetObjectItem(reply, "id")->valuedouble == 42);
         cJSON_Delete(reply);
         cJSON_Delete(request);
      }
   }
}

static void test_store_owner_envelope(void)
{
   cJSON *request =
       cJSON_Parse("{\"store\":\"kb\",\"key\":\"fixture\",\"content\":\"complete\",\"authority\":"
                   "\"user\",\"authority\":\"user\",\"actor\":\"forged\",\"include_all\":true,"
                   "\"scope_context\":false,\"operation\":\"delete\"}");
   const char *responses[] = {
       "{\"status\":\"ok\",\"store\":\"kb\",\"id\":9007199254740993,\"receipt\":\"complete owner "
       "receipt\"}",
       "{\"status\":\"error\",\"kind\":\"review_required\",\"message\":\"preserve authoritative "
       "source\"}",
       "{\"status\":\"error\",\"kind\":\"conflict\",\"message\":\"immutable\"}"};
   for (expected_store_authority = 0; expected_store_authority < 2; expected_store_authority++)
   {
      for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); i++)
      {
         store_reply = responses[i];
         cJSON *reply = memory_store_command(
             request, expected_store_authority ? MEMORY_AUTHORITY_USER : MEMORY_AUTHORITY_MODEL);
         char *rendered = cJSON_PrintUnformatted(reply);
         assert(rendered && !strcmp(rendered, store_reply));
         free(rendered);
         cJSON_Delete(reply);
      }
   }
   expected_store_authority = 0;
   const char *bad[] = {NULL, "{}", "[]", "{\"status\":\"ok\"}",
                        "{\"status\":\"ok\",\"id\":42} trailing"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      store_reply = bad[i];
      cJSON *reply = materialize_reply(memory_store_command(request, MEMORY_AUTHORITY_MODEL));
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(reply, "kind")->valuestring,
                     SERVER_ERR_UNAVAILABLE));
      cJSON_Delete(reply);
   }
   review_method = "memory.list";
   review_reply = "{\"status\":\"ok\",\"store\":\"kb\",\"memories\":[{\"id\":9007199254740993}],"
                  "\"receipt\":\"full owner reply\"}";
   cJSON *listed = memory_list_command(request);
   char *raw = cJSON_PrintUnformatted(listed);
   assert(raw && !strcmp(raw, review_reply));
   free(raw);
   cJSON_Delete(listed);
   cJSON_Delete(request);
}

static void test_search_owner_transport(void)
{
   char keyword[9001];
   memset(keyword, 'q', sizeof(keyword) - 1);
   keyword[sizeof(keyword) - 1] = 0;
   expected_search_keyword = keyword;
   cJSON *request = cJSON_Parse(
       "{\"store\":\"kb\",\"actor\":\"forged\",\"authority\":\"user\",\"operation\":\"delete\"}");
   cJSON *keywords = cJSON_AddArrayToObject(request, "keywords");
   cJSON_AddItemToArray(keywords, cJSON_CreateString(keyword));
   review_method = "memory.search";
   review_reply = "{\"status\":\"ok\",\"facts\":[{\"id\":9007199254740993}],\"windows\":[],"
                  "\"receipt\":\"owner\"}";
   handle_memory_search(NULL, NULL, request);
   assert(!strcmp(search_wire_reply, review_reply));
   review_reply =
       "{\"status\":\"error\",\"kind\":\"unavailable\",\"message\":\"window lane failed\"}";
   handle_memory_search(NULL, NULL, request);
   assert(!strcmp(search_wire_reply, review_reply));
   review_reply = "{\"status\":\"ok\",\"facts\":[]}";
   handle_memory_search(NULL, NULL, request);
   assert(strstr(search_wire_reply, "unavailable"));
   review_reply = "{\"status\":\"ok\",\"facts\":null}";
   handle_memory_search(NULL, NULL, request);
   assert(strstr(search_wire_reply, "unavailable"));
   free(search_wire_reply);
   search_wire_reply = NULL;
   cJSON_Delete(request);
}

static void test_read_owner_refusal(void)
{
   cJSON *request = cJSON_CreateObject();
   review_method = "memory.assemble_context";
   review_reply =
       "{\"status\":\"error\",\"kind\":\"unavailable\",\"message\":\"retrieval failed\"}";
   handle_memory_read(NULL, NULL, request);
   assert(!strcmp(search_wire_reply, review_reply));
   review_reply = "{\"status\":\"ok\",\"context\":\"\",\"active_context_missing\":true}";
   handle_memory_read(NULL, NULL, request);
   assert(!strcmp(search_wire_reply, review_reply));
   free(search_wire_reply);
   search_wire_reply = NULL;
   cJSON_Delete(request);
}

static void test_get_delete_owner_envelopes(void)
{
   cJSON *request = cJSON_Parse("{\"store\":\"kb\",\"id\":\"9007199254740993\",\"actor\":"
                                "\"forged\",\"authority\":\"user\"}");
   get_reply = "{\"status\":\"ok\",\"memory\":{\"id\":9007199254740993},\"receipt\":\"owner\"}";
   cJSON *reply = memory_get_command(request);
   char *raw = cJSON_PrintUnformatted(reply);
   assert(!strcmp(raw, get_reply));
   free(raw);
   cJSON_Delete(reply);
   get_reply = NULL;
   const char *replies[] = {
       "{\"status\":\"ok\",\"id\":9007199254740993,\"deleted\":true,\"destroyed\":false}",
       "{\"status\":\"error\",\"kind\":\"review_required\"}",
       "{\"status\":\"error\",\"kind\":\"conflict\"}"};
   for (expected_store_authority = 0; expected_store_authority < 2; expected_store_authority++)
      for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); i++)
      {
         store_reply = replies[i];
         reply = memory_delete_command(request, expected_store_authority ? "user" : "model");
         raw = cJSON_PrintUnformatted(reply);
         assert(!strcmp(raw, store_reply));
         free(raw);
         cJSON_Delete(reply);
      }
   cJSON_Delete(request);
}

static void test_personal_recall_owner_envelope(void)
{
   personal_recall_reply = "{\"status\":\"ok\",\"store\":\"user\",\"recall\":{\"identity\":[{"
                           "\"id\":9007199254740993,\"text\":\"個人設定\"}],\"approx_tokens\":81}}";
   char *body = server_user_memory_recall_json("private query", 8192, 1);
   assert(body && strcmp(body, personal_recall_reply) == 0);
   free(body);
   personal_recall_reply =
       "{\"status\":\"error\",\"kind\":\"unavailable\",\"receipt\":9007199254740995}";
   body = server_user_memory_recall_json("private query", 8192, 1);
   assert(body && strcmp(body, personal_recall_reply) == 0);
   free(body);
   personal_quarantine = 1;
   body = server_user_memory_recall_json("private query", 8192, 1);
   assert(body && strstr(body, "quarantined"));
   free(body);
   personal_quarantine = 0;
   personal_recall_reply = NULL;
   assert(server_user_memory_recall_json("private query", 8192, 1) == NULL);
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
      cJSON *response = materialize_reply(memory_get_command(request));
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
   cJSON *response = materialize_reply(memory_get_command(request));
   assert(!cJSON_HasObjectItem(response, "valid_at"));
   assert(!cJSON_HasObjectItem(response, "as_of"));
   cJSON_Delete(response);
   for (result = -1; result <= 1; result += 2)
   {
      response = materialize_reply(memory_get_command(request));
      const char *kind = cJSON_GetObjectItem(response, "kind")->valuestring;
      assert(strcmp(kind, result < 0 ? SERVER_ERR_UNAVAILABLE : SERVER_ERR_NOT_FOUND) == 0);
      cJSON_Delete(response);
   }
   assert(calls == 6 && clears == calls);
   cJSON_Delete(request);
   test_store_confidence();
   test_review_transport();
   test_store_owner_envelope();
   puts("server memory get: local privacy, explicit KB routing, temporal verdicts, and failures "
        "passed");
   test_stats_transport();
   test_search_owner_transport();
   test_get_delete_owner_envelopes();
   test_read_owner_refusal();
   test_personal_recall_owner_envelope();
   return 0;
}
