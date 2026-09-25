/* test_kb_client_cache.c: unit tests for aimee-server's kb result cache. */
#include "kb_client_cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
   printf("kb_client_cache: ");

   /* --- disabled by default (ttl 0): get/put are no-ops --- */
   kb_cache_configure(0);
   assert(!kb_cache_enabled());
   kb_cache_put("k", "v");
   assert(kb_cache_get("k") == NULL);

   /* --- enabled: miss → put → hit → invalidate → miss --- */
   kb_cache_configure(60);
   assert(kb_cache_enabled());
   assert(kb_cache_get("k1") == NULL); /* miss */
   kb_cache_put("k1", "{\"hits\":[]}");
   char *v = kb_cache_get("k1");
   assert(v && strcmp(v, "{\"hits\":[]}") == 0);
   free(v);

   /* overwrite same key */
   kb_cache_put("k1", "v1b");
   v = kb_cache_get("k1");
   assert(v && strcmp(v, "v1b") == 0);
   free(v);

   kb_cache_invalidate_all();
   assert(kb_cache_get("k1") == NULL); /* flushed */

   /* A fetch that started before erasure cannot repopulate the cache. */
   uint64_t old_generation = kb_cache_observe();
   kb_cache_invalidate_all();
   kb_cache_put_observed("late-private-copy", "erased payload", old_generation);
   assert(kb_cache_get("late-private-copy") == NULL);
   kb_cache_put_observed("fresh-copy", "current payload", kb_cache_observe());
   v = kb_cache_get("fresh-copy");
   assert(v && strcmp(v, "current payload") == 0);
   free(v);

   /* --- LRU eviction past capacity (256): oldest key gone, newest present --- */
   char key[32];
   for (int i = 0; i < 300; i++)
   {
      snprintf(key, sizeof(key), "e%d", i);
      kb_cache_put(key, "x");
   }
   assert(kb_cache_get("e0") == NULL); /* evicted */
   v = kb_cache_get("e299");
   assert(v != NULL); /* newest retained */
   free(v);

   /* --- stats reflect activity --- */
   long hits = 0, misses = 0, inval = 0;
   kb_cache_stats(&hits, &misses, &inval);
   assert(hits > 0 && misses > 0 && inval >= 1);

   /* --- disabling clears the gate --- */
   kb_cache_configure(0);
   kb_cache_put("z", "z");
   assert(kb_cache_get("z") == NULL);

   printf("all tests passed.\n");
   return 0;
}
