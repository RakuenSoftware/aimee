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
static const char *expected_read_policy;
static const char *expected_version;
static int expect_include_version;

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
static const char *private_command_reply, *private_command_operation;
static cJSON *materialize_reply(cJSON *reply);
static cJSON *private_envelope(const char *json)
{
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "status", "ok");
   cJSON_AddStringToObject(reply, "json", json);
   return reply;
}

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
static const char *request_account;
const char *server_request_account(void)
{
   return request_account;
}

static const char *request_principal = "";
static cJSON *observed_private_context;
const char *request_context_principal(void)
{
   return request_principal;
}

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

   if (private_command_operation)
   {
      assert(!strcmp(operation, private_command_operation));
      return private_command_reply
                 ? private_envelope(private_command_reply)
                 : server_error_kind_json(SERVER_ERR_UNAVAILABLE, unavailable_message, NULL);
   }

   if (strcmp(operation, "user-stats") == 0)
      return private_envelope("{\"status\":\"ok\",\"store\":\"user\",\"stats\":{\"total\":3}}");
   if (strcmp(operation, "user-store") == 0)
   {
      store_calls++;
      cJSON *confidence = cJSON_GetObjectItemCaseSensitive(request, "confidence");
      assert((confidence ? confidence->valuedouble : 1.0) == expected_confidence);
      return private_envelope("{\"status\":\"ok\",\"store\":\"user\",\"id\":42}");
   }
   user_calls++;
   assert(strcmp(operation, "user-get") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(request, "id")->valuedouble == 42);
   if (user_result < 0)
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, unavailable_message, NULL);
   if (user_result == 1)
      return server_error_kind_json(SERVER_ERR_NOT_FOUND, "user memory not found", NULL);
   return private_envelope(
       "{\"status\":\"ok\",\"store\":\"user\",\"memory\":{\"id\":42,\"content\":"
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
      if (expect_include_version)
         assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "include_version")));
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
      const cJSON *policy = cJSON_GetObjectItemCaseSensitive(request, "read_policy");
      if (expected_read_policy)
      {
         char *encoded = cJSON_PrintUnformatted(policy);
         assert(encoded && !strcmp(encoded, expected_read_policy));
         free(encoded);
      }
      else
         assert(!policy);
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
   if (!strcmp(method, "memory.store") || !strcmp(method, "memory.delete") ||
       !strcmp(method, "memory.supersede"))
   {
      store_calls++;
      if (expected_version)
      {
         char *encoded =
             cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(request, "expected_version"));
         assert(encoded && !strcmp(encoded, expected_version));
         assert(!strcmp(
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "idempotency_key")),
             "fixture-retry-key-001"));
         free(encoded);
      }
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
   if (!strcmp(method, "memory.restore") && expected_version)
   {
      char *encoded =
          cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(request, "expected_version"));
      assert(encoded && !strcmp(encoded, expected_version));
      free(encoded);
      assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(request, "idempotency_key")));
   }
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
   cJSON_AddNumberToObject(response, "http_status",
                           !strcmp(kind, "not_found")          ? 404
                           : !strcmp(kind, "invalid_argument") ? 400
                                                               : 503);
}
static void test_stats_transport(void)
{
   cJSON *request =
       cJSON_Parse("{\"operation\":\"delete\",\"authority\":\"user\",\"view\":\"console\"}");
   int previous_calls = review_calls;
   cJSON *reply = materialize_reply(memory_stats_command(request));
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
   expected_version = "{\"schema_version\":1,\"owner_id\":\"00000000-0000-0000-0000-000000000001\","
                      "\"record_id\":\"42\",\"record_revision\":\"9007199254740993\"}";
   cJSON_AddItemToObject(request, "expected_version", cJSON_Parse(expected_version));
   cJSON_AddNullToObject(request, "idempotency_key");
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
   expected_version = NULL;
   cJSON_Delete(request);
}

static void test_user_namespace(void)
{
   expected_time = "";
   cJSON *request = cJSON_Parse("{\"id\":42,\"project\":\"some-project\"}");
   cJSON *response = materialize_reply(memory_get_command(request));
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
         assert(cJSON_GetObjectItem(reply, "http_status")->valueint == 400);
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
         assert(rendered);
         if (i == 0)
            assert(!strcmp(rendered, store_reply));
         else
         {
            assert(!strncmp(rendered, "{\"http_status\":503,", 19));
            assert(!strcmp(rendered + 19, store_reply + 1));
         }
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
   assert(!strncmp(search_wire_reply, "{\"http_status\":503,", 19));
   assert(!strcmp(search_wire_reply + 19, review_reply + 1));
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
   assert(!strncmp(search_wire_reply, "{\"http_status\":503,", 19));
   assert(!strcmp(search_wire_reply + 19, review_reply + 1));
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
         if (i == 0)
            assert(!strcmp(raw, store_reply));
         else
         {
            assert(!strncmp(raw, "{\"http_status\":503,", 19));
            assert(!strcmp(raw + 19, store_reply + 1));
         }
         free(raw);
         cJSON_Delete(reply);
      }
   cJSON_Delete(request);
}

extern int handle_memory_supersede(server_ctx_t *, server_conn_t *, cJSON *);
static void test_shared_supersede_authority(void)
{
   cJSON *request = cJSON_Parse("{\"store\":\"kb\",\"old_id\":42,\"new_content\":\"corrected\","
                                "\"authority\":\"user\",\"actor\":\"forged\"}");
   expected_version = "{\"schema_version\":1,\"owner_id\":\"00000000-0000-0000-0000-000000000001\","
                      "\"record_id\":\"42\",\"record_revision\":\"9007199254740993\"}";
   cJSON_AddItemToObject(request, "expected_version", cJSON_Parse(expected_version));
   cJSON_AddStringToObject(request, "idempotency_key", "fixture-retry-key-001");
   store_reply = "{\"status\":\"ok\",\"store\":\"kb\",\"id\":9007199254740993}";
   for (expected_store_authority = 0; expected_store_authority < 2; expected_store_authority++)
   {
      request_account = expected_store_authority ? "user" : NULL;
      handle_memory_supersede(NULL, NULL, request);
      assert(search_wire_reply && !strcmp(search_wire_reply, store_reply));
   }
   request_account = NULL;
   expected_version = NULL;
   expected_store_authority = 0;
   store_reply = NULL;
   cJSON_Delete(request);
}
static void test_private_command_envelopes(void)
{
   const char *operations[] = {"user-store",  "user-get",       "user-list", "user-search",
                               "user-delete", "user-supersede", "user-stats"};
   const char *responses[] = {
       "{\"status\":\"ok\",\"store\":\"user\",\"id\":9007199254740993}",
       "{\"status\":\"ok\",\"store\":\"user\",\"memory\":{\"id\":9007199254740993,\"content\":"
       "\"個人設定\"}}",
       "{\"status\":\"ok\",\"store\":\"user\",\"memories\":[{\"id\":9007199254740993}]}",
       "{\"status\":\"ok\",\"facts\":[{\"id\":9007199254740993}],\"windows\":[]}",
       "{\"status\":\"ok\",\"store\":\"user\",\"id\":9007199254740993,\"deleted\":true,"
       "\"destroyed\":false}",
       "{\"status\":\"ok\",\"store\":\"user\",\"id\":9007199254740993,\"content\":\"complete "
       "replacement\"}",
       "{\"status\":\"ok\",\"store\":\"user\",\"stats\":{\"total\":9007199254740993}}"};
   cJSON *request = cJSON_Parse("{\"id\":\"9007199254740993\",\"old_id\":\"9007199254740993\"}");
   int shared_calls = calls;
   for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); i++)
   {
      private_command_operation = operations[i];
      for (int failure = 0; failure < 2; failure++)
      {
         private_command_reply =
             failure
                 ? "{\"status\":\"error\",\"kind\":\"unavailable\",\"receipt\":9007199254740995}"
                 : responses[i];
         cJSON *reply = NULL;
         switch (i)
         {
         case 0:
            reply = memory_store_command(request, MEMORY_AUTHORITY_MODEL);
            break;
         case 1:
            reply = memory_get_command(request);
            break;
         case 2:
            reply = memory_list_command(request);
            break;
         case 3:
            handle_memory_search(NULL, NULL, request);
            break;
         case 4:
            reply = memory_delete_command(request, "model");
            break;
         case 5:
            handle_memory_supersede(NULL, NULL, request);
            break;
         case 6:
            reply = memory_stats_command(request);
            break;
         }
         char *wire = reply ? cJSON_PrintUnformatted(reply) : strdup(search_wire_reply);
         if (failure)
         {
            const char *prefix = "{\"http_status\":503,";
            assert(wire && !strncmp(wire, prefix, strlen(prefix)) &&
                   !strcmp(wire + strlen(prefix), private_command_reply + 1));
         }
         else
            assert(wire && !strcmp(wire, private_command_reply));
         free(wire);
         cJSON_Delete(reply);
      }
   }
   private_command_operation = "user-get";
   const char *bad[] = {NULL,
                        "{}",
                        "[]",
                        "{\"status\":\"unknown\"}",
                        "{\"status\":\"ok\",\"store\":\"kb\"}",
                        "{\"status\":\"ok\",\"store\":\"user\"} trailing"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      private_command_reply = bad[i];
      cJSON *reply = materialize_reply(memory_get_command(request));
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(reply, "kind")->valuestring,
                     SERVER_ERR_UNAVAILABLE));
      cJSON_Delete(reply);
   }
   assert(calls == shared_calls);
   private_command_operation = NULL;
   private_command_reply = NULL;
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

int aimee_module_commands_dispatch_internal_context_timeout(const char *method, const cJSON *args,
                                                            const cJSON *context, int timeout_ms,
                                                            cJSON **result)
{
   assert(timeout_ms == 60000);
   cJSON_Delete(observed_private_context);
   observed_private_context = cJSON_Duplicate(context, 1);
   const char *operation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "operation"));
   *result =
       server_invoke_module_operation(method, operation, args, "user memory module unavailable");
   return *result ? 1 : -1;
}

static void test_private_verified_context(void)
{
   cJSON *request = cJSON_Parse("{\"key\":\"actor\",\"content\":\"content\",\"authority\":\"user\","
                                "\"principal\":\"forged\",\"operation\":\"forged\"}");
   private_command_operation = "user-store";
   private_command_reply = "{\"status\":\"ok\",\"store\":\"user\",\"id\":42}";
   for (int authenticated = 0; authenticated < 2; ++authenticated)
      for (int user = 0; user < 2; ++user)
      {
         request_account = authenticated ? "user" : "";
         request_principal = authenticated ? "verified-device" : "";
         cJSON *reply =
             memory_store_command(request, user ? MEMORY_AUTHORITY_USER : MEMORY_AUTHORITY_MODEL);
         assert(reply);
         assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(observed_private_context,
                                                              "authenticated")) == authenticated);
         assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                    observed_private_context, "user_authority")) == (authenticated && user));
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                            observed_private_context, "principal")),
                        authenticated ? "user" : ""));
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                            observed_private_context, "transport_identity")),
                        request_principal));
         cJSON_Delete(reply);
      }
   request_account = "user";
   request_principal = "verified-device";
   private_command_operation = "user-mcp-supersede";
   private_command_reply =
       "{\"status\":\"ok\",\"store\":\"user\",\"records\":[{\"id\":9007199254740993}]}";
   cJSON *reply = memory_user_mcp_supersede_command(request);
   assert(
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(observed_private_context, "authenticated")));
   assert(
       cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(observed_private_context, "user_authority")));
   char *wire = cJSON_PrintUnformatted(reply);
   assert(wire && strstr(wire, "9007199254740993"));
   free(wire);
   cJSON_Delete(reply);
   cJSON_Delete(observed_private_context);
   observed_private_context = NULL;
   private_command_operation = private_command_reply = NULL;
   request_account = request_principal = "";
   cJSON_Delete(request);
}

static void test_private_correction_review_transport(void)
{
   cJSON *request = cJSON_Parse("{\"store\":\"user\",\"proposal_id\":\"fixture\","
                                "\"action\":\"approve\",\"principal\":\"forged\","
                                "\"authority\":\"user\"}");
   for (int review = 0; review < 2; ++review)
      for (int authenticated = 0; authenticated < 2; ++authenticated)
      {
         request_account = authenticated ? "verified-user" : "";
         request_principal = authenticated ? "verified-device" : "";
         private_command_operation =
             review ? "user-correction-review" : "user-correction-proposals";
         private_command_reply = review
                                     ? "{\"status\":\"ok\",\"store\":\"user\",\"proposal\":{"
                                       "\"target_version\":{\"record_id\":\"9007199254740993\"}}}"
                                     : "{\"status\":\"ok\",\"store\":\"user\",\"proposals\":[]}";
         if (review)
            handle_memory_review_correction(NULL, NULL, request);
         else
            handle_memory_correction_proposals(NULL, NULL, request);
         assert(search_wire_reply && !strcmp(search_wire_reply, private_command_reply));
         assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                    observed_private_context, "user_authority")) == (review && authenticated));
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                            observed_private_context, "principal")),
                        authenticated ? "verified-user" : ""));
      }
   cJSON_Delete(observed_private_context);
   observed_private_context = NULL;
   private_command_operation = private_command_reply = NULL;
   request_account = request_principal = "";
   cJSON_Delete(request);
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
   result = 0;
   expected_read_policy =
       "{\"schema_version\":1,\"mode\":\"historical\",\"valid_at\":\"2026-01-01T00:00:00Z\"}";
   request = cJSON_Parse("{\"store\":\"kb\",\"id\":42}");
   cJSON_AddItemToObject(request, "read_policy", cJSON_Parse(expected_read_policy));
   cJSON_AddBoolToObject(request, "include_version", 1);
   expect_include_version = 1;
   response = materialize_reply(memory_get_command(request));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(response, "memory")));
   cJSON_Delete(response);
   cJSON_Delete(request);
   expected_read_policy = NULL;
   expect_include_version = 0;
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
   test_private_command_envelopes();
   test_shared_supersede_authority();
   test_private_verified_context();
   test_private_correction_review_transport();
   return 0;
}
