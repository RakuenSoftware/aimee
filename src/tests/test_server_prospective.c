/* The actual MCP consumer must screen before sending the generic KB request. */
#include "cJSON.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern cJSON *tool_create_prospective_memory(cJSON *args);
extern cJSON *tool_list_prospective_memories(cJSON *args);
extern cJSON *tool_complete_prospective_memory(cJSON *args);
static int sends, audits, unavailable;
static cJSON *sent;
static const char *expected;

int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.screen_content") == 0);
   *result = NULL;
   if (unavailable)
      return -1;
   const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "content"));
   assert(text);
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", "ok");
   cJSON_AddStringToObject(*result, "verdict",
                           strstr(text, "reject-me")   ? "reject"
                           : strstr(text, "redact-me") ? "redact"
                                                       : "allow");
   cJSON_AddStringToObject(*result, "redacted", "[REDACTED]");
   return 1;
}
char *kb_v1_action_request(const char *method, cJSON *request)
{
   assert(strcmp(method, expected) == 0);
   sends++;
   cJSON_Delete(sent);
   sent = cJSON_Duplicate(request, 1);
   cJSON_Delete(request);
   if (strcmp(method, "memory.prospective_create") == 0)
   {
      cJSON *response = cJSON_CreateObject();
      cJSON_AddStringToObject(response, "status", "ok");
      cJSON_AddItemToObject(response, "prospective", cJSON_Duplicate(sent, 1));
      char *json = cJSON_PrintUnformatted(response);
      cJSON_Delete(response);
      return json;
   }
   return strdup("{\"status\":\"ok\",\"prospectives\":[]}");
}
char *kb_client_last_result_json(const char *message)
{
   (void)message;
   return strdup("{\"status\":\"unavailable\"}");
}
void kb_client_memory_audit_note(const char *op, int64_t id, const char *tier, const char *kind,
                                 const char *key, double confidence, const char *session, int ok)
{
   (void)id;
   (void)tier;
   (void)kind;
   (void)key;
   (void)confidence;
   (void)session;
   assert(!ok && strcmp(op, "memory.prospective_create.withheld_pii") == 0);
   audits++;
}
int main(void)
{
   expected = "memory.prospective_create";
   const char *fields[] = {"trigger_text", "action_text", "anchor_entity", "anchor_file"};
   for (int i = 0; i < 4; i++)
   {
      cJSON *args = cJSON_Parse("{\"trigger_text\":\"when\",\"action_text\":\"do\"}");
      cJSON_DeleteItemFromObjectCaseSensitive(args, fields[i]);
      cJSON_AddStringToObject(args, fields[i], "reject-me");
      cJSON *result = tool_create_prospective_memory(args);
      cJSON_Delete(result);
      cJSON_Delete(args);
      assert(sends == 0 && audits == i + 1);
   }
   cJSON *args = cJSON_Parse(
       "{\"trigger_text\":\"redact-me\",\"action_text\":\"do\",\"anchor_entity\":\"redact-me\"}");
   cJSON *result = tool_create_prospective_memory(args);
   cJSON_Delete(result);
   assert(sends == 1);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(sent, "trigger_text")->valuestring,
                 "[REDACTED]") == 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(sent, "anchor_entity")->valuestring,
                 "[REDACTED]") == 0);
   cJSON_AddStringToObject(args, "anchor_file", "redact-me");
   result = tool_create_prospective_memory(args);
   cJSON_Delete(result);
   assert(sends == 1);
   cJSON_DeleteItemFromObjectCaseSensitive(args, "anchor_file");
   unavailable = 1;
   result = tool_create_prospective_memory(args);
   cJSON_Delete(result);
   assert(sends == 1);
   unavailable = 0;
   char long_text[9001];
   memset(long_text, 'x', 9000);
   long_text[9000] = 0;
   cJSON_DeleteItemFromObjectCaseSensitive(args, "action_text");
   cJSON_AddStringToObject(args, "action_text", long_text);
   result = tool_create_prospective_memory(args);
   const char *rendered =
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(result, 0), "text")->valuestring;
   assert(strlen(cJSON_GetObjectItemCaseSensitive(sent, "action_text")->valuestring) == 9000 &&
          strlen(rendered) > 9000);
   cJSON_Delete(result);
   cJSON_Delete(args);
   expected = "memory.prospective_list";
   args = cJSON_Parse("{\"state\":\"armed\",\"limit\":7}");
   result = tool_list_prospective_memories(args);
   assert(cJSON_GetObjectItemCaseSensitive(sent, "limit")->valueint == 7);
   cJSON_Delete(result);
   cJSON_Delete(args);
   expected = "memory.prospective_complete";
   args = cJSON_Parse("{\"id\":42}");
   result = tool_complete_prospective_memory(args);
   assert(cJSON_GetObjectItemCaseSensitive(sent, "id")->valueint == 42);
   cJSON_Delete(result);
   cJSON_Delete(args);
   cJSON_Delete(sent);
   puts("server prospective generic commands: ok");
   return 0;
}
