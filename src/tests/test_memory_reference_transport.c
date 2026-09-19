/* The actual retrieval-reference writer preserves the Go record version. */
#include <assert.h>
#include "../modules/db2/c/demotion.c"

static int available = 1;
static const char *version = "2026-09-17T23:45:00Z";
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.runtime") == 0);
   assert(strcmp(jo_cstr(args, "operation"), "record") == 0);
   assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(args, "id")) == 77);
   if (!available)
      return 0;
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", version ? "ok" : "error");
   if (version)
   {
      cJSON *memory = cJSON_AddObjectToObject(*result, "memory");
      cJSON_AddStringToObject(memory, "updated_at", version);
   }
   return 1;
}
int main(void)
{
   cJSON *ref = make_memory_ref(77);
   assert(strcmp(jo_cstr(ref, "type"), "memory") == 0 && jo_i64(ref, "id", 0) == 77);
   assert(strcmp(jo_cstr(ref, "v"), version) == 0);
   cJSON_Delete(ref);
   version = NULL;
   ref = make_memory_ref(77);
   assert(!cJSON_HasObjectItem(ref, "v"));
   cJSON_Delete(ref);
   available = 0;
   ref = make_memory_ref(77);
   assert(!cJSON_HasObjectItem(ref, "v"));
   cJSON_Delete(ref);
   return 0;
}
