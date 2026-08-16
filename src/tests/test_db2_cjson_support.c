/* Runtime coverage for DB2's descriptor-owned cJSON input. */
#include "cJSON.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t live_allocations;

static void *counted_malloc(size_t size)
{
   void *value = malloc(size);
   if (value)
      live_allocations++;
   return value;
}

static void counted_free(void *value)
{
   if (value)
   {
      assert(live_allocations > 0);
      live_allocations--;
   }
   free(value);
}

static void test_parse_and_lookup(void)
{
   cJSON *root =
       cJSON_Parse("{\"ready\":true,\"generation\":7,\"providers\":[\"pgvector\",\"qdrant\"]}");
   assert(cJSON_IsObject(root));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ready")));
   assert(cJSON_GetObjectItemCaseSensitive(root, "generation")->valueint == 7);
   cJSON *providers = cJSON_GetObjectItemCaseSensitive(root, "providers");
   assert(cJSON_IsArray(providers));
   assert(cJSON_GetArraySize(providers) == 2);
   assert(strcmp(cJSON_GetArrayItem(providers, 1)->valuestring, "qdrant") == 0);
   cJSON_Delete(root);

   assert(cJSON_Parse("{\"unterminated\":") == NULL);
   const char *end = NULL;
   root = cJSON_ParseWithOpts("{\"ok\":1} trailing", &end, 0);
   assert(cJSON_IsObject(root));
   assert(end && strcmp(end, " trailing") == 0);
   cJSON_Delete(root);
}

static void test_build_print_and_delete(void)
{
   cJSON *root = cJSON_CreateObject();
   assert(root);
   assert(cJSON_AddStringToObject(root, "route", "external"));
   assert(cJSON_AddNumberToObject(root, "generation", 9));
   assert(cJSON_AddBoolToObject(root, "fallback", 0));
   cJSON *candidates = cJSON_AddArrayToObject(root, "candidates");
   assert(candidates);
   assert(cJSON_AddItemToArray(candidates, cJSON_CreateNumber(42)));
   assert(cJSON_AddItemToArray(candidates, cJSON_CreateString("opaque-id")));

   char *encoded = cJSON_PrintUnformatted(root);
   assert(encoded);
   assert(strcmp(encoded, "{\"route\":\"external\",\"generation\":9,\"fallback\":false,"
                          "\"candidates\":[42,\"opaque-id\"]}") == 0);
   cJSON_free(encoded);

   cJSON_DeleteItemFromObjectCaseSensitive(root, "fallback");
   assert(cJSON_GetObjectItemCaseSensitive(root, "fallback") == NULL);
   cJSON *copy = cJSON_Duplicate(root, 1);
   assert(copy && cJSON_Compare(root, copy, 1));
   cJSON_Delete(copy);
   cJSON_Delete(root);
}

static void test_allocator_hooks_balance(void)
{
   cJSON_Hooks hooks = {
       .malloc_fn = counted_malloc,
       .free_fn = counted_free,
   };
   live_allocations = 0;
   cJSON_InitHooks(&hooks);
   cJSON *root = cJSON_Parse("[1,2,3,{\"value\":\"owned\"}]");
   assert(root);
   char *encoded = cJSON_PrintUnformatted(root);
   assert(encoded);
   cJSON_free(encoded);
   cJSON_Delete(root);
   assert(live_allocations == 0);
   cJSON_InitHooks(NULL);
}

int main(void)
{
   test_parse_and_lookup();
   test_build_print_and_delete();
   test_allocator_hooks_balance();
   return 0;
}
