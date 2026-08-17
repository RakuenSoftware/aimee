/* ABI and behavior parity for descriptor-owned DB2 node-kind text. */
#include "modules/memory/memory_ontology.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

const char *db2_support_memory_ontology_node_kind_to_text(int kind);

_Static_assert((int)NODE_FILE == 0, "file node ABI drifted");
_Static_assert((int)NODE_FUNCTION == 1, "function node ABI drifted");
_Static_assert((int)NODE_STRUCT == 2, "struct node ABI drifted");
_Static_assert((int)NODE_MODULE == 3, "module node ABI drifted");
_Static_assert((int)NODE_BUG == 4, "bug node ABI drifted");
_Static_assert((int)NODE_COMMIT == 5, "commit node ABI drifted");
_Static_assert((int)NODE_PR == 6, "PR node ABI drifted");
_Static_assert((int)NODE_DEVELOPER == 7, "developer node ABI drifted");
_Static_assert((int)NODE_CONCEPT == 8, "concept node ABI drifted");
_Static_assert((int)NODE_EVENT == 9, "event node ABI drifted");
_Static_assert((int)NODE_PERSON == 10, "person node ABI drifted");
_Static_assert((int)NODE_PLACE == 11, "place node ABI drifted");
_Static_assert((int)NODE_TIME_EXPR == 12, "time-expression node ABI drifted");
_Static_assert((int)NODE_DEVICE == 13, "device node ABI drifted");
_Static_assert((int)NODE_ORG == 14, "organization node ABI drifted");
_Static_assert((int)NODE_IP == 15, "IP node ABI drifted");
_Static_assert((int)NODE_SCALAR == 16, "scalar node ABI drifted");
_Static_assert((int)NODE_OTHER == 99, "other node ABI drifted");
_Static_assert(sizeof(memory_node_kind_t) == sizeof(int),
               "node kind calling convention is no longer int-sized");

static void assert_node_kind(int value, const char *expected)
{
   const char *legacy = memory_ontology_node_kind_to_text((memory_node_kind_t)value);
   const char *support = db2_support_memory_ontology_node_kind_to_text(value);
   assert(strcmp(legacy, expected) == 0);
   assert(strcmp(support, expected) == 0);
}

static void test_complete_signed_16_bit_partition(void)
{
   static const char *const node_text[] = {
       "file",  "function", "struct", "module",    "bug",    "commit", "pr", "developer", "concept",
       "event", "person",   "place",  "time_expr", "device", "org",    "ip", "scalar",
   };
   for (int value = INT16_MIN; value <= INT16_MAX; value++)
   {
      const char *expected = value >= 0 && value < (int)(sizeof(node_text) / sizeof(node_text[0]))
                                 ? node_text[value]
                                 : "other";
      assert_node_kind(value, expected);
   }
}

static void test_remaining_int_boundaries(void)
{
   static const int values[] = {INT_MIN, INT16_MIN - 1, INT16_MAX + 1, INT_MAX};
   for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
      assert_node_kind(values[i], "other");
}

int main(void)
{
   test_complete_signed_16_bit_partition();
   test_remaining_int_boundaries();
   return 0;
}
