/* Legacy host connection exercised against the actual Go pattern detector. */
#include "aimee.h"
#include "cJSON.h"
#include "db1_client/execution_trace.h"
#include "modules/db2/c/anti_patterns.h"
#include "support/module_runtime_fixture.h"
#include "trace_analysis.h"
#include <assert.h>

static int load_count = 4, unavailable, fail_write, fail_cursor, exists_result;
static int writes, records;
static const char *malformed;
static const int64_t last_id = INT64_C(9007199254741507);

int64_t db2_trace_mining_last_id(void)
{
   return last_id;
}
int db1_execution_trace_list_after_id(int64_t after, db1_execution_trace_mining_row_t *out, int max)
{
   assert(after == last_id && max >= 4);
   for (int i = 0; i < load_count; i++)
   {
      out[i].id = last_id + 1 + i;
      out[i].plan_id = 7;
      snprintf(out[i].tool_name, sizeof(out[i].tool_name), "%s", i == 3 ? "Search" : "Read");
      snprintf(out[i].tool_result, sizeof(out[i].tool_result), "%s", i == 3 ? "ok" : "error");
   }
   return load_count;
}
int db2_trace_mining_record(int64_t id)
{
   assert(id == last_id + load_count);
   records++;
   return fail_cursor ? -1 : 0;
}
const char *session_id(void)
{
   return "trace-session";
}
int db2_anti_pattern_exists_exact(const char *key)
{
   assert(!strcmp(key, "Retry loop: Read called 3 times with 3 errors"));
   return exists_result;
}
int db2_memory_key_exists(const char *key)
{
   assert(!strcmp(key, "recovery:Read->Search"));
   return exists_result;
}
int db2_anti_pattern_insert(const char *key, const char *content, const char *source,
                            const char *source_ref, double confidence, anti_pattern_t *out)
{
   assert(!strcmp(key, "Retry loop: Read called 3 times with 3 errors"));
   assert(!strcmp(content, "Tool 'Read' was called 3 consecutive times with 3 failures. Consider a "
                           "different approach after 2 failures."));
   assert(!strcmp(source, "trace_mining") && !source_ref[0] && confidence == .7 && !out);
   writes++;
   return fail_write ? -1 : 0;
}
int memory_insert(const char *tier, const char *kind, const char *key, const char *content,
                  double confidence, const char *session, memory_t *out)
{
   assert(!strcmp(tier, TIER_L0) && !strcmp(kind, KIND_PROCEDURE));
   assert(!strcmp(key, "recovery:Read->Search"));
   assert(!strcmp(content, "When 'Read' fails, try 'Search' as a recovery step."));
   assert(confidence == .7 && !strcmp(session, "trace-session") && !out);
   writes++;
   return fail_write ? -1 : 0;
}
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **response)
{
   assert(!strcmp(method, "memory.runtime"));
   if (unavailable)
   {
      *response = NULL;
      return -1;
   }
   if (malformed)
   {
      *response = cJSON_Parse(malformed);
      return 1;
   }
   return module_runtime_fixture_call(args, response);
}
int main(void)
{
   assert(trace_mine() == 2 && writes == 2 && records == 1);
   writes = records = 0;
   exists_result = 1;
   assert(trace_mine() == 0 && writes == 0 && records == 1);
   exists_result = -1;
   records = 0;
   assert(trace_mine() == -1 && writes == 0 && records == 0);
   exists_result = 0;
   fail_write = 1;
   assert(trace_mine() == -1 && writes == 1 && records == 0);
   fail_write = 0;
   writes = 0;
   fail_cursor = 1;
   assert(trace_mine() == -1 && writes == 2 && records == 1);
   fail_cursor = 0;
   writes = records = 0;
   unavailable = 1;
   assert(trace_mine() == -1 && writes == 0 && records == 0);
   unavailable = 0;
   const char *bad[] = {"{}", "{\"status\":\"error\"}", "{\"status\":\"ok\",\"patterns\":{}}",
                        "{\"status\":\"ok\",\"patterns\":[{\"type\":\"procedure\",\"key\":\"k\","
                        "\"content\":\"c\",\"confidence\":0.7},{\"type\":\"bogus\"}]}",
                        "{\"status\":\"ok\",\"patterns\":[{\"type\":\"procedure\",\"key\":\"k\","
                        "\"content\":\"c\",\"confidence\":2}]}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      malformed = bad[i];
      assert(trace_mine() == -1 && writes == 0 && records == 0);
   }
   malformed = NULL;
   load_count = -1;
   assert(trace_mine() == -1 && writes == 0 && records == 0);
   load_count = 0;
   assert(trace_mine() == 0 && writes == 0 && records == 0);
   puts("trace transport: real Go analysis, write failures and cursor acknowledgement passed");
   return 0;
}
