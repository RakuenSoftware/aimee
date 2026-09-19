#include "aimee.h"
#include "cJSON.h"
#include "kb_client.h"
#include "json_fluent.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kb_client_memory_audit_note(const char *op, int64_t id, const char *tier, const char *kind,
                                 const char *key, double confidence, const char *session, int ok)
{
   (void)op;
   (void)id;
   (void)tier;
   (void)kind;
   (void)key;
   (void)confidence;
   (void)session;
   assert(ok);
}
extern cJSON *tool_search_memory(cJSON *args);
extern cJSON *tool_memory_mutate(cJSON *args);
extern cJSON *tool_list_facts(cJSON *args);
cJSON *memory_list_command(cJSON *args)
{
   (void)args;
   abort();
}
cJSON *memory_store_command(cJSON *args, memory_authority_t authority)
{
   (void)args;
   (void)authority;
   abort();
}
cJSON *memory_delete_command(cJSON *args, const char *account)
{
   (void)args;
   (void)account;
   abort();
}
cJSON *server_invoke_module_operation(const char *method, const char *operation, const cJSON *args,
                                      const char *error)
{
   (void)method;
   (void)operation;
   (void)args;
   (void)error;
   abort();
}
extern cJSON *tool_memory_briefing(cJSON *args);
extern cJSON *tool_memory_alerts(cJSON *args);
extern cJSON *tool_memory_ask(cJSON *args, cJSON **structured_out);
static char project[1024], workspace[1024];
static int active, all, calls;
static cJSON *reply;
static const char *expected_project;
static const char *expected_action = "memory.ask";
static int expected_all;

int server_memory_store_selection(const cJSON *request)
{
   (void)request;
   return 1;
}
cJSON *server_module_memory_data(const cJSON *request)
{
   (void)request;
   assert(0 && "KB search must not query user memory");
   return NULL;
}

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
   assert(active && strcmp(action, expected_action) == 0);
   if (!strcmp(action, "memory.ask") || !strcmp(action, "memory.find_facts_visible"))
      assert(strcmp(jo_cstr(request, "query"), "query") == 0);
   assert(strcmp(jo_cstr(request, "project"), expected_project) == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "scope_context")));
   if (strcmp(action, "memory.find_facts_visible") == 0)
   {
      assert(strcmp(jo_cstr(request, "format"), "mcp") == 0);
      assert(jo_int(request, "limit", 0) == 20);
      assert(jo_bool(request, "include_all", -1) == expected_all);
      assert(strcmp(jo_cstr(cJSON_GetObjectItemCaseSensitive(
                                cJSON_GetObjectItemCaseSensitive(request, "filter"), "scope"),
                            "workspace"),
                    "filtered-workspace") == 0);
   }
   if (strcmp(action, "memory.briefing") == 0)
   {
      assert(strcmp(jo_cstr(request, "format"), "mcp") == 0);
      assert(jo_int(request, "limit_tokens", 0) == 768);
      assert(jo_bool(request, "include_all", -1) == expected_all);
   }
   if (strcmp(action, "memory.alerts") == 0)
   {
      assert(strcmp(jo_cstr(request, "format"), "mcp") == 0);
      assert(strcmp(jo_cstr(request, "since"), "2026-09-01") == 0);
      assert(jo_bool(request, "include_all", -1) == expected_all);
   }
   if (!strcmp(action, "memory.list"))
   {
      assert(!strcmp(jo_cstr(request, "tier"), "L2") && !strcmp(jo_cstr(request, "kind"), "fact"));
      assert(jo_int(request, "limit", 0) == 64 && !strcmp(jo_cstr(request, "format"), "mcp"));
   }
   if (!strcmp(action, "memory.update"))
   {
      assert(!strcmp(jo_cstr(request, "id"), "9007199254740993"));
      assert(!cJSON_HasObjectItem(request, "authority"));
      assert(!strcmp(jo_cstr(request, "view"), "mcp"));
   }
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
   expected_action = "memory.find_facts_visible";
   expected_project = "project-a";
   args = cJSON_Parse("{\"query\":\"query\",\"project\":\"project-a\",\"include_all\":true,"
                      "\"filter\":{\"scope\":{\"workspace\":\"filtered-workspace\"}}}");
   reply = cJSON_Parse("{\"status\":\"ok\"}");
   char search[14000];
   memset(search, 'x', sizeof(search) - 1);
   search[sizeof(search) - 1] = 0;
   cJSON_AddStringToObject(reply, "text", search);
   content = tool_search_memory(args);
   assert(!active && strcmp(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), search) == 0);
   cJSON_Delete(content);
   // Only the boundary-resolved scope=all can enable an all-project query.
   expected_all = 1;
   cJSON_AddStringToObject(args, "scope", "all");
   content = tool_search_memory(args);
   assert(!active && strcmp(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), search) == 0);
   cJSON_Delete(content);
   cJSON_DeleteItemFromObjectCaseSensitive(args, "project");
   cJSON_DeleteItemFromObjectCaseSensitive(args, "scope");
   expected_all = 0;
   expected_project = "__aimee_scope_missing__";
   cJSON_ReplaceItemInObjectCaseSensitive(reply, "text", cJSON_CreateNumber(42));
   content = tool_search_memory(args);
   assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "invalid output"));
   cJSON_Delete(content);
   cJSON_ReplaceItemInObjectCaseSensitive(reply, "status", cJSON_CreateString("unavailable"));
   content = tool_search_memory(args);
   assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "unavailable"));
   cJSON_Delete(content);
   cJSON_Delete(reply);
   reply = NULL;
   content = tool_search_memory(args);
   assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "unavailable"));
   cJSON_Delete(content);
   cJSON_Delete(args);
   cJSON *(*bundle_tools[])(cJSON *) = {tool_memory_briefing, tool_memory_alerts};
   for (int i = 0; i < 2; i++)
   {
      expected_action = i == 0 ? "memory.briefing" : "memory.alerts";
      expected_project = "__aimee_scope_missing__";
      expected_all = 0;
      args = cJSON_Parse("{\"limit_tokens\":768,\"since\":\"2026-09-01\",\"include_all\":true}");
      reply = cJSON_Parse("{\"status\":\"ok\",\"output\":\"{\\\"key_facts\\\":[{\\\"memory_id\\\":"
                          "9223372036854775807}]}\"}");
      assert(reply);
      content = bundle_tools[i](args);
      assert(!active && strcmp(jo_cstr(cJSON_GetArrayItem(content, 0), "text"),
                               jo_cstr(reply, "output")) == 0);
      assert(strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "9223372036854775807"));
      cJSON_Delete(content);
      cJSON_AddStringToObject(args, "scope", "all");
      expected_all = 1;
      expected_project = "";
      content = bundle_tools[i](args);
      assert(!active);
      cJSON_Delete(content);
      cJSON_ReplaceItemInObjectCaseSensitive(reply, "output", cJSON_CreateNumber(7));
      content = bundle_tools[i](args);
      assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "invalid output"));
      cJSON_Delete(content);
      cJSON_ReplaceItemInObjectCaseSensitive(reply, "status", cJSON_CreateString("unavailable"));
      content = bundle_tools[i](args);
      assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "unavailable"));
      cJSON_Delete(content);
      cJSON_Delete(reply);
      reply = NULL;
      content = bundle_tools[i](args);
      assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "unavailable"));
      cJSON_Delete(content);
      cJSON_Delete(args);
   }
   cJSON *(*record_tools[])(cJSON *) = {tool_memory_mutate, tool_list_facts};
   char long_text[24000];
   memset(long_text, 'x', sizeof(long_text) - 1);
   long_text[sizeof(long_text) - 1] = 0;
   for (int i = 0; i < 2; i++)
   {
      expected_action = i == 0 ? "memory.update" : "memory.list";
      expected_project = "project-a";
      args = cJSON_Parse("{\"verb\":\"update\",\"id\":\"9007199254740993\",\"content\":"
                         "\"replacement\",\"authority\":\"user\",\"project\":\"project-a\"}");
      reply = cJSON_CreateObject();
      cJSON_AddStringToObject(reply, "status", "ok");
      cJSON_AddStringToObject(reply, "text", long_text);
      content = record_tools[i](args);
      assert(!active && !strcmp(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), long_text));
      cJSON_Delete(content);
      cJSON_ReplaceItemInObjectCaseSensitive(reply, "text", cJSON_CreateNumber(7));
      content = record_tools[i](args);
      assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "invalid memory"));
      cJSON_Delete(content);
      cJSON_ReplaceItemInObjectCaseSensitive(reply, "status", cJSON_CreateString("error"));
      cJSON_AddStringToObject(reply, "kind", "review_required");
      content = record_tools[i](args);
      assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "review_required"));
      cJSON_Delete(content);
      cJSON_Delete(reply);
      reply = NULL;
      content = record_tools[i](args);
      assert(!active && strstr(jo_cstr(cJSON_GetArrayItem(content, 0), "text"), "unavailable"));
      cJSON_Delete(content);
      cJSON_Delete(args);
   }
   puts("mcp_memory_answer: PASS (scope, long answers, trace, citations, failure)");
   return 0;
}
