/* The actual Server consumer preserves verified authority and Go refusals. */
#include "cJSON.h"
#include "json_fluent.h"
#include "server/server_error_kind.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern cJSON *facts_retract_command(cJSON *req, const char *account);
static const char *reply;
static const char *expected_method = "facts.retract";
extern cJSON *entities_merge_command(cJSON *req);
extern cJSON *entities_unmerge_command(cJSON *req);
static cJSON *sent;
static int audited, sends;

int server_account_is_person(const char *account)
{
   return account && strcmp(account, "verified-user") == 0;
}
char *kb_v1_action_request(const char *method, cJSON *request)
{
   assert(strcmp(method, expected_method) == 0);
   cJSON_Delete(sent);
   sent = request;
   sends++;
   return reply ? strdup(reply) : NULL;
}
void kb_client_memory_audit_note(const char *op, int64_t id, const char *tier, const char *kind,
                                 const char *key, double confidence, const char *session_id, int ok)
{
   assert(strcmp(op, "facts.retract") == 0 && id == 0 && tier == NULL && key == NULL);
   assert(strcmp(kind, "works_for") == 0 && confidence == 0 && session_id == NULL);
   audited = ok;
}
extern cJSON *server_invoke_module_operation(const char *method, const char *operation,
                                             const cJSON *args, const char *unavailable_message);
static int internal_result = 1;
int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout_ms, cJSON **result)
{
   assert(timeout_ms == 60000);
   assert(strcmp(method, "memory.runtime") == 0);
   assert(strcmp(jo_cstr(args, "operation"), "user-get") == 0);
   assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(args, "id")) == 42);
   *result = reply ? cJSON_Parse(reply) : NULL;
   return internal_result;
}
static void test_named_module_envelope(void)
{
   cJSON *args = cJSON_Parse("{\"id\":42,\"operation\":\"delete\"}");
   const char *responses[] = {NULL, "{}", "[]", "{\"status\":17}", "{\"status\":\"unknown\"}"};
   for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); i++)
   {
      reply = responses[i];
      cJSON *r = server_invoke_module_operation("memory.runtime", "user-get", args, "unavailable");
      assert(strcmp(jo_cstr(r, "kind"), "unavailable") == 0);
      assert(jo_int(r, "http_status", 0) == 503);
      cJSON_Delete(r);
   }
   reply = "{\"status\":\"error\",\"kind\":\"conflict\",\"reason\":\"retired\"}";
   cJSON *r = server_invoke_module_operation("memory.runtime", "user-get", args, "unavailable");
   assert(jo_int(r, "http_status", 0) == 409 && strcmp(jo_cstr(r, "reason"), "retired") == 0);
   cJSON_Delete(r);
   reply = "{\"status\":\"ok\",\"memory\":{\"id\":42}}";
   r = server_invoke_module_operation("memory.runtime", "user-get", args, "unavailable");
   assert(jo_int(cJSON_GetObjectItemCaseSensitive(r, "memory"), "id", 0) == 42);
   assert(strcmp(jo_cstr(args, "operation"), "delete") == 0); /* Caller owns its arguments. */
   cJSON_Delete(r);
   internal_result = 0;
   r = server_invoke_module_operation("memory.runtime", "user-get", args, "unavailable");
   assert(strcmp(jo_cstr(r, "kind"), "unavailable") == 0);
   cJSON_Delete(r);
   cJSON_Delete(args);
}

extern int kb_handle_entities_merge(int fd, cJSON *req);
extern int kb_handle_entities_unmerge(int fd, cJSON *req);
static const char *kb_reply;
static int kb_transport = 1;
static char *kb_sent;
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(!strcmp(method, "memory.runtime") &&
          !strcmp(jo_cstr(args, "operation"), "entity-mutate"));
   assert(!strcmp(jo_cstr(args, "action"), expected_method + 9));
   assert(!cJSON_GetObjectItemCaseSensitive(args, "actor"));
   assert(!strcmp(jo_cstr(args, "from_id"), "9007199254740993"));
   *result = NULL;
   if (kb_transport == 1)
   {
      *result = cJSON_CreateObject();
      cJSON_AddStringToObject(*result, "status", "ok");
      cJSON_AddStringToObject(*result, "json", kb_reply);
   }
   return kb_transport;
}
int kb_reply_or_error(int fd, cJSON *resp, const char *message)
{
   (void)fd;
   (void)message;
   free(kb_sent);
   kb_sent = resp ? cJSON_PrintUnformatted(resp) : NULL;
   cJSON_Delete(resp);
   return kb_sent ? 0 : -1;
}
static void test_entity_transport(void)
{
   cJSON *args = cJSON_Parse("{\"from_id\":\"9007199254740993\",\"into_id\":2,\"merge_id\":3,"
                             "\"action\":\"forged\",\"actor\":{\"rank\":40}}");
   const char *success =
       "{\"status\":\"ok\",\"merge_id\":9007199254740993,\"commit_id\":\"owner-commit\"}";
   for (int undo = 0; undo < 2; undo++)
   {
      expected_method = undo ? "entities.unmerge" : "entities.merge";
      reply = success;
      cJSON *r = undo ? entities_unmerge_command(args) : entities_merge_command(args);
      char *raw = cJSON_PrintUnformatted(r);
      assert(raw && !strcmp(raw, success));
      free(raw);
      cJSON_Delete(r);
      assert(!cJSON_GetObjectItemCaseSensitive(sent, "actor") &&
             !cJSON_GetObjectItemCaseSensitive(sent, "action"));
      assert(!strcmp(jo_cstr(sent, "from_id"), "9007199254740993"));
      kb_reply = success;
      assert((undo ? kb_handle_entities_unmerge(0, args) : kb_handle_entities_merge(0, args)) == 0);
      assert(!strcmp(kb_sent, success));
   }
   const char *bad[] = {NULL,
                        "{}",
                        "[]",
                        "{\"status\":\"ok\"}",
                        "{\"status\":\"ok\",\"merge_id\":-1,\"commit_id\":\"c\"}",
                        "{\"status\":\"unknown\"}",
                        "{\"status\":\"ok\"} trailing"};
   expected_method = "entities.merge";
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      reply = bad[i];
      cJSON *r = entities_merge_command(args);
      assert(!strcmp(jo_cstr(r, "kind"), "unavailable"));
      cJSON_Delete(r);
      kb_reply = bad[i] ? bad[i] : "";
      assert(kb_handle_entities_merge(0, args) == -1);
   }
   reply = "{\"status\":\"error\",\"kind\":\"conflict\",\"message\":\"stale merge\"}";
   cJSON *r = entities_merge_command(args);
   assert(jo_int(r, "http_status", 0) == 409);
   cJSON_Delete(r);
   kb_reply = reply;
   assert(kb_handle_entities_merge(0, args) == 0 && !strcmp(kb_sent, reply));
   kb_transport = -1;
   assert(kb_handle_entities_merge(0, args) == -1);
   cJSON_Delete(args);
   free(kb_sent);
   kb_sent = NULL;
   expected_method = "facts.retract";
}
static int classify(const char *kind, uint32_t *status)
{
   *status = strcmp(kind, "conflict") == 0           ? 409
             : strcmp(kind, "invalid_argument") == 0 ? 400
                                                     : 503;
   return 0;
}
int main(void)
{
   server_error_kind_register_http_status_provider(classify);
   test_named_module_envelope();
   test_entity_transport();
   cJSON *args = cJSON_Parse("{\"source\":\"user\",\"relation\":\"works_for\",\"target\":\"Private "
                             "Corp\",\"authority\":\"user\"}");
   reply = "{\"status\":\"ok\",\"retracted\":1,\"authority\":\"user\"}";
   cJSON *r = facts_retract_command(args, "verified-user");
   assert(jo_int(r, "retracted", -1) == 1 && audited == 1);
   assert(strcmp(jo_cstr(sent, "authority"), "user") == 0);
   assert(strcmp(jo_cstr(sent, "source"), "user") == 0);
   assert(strcmp(jo_cstr(sent, "target"), "Private Corp") == 0);
   cJSON_Delete(r);
   r = facts_retract_command(args, "machine");
   assert(strcmp(jo_cstr(sent, "authority"), "model") == 0);
   cJSON_Delete(r);
   cJSON_ReplaceItemInObjectCaseSensitive(args, "authority", cJSON_CreateString("model"));
   r = facts_retract_command(args, "verified-user");
   assert(strcmp(jo_cstr(sent, "authority"), "model") == 0);
   cJSON_Delete(r);
   const char *failures[] = {NULL,
                             "bad-json",
                             "{}",
                             "{\"status\":\"ok\"}",
                             "{\"status\":\"ok\",\"retracted\":-1}",
                             "{\"status\":\"ok\",\"retracted\":1.5}",
                             "{\"status\":\"ok\",\"retracted\":4294967296}"};
   for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++)
   {
      reply = failures[i];
      r = facts_retract_command(args, "verified-user");
      assert(strcmp(jo_cstr(r, "kind"), "unavailable") == 0 && audited == 0);
      assert(jo_int(r, "http_status", 0) == 503);
      cJSON_Delete(r);
   }
   reply = "{\"status\":\"error\",\"kind\":\"conflict\",\"reason\":\"immutable\",\"message\":"
           "\"correction refused\"}";
   r = facts_retract_command(args, "verified-user");
   assert(strcmp(jo_cstr(r, "reason"), "immutable") == 0 && audited == 0);
   assert(strcmp(jo_cstr(r, "kind"), "conflict") == 0);
   assert(jo_int(r, "http_status", 0) == 409);
   cJSON_Delete(r);
   reply = "{\"status\":\"ok\",\"retracted\":0}";
   r = facts_retract_command(args, "verified-user");
   assert(jo_int(r, "retracted", -1) == 0 && audited == 1);
   cJSON_Delete(r);
   int before = sends;
   cJSON_ReplaceItemInObjectCaseSensitive(args, "authority", cJSON_CreateNumber(30));
   r = facts_retract_command(args, "verified-user");
   assert(strcmp(jo_cstr(r, "kind"), "invalid_argument") == 0 && before == sends);
   cJSON_Delete(r);
   cJSON_Delete(args);
   cJSON_Delete(sent);
   puts("server facts generic transport: ok");
   return 0;
}
