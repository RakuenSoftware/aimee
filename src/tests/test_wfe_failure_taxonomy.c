/* test_wfe_failure_taxonomy.c: the Phase-C failure taxonomy (Q4). The classifier is
 * the single place the autonomy run loop's retry/terminal/park decision lives, and
 * its core invariant is NEVER auto-retry without genuinely new input. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wfe_iface.h"

static void test_disposition(void)
{
   /* transient: retry ONLY with new input, else park stuck (no spin) */
   assert(wfe_failure_disposition(WFE_FAIL_TRANSIENT, 1) == WFE_FDISP_RETRY);
   assert(wfe_failure_disposition(WFE_FAIL_TRANSIENT, 0) == WFE_FDISP_PARK_STUCK);

   /* terminal-reject classes */
   assert(wfe_failure_disposition(WFE_FAIL_REFUSAL, 0) == WFE_FDISP_TERMINAL);
   assert(wfe_failure_disposition(WFE_FAIL_REFUSAL, 1) == WFE_FDISP_TERMINAL); /* input ignored */
   assert(wfe_failure_disposition(WFE_FAIL_PERMANENT, 0) == WFE_FDISP_TERMINAL);
   assert(wfe_failure_disposition(WFE_FAIL_CORRUPTION, 0) == WFE_FDISP_TERMINAL);
   assert(wfe_failure_disposition(WFE_FAIL_NONE, 0) == WFE_FDISP_TERMINAL); /* unclassified stops */
   assert(wfe_failure_disposition(WFE_FAIL_NONE, 1) ==
          WFE_FDISP_TERMINAL); /* has_new_input ignored */
   assert(wfe_failure_disposition(WFE_FAIL_PERMANENT, 1) == WFE_FDISP_TERMINAL);

   /* park-for-human classes */
   assert(wfe_failure_disposition(WFE_FAIL_DEGRADED, 0) == WFE_FDISP_PARK_HUMAN);
   assert(wfe_failure_disposition(WFE_FAIL_BUDGET, 0) == WFE_FDISP_PARK_HUMAN);
   assert(wfe_failure_disposition(WFE_FAIL_FORGE, 0) == WFE_FDISP_PARK_HUMAN);
   printf("  PASS: disposition mapping (never-retry-without-new-input)\n");
}

static void test_constructors(void)
{
   /* back-compat: an unclassified failed() is a conservative PERMANENT terminal */
   wfe_step_result_t f = wfe_step_failed();
   assert(f.status == WFE_STEP_FAILED);
   assert(f.failure_class == WFE_FAIL_PERMANENT);
   assert(f.failure_has_new_input == 0);
   assert(wfe_failure_disposition(f.failure_class, f.failure_has_new_input) == WFE_FDISP_TERMINAL);

   /* classified transient with new input -> retry */
   wfe_step_result_t t = wfe_step_failed_class(WFE_FAIL_TRANSIENT, 1);
   assert(t.status == WFE_STEP_FAILED);
   assert(t.failure_class == WFE_FAIL_TRANSIENT);
   assert(t.failure_has_new_input == 1);
   assert(wfe_failure_disposition(t.failure_class, t.failure_has_new_input) == WFE_FDISP_RETRY);

   /* a WFE_FAIL_NONE passed to the classed constructor is normalized to PERMANENT
    * (never a silently-unclassified FAILED that reads as "not a failure") */
   wfe_step_result_t n = wfe_step_failed_class(WFE_FAIL_NONE, 1);
   assert(n.failure_class == WFE_FAIL_PERMANENT);

   /* degraded -> park human */
   wfe_step_result_t d = wfe_step_failed_class(WFE_FAIL_DEGRADED, 0);
   assert(wfe_failure_disposition(d.failure_class, d.failure_has_new_input) ==
          WFE_FDISP_PARK_HUMAN);
   printf("  PASS: constructors + back-compat\n");
}

int main(void)
{
   printf("wfe_failure_taxonomy:\n");
   test_disposition();
   test_constructors();
   printf("ok\n");
   return 0;
}
