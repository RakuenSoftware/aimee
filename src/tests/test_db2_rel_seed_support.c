/* Full ABI and behavior parity for DB2's generated relationship seed. */
#define rel_types_seed_count  db2_support_rel_types_seed_count
#define rel_types_seed_at     db2_support_rel_types_seed_at
#define rel_types_seed_lookup db2_support_rel_types_seed_lookup
#include "../modules/db2/support/db2_rel_seed.h"
#undef rel_types_seed_count
#undef rel_types_seed_at
#undef rel_types_seed_lookup

#include "rel_types.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

void db2_support_rel_type_normalize(const char *in, char *out, size_t out_len);

_Static_assert(DB2_REL_TYPE_NAME_MAX == REL_TYPE_NAME_MAX, "relation name ABI drifted");
_Static_assert(DB2_REL_TYPE_MAX_KINDS == REL_TYPE_MAX_KINDS, "kind array ABI drifted");
_Static_assert(sizeof(memory_node_kind_t) == sizeof(int), "node kind is no longer int-sized");
_Static_assert(sizeof(correction_behavior_t) == sizeof(int),
               "correction behavior is no longer int-sized");
_Static_assert(sizeof(rel_sensitivity_t) == sizeof(int), "sensitivity is no longer int-sized");
_Static_assert(sizeof(rel_status_t) == sizeof(int), "relation status is no longer int-sized");
_Static_assert(sizeof(db2_rel_seed_def_t) == sizeof(rel_type_def_t),
               "relationship seed row size drifted");

#define ASSERT_FIELD_OFFSET(field)                                                                 \
   _Static_assert(offsetof(db2_rel_seed_def_t, field) == offsetof(rel_type_def_t, field),          \
                  #field " offset drifted")
ASSERT_FIELD_OFFSET(rel_type);
ASSERT_FIELD_OFFSET(head_kinds);
ASSERT_FIELD_OFFSET(head_kind_count);
ASSERT_FIELD_OFFSET(tail_kinds);
ASSERT_FIELD_OFFSET(tail_kind_count);
ASSERT_FIELD_OFFSET(is_symmetric);
ASSERT_FIELD_OFFSET(inverse_rel_type);
ASSERT_FIELD_OFFSET(correction_behavior);
ASSERT_FIELD_OFFSET(category);
ASSERT_FIELD_OFFSET(sensitivity);
ASSERT_FIELD_OFFSET(is_hierarchy_rel);
ASSERT_FIELD_OFFSET(status);
#undef ASSERT_FIELD_OFFSET

static int nullable_string_equal(const char *left, const char *right)
{
   return (!left && !right) || (left && right && strcmp(left, right) == 0);
}

static void assert_row_equal(const rel_type_def_t *legacy, const db2_rel_seed_def_t *support)
{
   assert(legacy != NULL);
   assert(support != NULL);
   assert(nullable_string_equal(legacy->rel_type, support->rel_type));
   for (int i = 0; i < REL_TYPE_MAX_KINDS; i++)
   {
      assert((int)legacy->head_kinds[i] == support->head_kinds[i]);
      assert((int)legacy->tail_kinds[i] == support->tail_kinds[i]);
   }
   assert(legacy->head_kind_count == support->head_kind_count);
   assert(legacy->tail_kind_count == support->tail_kind_count);
   assert(legacy->is_symmetric == support->is_symmetric);
   assert(nullable_string_equal(legacy->inverse_rel_type, support->inverse_rel_type));
   assert((int)legacy->correction_behavior == support->correction_behavior);
   assert(nullable_string_equal(legacy->category, support->category));
   assert((int)legacy->sensitivity == support->sensitivity);
   assert(legacy->is_hierarchy_rel == support->is_hierarchy_rel);
   assert((int)legacy->status == support->status);
}

static void test_complete_seed_parity(void)
{
   int count = rel_types_seed_count();
   assert(count > 0);
   assert(db2_support_rel_types_seed_count() == count);
   for (int i = 0; i < count; i++)
   {
      const rel_type_def_t *legacy = rel_types_seed_at(i);
      const db2_rel_seed_def_t *support = db2_support_rel_types_seed_at(i);
      assert_row_equal(legacy, support);
      assert(rel_types_seed_lookup(legacy->rel_type) == legacy);
      assert(db2_support_rel_types_seed_lookup(support->rel_type) == support);
   }
}

static void test_iteration_bounds(void)
{
   int count = rel_types_seed_count();
   static const int invalid[] = {INT_MIN, -2, -1, INT_MAX};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
   {
      assert(rel_types_seed_at(invalid[i]) == NULL);
      assert(db2_support_rel_types_seed_at(invalid[i]) == NULL);
   }
   assert(rel_types_seed_at(count) == NULL);
   assert(db2_support_rel_types_seed_at(count) == NULL);
   assert(rel_types_seed_at(count + 1) == NULL);
   assert(db2_support_rel_types_seed_at(count + 1) == NULL);
}

static void test_lookup_misses_and_normalization(void)
{
   static const char *const corpus[] = {
       NULL, "", "Works For", "worksFor", "DEVICE-HAS-IP", "definitely_not_a_seed_relation",
   };
   for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++)
   {
      char legacy_norm[REL_TYPE_NAME_MAX];
      char support_norm[DB2_REL_TYPE_NAME_MAX];
      rel_type_normalize(corpus[i], legacy_norm, sizeof(legacy_norm));
      db2_support_rel_type_normalize(corpus[i], support_norm, sizeof(support_norm));
      assert(strcmp(legacy_norm, support_norm) == 0);
      const rel_type_def_t *legacy = rel_types_seed_lookup(corpus[i]);
      const db2_rel_seed_def_t *support = db2_support_rel_types_seed_lookup(corpus[i]);
      assert((legacy == NULL) == (support == NULL));
      if (legacy)
         assert_row_equal(legacy, support);
   }

   char overlong[DB2_REL_TYPE_NAME_MAX + 64];
   memset(overlong, 'x', sizeof(overlong) - 1);
   overlong[sizeof(overlong) - 1] = '\0';
   assert(strlen(overlong) > DB2_REL_TYPE_NAME_MAX);
   assert(rel_types_seed_lookup(overlong) == NULL);
   assert(db2_support_rel_types_seed_lookup(overlong) == NULL);
}

int main(void)
{
   test_complete_seed_parity();
   test_iteration_bounds();
   test_lookup_misses_and_normalization();
   return 0;
}
