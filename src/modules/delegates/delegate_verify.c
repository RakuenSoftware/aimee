/* delegate_verify.c: verification outcome classification and escalation policy. */
#include "delegate_verify.h"

verify_outcome_t verify_classify(int exec_rc)
{
   if (exec_rc == 0)
      return VERIFY_OUTCOME_PASS;
   /* safe_exec_capture returns -1 when it could not fork/exec, or when the CHILD
    * SHELL did not exit normally. */
   if (exec_rc < 0)
      return VERIFY_OUTCOME_INFRA_ERROR;

   /* HEURISTIC, and deliberately biased toward NOT escalating.
    *
    * Running the verifier through `/bin/sh -c` flattens everything into one
    * integer, and these codes are genuinely AMBIGUOUS: a verifier may itself
    * exit 126, 127 or 137 on purpose. We cannot distinguish that from the shell
    * reporting it never ran the command, or from the command being killed,
    * without a richer verifier protocol.
    *
    * So the tie is broken toward INFRA_ERROR. Misreading a real test failure as
    * infrastructure means we skip an escalation and hand the work to a human -
    * conservative. Misreading an OOM kill or a missing binary as a work-product
    * failure would blame the model for the environment and burn a dearer seat
    * for nothing. Only the first is acceptable.
    *
    *   126/127  shell's "not executable" / "not found"
    *   124      GNU coreutils `timeout` reporting expiry
    *   128+N    a command killed by signal N (137 = SIGKILL/OOM, 143 = SIGTERM)
    */
   /* 128+N only for a PLAUSIBLE signal number. Treating everything above 128 as
    * infrastructure was too broad: a verifier may document 200 as a work-product
    * failure, and suppressing escalation for it is exactly the mistake this
    * classification exists to avoid, just in the other direction. Linux signals
    * run to SIGRTMAX (64), so 129..192 is the credible band. */
   if (exec_rc == 126 || exec_rc == 127 || exec_rc == 124 || (exec_rc >= 129 && exec_rc <= 192))
      return VERIFY_OUTCOME_INFRA_ERROR;
   return VERIFY_OUTCOME_FAILED;
}

const char *verify_outcome_name(verify_outcome_t o)
{
   switch (o)
   {
   case VERIFY_OUTCOME_PASS:
      return "pass";
   case VERIFY_OUTCOME_FAILED:
      return "failed";
   case VERIFY_OUTCOME_INFRA_ERROR:
      return "infra_error";
   default:
      return "unknown";
   }
}

int verify_escalation_warranted(int delegate_rc, verify_outcome_t outcome, int already_escalated)
{
   if (already_escalated)
      return 0;
   /* The delegate must have finished. A failed run is an availability problem for
    * retry/failover to handle; escalating would blame the model for a transport
    * or process failure it did not cause. */
   if (delegate_rc != 0)
      return 0;
   return outcome == VERIFY_OUTCOME_FAILED;
}
