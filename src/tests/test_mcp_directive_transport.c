/* Invoke the actual MCP consumers so retiring typed clients cannot bypass the
 * pre-transmission screen or truncate the Go response. */
#include "../server/server_mcp_call_table.c"
#include <assert.h>

static int sent, screen_available = 1;
static cJSON *last_request;
cJSON *text_content(const char *text)
{
   return cJSON_CreateString(text);
}
char *kb_client_last_result_json(const char *message)
{
   (void)message;
   return strdup("unavailable");
}
int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.screen_content") == 0);
   if (!screen_available)
      return 0;
   const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "content"));
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", "ok");
   cJSON_AddStringToObject(*result, "verdict", strstr(text, "secret") ? "redact" : "allow");
   cJSON_AddStringToObject(*result, "redacted", "[redacted]");
   return 1;
}
char *kb_v1_action_request(const char *method, cJSON *request)
{
   sent++;
   cJSON_Delete(last_request);
   last_request = cJSON_Duplicate(request, 1);
   char *encoded = cJSON_PrintUnformatted(request);
   assert(!strstr(encoded, "secret"));
   free(encoded);
   cJSON_Delete(request);
   if (strcmp(method, "memory.directive_list") == 0)
      return strdup("{\"status\":\"ok\",\"directives\":[{\"id\":7,\"extra\":\"preserved\"}]}");
   if (strcmp(method, "memory.directive_create") == 0)
      return strdup("{\"status\":\"ok\",\"directive\":{\"id\":7,\"state\":\"open\"}}");
   assert(strcmp(method, "memory.directive_resolve") == 0 ||
          strcmp(method, "memory.directive_suppress") == 0);
   return strdup("{\"status\":\"ok\"}");
}
static const char *recall_envelope;
int server_memory_store_selection(const cJSON *request)
{
   const char *store = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "store"));
   return store && strcmp(store, "kb") == 0;
}
void mcp_memory_scope_begin(cJSON *args, int *active_context_missing)
{
   (void)args;
   *active_context_missing = 0;
}
void mcp_memory_scope_end(void)
{
}
char *kb_client_memory_recall_shared_json(const char *hint, int limit, int session_start)
{
   (void)hint;
   (void)limit;
   (void)session_start;
   return strdup(recall_envelope);
}
char *server_user_memory_recall_json(const char *hint, int limit, int session_start)
{
   return kb_client_memory_recall_shared_json(hint, limit, session_start);
}
static void test_recall_refusals(void)
{
   const char *refusals[] = {
       "{\"status\":\"error\",\"kind\":\"protected_context_overflow\"}",
       "{\"status\":\"quarantined\",\"recall\":{}}",
       "{\"status\":\"degraded\",\"recall\":{}}",
   };
   for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++)
      for (int store = 0; store < 2; store++)
         for (int start = 0; start < 2; start++)
         {
            recall_envelope = refusals[i];
            struct mcp_call call = {0};
            call.jargs = cJSON_CreateObject();
            cJSON_AddStringToObject(call.jargs, "store", store ? "kb" : "user");
            cJSON_AddBoolToObject(call.jargs, "session_start", start);
            cJSON *reply = mcph_memory_recall(&call);
            assert(strcmp(cJSON_GetStringValue(reply), recall_envelope) == 0);
            cJSON_Delete(reply);
            cJSON_Delete(call.jargs);
         }
}
int main(void)
{
   test_recall_refusals();
   struct mcp_call call = {0};
   call.jargs = cJSON_Parse("{\"question\":\"secret question\",\"topic\":\"topic\"}");
   cJSON *reply = mcph_create_epistemic_directive(&call);
   assert(sent == 1);
   assert(strcmp(jo_str(last_request, "question", ""), "[redacted]") == 0);
   cJSON_Delete(reply);
   cJSON_Delete(call.jargs);
   call.jargs = cJSON_Parse("{\"question\":\"question\",\"anchor_file\":\"secret-path\"}");
   reply = mcph_create_epistemic_directive(&call);
   assert(sent == 1);
   cJSON_Delete(reply);
   cJSON_Delete(call.jargs);
   screen_available = 0;
   call.jargs = cJSON_Parse("{\"question\":\"question\"}");
   reply = mcph_create_epistemic_directive(&call);
   assert(sent == 1);
   cJSON_Delete(reply);
   cJSON_Delete(call.jargs);
   screen_available = 1;
   call.jargs = cJSON_Parse("{\"id\":7,\"note\":\"secret note\"}");
   reply = mcph_resolve_epistemic_directive(&call);
   assert(sent == 2);
   assert(strcmp(jo_str(last_request, "note", ""), "[redacted]") == 0);
   cJSON_Delete(reply);
   cJSON_Delete(call.jargs);
   call.jargs = cJSON_CreateObject();
   reply = mcph_list_epistemic_directives(&call);
   assert(sent == 3);
   assert(strstr(cJSON_GetStringValue(reply), "preserved"));
   cJSON_Delete(reply);
   cJSON_Delete(call.jargs);
   cJSON_Delete(last_request);
   puts("MCP directives: generic transport, screening and response preservation passed");
   return 0;
}
