/* ABI and behavior parity for descriptor-owned DB2 management-read support. */
#include "management_read.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

const char *db2_support_server_mgmt_read_selector_name(int selector);

_Static_assert((int)SERVER_MGMT_READ_SELECTOR_AGENTS == 1, "agents selector ABI drifted");
_Static_assert((int)SERVER_MGMT_READ_SELECTOR_CONFIG == 2, "config selector ABI drifted");
_Static_assert(sizeof(server_mgmt_read_selector_t) == sizeof(int),
               "legacy selector calling convention is no longer int-sized");

static void assert_result(int selector, const char *expected)
{
   const char *legacy = server_mgmt_read_selector_name((server_mgmt_read_selector_t)selector);
   const char *support = db2_support_server_mgmt_read_selector_name(selector);

   if (!expected)
   {
      assert(legacy == NULL);
      assert(support == NULL);
      return;
   }
   assert(legacy != NULL);
   assert(support != NULL);
   assert(strcmp(legacy, expected) == 0);
   assert(strcmp(support, expected) == 0);
}

static void test_complete_selector_partition(void)
{
   /* A dense signed 16-bit subdomain exercises every selector equivalence class. */
   for (int selector = INT16_MIN; selector <= INT16_MAX; selector++)
   {
      const char *expected = selector == SERVER_MGMT_READ_SELECTOR_AGENTS   ? "agents"
                             : selector == SERVER_MGMT_READ_SELECTOR_CONFIG ? "config"
                                                                            : NULL;
      assert_result(selector, expected);
   }

   /* Exercise the remaining int-width boundary classes. */
   assert_result(INT_MIN, NULL);
   assert_result(INT16_MIN - 1, NULL);
   assert_result(INT16_MAX + 1, NULL);
   assert_result(INT_MAX, NULL);
}

int main(void)
{
   test_complete_selector_partition();
   return 0;
}
