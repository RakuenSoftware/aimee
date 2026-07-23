/* delegate_verify.h: classify a verification run, and decide whether its outcome
 * justifies escalating the packet to a more capable agent.
 *
 * The distinction this header exists for: a verify command that RAN and reported
 * a build/test failure indicts the delegate's work product. A verify command that
 * could not be run at all indicts the environment. Only the first is evidence
 * about the model's competence, and only the first may trigger a capability
 * escalation. Conflating them turns a missing binary or a killed process into a
 * "the model wasn't good enough" signal, which would escalate real work for
 * reasons that have nothing to do with the model. */
#ifndef AIMEE_DELEGATE_VERIFY_H
#define AIMEE_DELEGATE_VERIFY_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      VERIFY_OUTCOME_PASS = 0,
      /* The verifier ran to completion and reported failure: a genuine,
       * attributable statement about the delegate's work product. */
      VERIFY_OUTCOME_FAILED = 1,
      /* The verifier could not be run, or did not exit normally: missing or
       * non-executable command, spawn failure, or death by signal (a timeout
       * kill looks like this). Says nothing about the work product. */
      VERIFY_OUTCOME_INFRA_ERROR = 2,
   } verify_outcome_t;

   /* Classify the return of safe_exec_capture() over `/bin/sh -c <verify_cmd>`.
    *
    * This is a HEURISTIC. `/bin/sh -c` flattens everything into one integer, and
    * 124, 126, 127 and 128+N are all values a verifier may also return on
    * purpose - the distinction cannot be made reliably at this layer without a
    * richer verifier protocol.
    *
    * The tie is therefore broken toward INFRA_ERROR, because the two mistakes are
    * not symmetric: treating a real test failure as infrastructure merely skips an
    * escalation and defers to a human, whereas treating an OOM kill, a timeout or
    * a missing binary as a work-product failure blames the model for its
    * environment and burns a dearer seat for nothing. */
   verify_outcome_t verify_classify(int exec_rc);

   const char *verify_outcome_name(verify_outcome_t o);

   /* May this delegate result trigger a CAPABILITY escalation - a re-dispatch to
    * a more capable agent?
    *
    * An escalation asserts "this model was not good enough for this work", so it
    * requires an attributable, verified work-product failure. It is deliberately
    * NOT triggered by availability problems (API errors, transport failures,
    * timeouts, crashes) or by verification-infrastructure problems: those are
    * retry/failover concerns, and escalating on them would burn a dearer seat for
    * a reason the cheaper one was never responsible for.
    *
    *   delegate_rc      : 0 when the delegate run itself completed
    *   outcome          : classification of the verify run
    *   already_escalated: escalation allowance already consumed for this packet
    *
    * Returns 1 only when the delegate completed, a verifier genuinely ran and
    * failed, and the allowance is intact. With no verifier configured there is no
    * objective signal at all, so the caller must pass VERIFY_OUTCOME_PASS and this
    * returns 0 - fail for review rather than guessing from delegate prose. */
   int verify_escalation_warranted(int delegate_rc, verify_outcome_t outcome,
                                   int already_escalated);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DELEGATE_VERIFY_H */
