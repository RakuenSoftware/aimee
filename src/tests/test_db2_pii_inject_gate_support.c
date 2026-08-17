/* ABI and behavior parity for DB2's descriptor-owned PII injection gate. */
#include "modules/memory/memory_pii_gate.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

int db2_support_memory_pii_should_inject(int sensitivity, double confidence,
                                         int turn_requests_sensitive);

_Static_assert((int)SENS_NORMAL == 0, "normal sensitivity ABI drifted");
_Static_assert((int)SENS_PII == 1, "PII sensitivity ABI drifted");
_Static_assert((int)SENS_SECRET == 2, "secret sensitivity ABI drifted");
_Static_assert(sizeof(rel_sensitivity_t) == sizeof(int),
               "relationship sensitivity calling convention is no longer int-sized");
_Static_assert(PII_GATE_CONFIDENCE_FLOOR == 0.4, "PII confidence floor drifted");

static int expected(int sensitivity, double confidence, int turn_requests_sensitive)
{
   if (!(confidence >= PII_GATE_CONFIDENCE_FLOOR))
      return 0;
   if (sensitivity == SENS_NORMAL)
      return 1;
   if (sensitivity == SENS_PII)
      return turn_requests_sensitive ? 1 : 0;
   return 0;
}

static void assert_case(int sensitivity, double confidence, int turn_requests_sensitive)
{
   int want = expected(sensitivity, confidence, turn_requests_sensitive);
   int legacy = memory_pii_should_inject((rel_sensitivity_t)sensitivity, confidence,
                                         turn_requests_sensitive);
   int support =
       db2_support_memory_pii_should_inject(sensitivity, confidence, turn_requests_sensitive);
   assert(legacy == want);
   assert(support == want);
}

static void test_complete_signed_16_bit_partition(void)
{
   static const double confidences[] = {
       -INFINITY, -DBL_MAX,           -1.0, -0.0,    0.0,      0.39999999999999997,
       0.4,       0.4000000000000001, 1.0,  DBL_MAX, INFINITY, NAN,
   };
   static const int requests[] = {INT_MIN, -1, 0, 1, 2, INT_MAX};

   for (int sensitivity = INT16_MIN; sensitivity <= INT16_MAX; sensitivity++)
      for (size_t confidence = 0; confidence < sizeof(confidences) / sizeof(confidences[0]);
           confidence++)
         for (size_t request = 0; request < sizeof(requests) / sizeof(requests[0]); request++)
            assert_case(sensitivity, confidences[confidence], requests[request]);
}

static void test_remaining_int_boundaries(void)
{
   static const int sensitivities[] = {INT_MIN, INT16_MIN - 1, INT16_MAX + 1, INT_MAX};
   for (size_t i = 0; i < sizeof(sensitivities) / sizeof(sensitivities[0]); i++)
   {
      assert_case(sensitivities[i], 1.0, 0);
      assert_case(sensitivities[i], 1.0, 1);
   }
}

int main(void)
{
   test_complete_signed_16_bit_partition();
   test_remaining_int_boundaries();
   return 0;
}
