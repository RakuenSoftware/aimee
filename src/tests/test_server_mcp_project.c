/* Exercise the production native provider wrapper, including its owned argument
 * copy. The lookup and stable project owner are typed boundary spies. */
#include "server_mcp_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *host_cwd = "/held/registered-worktree";
static const char *expected_cwd = "/held/registered-worktree";
static const cJSON *original;
static cJSON *observed;
static int resolver_calls, handler_calls, resolver_result;
static int index_calls, index_count = -1;
static int borrowed_response;

const char *run_cmd_get_cwd(void)
{
   return host_cwd;
}
int workspace_repo_identity(const char *cwd, char *project, size_t capacity, char *workspace,
                            size_t workspace_capacity)
{
   (void)workspace;
   (void)workspace_capacity;
   assert(cwd && expected_cwd && !strcmp(cwd, expected_cwd));
   resolver_calls++;
   snprintf(project, capacity, "%s", "local:registered-project");
   return resolver_result;
}
int kb_client_index_list(project_info_t *out, int capacity)
{
   if (index_count > 0)
   {
      assert(out && capacity > 0 && index_count == 1);
      memset(out, 0, sizeof *out);
      snprintf(out->name, sizeof out->name, "%s", "unrelated-index-project");
      snprintf(out->root, sizeof out->root, "%s", "/different/checkout");
   }
   index_calls++;
   return index_count; /* No KB instance is configured for this real resolver. */
}
int workspace_turn_source_identity(const char *cwd, int *fd, char digest[65])
{
   (void)cwd;
   (void)fd;
   (void)digest;
   assert(!"Native index project resolution must not invoke the learning source API");
   return -1;
}
cJSON *text_content(const char *text)
{
   return cJSON_CreateString(text);
}
static cJSON *native_handler(struct mcp_call *call)
{
   assert(call && !call->ctx && !call->conn && !call->structured);
   assert(!strcmp(call->tool, "index_investigate") && !strcmp(call->sid, "canonical-session"));
   assert(call->jargs != original);
   handler_calls++;
   cJSON_Delete(observed);
   observed = cJSON_Duplicate(call->jargs, 1);
   /* A handler cannot accidentally mutate the canonical admitted input. */
   cJSON_AddBoolToObject(call->jargs, "handler-local", 1);
   if (borrowed_response)
   {
      cJSON *reply = cJSON_CreateObject();
      cJSON_AddItemReferenceToObject(reply, "arguments", call->jargs);
      return reply;
   }
   return cJSON_CreateString("native-result");
}
mcp_tool_handler_fn mcp_tool_lookup(const char *tool)
{
   return tool && !strcmp(tool, "index_investigate") ? native_handler : NULL;
}
static void invoke(const char *json)
{
   cJSON *args = cJSON_Parse(json);
   assert(args);
   char *before = cJSON_PrintUnformatted(args);
   original = args;
   cJSON *reply = server_mcp_native_call("index_investigate", args, "canonical-session");
   assert(cJSON_IsString(reply) && !strcmp(reply->valuestring, "native-result"));
   char *after = cJSON_PrintUnformatted(args);
   assert(!strcmp(before, after));
   free(before);
   free(after);
   cJSON_Delete(reply);
   cJSON_Delete(args);
   original = NULL;
}
int main(void)
{
   invoke("{\"query\":\"owner\",\"cwd\":\"/model/supplied/foreign\"}");
   assert(resolver_calls == 1 && handler_calls == 1);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(observed, "project")),
                  "local:registered-project"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(observed, "cwd")),
                  "/model/supplied/foreign"));

   /* A resolved canonical owner never consults unrelated index labels. */
   assert(index_calls == 0);
   index_count = 0;
   invoke("{\"query\":\"owner\"}");
   assert(index_calls == 0 && resolver_calls == 2);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(observed, "project")),
                  "local:registered-project"));
   index_count = 1;
   invoke("{\"query\":\"owner\"}");
   assert(index_calls == 0 && resolver_calls == 3);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(observed, "project")),
                  "local:registered-project"));
   index_count = -1;
   resolver_calls = 1; /* Keep the argument-preservation cases independent. */

   const char *explicit_values[] = {"\"explicit-project\"", "\"\"", "null", "7", "{}"};
   for (size_t i = 0; i < sizeof explicit_values / sizeof explicit_values[0]; i++)
   {
      char json[256];
      snprintf(json, sizeof json, "{\"query\":\"owner\",\"project\":%s}", explicit_values[i]);
      invoke(json);
      cJSON *expected = cJSON_Parse(explicit_values[i]);
      assert(cJSON_Compare(expected, cJSON_GetObjectItemCaseSensitive(observed, "project"), 1));
      assert(resolver_calls == 1);
      cJSON_Delete(expected);
   }

   /* Neither an absent host cwd nor failed owner resolution uses args.cwd or a
    * process-global project. Missing project remains for the handler to refuse. */
   host_cwd = NULL;
   invoke("{\"query\":\"owner\",\"cwd\":\"/model/supplied/foreign\"}");
   assert(resolver_calls == 1 && !cJSON_HasObjectItem(observed, "project"));
   host_cwd = "";
   invoke("{\"query\":\"owner\"}");
   assert(resolver_calls == 1 && !cJSON_HasObjectItem(observed, "project"));
   host_cwd = expected_cwd;
   resolver_result = -1;
   invoke("{\"query\":\"owner\"}");
   assert(resolver_calls == 2 && index_calls == 1 && !cJSON_HasObjectItem(observed, "project"));
   resolver_result = 0;

   cJSON *args = cJSON_CreateObject();
   int calls = handler_calls;
   assert(!server_mcp_native_call("not-registered", args, "canonical-session"));
   assert(handler_calls == calls && resolver_calls == 2);
   cJSON_Delete(args);
   args = cJSON_CreateArray();
   cJSON *reply = server_mcp_native_call("index_investigate", args, "canonical-session");
   assert(cJSON_IsString(reply) && strstr(reply->valuestring, "must be an object"));
   assert(handler_calls == calls && resolver_calls == 2);
   cJSON_Delete(reply);
   cJSON_Delete(args);

   /* The callback owns its result container but can reference call-scoped
    * data. The public response must outlive both the callback and input. */
   borrowed_response = 1;
   args = cJSON_Parse("{\"query\":\"retained-response\"}");
   original = args;
   reply = server_mcp_native_call("index_investigate", args, "canonical-session");
   cJSON_Delete(args);
   original = NULL;
   const cJSON *retained = cJSON_GetObjectItemCaseSensitive(reply, "arguments");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(retained, "query")),
                  "retained-response"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(retained, "project")),
                  "local:registered-project"));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(retained, "handler-local")));
   cJSON_Delete(reply);
   borrowed_response = 0;
   resolver_calls--; /* Keep the HTTP contract assertion independent. */

   /* The HTTP caller explicitly supplies its existing transport cwd; inference
    * remains shared, while native can never choose this model-provided field. */
   expected_cwd = "/http/transport-cwd";
   args = cJSON_Parse("{\"cwd\":\"/http/transport-cwd\"}");
   server_mcp_inject_active_project(
       args, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "cwd")));
   assert(resolver_calls == 3 && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "project")));
   server_mcp_inject_active_project(args, "/must-not-resolve-explicit-project");
   assert(resolver_calls == 3);
   cJSON_Delete(args);
   cJSON_Delete(observed);
   puts(
       "MCP native/HTTP project preparation: host cwd, immutable inputs and explicit scope passed");
   return 0;
}
