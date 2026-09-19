/* Exercise only host transport; corpus parsing and scoring are Go owner tests. */
#include "aimee.h"
#include "server.h"
#include "kb_client.h"
#include "json_fluent.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static char *captured;
static const char *owner_reply, *expected_path;
static int scoped, calls, failed;
int server_memory_scope_begin(cJSON *request)
{
   assert(!strcmp(jo_cstr(request, "project"), "local"));
   scoped = 1;
   return 0;
}
void kb_client_memory_scope_context_apply(cJSON *request)
{
   assert(scoped);
   cJSON_AddBoolToObject(request, "scope_context", 1);
   cJSON_AddStringToObject(request, "project", "local");
}
void kb_client_memory_scope_context_clear(void)
{
   scoped = 0;
}
char *kb_client_memory_benchmark_json(cJSON *request, const char *path)
{
   assert(scoped && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "scope_context")));
   assert(!cJSON_HasObjectItem(request, "actor") && !cJSON_HasObjectItem(request, "authority"));
   assert(!cJSON_HasObjectItem(request, "operation") &&
          !cJSON_HasObjectItem(request, "include_all"));
   assert(!strcmp(jo_cstr(request, "project"), "local"));
   assert(expected_path ? path && !strcmp(expected_path, path) : path == NULL);
   ++calls;
   cJSON_Delete(request);
   return owner_reply ? strdup(owner_reply) : NULL;
}
int send_and_free(server_conn_t *conn, cJSON *reply)
{
   (void)conn;
   assert(!scoped);
   free(captured);
   captured = cJSON_PrintUnformatted(reply);
   cJSON_Delete(reply);
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *id)
{
   (void)conn;
   (void)message;
   (void)id;
   assert(!scoped);
   ++failed;
   return 0;
}
int main(void)
{
   cJSON *request =
       cJSON_Parse("{\"suite\":\"live\",\"project\":\"local\",\"actor\":\"forged\",\"authority\":"
                   "\"user\",\"operation\":\"delete\",\"include_all\":true}");
   owner_reply =
       "{\"status\":\"ok\",\"case_results\":[{\"id\":9007199254740993}],\"receipt\":\"owner\"}";
   handle_memory_benchmark(NULL, NULL, request);
   assert(!strcmp(captured, owner_reply));
   cJSON_ReplaceItemInObject(request, "suite", cJSON_CreateString("code-graph-fusion"));
   expected_path = "benchmarks/code-vector-graph/production-corpus.json";
   handle_memory_benchmark(NULL, NULL, request);
   assert(!strcmp(captured, owner_reply));
   expected_path = "host-file.json";
   cJSON_AddStringToObject(request, "corpus", expected_path);
   handle_memory_benchmark(NULL, NULL, request);
   owner_reply = "{\"status\":\"error\",\"kind\":\"unavailable\",\"message\":\"query failed\"}";
   handle_memory_benchmark(NULL, NULL, request);
   assert(!strcmp(captured, owner_reply));
   owner_reply = "{\"status\":\"async-only\",\"run_via\":\"aimee memory benchmark locomo\"}";
   handle_memory_benchmark(NULL, NULL, request);
   assert(!strcmp(captured, owner_reply));
   const char *bad[] = {NULL, "not-json", "{\"status\":\"ok\"}",
                        "{\"status\":\"ok\",\"case_results\":null}",
                        "{\"status\":\"ok\",\"case_results\":[]} trailing"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      owner_reply = bad[i];
      handle_memory_benchmark(NULL, NULL, request);
   }
   assert(failed == 5 && calls == 10);
   free(captured);
   cJSON_Delete(request);
   puts("Server memory benchmark owner transport passed");
   return 0;
}
