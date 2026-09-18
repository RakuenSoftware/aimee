/* Legacy trace collection/persistence connection. Pattern detection belongs to
 * the shared Go memory owner. This unshipped monolithic path still needs a
 * typed Server/KB handoff, source-specific cursor and atomic batch persistence. */
#include "aimee.h"
#include "db1_client/db1.h"
#include "modules/db2/c/anti_patterns.h"
#include "modules/db2/c/memory_payload.h"
#include "modules/db2/c/trace_mining.h"
#include "headers/module_commands.h"
#include "memory.h"
#include "trace_analysis.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <math.h>

#define MAX_TRACES 512

int trace_mine(void)
{
   int result = -1;
   int64_t last_id = db2_trace_mining_last_id();
   db1_execution_trace_mining_row_t *rows = calloc(MAX_TRACES, sizeof(*rows));
   cJSON *request = NULL, *response = NULL;
   if (!rows)
      return -1;
   int count = db1_execution_trace_list_after_id(last_id, rows, MAX_TRACES);
   if (count <= 0)
   {
      free(rows);
      return count;
   }
   if (count > MAX_TRACES)
      goto done;
   request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "trace-patterns"))
      goto done;
   cJSON *traces = cJSON_AddArrayToObject(request, "traces");
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
      if (!cJSON_AddNumberToObject(row, "plan_id", rows[i].plan_id) ||
          !cJSON_AddStringToObject(row, "tool_name", rows[i].tool_name) ||
          !cJSON_AddStringToObject(row, "tool_result", rows[i].tool_result))
         goto done;
   }
   if (aimee_module_commands_dispatch_internal("memory.runtime", request, &response) <= 0 ||
       strcmp(jo_cstr(response, "status"), "ok"))
      goto done;
   const cJSON *patterns = cJSON_GetObjectItemCaseSensitive(response, "patterns");
   if (!cJSON_IsArray(patterns) || cJSON_GetArraySize(patterns) > 1536)
      goto done;
   const cJSON *pattern;
   /* Validate the complete envelope before performing any writes. */
   cJSON_ArrayForEach(pattern, patterns)
   {
      const cJSON *key = cJSON_GetObjectItemCaseSensitive(pattern, "key");
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(pattern, "content");
      const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(pattern, "confidence");
      const char *type = jo_cstr(pattern, "type");
      if ((strcmp(type, "anti-pattern") && strcmp(type, "procedure")) || !cJSON_IsString(key) ||
          !key->valuestring[0] || !cJSON_IsString(content) || !content->valuestring[0] ||
          !cJSON_IsNumber(confidence) || !isfinite(confidence->valuedouble) ||
          confidence->valuedouble < 0 || confidence->valuedouble > 1)
         goto done;
   }
   int written = 0;
   cJSON_ArrayForEach(pattern, patterns)
   {
      const char *key = jo_cstr(pattern, "key"), *content = jo_cstr(pattern, "content");
      double confidence = cJSON_GetObjectItemCaseSensitive(pattern, "confidence")->valuedouble;
      int exists, rc;
      if (!strcmp(jo_cstr(pattern, "type"), "anti-pattern"))
      {
         exists = db2_anti_pattern_exists_exact(key);
         if (exists < 0)
            goto done;
         if (exists)
            continue;
         rc = db2_anti_pattern_insert(key, content, "trace_mining", "", confidence, NULL);
      }
      else
      {
         exists = db2_memory_key_exists(key);
         if (exists < 0)
            goto done;
         if (exists)
            continue;
         rc = memory_insert(TIER_L0, KIND_PROCEDURE, key, content, confidence, session_id(), NULL);
      }
      if (rc < 0)
         goto done;
      written++;
   }
   /* Only acknowledged writes permit advancing the legacy cursor. This is not
    * yet an atomic transaction or a source-specific cursor. */
   if (db2_trace_mining_record(rows[count - 1].id) == 0)
      result = written;
done:
   cJSON_Delete(request);
   cJSON_Delete(response);
   free(rows);
   return result;
}
