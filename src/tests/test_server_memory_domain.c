/* Exercise the actual MCP consumers across the generic authenticated transport. */
#include "cJSON.h"
#include "json_fluent.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern cJSON *tool_search_graph(cJSON *args);
extern cJSON *tool_get_entity_edges(cJSON *args);
extern cJSON *tool_get_entity(cJSON *args);
extern cJSON *tool_get_episode(cJSON *args);
static const char *expected, *reply;
static cJSON *sent;
static char workspace[256], project[256];
static int include_all, scope_active;

int workspace_repo_identity(const char *cwd, char *p, size_t pn, char *w, size_t wn)
{
   (void)cwd;
   (void)p;
   (void)pn;
   (void)w;
   (void)wn;
   return -1;
}
int server_active_project_from_cwd(const char *cwd, char *out, size_t len)
{
   (void)cwd;
   (void)out;
   (void)len;
   return -1;
}
void kb_client_memory_scope_context_set(const char *w, const char *p, int all)
{
   snprintf(workspace, sizeof(workspace), "%s", w);
   snprintf(project, sizeof(project), "%s", p);
   include_all = all;
   scope_active = 1;
}
void kb_client_memory_scope_context_clear(void)
{
   scope_active = 0;
}
void kb_client_memory_scope_context_apply(cJSON *request)
{
   assert(scope_active);
   cJSON_AddBoolToObject(request, "scope_context", 1);
   cJSON_AddStringToObject(request, "workspace", workspace);
   cJSON_AddStringToObject(request, "project", project);
   cJSON_AddBoolToObject(request, "include_all", include_all);
}
char *kb_v1_action_request(const char *method, cJSON *request)
{
   assert(scope_active && strcmp(method, expected) == 0);
   cJSON_Delete(sent);
   sent = request;
   return reply ? strdup(reply) : NULL;
}
char *kb_client_last_result_json(const char *message)
{
   (void)message;
   return strdup("{\"status\":\"unavailable\"}");
}
static const char *rendered(cJSON *result)
{
   assert(!scope_active);
   return jo_cstr(cJSON_GetArrayItem(result, 0), "text");
}
int main(void)
{
   struct
   {
      const char *method, *field, *text_field;
      cJSON *(*call)(cJSON *);
      int array;
   } cases[] = {
       {"memory.search_graph", "relations", "src_entity", tool_search_graph, 1},
       {"memory.entity_edges", "edges", "dst_entity", tool_get_entity_edges, 1},
       {"memory.entity_profile", "profile", "summary", tool_get_entity, 0},
       {"memory.get_episode", "episode", "episode_text", tool_get_episode, 0},
   };
   char long_text[12001];
   memset(long_text, 'x', sizeof(long_text) - 1);
   long_text[sizeof(long_text) - 1] = 0;
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      expected = cases[i].method;
      cJSON *args =
          cJSON_Parse("{\"query\":\"q\",\"entity\":\"e\",\"episode_key\":\"ep\",\"project\":"
                      "\"project-a\",\"workspace\":\"workspace-a\",\"limit\":99}");
      cJSON *envelope = cJSON_CreateObject(), *row = cJSON_CreateObject();
      cJSON_AddStringToObject(envelope, "status", "ok");
      cJSON_AddStringToObject(row, cases[i].text_field, long_text);
      if (cases[i].array)
      {
         cJSON *rows = cJSON_AddArrayToObject(envelope, cases[i].field);
         cJSON_AddItemToArray(rows, row);
      }
      else
         cJSON_AddItemToObject(envelope, cases[i].field, row);
      char *success = cJSON_PrintUnformatted(envelope);
      reply = success;
      cJSON *result = cases[i].call(args);
      assert(strstr(rendered(result), long_text));
      assert(strcmp(jo_cstr(sent, "project"), "project-a") == 0);
      assert(strcmp(jo_cstr(sent, "workspace"), "workspace-a") == 0);
      assert(jo_bool(sent, "scope_context", 0) && !jo_bool(sent, "include_all", 1));
      if (cases[i].array)
         assert(jo_int(sent, "limit", 0) == 20);
      cJSON_Delete(result);
      cJSON_DeleteItemFromObjectCaseSensitive(args, "workspace");
      cJSON_DeleteItemFromObjectCaseSensitive(args, "project");
      result = cases[i].call(args);
      assert(strstr(rendered(result), "Active project context is unavailable"));
      assert(strcmp(jo_cstr(sent, "project"), "__aimee_scope_missing__") == 0);
      cJSON_Delete(result);
      cJSON_AddStringToObject(args, "scope", "all");
      result = cases[i].call(args);
      assert(jo_bool(sent, "include_all", 0));
      cJSON_Delete(result);
      const char *errors[] = {
          NULL, "not-json", "{\"status\":\"ok\"}",
          "{\"status\":\"error\",\"kind\":\"forbidden\",\"message\":\"denied\"}",
          "{\"status\":\"error\",\"kind\":\"not_found\"}"};
      for (size_t j = 0; j < sizeof(errors) / sizeof(errors[0]); j++)
      {
         reply = errors[j];
         result = cases[i].call(args);
         const char *text = rendered(result);
         if (j == 0)
            assert(strstr(text, "unavailable"));
         if (j == 1 || j == 2)
            assert(strstr(text, "invalid"));
         if (j == 3)
            assert(strstr(text, "forbidden") && !strstr(text, "empty"));
         if (j == 4 && !cases[i].array)
            assert(strstr(text, "empty"));
         cJSON_Delete(result);
      }
      free(success);
      cJSON_Delete(envelope);
      cJSON_Delete(args);
   }
   cJSON_Delete(sent);
   puts("server memory domain generic commands: ok");
   return 0;
}
