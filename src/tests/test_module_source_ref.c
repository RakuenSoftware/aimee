#include "module_source_ref.h"
#include <assert.h>
#include <stdlib.h>

static cJSON *reference(const char *scheme, const char *owner, const char *record,
                        const char *version)
{
   cJSON *value = cJSON_CreateObject();
   assert(value);
   assert(cJSON_AddNumberToObject(value, "schema_version", 1));
   assert(cJSON_AddStringToObject(value, "scheme", scheme));
   assert(cJSON_AddStringToObject(value, "owner", owner));
   assert(cJSON_AddStringToObject(value, "record", record));
   assert(cJSON_AddStringToObject(value, "version", version));
   return value;
}

int main(void)
{
   const int64_t wide = INT64_C(9007199254740993);
   cJSON *captured = reference("example-record/v1", "owner-a", "9007199254740993", "opaque-a");
   cJSON *current = cJSON_Duplicate(captured, 1);
   assert(module_source_ref_valid(captured, wide));
   assert(!module_source_ref_valid(captured, wide + 1));
   assert(module_source_ref_compare(captured, current, wide) == MODULE_SOURCE_REF_EQUAL);
   assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(current, "version"), "opaque-b"));
   assert(module_source_ref_compare(captured, current, wide) == MODULE_SOURCE_REF_CHANGED);
   assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(current, "owner"), "owner-b"));
   assert(module_source_ref_compare(captured, current, wide) == MODULE_SOURCE_REF_UNKNOWN);
   assert(module_source_ref_compare_legacy(NULL, NULL, wide, "old", "old") ==
          MODULE_SOURCE_REF_EQUAL);
   assert(module_source_ref_compare_legacy(NULL, NULL, wide, "old", "new") ==
          MODULE_SOURCE_REF_CHANGED);
   assert(module_source_ref_compare_legacy(NULL, NULL, wide, "old", "") ==
          MODULE_SOURCE_REF_UNKNOWN);
   assert(module_source_ref_compare_legacy(captured, NULL, wide, "old", "old") ==
          MODULE_SOURCE_REF_UNKNOWN);
   assert(module_source_ref_compare_legacy(NULL, current, wide, "old", "old") ==
          MODULE_SOURCE_REF_UNKNOWN);
   assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(current, "owner"), "owner-a"));
   assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(current, "scheme"), "other/v1"));
   assert(module_source_ref_compare(captured, current, wide) == MODULE_SOURCE_REF_UNKNOWN);
   cJSON *source = cJSON_CreateObject(), *destination = cJSON_CreateObject();
   assert(cJSON_AddItemToObject(source, "source_ref", cJSON_Duplicate(captured, 1)));
   assert(module_source_ref_copy(destination, source, "source_ref", wide) == 1);
   assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(destination, "source_ref"), captured, 1));
   assert(module_source_ref_copy(destination, source, "state_ref", wide) == 0);
   assert(module_source_ref_copy(destination, source, "source_ref", wide + 1) == -1);
   const char *invalid[] = {
       "{}",
       "{\"schema_version\":1,\"scheme\":\"x\",\"owner\":\"a\",\"record\":77,\"version\":\"v\"}",
       "{\"schema_version\":1,\"scheme\":\"x\",\"owner\":\"a\",\"record\":\"077\",\"version\":"
       "\"v\"}",
       "{\"schema_version\":2,\"scheme\":\"x\",\"owner\":\"a\",\"record\":\"77\",\"version\":"
       "\"v\"}",
       "{\"schema_version\":1,\"scheme\":\"x\",\"owner\":\"a\",\"record\":\"77\",\"version\":\"\"}",
       "{\"schema_version\":1,\"scheme\":\"x\",\"owner\":\"a\",\"record\":\"77\",\"version\":\"v\","
       "\"extra\":true}"};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
   {
      cJSON *bad = cJSON_Parse(invalid[i]);
      assert(bad && !module_source_ref_valid(bad, 77));
      assert(module_source_ref_compare(bad, bad, 77) == MODULE_SOURCE_REF_UNKNOWN);
      cJSON_Delete(bad);
   }
   cJSON_Delete(destination);
   cJSON_Delete(source);
   cJSON_Delete(current);
   cJSON_Delete(captured);
   return 0;
}
