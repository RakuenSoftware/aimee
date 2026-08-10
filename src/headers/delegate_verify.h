/* delegate_verify.h: classify a verification run, and judge whether its outcome
 * is evidence the packet was placed on too weak a seat.
 *
 * The distinction this header exists for: a verify command that RAN and reported
 * a build/test failure indicts the delegate's work product. A verify command that
 * could not be run at all indicts the environment. Only the first is evidence
 * about the model's competence. Conflating them turns a missing binary or a
 * killed process into a "the model wasn't good enough" signal.
 *
 * ADVISORY ONLY. Nothing here re-dispatches. Automatic verifier-driven
 * escalation was retired: a verifier failure has many causes a dearer model will
 * not fix - invalid tests, a broken environment, ambiguous requirements, an
 * impossible task - so "the output failed a check" does not establish "the seat
 * was badly chosen", and automatic spend is a poor default response to that
 * ambiguity. Capability gating and the scope ceiling are what pick a sufficient
 * seat on the first attempt; this reports when that judgement looks wrong so a
 * human or an operator-owned policy can decide, and dispatch explicitly. */
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
    * not symmetric: treating a real test failure as infrastructure merely
    * withholds a placement warning, whereas treating an OOM kill, a timeout or a
    * missing binary as a work-product failure blames the model for its
    * environment. */
   /* The classification and the escalation policy both live in the delegates
    * module (server-go/modules/delegates/verify.go). This is the seam the C
    * side calls through; with no provider registered, classification reports
    * INFRA_ERROR and no escalation is raised -- unable to judge, this must not
    * claim a work-product failure, because that is the direction that blames
    * the model for its environment. */
#define DELEGATE_VERIFY_OP_CLASSIFY 0
#define DELEGATE_VERIFY_OP_ESCALATE 1

   /* op CLASSIFY: a=exec_rc,  b unused,      max_signal_status=platform ceiling
    * op ESCALATE: a=outcome,  b=delegate_rc, max_signal_status unused */
   typedef int (*delegate_verify_provider_fn)(int op, int a, int b, int max_signal_status,
                                              int *outcome_out, int *escalate_out);
   void delegate_register_verify_provider(delegate_verify_provider_fn provider);

   /* The highest status this PLATFORM can report as "killed by signal N". A
    * compile-time property of the host, so it travels with the request rather
    * than being guessed by the module. */
   int delegate_verify_max_signal_status(void);

   verify_outcome_t verify_classify(int exec_rc);

   const char *verify_outcome_name(verify_outcome_t o);

   /* Is this delegate result evidence the packet was placed on too weak a seat -
    * i.e. worth REPORTING as a misplacement, and worth a human considering a
    * dearer retry?
    *
    * The claim is "this model was not good enough for this work", so it requires
    * an attributable, verified work-product failure. It is deliberately NOT
    * raised by availability problems (API errors, transport failures, timeouts,
    * crashes) or by verification-infrastructure problems: those are
    * retry/failover concerns and say nothing about the seat.
    *
    * The caller does not act on this automatically - see the header note.
    *
    *   delegate_rc: 0 when the delegate run itself completed
    *   outcome    : classification of the verify run
    *
    * Returns 1 only when the delegate completed and a verifier genuinely ran and
    * failed. With no verifier configured there is no objective signal at all, so
    * the caller must pass VERIFY_OUTCOME_PASS and this returns 0 - report for
    * review rather than guessing from delegate prose.
    *
    * There is deliberately no `already_escalated` argument. It existed to stop
    * the automatic re-dispatch forming a ladder; with the re-dispatch retired,
    * no caller passes anything but 0, and keeping a parameter for a hypothetical
    * future caller is exactly the speculative generality this codebase avoids.
    * A caller that ever builds an explicit retry loop can track its own attempt
    * count. */
   int verify_escalation_warranted(int delegate_rc, verify_outcome_t outcome);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DELEGATE_VERIFY_H */
