/* Exercise the real host adapter; commits and leases belong to Go. */
#include "aimee.h"
#include "cJSON.h"
#include "kb/kb_memory_facts.h"
#include "kb/kb_module_stage_adapters.h"
#include "modules/kb-synthesis/kb_curator_llm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int claims, completions, providers, fail_provider, fail_commit, stale, malformed;
void db2_lease_release_idle(void)
{
}
cJSON *kb_module_memory_data(const cJSON *request)
{
   const char *op = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "operation"));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "include_all")));
   if (!strcmp(op, "memory-facts-claim"))
   {
      if (claims++)
         return cJSON_Parse("{}");
      if (malformed)
         return cJSON_Parse("{\"fact_work\":{\"job_id\":1,\"generation\":2,\"lease_token\":"
                            "\"lease\",\"source_hash\":\"hash\"}}");
      return cJSON_Parse(
          "{\"fact_work\":{\"job_id\":1,\"generation\":2,\"lease_token\":\"lease\",\"source_hash\":"
          "\"hash\",\"system_prompt\":\"extract\",\"content\":\"original note\"}}");
   }
   assert(!strcmp(op, "memory-facts-complete"));
   completions++;
   const cJSON *work = cJSON_GetObjectItemCaseSensitive(request, "fact_work");
   assert(cJSON_GetObjectItemCaseSensitive(work, "generation")->valueint == 2);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(work, "lease_token")),
                  "lease"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(work, "source_hash")),
                  "hash"));
   assert(!cJSON_HasObjectItem(work, "content"));
   if (fail_provider || malformed || (fail_commit && completions > 1))
      assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(request, "success")));
   else
   {
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "success")));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "content")),
                     "provider response"));
   }
   if (fail_commit && completions == 1)
      return NULL;
   return cJSON_Parse(stale ? "{\"updated\":false}" : "{\"updated\":true}");
}
char *kb_curator_llm_run(kb_curator_stage_t stage, const char *prompt, const char *request,
                         struct cJSON *schema, const char *fallback, int cap, char *error,
                         size_t errorlen)
{
   providers++;
   assert(stage == KB_CURATOR_STAGE_EXTRACT_DOCS && !strcmp(prompt, "extract"));
   assert(!schema && !fallback[0] && cap == 8192);
   cJSON *input = cJSON_Parse(request);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "content")),
                  "original note"));
   cJSON_Delete(input);
   if (fail_provider)
   {
      snprintf(error, errorlen, "provider HTTP 503");
      return NULL;
   }
   return strdup("provider response");
}
int main(void)
{
   for (int mode = 0; mode < 5; mode++)
   {
      claims = completions = providers = 0;
      fail_provider = mode == 1;
      fail_commit = mode == 2;
      stale = mode == 3;
      malformed = mode == 4;
      assert(kb_memory_facts_drain(3) == 1);
      assert(completions == (fail_commit ? 2 : 1));
      assert(providers == (malformed ? 0 : 1));
   }
   puts("memory facts connection: all tests passed");
   return 0;
}
