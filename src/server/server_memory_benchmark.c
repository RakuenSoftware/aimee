/* Server host transport for the shared Go memory benchmark owner. */
#include "aimee.h"
#include "server.h"
#include "server_state_internal.h"
#include "json_fluent.h"
#include "kb_client.h"
#include <aimee/core/event_bus/module_protocol.h>

int handle_memory_benchmark(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return server_send_error(conn, "memory benchmark request unavailable", NULL);
   const char *fields[] = {"suite", "corpus", "max_cases", "limit", "fusion_state", "arm", NULL};
   for (int i = 0; fields[i]; ++i)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(req, fields[i]);
      if (value)
         cJSON_AddItemToObject(request, fields[i], cJSON_Duplicate(value, 1));
   }
   const char *suite = jo_str(req, "suite", "code-graph-fusion");
   const char *path =
       !strcmp(suite, "code-graph-fusion")
           ? jo_str(req, "corpus", "benchmarks/code-vector-graph/production-corpus.json")
           : NULL;
   server_memory_scope_begin(req);
   kb_client_memory_scope_context_apply(request);
   char *raw = kb_client_memory_benchmark_json(request, path);
   kb_client_memory_scope_context_clear();
   cJSON *parsed = raw && strlen(raw) <= AIMEE_MODULE_MESSAGE_MAX_BODY
                       ? cJSON_ParseWithOpts(raw, NULL, 1)
                       : NULL;
   const char *status = jo_cstr(parsed, "status");
   int valid = cJSON_IsObject(parsed) &&
               ((!strcmp(status, "ok") &&
                 cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(parsed, "case_results"))) ||
                (!strcmp(status, "error") &&
                 cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "kind"))) ||
                (!strcmp(status, "async-only") &&
                 cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "run_via"))));
   cJSON *response = valid ? cJSON_CreateRaw(raw) : NULL;
   cJSON_Delete(parsed);
   free(raw);
   if (!response)
      return server_send_error(conn, "memory benchmark owner or corpus file unavailable", NULL);
   return send_and_free(conn, response);
}
