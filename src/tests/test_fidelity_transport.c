#include "cJSON.h"
#include "json_fluent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern int kb_handle_evidence_fidelity(int fd, cJSON *req);
static const char *response;
static cJSON *sent;
static int dispatches, transport = 1;

int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(!strcmp(method, "memory.runtime"));
   assert(!strcmp(jo_cstr(args, "operation"), "fidelity-read"));
   assert(!strcmp(jo_cstr(args, "turn_id"), " turn "));
   assert(!cJSON_GetObjectItemCaseSensitive(args, "sql"));
   dispatches++;
   *result = response ? cJSON_Parse(response) : NULL;
   return transport;
}
int kb_reply_or_error(int fd, cJSON *resp, const char *message)
{
   (void)fd;
   (void)message;
   cJSON_Delete(sent);
   sent = resp;
   return resp ? 0 : -1;
}
int kb_send_error(int fd, const char *message)
{
   (void)fd;
   (void)message;
   return -1;
}
int main(void)
{
   cJSON *req = cJSON_Parse("{\"turn_id\":\" turn \",\"operation\":\"delete\",\"sql\":\"forged\"}");
   response =
       "{\"status\":\"ok\",\"turn_id\":\" turn \",\"fidelity_status\":\"ok\","
       "\"report\":{\"supported\":3,\"unsupported\":1,\"abstained\":2},\"attribution_count\":1}";
   assert(kb_handle_evidence_fidelity(0, req) == 0);
   assert(jo_int(sent, "attribution_count", -1) == 1);
   assert(jo_int(cJSON_GetObjectItemCaseSensitive(sent, "report"), "supported", -1) == 3);
   assert(!strcmp(jo_cstr(req, "operation"), "delete"));
   response = "{\"status\":\"ok\",\"turn_id\":\" turn \",\"fidelity_status\":\"not_evaluated\"}";
   assert(kb_handle_evidence_fidelity(0, req) == 0);
   assert(!strcmp(jo_cstr(sent, "fidelity_status"), "not_evaluated"));
   const char *bad[] = {NULL, "{}", "[]", "{\"status\":\"error\"}",
                        "{\"status\":\"ok\",\"turn_id\":\"other\",\"fidelity_status\":\"ok\"}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      response = bad[i];
      assert(kb_handle_evidence_fidelity(0, req) == 0);
      assert(!strcmp(jo_cstr(sent, "fidelity_status"), "evidence_unavailable"));
      assert(!cJSON_GetObjectItemCaseSensitive(sent, "report"));
   }
   transport = -1;
   assert(kb_handle_evidence_fidelity(0, req) == 0);
   assert(!strcmp(jo_cstr(sent, "fidelity_status"), "evidence_unavailable"));
   int before = dispatches;
   cJSON_DeleteItemFromObjectCaseSensitive(req, "turn_id");
   assert(kb_handle_evidence_fidelity(0, req) == -1 && dispatches == before);
   cJSON_AddStringToObject(req, "turn_id", "");
   assert(kb_handle_evidence_fidelity(0, req) == -1 && dispatches == before);
   cJSON_Delete(sent);
   cJSON_Delete(req);
   puts("fidelity transport: pass");
   return 0;
}
