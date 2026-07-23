/* delegate_verify.c: verification outcome classification and escalation policy. */
#include "delegate_verify.h"

verify_outcome_t verify_classify(int exec_rc)
{
   if (exec_rc == 0)
      return VERIFY_OUTCOME_PASS;
   /* safe_exec_capture returns -1 when it could not fork/exec, or when the child
    * did not exit normally - a timeout kill lands here. */
   if (exec_rc < 0)
      return VERIFY_OUTCOME_INFRA_ERROR;
   /* The shell's own "not executable" / "not found" codes: it never ran the
    * verifier, so this is a setup defect rather than a failed build. */
   if (exec_rc == 126 || exec_rc == 127)
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
