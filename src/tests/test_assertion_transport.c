/* Scope/precision/error contract of the remaining assertion bus transport. */
#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "modules/memory/memory_bus_context.h"
#include "modules/db2/c/kb_service_backend.h"
#include <assert.h>

static const char *receipt;
static int unavailable;
void memory_bus_read_context(db2_memory_scope_context_t *context)
{
   memset(context, 0, sizeof(*context));
   context->active = 1;
   snprintf(context->project, sizeof(context->project), "assertion-project");
   snprintf(context->scope_type, sizeof(context->scope_type), "project");
   snprintf(context->scope_value, sizeof(context->scope_value), "assertion-project");
}
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **out)
{
   assert(!strcmp(method, "memory.runtime"));
   assert(!strcmp(jo_cstr(args, "operation"), "assertion-search"));
   assert(!strcmp(jo_cstr(args, "project"), "assertion-project"));
   assert(!strcmp(jo_cstr(cJSON_GetObjectItemCaseSensitive(args, "scope"), "value"),
                  "assertion-project"));
   assert(!strcmp(jo_cstr(args, "query"), "query"));
   assert(!strcmp(jo_cstr(args, "valid_at"), "2026-01-01T00:00:00Z"));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "include_historical")));
   assert(cJSON_GetObjectItemCaseSensitive(args, "max_hops")->valueint == 2 &&
          cJSON_GetObjectItemCaseSensitive(args, "limit")->valueint == 10);
   *out = unavailable ? NULL : cJSON_Parse(receipt);
   return unavailable ? -1 : 1;
}
static cJSON *call(void)
{
   return db2_kb_service_memory_search_assertions_json("query", "2026-01-01T00:00:00Z", "", 1, 2,
                                                       10);
}
int main(void)
{
   receipt = "{\"status\":\"ok\",\"assertions\":[{\"assertion_id\":9007199254742002,\"stable_id\":"
             "\"9007199254742002\",\"rendered\":\"full 界 evidence\",\"historical\":false}]}";
   cJSON *result = call();
   assert(result);
   char *serialized = cJSON_PrintUnformatted(result);
   assert(serialized);
   assert(strstr(serialized, "\"assertion_id\":9007199254742002") &&
          strstr(serialized, "full 界 evidence"));
   cJSON *copy = cJSON_Duplicate(result, 1);
   char *copied = cJSON_PrintUnformatted(copy);
   assert(copied && strstr(copied, "\"assertion_id\":9007199254742002"));
   free(copied);
   cJSON_Delete(copy);
   free(serialized);
   cJSON_Delete(result);
   const char *bad[] = {
       "{}",
       "{\"assertions\":null}",
       "{\"assertions\":[{\"stable_id\":\"01\",\"rendered\":\"x\",\"historical\":false}]}",
       "{\"assertions\":[{\"assertion_id\":1,\"stable_id\":\"9223372036854775808\",\"rendered\":"
       "\"x\",\"historical\":false}]}",
       "{\"assertions\":[{\"assertion_id\":1,\"stable_id\":\"1\",\"rendered\":\"x\",\"historical\":"
       "1}]}",
       "{\"assertions\":[{\"stable_id\":\"1\",\"rendered\":\"x\",\"historical\":false}]}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      receipt = bad[i];
      assert(!call());
   }
   receipt = "{\"status\":\"error\",\"error_type\":\"invalid_timestamp\",\"assertions\":[]}";
   result = call();
   assert(result && !strcmp(jo_cstr(result, "error_type"), "invalid_timestamp"));
   cJSON_Delete(result);
   unavailable = 1;
   assert(!call());
   puts("assertion transport: scope, exact IDs, copied receipts and failures passed");
   return 0;
}
