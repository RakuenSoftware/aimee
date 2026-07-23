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

   /* Ambiguous codes, resolved toward INFRA_ERROR on purpose. Through
    * `/bin/sh -c` these can be the shell reporting it never ran the command, OR
    * a verifier returning them itself - indistinguishable at this layer. The two
    * mistakes are not symmetric: reading a real failure as infrastructure only
    * skips an escalation and defers to a human, while reading an OOM kill or a
    * missing binary as a work-product failure blames the model for its
    * environment and burns a dearer seat for nothing. */
   assert(verify_classify(126) == VERIFY_OUTCOME_INFRA_ERROR); /* not executable */
   assert(verify_classify(127) == VERIFY_OUTCOME_INFRA_ERROR); /* not found */
   assert(verify_classify(124) == VERIFY_OUTCOME_INFRA_ERROR); /* coreutils timeout */
   assert(verify_classify(137) == VERIFY_OUTCOME_INFRA_ERROR); /* 128+9  SIGKILL/OOM */
   assert(verify_classify(143) == VERIFY_OUTCOME_INFRA_ERROR); /* 128+15 SIGTERM */

   /* Boundaries of the signal band. 128 is not 128+N for any positive N, and
    * above SIGRTMAX (64) a code cannot plausibly be signal-derived - a verifier
    * documenting 200 as a work-product failure must still be able to trigger an
    * escalation, so a blanket "anything over 128 is infrastructure" was too
    * broad in the opposite direction. */
   assert(verify_classify(128) == VERIFY_OUTCOME_FAILED);
   assert(verify_classify(129) == VERIFY_OUTCOME_INFRA_ERROR); /* 128+1  SIGHUP */
   assert(verify_classify(192) == VERIFY_OUTCOME_INFRA_ERROR); /* 128+64 SIGRTMAX */
   assert(verify_classify(193) == VERIFY_OUTCOME_FAILED);      /* beyond any signal */
   assert(verify_classify(200) == VERIFY_OUTCOME_FAILED);      /* a real failure code */
   assert(verify_classify(255) == VERIFY_OUTCOME_FAILED);

   /* ...and a documented 200 must still be able to warrant escalation. */
   assert(verify_escalation_warranted(0, verify_classify(200), 0) == 1);

   /* An OOM-killed test suite must NOT warrant escalation - that would blame the
    * model for the machine running out of memory. */
   assert(verify_escalation_warranted(0, verify_classify(137), 0) == 0);
   assert(verify_escalation_warranted(0, verify_classify(124), 0) == 0);

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
