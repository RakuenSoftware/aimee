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

/* relations.schema_list publishes memory_ontology_rules(). It previously read
 * `memory_relation_schema`, a table nothing in the tree writes, so it served an
 * empty list while memory_ontology_validate() enforced a different, static
 * table. These assertions tie the published set to the enforced one: if a rule
 * is ever advertised that the validator would reject, the surface is lying
 * about what the system does, and that is the failure this catches. */
static void test_published_rules_are_the_enforced_rules(void)
{
   const memory_ontology_rule_t *rules = NULL;
   int n = memory_ontology_rules(&rules);

   /* An empty list is the exact symptom of the defect being fixed here: it is
    * what the DB-backed reader returned on every deployment. */
   assert(n > 0);
   assert(rules != NULL);

   for (int i = 0; i < n; i++)
   {
      /* Every advertised triple must actually pass the validator. */
      assert(memory_ontology_validate(rules[i].sk, rules[i].rel, rules[i].ok) == 1);

      /* The sentinel terminates the table and is not a rule; publishing it
       * would advertise (other, other, other), which reads as "anything
       * goes" for a relation that is merely experimental. */
      assert(
          !(rules[i].sk == NODE_OTHER && rules[i].rel == REL_OTHER && rules[i].ok == NODE_OTHER));

      /* The surface labels each code; an unmapped code would publish "other"
       * for a kind that is not NODE_OTHER, which misnames the rule. */
      assert(memory_ontology_node_kind_to_text(rules[i].sk) != NULL);
      assert(memory_ontology_relation_to_text(rules[i].rel) != NULL);
   }
}

int main(void)
{
   test_complete_signed_16_bit_partition();
   test_remaining_int_boundaries();
   test_published_rules_are_the_enforced_rules();
   return 0;
}
