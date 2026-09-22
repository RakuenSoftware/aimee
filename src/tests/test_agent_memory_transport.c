/* Exercise the shipping tools dispatcher, including its failure continuation. */
#include <assert.h>
#include "../modules/tools/agent_tools_dispatch.c"
static const char *owner_reply;
static kb_client_result_status_t transport_status = KB_CLIENT_RESULT_OK;
void kb_client_memory_scope_context_apply(cJSON *request)
{
   cJSON_AddBoolToObject(request, "scope_context", 1);
   cJSON_AddStringToObject(request, "project", "app");
}
char *kb_v1_action_request(const char *method, cJSON *request)
{
   assert(!strcmp(method, "memory.find_facts"));
   assert(!strcmp(jo_cstr(request, "query"), "deployment"));
   assert(!strcmp(jo_cstr(request, "format"), "tool"));
   assert(!strcmp(jo_cstr(request, "project"), "app"));
   assert(jo_int(request, "limit", 0) == 20);
   cJSON_Delete(request);
   return owner_reply ? strdup(owner_reply) : NULL;
}
kb_client_result_status_t kb_client_last_result_status(void)
{
   return transport_status;
}
int main(void)
{
   cJSON *args = cJSON_Parse("{\"query\":\"deployment\"}");
   char full[24000];
   memset(full, 'x', sizeof(full) - 1);
   full[sizeof(full) - 1] = 0;
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "status", "ok");
   cJSON_AddNumberToObject(reply, "count", 1);
   cJSON_AddStringToObject(reply, "text", full);
   char *raw = cJSON_PrintUnformatted(reply);
   owner_reply = raw;
   char *out = td_search_memory(args, "search_memory", "", "", 1000);
   assert(!strcmp(out, full));
   free(out);
   free(raw);
   cJSON_Delete(reply);
   owner_reply = "{\"status\":\"ok\",\"count\":0,\"text\":\"No facts\"}";
   out = td_search_memory(args, "search_memory", "", "", 1000);
   assert(strstr(out, "no local memory facts matched") && strstr(out, "aimee_retrieval_outcome"));
   free(out);
   const char *bad[] = {NULL, "{}", "{\"status\":\"ok\",\"count\":1.5,\"text\":\"bad\"}",
                        "{\"status\":\"ok\",\"count\":21,\"text\":\"bad\"}",
                        "{\"status\":\"ok\",\"count\":1,\"text\":7}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      owner_reply = bad[i];
      out = td_search_memory(args, "search_memory", "", "", 1000);
      assert(strstr(out, "error: memory service unavailable"));
      free(out);
   }
   transport_status = KB_CLIENT_RESULT_UNAUTHORIZED;
   owner_reply = NULL;
   out = td_search_memory(args, "search_memory", "", "", 1000);
   assert(strstr(out, "memory retrieval unauthorized"));
   free(out);
   cJSON_Delete(args);
   return 0;
}
