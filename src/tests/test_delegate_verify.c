/* test_delegate_verify.c: verification classification + escalation policy.
 *
 * The property under test is the one the design review called a blocker: an
 * escalation asserts "this model was not good enough", so it may only fire on an
 * attributable, VERIFIED work-product failure. Anything that merely means the
 * verifier could not run must not indict the model. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "delegate_verify.h"

static void test_classification(void)
{
   assert(verify_classify(0) == VERIFY_OUTCOME_PASS);

   /* A verifier that RAN and reported failure: attributable to the work product. */
   assert(verify_classify(1) == VERIFY_OUTCOME_FAILED);
   assert(verify_classify(2) == VERIFY_OUTCOME_FAILED);
   assert(verify_classify(3) == VERIFY_OUTCOME_FAILED);

   /* Could not spawn, or the child died by signal - safe_exec_capture returns -1
    * for both. A timeout kill lands here and must NOT read as a failed build. */
   assert(verify_classify(-1) == VERIFY_OUTCOME_INFRA_ERROR);

   /* The shell's own report that it never executed the verifier. A missing test
    * runner in a delegate's environment is a setup defect, not a failing test. */
   assert(verify_classify(126) == VERIFY_OUTCOME_INFRA_ERROR);
   assert(verify_classify(127) == VERIFY_OUTCOME_INFRA_ERROR);

   assert(strcmp(verify_outcome_name(VERIFY_OUTCOME_PASS), "pass") == 0);
   assert(strcmp(verify_outcome_name(VERIFY_OUTCOME_FAILED), "failed") == 0);
   assert(strcmp(verify_outcome_name(VERIFY_OUTCOME_INFRA_ERROR), "infra_error") == 0);

   printf("  PASS: test_classification\n");
}

static void test_escalation_policy(void)
{
   /* The ONLY case that warrants escalation: the delegate finished, and a
    * verifier genuinely ran and failed. */
   assert(verify_escalation_warranted(0, VERIFY_OUTCOME_FAILED, 0) == 1);

   /* A passing verifier obviously does not. Nor does the no-verifier case: the
    * caller passes PASS when none is configured, because with no objective signal
    * the right answer is "fail for review", never "guess from delegate prose". */
   assert(verify_escalation_warranted(0, VERIFY_OUTCOME_PASS, 0) == 0);

   /* An unusable verifier says NOTHING about the model. Escalating here would
    * burn a dearer seat because a binary was missing. */
   assert(verify_escalation_warranted(0, VERIFY_OUTCOME_INFRA_ERROR, 0) == 0);

   /* The delegate run itself failing is an AVAILABILITY problem - API error,
    * transport failure, crash, timeout - which belongs to retry/failover. The
    * model is not to blame, so competence escalation must not fire. */
   assert(verify_escalation_warranted(1, VERIFY_OUTCOME_FAILED, 0) == 0);
   assert(verify_escalation_warranted(-1, VERIFY_OUTCOME_FAILED, 0) == 0);

   /* The allowance is spent once. This is what keeps a ladder from forming:
    * cheap -> capable -> more capable across three delegates is exactly the
    * pattern the operator rejected as the fundamental mechanism. */
   assert(verify_escalation_warranted(0, VERIFY_OUTCOME_FAILED, 1) == 0);

   printf("  PASS: test_escalation_policy\n");
}

int main(void)
{
   printf("delegate_verify:\n");
   test_classification();
   test_escalation_policy();
   printf("delegate_verify: all tests passed\n");
   return 0;
}
