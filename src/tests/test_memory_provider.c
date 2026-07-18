/* test_memory_provider.c: unit tests for the exclusive-kind memory provider registry */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "memory_provider.h"

int main(void)
{
   printf("memory_provider: ");

   /* ---------------------------------------------------------------
    * 1. Empty registry: get_active returns NULL
    * ------------------------------------------------------------- */
   {
      memory_provider_reset();
      assert(memory_provider_get_active() == NULL);
      assert(memory_provider_get_bundled() == NULL);
      assert(memory_provider_get_external() == NULL);
      printf("1");
   }

   /* ---------------------------------------------------------------
    * 2. Bundled provider registers successfully
    * ------------------------------------------------------------- */
   {
      memory_provider_reset();
      memory_provider_t p = {0};
      p.name = "aimee-kb";
      p.is_bundled = 1;

      int rc = memory_provider_register(&p);
      assert(rc == 0);
      assert(memory_provider_get_bundled() != NULL);
      assert(strcmp(memory_provider_get_bundled()->name, "aimee-kb") == 0);
      assert(memory_provider_get_external() == NULL);
      /* active == bundled when no external is set */
      assert(memory_provider_get_active() == memory_provider_get_bundled());
      printf("2");
   }

   /* ---------------------------------------------------------------
    * 3. First external provider registers successfully
    * ------------------------------------------------------------- */
   {
      memory_provider_reset();

      memory_provider_t bundled = {0};
      bundled.name = "aimee-kb";
      bundled.is_bundled = 1;
      assert(memory_provider_register(&bundled) == 0);

      memory_provider_t ext = {0};
      ext.name = "neo4j-provider";
      ext.is_bundled = 0;

      int rc = memory_provider_register(&ext);
      assert(rc == 0);
      assert(memory_provider_get_external() != NULL);
      assert(strcmp(memory_provider_get_external()->name, "neo4j-provider") == 0);
      /* active == external when external is set */
      assert(memory_provider_get_active() == memory_provider_get_external());
      printf("3");
   }

   /* ---------------------------------------------------------------
    * 4. Second external provider is rejected (WARN logged, return -1)
    *    Bundled + one external is the maximum.
    * ------------------------------------------------------------- */
   {
      memory_provider_reset();

      memory_provider_t bundled = {0};
      bundled.name = "aimee-kb";
      bundled.is_bundled = 1;
      assert(memory_provider_register(&bundled) == 0);

      memory_provider_t ext1 = {0};
      ext1.name = "first-external";
      ext1.is_bundled = 0;
      assert(memory_provider_register(&ext1) == 0);

      memory_provider_t ext2 = {0};
      ext2.name = "second-external";
      ext2.is_bundled = 0;
      int rc = memory_provider_register(&ext2);
      assert(rc == -1); /* rejected */

      /* The first external is still active; the second was not stored */
      assert(strcmp(memory_provider_get_external()->name, "first-external") == 0);
      printf("4");
   }

   /* ---------------------------------------------------------------
    * 5. NULL / unnamed provider rejected
    * ------------------------------------------------------------- */
   {
      memory_provider_reset();
      assert(memory_provider_register(NULL) == -1);

      memory_provider_t unnamed = {0};
      unnamed.name = "";
      unnamed.is_bundled = 0;
      assert(memory_provider_register(&unnamed) == -1);
      printf("5");
   }

   /* ---------------------------------------------------------------
    * 6. Bundled can be re-registered (replace the built-in stub)
    * ------------------------------------------------------------- */
   {
      memory_provider_reset();

      memory_provider_t b1 = {0};
      b1.name = "bundled-v1";
      b1.is_bundled = 1;
      assert(memory_provider_register(&b1) == 0);
      assert(strcmp(memory_provider_get_bundled()->name, "bundled-v1") == 0);

      memory_provider_t b2 = {0};
      b2.name = "bundled-v2";
      b2.is_bundled = 1;
      assert(memory_provider_register(&b2) == 0);
      assert(strcmp(memory_provider_get_bundled()->name, "bundled-v2") == 0);
      printf("6");
   }

   printf(" OK\n");
   return 0;
}
