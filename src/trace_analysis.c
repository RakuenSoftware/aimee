/* Legacy DB1 collection connection. The Go KB memory owner atomically detects
 * and persists findings with its cursor. Source/epoch handoff, full-width DB1
 * collection and cross-page plan continuity still block shipping this path. */
#include "aimee.h"
#include "db1_client/db1.h"
#include "headers/module_commands.h"
#include "modules/memory/memory_bus_context.h"
#include "trace_analysis.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <errno.h>
#include <math.h>

#define MAX_TRACES 512

int trace_mine(void)
{
   int result = -1;
   db1_execution_trace_mining_row_t *rows = NULL;
   cJSON *request = cJSON_CreateObject(), *response = NULL;
   if (!request || !cJSON_AddStringToObject(request, "operation", "trace-state") ||
       aimee_module_commands_dispatch_internal("memory.runtime", request, &response) <= 0 ||
       strcmp(jo_cstr(response, "status"), "ok"))
      goto done;
   const char *cursor = jo_cstr(response, "last_id");
   char *end = NULL, decimal[32];
   errno = 0;
   int64_t last_id = strtoll(cursor, &end, 10);
   snprintf(decimal, sizeof(decimal), "%lld", (long long)last_id);
   if (errno || !cursor[0] || !end || *end || last_id < 0 || strcmp(cursor, decimal))
      goto done;
   cJSON_Delete(request);
   cJSON_Delete(response);
   request = cJSON_CreateObject();
   response = NULL;
   rows = calloc(MAX_TRACES, sizeof(*rows));
   if (!rows)
      goto done;
   int count = db1_execution_trace_list_after_id(last_id, rows, MAX_TRACES);
   if (count <= 0)
   {
      result = count;
      goto done;
   }
   if (count > MAX_TRACES || !request ||
       !cJSON_AddStringToObject(request, "operation", "trace-apply") ||
       !cJSON_AddStringToObject(request, "session_id", session_id()) ||
       memory_bus_add_context(request) != 0)
      goto done;
   cJSON *batch = cJSON_AddObjectToObject(request, "batch");
   if (!batch || !cJSON_AddStringToObject(batch, "after_id", decimal))
      goto done;
   cJSON *traces = cJSON_AddArrayToObject(batch, "rows");
   if (!traces)
      goto done;
   for (int i = 0; i < count; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row || !cJSON_AddItemToArray(traces, row))
      {
         cJSON_Delete(row);
         goto done;
      }
      snprintf(decimal, sizeof(decimal), "%lld", (long long)rows[i].id);
      if (!cJSON_AddStringToObject(row, "id", decimal) ||
          !cJSON_AddNumberToObject(row, "plan_id", rows[i].plan_id) ||
          !cJSON_AddNumberToObject(row, "turn", rows[i].turn) ||
          !cJSON_AddStringToObject(row, "tool_name", rows[i].tool_name) ||
          !cJSON_AddStringToObject(row, "tool_result", rows[i].tool_result))
         goto done;
   }
   if (aimee_module_commands_dispatch_internal("memory.runtime", request, &response) <= 0 ||
       strcmp(jo_cstr(response, "status"), "ok") || strcmp(jo_cstr(response, "last_id"), decimal))
      goto done;
   const cJSON *emitted = cJSON_GetObjectItemCaseSensitive(response, "emitted");
   if (cJSON_IsNumber(emitted) && isfinite(emitted->valuedouble) && emitted->valuedouble >= 0 &&
       emitted->valuedouble <= 1536 && floor(emitted->valuedouble) == emitted->valuedouble)
      result = (int)emitted->valuedouble;
done:
   cJSON_Delete(request);
   cJSON_Delete(response);
   free(rows);
   return result;
}
