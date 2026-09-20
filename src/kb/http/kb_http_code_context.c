/* Native code-search transport; the shared Go memory owner builds the packet. */
#include "kb_http_code.h"
#include "canonical_index.h"
#include "code_index.h"
#include "module_commands.h"
#include "json_fluent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_CONTEXT_HYBRID_BUF (256 * 1024)
#define CODE_CONTEXT_ENRICH_MAX 25

int handle_get_code_context(const char *query_string, char *out_buf, int out_cap)
{
   char query[512] = "";
   char symbol[256] = "";
   char project[256] = "";
   if (!code_qparam(query_string, "query", query, sizeof(query)) || !query[0])
      return code_scan_write_error(out_buf, out_cap, "missing query");
   code_qparam(query_string, "symbol", symbol, sizeof(symbol));
   int scope_status =
       code_request_project(query_string, project, sizeof(project), 0, NULL, out_buf, out_cap);
   if (scope_status)
      return scope_status;

   int64_t generation = 0;
   if (db2_code_index_project_current_generation(project, &generation) != 0 || generation <= 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":{\"type\":\"project_not_current\",\"message\":\"active project "
               "has no current generation\"}}");
      return 404;
   }

   char *hybrid = malloc(CODE_CONTEXT_HYBRID_BUF);
   if (!hybrid)
      return code_scan_write_error(out_buf, out_cap, "oom");
   int hybrid_status = handle_get_code_hybrid(query_string, hybrid, CODE_CONTEXT_HYBRID_BUF);
   if (hybrid_status != 200)
   {
      snprintf(out_buf, (size_t)out_cap, "%s", hybrid);
      free(hybrid);
      return hybrid_status;
   }
   cJSON *source = cJSON_Parse(hybrid);
   free(hybrid);
   if (!source)
      return code_scan_write_error(out_buf, out_cap, "hybrid response was invalid");

   code_search_hit_t enriched[CODE_CONTEXT_ENRICH_MAX];
   memset(enriched, 0, sizeof(enriched));
   int enriched_count =
       canonical_index_code_search(query, project, enriched, CODE_CONTEXT_ENRICH_MAX, 1);
   if (enriched_count < 0)
      enriched_count = 0;

   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "code-context");
   cJSON_AddStringToObject(request, "query", query);
   cJSON_AddStringToObject(request, "symbol", symbol);
   cJSON_AddStringToObject(request, "project", project);
   char generation_text[32];
   snprintf(generation_text, sizeof(generation_text), "%lld", (long long)generation);
   cJSON_AddRawToObject(request, "generation", generation_text);
   cJSON_AddItemToObject(request, "hybrid", source);
   cJSON *hits = cJSON_AddArrayToObject(request, "enriched");
   for (int i = 0; i < enriched_count && i < CODE_CONTEXT_ENRICH_MAX; i++)
   {
      cJSON *hit = cJSON_CreateObject();
      cJSON_AddStringToObject(hit, "project", enriched[i].project);
      cJSON_AddStringToObject(hit, "file_path", enriched[i].file_path);
      cJSON_AddNumberToObject(hit, "line", enriched[i].line);
      cJSON_AddItemToArray(hits, hit);
   }
   cJSON *reply = NULL;
   int rc =
       aimee_module_commands_dispatch_internal_timeout("memory.runtime", request, 10000, &reply);
   cJSON_Delete(request);
   const cJSON *http = cJSON_GetObjectItemCaseSensitive(reply, "http_status");
   const cJSON *packet = cJSON_GetObjectItemCaseSensitive(reply, "packet_json");
   int status = 503;
   if (rc != 1 || strcmp(jo_cstr(reply, "status"), "ok") != 0 || !cJSON_IsNumber(http) ||
       !cJSON_IsString(packet) ||
       (http->valueint != 200 && http->valueint != 401 && http->valueint != 409 &&
        http->valueint != 503))
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"unavailable\",\"dependency\":\"memory\",\"retryable\":true,\"retry_"
               "after_ms\":1000}");
   }
   else if (strlen(packet->valuestring) >= (size_t)out_cap)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"context packet exceeds response capacity\"}");
      status = 413;
   }
   else
   {
      snprintf(out_buf, (size_t)out_cap, "%s", packet->valuestring);
      status = http->valueint;
   }
   cJSON_Delete(reply);
   return status;
}

int handle_get_code_context_route(const char *method, const char *query_string, char *out_buf,
                                  int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_context(query_string, out_buf, out_cap);
}
