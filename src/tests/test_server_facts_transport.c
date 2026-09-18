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
static cJSON *sent;
static int audited, sends;

int server_account_is_person(const char *account)
{
   return account && strcmp(account, "verified-user") == 0;
}
char *kb_v1_action_request(const char *method, cJSON *request)
{
   assert(strcmp(method, "facts.retract") == 0);
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
