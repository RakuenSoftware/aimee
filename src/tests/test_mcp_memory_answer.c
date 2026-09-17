#include "aimee.h"
#include "cJSON.h"
#include "kb_client.h"
#include "json_fluent.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern cJSON *tool_memory_ask(cJSON *args, cJSON **structured_out);
static char project[1024], workspace[1024];
static int active, all, calls;
static cJSON *reply;
static const char *expected_project;

int workspace_repo_identity(const char *cwd, char *p, size_t pc, char *w, size_t wc)
{
   (void)cwd;
   (void)p;
   (void)pc;
   (void)w;
   (void)wc;
   return -1;
}
int server_active_project_from_cwd(const char *cwd, char *out, size_t cap)
{
   (void)cwd;
   (void)out;
   (void)cap;
   return -1;
}
void kb_client_memory_scope_context_set(const char *w, const char *p, int include_all)
{
   snprintf(project, sizeof(project), "%s", p);
   snprintf(workspace, sizeof(workspace), "%s", w);
   active = 1;
   all = include_all;
}
void kb_client_memory_scope_context_clear(void)
{
   active = 0;
}
void kb_client_memory_scope_context_apply(cJSON *request)
{
   assert(active);
   cJSON_AddBoolToObject(request, "scope_context", 1);
   cJSON_AddBoolToObject(request, "include_all", all);
   cJSON_AddStringToObject(request, "workspace", workspace);
   cJSON_AddStringToObject(request, "project", project);
}
char *kb_v1_action_request(const char *action, cJSON *request)
{
   calls++;
   assert(active && strcmp(action, "memory.ask") == 0);
   assert(strcmp(jo_cstr(request, "query"), "query") == 0);
   assert(strcmp(jo_cstr(request, "project"), expected_project) == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "scope_context")));
   cJSON_Delete(request);
   return reply ? cJSON_PrintUnformatted(reply) : NULL;
}
char *kb_client_last_result_json(const char *message)
{
   cJSON *error = cJSON_CreateObject();
   cJSON_AddStringToObject(error, "status", "unavailable");
   cJSON_AddStringToObject(error, "message", message);
   char *raw = cJSON_PrintUnformatted(error);
   cJSON_Delete(error);
   return raw;
}

int main(void)
{
   reply = cJSON_Parse(
       "{\"status\":\"ok\",\"no_answer\":false,\"low_confidence\":false,\"confidence\":0.8,"
       "\"evidence_mode\":\"verbatim\",\"citation_ids\":[41,52],\"evidence_trace\":{\"decision\":"
       "\"answerable\",\"reason\":\"ok\",\"future_field\":\"retained\"}}");
   char answer[6000];
   memset(answer, 'a', sizeof(answer) - 1);
   answer[sizeof(answer) - 1] = 0;
   cJSON_AddStringToObject(reply, "answer", answer);
   cJSON *args =
       cJSON_Parse("{\"query\":\"query\",\"project\":\"project-a\",\"workspace\":\"team\"}");
   expected_project = "project-a";
   cJSON *structured = NULL;
   cJSON *content = tool_memory_ask(args, &structured);
   assert(!active && calls == 1);
   assert(strcmp(jo_cstr(structured, "answer"), answer) == 0);
   assert(strcmp(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), answer) == 0);
   assert(strcmp(jo_cstr(cJSON_GetObjectItemCaseSensitive(structured, "evidence_trace"),
                         "future_field"),
                 "retained") == 0);
   assert(jo_i64(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(structured, "citations"), 1),
                 "memory_id", 0) == 52);
   assert(!jo_bool(structured, "active_context_missing", 1));
   cJSON_Delete(content);
   cJSON_Delete(structured);

   cJSON_DeleteItemFromObjectCaseSensitive(args, "project");
   cJSON_DeleteItemFromObjectCaseSensitive(args, "workspace");
   expected_project = "__aimee_scope_missing__";
   cJSON_ReplaceItemInObjectCaseSensitive(reply, "no_answer", cJSON_CreateBool(1));
   structured = NULL;
   content = tool_memory_ask(args, &structured);
   assert(!active && strcmp(jo_cstr(structured, "status"), "abstained") == 0);
   assert(jo_bool(structured, "active_context_missing", 0));
   assert(strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "No confident answer"));
   cJSON_Delete(content);
   cJSON_Delete(structured);

   cJSON_ReplaceItemInObjectCaseSensitive(reply, "citation_ids", cJSON_CreateString("malformed"));
   structured = NULL;
   content = tool_memory_ask(args, &structured);
   assert(!active && !structured);
   assert(strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "invalid answer"));
   cJSON_Delete(content);
   cJSON_Delete(reply);
   reply = NULL;
   content = tool_memory_ask(args, &structured);
   assert(!active && !structured);
   assert(strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "unavailable"));
   cJSON_Delete(content);
   cJSON_Delete(args);
   puts("mcp_memory_answer: PASS (scope, long answers, trace, citations, failure)");
   return 0;
}
