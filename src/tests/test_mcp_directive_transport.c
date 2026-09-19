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
int main(void)
{
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
