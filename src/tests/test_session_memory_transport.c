/* Session startup forwards Go sections; it never ranks or renders memory rows. */
#include <assert.h>
#include "../cmd_session_lifecycle.c"
static const char *section_reply;
void kb_client_memory_scope_context_apply(cJSON *request)
{
   cJSON_AddBoolToObject(request, "scope_context", 1);
   cJSON_AddStringToObject(request, "project", "app");
}
char *kb_v1_action_request(const char *method, cJSON *request)
{
   assert(!strcmp(method, "memory.top_l2_facts") ||
          !strcmp(method, "memory.list_session_scope_priority"));
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "project")), "app"));
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "view")), "session"));
   cJSON_Delete(request);
   return section_reply ? strdup(section_reply) : NULL;
}
int main(void)
{
   char buf[25000] = "host prefix\n";
   char text[16000];
   memset(text, 'x', sizeof(text) - 1);
   text[sizeof(text) - 1] = 0;
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "status", "ok");
   cJSON_AddStringToObject(reply, "text", text);
   char *raw = cJSON_PrintUnformatted(reply);
   section_reply = raw;
   size_t start = strlen(buf),
          pos = session_append_memory_section(buf, start, sizeof(buf), "project");
   assert(pos == start + strlen(text) && !strcmp(buf + start, text));
   free(raw);
   cJSON_Delete(reply);
   section_reply = "{\"status\":\"ok\",\"text\":\"\"}";
   assert(session_append_memory_section(buf, pos, sizeof(buf), "shared") == pos);
   section_reply = NULL;
   size_t end = session_append_memory_section(buf, pos, sizeof(buf), "facts");
   assert(end > pos && strstr(buf + pos, "Memory context unavailable"));
   section_reply = "{\"status\":\"ok\",\"text\":123}";
   assert(session_append_memory_section(buf, end, sizeof(buf), "facts") > end);
   return 0;
}
