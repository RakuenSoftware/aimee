/* delegate_verify.c: verification outcome classification and escalation policy. */
#include "delegate_verify.h"
#include <signal.h>

/* Highest status a POSIX shell can report as "command killed by signal N".
 * Derived from the PLATFORM's own signal range rather than hardcoded: Linux runs
 * to SIGRTMAX (64, so 192), while platforms without realtime signals top out far
 * lower (NSIG-1 ~ 31, so 159). Hardcoding Linux's ceiling would classify a
 * deliberate exit 160-192 as infrastructure on those platforms and silently
 * suppress its escalation. */
#if defined(SIGRTMAX)
#define VERIFY_MAX_SIGNAL_STATUS (128 + SIGRTMAX)
#elif defined(NSIG)
#define VERIFY_MAX_SIGNAL_STATUS (128 + (NSIG - 1))
#else
#define VERIFY_MAX_SIGNAL_STATUS 159 /* 128 + 31, the conservative POSIX floor */
#endif

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
   /* 128+N only for a signal number this PLATFORM can actually produce. Treating
    * everything above 128 as infrastructure was too broad: a verifier may
    * document 200 as a work-product failure, and suppressing escalation for it
    * is exactly the mistake this classification exists to avoid, just in the
    * other direction.
    *
    * This remains a heuristic and cannot be made exact: `sh -c` collapses "died
    * from signal N" and "deliberately exited 128+N" into the same integer, and
    * nothing in the status distinguishes them afterwards. A verifier that needs
    * an unambiguous work-product failure should exit below 124. */
   if (exec_rc == 126 || exec_rc == 127 || exec_rc == 124 ||
       (exec_rc >= 129 && exec_rc <= VERIFY_MAX_SIGNAL_STATUS))
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

int verify_escalation_warranted(int delegate_rc, verify_outcome_t outcome)
{
   /* The delegate must have finished. A failed run is an availability problem for
    * retry/failover to handle; reporting a misplacement would blame the model for
    * a transport or process failure it did not cause. */
   if (delegate_rc != 0)
      return 0;
   return outcome == VERIFY_OUTCOME_FAILED;
}
