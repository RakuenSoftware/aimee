/* ABI and behavior parity for descriptor-owned DB2 relationship enum text. */
#include "rel_types.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

const char *db2_support_correction_behavior_to_text(int behavior);
const char *db2_support_rel_sensitivity_to_text(int sensitivity);

_Static_assert((int)CORR_SUPERSEDE == 0, "supersede ABI drifted");
_Static_assert((int)CORR_HARD_DELETE == 1, "hard-delete ABI drifted");
_Static_assert((int)CORR_IMMUTABLE == 2, "immutable ABI drifted");
_Static_assert((int)SENS_NORMAL == 0, "normal sensitivity ABI drifted");
_Static_assert((int)SENS_PII == 1, "PII sensitivity ABI drifted");
_Static_assert((int)SENS_SECRET == 2, "secret sensitivity ABI drifted");
_Static_assert(sizeof(correction_behavior_t) == sizeof(int),
               "correction behavior calling convention is no longer int-sized");
_Static_assert(sizeof(rel_sensitivity_t) == sizeof(int),
               "relationship sensitivity calling convention is no longer int-sized");

static void assert_correction(int value, const char *expected)
{
   const char *legacy = correction_behavior_to_text((correction_behavior_t)value);
   const char *support = db2_support_correction_behavior_to_text(value);
   assert(strcmp(legacy, expected) == 0);
   assert(strcmp(support, expected) == 0);
}

static void assert_sensitivity(int value, const char *expected)
{
   const char *legacy = rel_sensitivity_to_text((rel_sensitivity_t)value);
   const char *support = db2_support_rel_sensitivity_to_text(value);
   assert(strcmp(legacy, expected) == 0);
   assert(strcmp(support, expected) == 0);
}

static void test_complete_signed_16_bit_partition(void)
{
   for (int value = INT16_MIN; value <= INT16_MAX; value++)
   {
      const char *correction = value == CORR_HARD_DELETE ? "hard_delete"
                               : value == CORR_IMMUTABLE ? "immutable"
                                                         : "supersede";
      const char *sensitivity = value == SENS_NORMAL   ? "normal"
                                : value == SENS_SECRET ? "secret"
                                                       : "pii";
      assert_correction(value, correction);
      assert_sensitivity(value, sensitivity);
   }
}

static void test_remaining_int_boundaries(void)
{
   static const int values[] = {INT_MIN, INT16_MIN - 1, INT16_MAX + 1, INT_MAX};
   for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
   {
      assert_correction(values[i], "supersede");
      assert_sensitivity(values[i], "pii");
   }
}

int main(void)
{
   test_complete_signed_16_bit_partition();
   test_remaining_int_boundaries();
   return 0;
}
