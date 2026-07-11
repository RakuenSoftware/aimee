/* Unit tests for the pure self-update helpers (self_update_util.c):
 * version comparison, version-string validation, platform asset name. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cmd_self_update.h"

static void test_version_compare(void)
{
   /* Equality, ignoring a leading 'v' on either side. */
   assert(aimee_version_compare("0.2.182", "0.2.182") == 0);
   assert(aimee_version_compare("v0.2.182", "0.2.182") == 0);
   assert(aimee_version_compare("0.2.182", "v0.2.182") == 0);

   /* Ordering by major, minor, patch. */
   assert(aimee_version_compare("0.2.183", "0.2.182") > 0);
   assert(aimee_version_compare("0.2.182", "0.2.183") < 0);
   assert(aimee_version_compare("0.3.0", "0.2.999") > 0);
   assert(aimee_version_compare("1.0.0", "0.9.9") > 0);
   assert(aimee_version_compare("0.2.9", "0.2.10") < 0); /* numeric, not lexical */

   /* git-describe suffix is ignored (compares by the tag part). */
   assert(aimee_version_compare("v0.2.180-31-g3342b09e", "0.2.180") == 0);
   assert(aimee_version_compare("v0.2.181-1-gabc", "0.2.180") > 0);

   /* Missing components count as zero. */
   assert(aimee_version_compare("0.2", "0.2.0") == 0);
   assert(aimee_version_compare("1", "0.9.9") > 0);

   /* Defensive: NULL/garbage parse to 0.0.0. */
   assert(aimee_version_compare(NULL, "0.0.0") == 0);
   assert(aimee_version_compare("0.0.1", NULL) > 0);
   printf("  test_version_compare ok\n");
}

static void test_version_is_safe(void)
{
   assert(aimee_version_is_safe("0.2.182"));
   assert(aimee_version_is_safe("v0.2.180-31-g3342b09e"));
   assert(aimee_version_is_safe("1.2.3_rc1"));
   assert(!aimee_version_is_safe(""));
   assert(!aimee_version_is_safe(NULL));
   assert(!aimee_version_is_safe("0.2.1; rm -rf /")); /* shell metachars rejected */
   assert(!aimee_version_is_safe("0.2.1$(x)"));
   assert(!aimee_version_is_safe("0.2.1/../etc"));
   printf("  test_version_is_safe ok\n");
}

static void test_asset(void)
{
   /* On the platforms we support, the asset name is non-NULL and starts with
    * the "aimee-" prefix and matches the running OS family. NULL is acceptable
    * (unsupported platform) but on Linux/macOS CI we expect a name. */
   const char *a = aimee_self_update_asset();
   if (a)
   {
      assert(strncmp(a, "aimee-", 6) == 0);
      assert(strstr(a, "linux") || strstr(a, "macos") || strstr(a, "windows"));
   }
   printf("  test_asset ok (%s)\n", a ? a : "(unsupported platform -> NULL)");
}

int main(void)
{
   test_version_compare();
   test_version_is_safe();
   test_asset();
   printf("test_self_update: all passed\n");
   return 0;
}
