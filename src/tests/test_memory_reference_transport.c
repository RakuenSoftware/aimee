/* The actual retrieval-reference writer preserves the Go record version. */
#include <assert.h>
#include "json_fluent.h"
#include "../modules/kb/c/demotion.c"

static int available = 1;
static const char *version = "2026-09-17T23:45:00Z";
static const char *source_version;
static const char *state_version = "state-a";
static int64_t expected_id = 77;
static cJSON *opaque_reference(const char *scheme, const char *token)
{
   cJSON *value = cJSON_CreateObject();
   char id[32];
   snprintf(id, sizeof(id), "%" PRId64, expected_id);
   cJSON_AddNumberToObject(value, "schema_version", 1);
   cJSON_AddStringToObject(value, "scheme", scheme);
   cJSON_AddStringToObject(value, "owner", "fixture-owner");
   cJSON_AddStringToObject(value, "record", id);
   cJSON_AddStringToObject(value, "version", token);
   return value;
}
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.runtime") == 0);
   assert(strcmp(jo_cstr(args, "operation"), "record") == 0);
   int64_t id = 0;
   assert(jo_read_i64_exact(cJSON_GetObjectItemCaseSensitive(args, "id"), &id));
   assert(id == expected_id);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "include_version")));
   if (!available)
      return 0;
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", version || source_version ? "ok" : "error");
   if (version || source_version)
   {
      cJSON *memory = cJSON_AddObjectToObject(*result, "memory");
      if (version)
         cJSON_AddStringToObject(memory, "updated_at", version);
      if (source_version)
      {
         cJSON_AddItemToObject(memory, "source_ref",
                               opaque_reference("fixture-record/v1", source_version));
         cJSON_AddItemToObject(memory, "state_ref",
                               opaque_reference("fixture-state/v1", state_version));
      }
   }
   return 1;
}
static size_t allocation, fail_allocation;
static void *failing_allocate(size_t bytes)
{
   if (++allocation == fail_allocation)
      return NULL;
   return malloc(bytes);
}
static void projection_allocation_failures(void)
{
   int completed = 0;
   for (size_t fail = 1; fail < 128; ++fail)
   {
      cJSON *payload = cJSON_CreateObject();
      cJSON *refs = cJSON_AddArrayToObject(payload, "surfaced_refs");
      cJSON *ref = cJSON_CreateObject();
      cJSON_AddStringToObject(ref, "type", "memory");
      cJSON_AddNumberToObject(ref, "id", 77);
      cJSON_AddStringToObject(ref, "v", "captured");
      expected_id = 77;
      cJSON_AddItemToObject(ref, "source_ref", opaque_reference("fixture-record/v1", "source-a"));
      cJSON_AddItemToObject(ref, "state_ref", opaque_reference("fixture-state/v1", "state-a"));
      cJSON_AddStringToObject(ref, "reference_origin", "observed_lookup");
      cJSON_AddItemToArray(refs, ref);
      cJSON *old_ids = cJSON_AddArrayToObject(payload, "surfaced_ids");
      cJSON *old_items = cJSON_AddArrayToObject(payload, "surfaced_items");
      cJSON_AddItemToArray(old_ids, cJSON_CreateNumber(12));
      allocation = 0;
      fail_allocation = fail;
      cJSON_Hooks hooks = {.malloc_fn = failing_allocate, .free_fn = free};
      cJSON_InitHooks(&hooks);
      int result = project_memory_refs(payload);
      cJSON_InitHooks(NULL);
      if (!result)
      {
         assert(cJSON_GetObjectItemCaseSensitive(payload, "surfaced_ids") == old_ids);
         assert(cJSON_GetObjectItemCaseSensitive(payload, "surfaced_items") == old_items);
         assert(cJSON_GetArraySize(old_ids) == 1 && cJSON_GetArraySize(old_items) == 0);
      }
      else
      {
         assert(allocation < fail);
         assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(payload, "surfaced_items")) ==
                1);
         completed = 1;
      }
      cJSON_Delete(payload);
      if (completed)
         break;
   }
   assert(completed);
}
int main(void)
{
   projection_allocation_failures();
   cJSON *ref = make_memory_ref(77);
   assert(strcmp(jo_cstr(ref, "type"), "memory") == 0 && jo_i64(ref, "id", 0) == 77);
   assert(strcmp(jo_cstr(ref, "v"), version) == 0);
   cJSON_Delete(ref);
   source_version = "source-a";
   ref = make_memory_ref(77);
   assert(strcmp(jo_cstr(ref, "v"), version) == 0);
   assert(module_source_ref_valid(cJSON_GetObjectItemCaseSensitive(ref, "source_ref"), 77));
   cJSON *payload = cJSON_CreateObject();
   cJSON *refs = cJSON_AddArrayToObject(payload, "surfaced_refs");
   cJSON_AddItemToArray(refs, ref);
   assert(project_memory_refs(payload));
   cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(payload, "surfaced_items"), 0);
   assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(item, "source_ref"),
                        cJSON_GetObjectItemCaseSensitive(ref, "source_ref"), 1));
   assert(strcmp(jo_cstr(item, "v"), version) == 0);
   assert(strcmp(jo_cstr(item, "reference_origin"), "observed_lookup") == 0);
   cJSON_DeleteItemFromObjectCaseSensitive(payload, "surfaced_refs");
   source_version = "source-b"; /* migration must preserve the earlier capture */
   refs = ensure_surfaced_refs(payload);
   assert(refs && cJSON_GetArraySize(refs) == 1);
   ref = cJSON_GetArrayItem(refs, 0);
   assert(strcmp(jo_cstr(cJSON_GetObjectItemCaseSensitive(ref, "source_ref"), "version"),
                 "source-a") == 0);
   cJSON_Delete(payload);
   version = NULL;
   ref = make_memory_ref(77);
   assert(!cJSON_HasObjectItem(ref, "v"));
   assert(module_source_ref_valid(cJSON_GetObjectItemCaseSensitive(ref, "source_ref"), 77));
   cJSON_Delete(ref);
   expected_id = INT64_C(9007199254740993);
   ref = make_memory_ref(expected_id);
   int64_t exact_id;
   assert(jo_read_i64_exact(cJSON_GetObjectItemCaseSensitive(ref, "id"), &exact_id));
   assert(exact_id == expected_id);
   assert(
       module_source_ref_valid(cJSON_GetObjectItemCaseSensitive(ref, "source_ref"), expected_id));
   cJSON_Delete(ref);
   source_version = NULL;
   ref = make_memory_ref(expected_id);
   assert(!cJSON_HasObjectItem(ref, "source_ref"));
   cJSON_Delete(ref);
   available = 0;
   expected_id = 77;
   ref = make_memory_ref(77);
   assert(!cJSON_HasObjectItem(ref, "v"));
   cJSON_Delete(ref);
   return 0;
}
