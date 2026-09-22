/* Host connection for the shared Go fact worker. Go owns leases, source
 * validation, extraction, authority, commits, evidence and retry state. The
 * host supplies its existing configured curator provider connection. */
#include "kb_memory_facts.h"
#include "kb_module_stage_adapters.h"
#include "cJSON.h"
#include "kb_curator_llm.h"
#include "modules/db2/c/db2_internal.h"

#include <stdlib.h>

static int complete_job(const cJSON *work, const char *content, int success, const char *reason)
{
   cJSON *request = cJSON_CreateObject();
   cJSON *lease = request ? cJSON_AddObjectToObject(request, "fact_work") : NULL;
   if (!lease)
   {
      cJSON_Delete(request);
      return -1;
   }
   const char *fields[] = {"job_id", "generation", "lease_token", "source_hash"};
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
   {
      cJSON *field = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(work, fields[i]), 1);
      if (!field || !cJSON_AddItemToObject(lease, fields[i], field))
      {
         cJSON_Delete(field);
         cJSON_Delete(request);
         return -1;
      }
   }
   cJSON_AddStringToObject(request, "operation", "memory-facts-complete");
   cJSON_AddBoolToObject(request, "include_all", 1);
   cJSON_AddBoolToObject(request, "success", success != 0);
   cJSON_AddStringToObject(request, "content", content ? content : "");
   cJSON_AddStringToObject(request, "reason", reason ? reason : "");
   cJSON *reply = kb_module_memory_data(request);
   cJSON_Delete(request);
   /* A stale lease is a successful no-op; Go leaves the new generation intact. */
   int ok = reply && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(reply, "updated"));
   cJSON_Delete(reply);
   return ok ? 0 : -1;
}

int kb_memory_facts_drain(int batch)
{
   int processed = 0;
   for (int i = 0; i < batch; i++)
   {
      cJSON *request = cJSON_CreateObject();
      if (!request)
         break;
      cJSON_AddStringToObject(request, "operation", "memory-facts-claim");
      cJSON_AddBoolToObject(request, "include_all", 1);
      cJSON *reply = kb_module_memory_data(request);
      cJSON_Delete(request);
      const cJSON *work = cJSON_GetObjectItemCaseSensitive(reply, "fact_work");
      const cJSON *prompt = cJSON_GetObjectItemCaseSensitive(work, "system_prompt");
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(work, "content");
      if (!cJSON_IsObject(work))
      {
         cJSON_Delete(reply);
         break;
      }
      if (!cJSON_IsString(prompt) || !cJSON_IsString(content))
      {
         (void)complete_job(work, NULL, 0, "invalid memory module work item");
         cJSON_Delete(reply);
         processed++;
         continue;
      }
      cJSON *input = cJSON_CreateObject();
      if (input)
         cJSON_AddStringToObject(input, "content", content->valuestring);
      char *encoded = input ? cJSON_PrintUnformatted(input) : NULL;
      cJSON_Delete(input);
      char error[256] = "could not encode curator request";
      char *output = NULL;
      if (encoded)
      {
         db2_lease_release_idle();
         output = kb_curator_llm_run(KB_CURATOR_STAGE_EXTRACT_DOCS, prompt->valuestring, encoded,
                                     NULL, "", 8192, error, sizeof(error));
      }
      free(encoded);
      if (complete_job(work, output, output != NULL, output ? "" : error) != 0)
         (void)complete_job(work, NULL, 0, "fact commit unavailable");
      free(output);
      cJSON_Delete(reply);
      processed++;
   }
   return processed;
}
