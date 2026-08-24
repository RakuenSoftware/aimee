/* policy_arms.h: the instructions themselves become something measurable
 * (recursive-self-improvement S6).
 *
 * aimee fits which DOCUMENTS to trust — earned-trust reranking, a fitted ranker
 * model, bandit arms on retrieval decisions. It has never fitted which
 * INSTRUCTIONS work. Prompt blocks, role-template sections, and advisory
 * preambles are hand-written constants: nobody knows whether any of them earn
 * their tokens, because no variant is ever compared against another.
 *
 * This is the registry that makes a fragment a measurable decision. A named
 * decision point declares its arms; selection goes through an installed
 * sampler when one exists (the bandit lives in the knowledge service, not
 * here) and falls back to the declared default otherwise, so an installation
 * with no sampler behaves exactly as it did before this existed.
 *
 * Three things this deliberately does NOT do:
 *
 *   - It does not author prompt text. Arms name variants that already exist in
 *     the codebase; a loop that writes its own instructions is a different and
 *     much larger proposal.
 *   - It does not let a sample change the default. Promotion is a separate,
 *     gated call — and it consults the endogeneity gate, because a loop
 *     feeding on its own output must not get to rewrite its own instructions.
 *   - It does not reward anything itself. Reward arrives from measured
 *     outcomes (S2), through a sink the host installs.
 *
 * The selection policy is pure and unit-testable; only the sink calls leave.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */
#ifndef DEC_LEARNING_POLICY_ARMS_H
#define DEC_LEARNING_POLICY_ARMS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define LEARNING_POLICY_POINT_LEN 48
#define LEARNING_POLICY_ARM_LEN   32
#define LEARNING_POLICY_MAX_ARMS  8

/* The one decision point this slice ships. The advisory block S3 renders at
 * plan time is exactly the kind of fragment whose value nobody has measured:
 * it costs tokens on every plan and its benefit is assumed. Registering it as
 * arms is what turns that assumption into a question with an answer. */
#define LEARNING_POLICY_PLAN_ADVISORY "plan_advisory"
/* Say nothing. The arm that must exist, or "is this block worth it?" has no
 * control to measure against. */
#define LEARNING_POLICY_ADVISORY_OFF "off"
/* One line naming how many dead ends are known, without their detail. */
#define LEARNING_POLICY_ADVISORY_BRIEF "brief"
/* The full rendered block. The current behaviour, and the declared default. */
#define LEARNING_POLICY_ADVISORY_FULL "full"

   /* Chooses an arm for `decision_point` from `arms` (n entries). Returns the
    * index chosen, or a negative value to decline — declining is normal and
    * means "fall back to the default", not an error. */
   typedef int (*learning_policy_sampler_fn)(const char *decision_point,
                                             const char (*arms)[LEARNING_POLICY_ARM_LEN], int n);

   /* Receives the measured outcome of a previously selected arm. */
   typedef void (*learning_policy_reward_fn)(const char *decision_point, const char *arm,
                                             double reward);

   /* Install the sampler / reward sink. NULL clears. Absent a sampler, every
    * selection returns the declared default, which is the behaviour that
    * shipped before this registry existed. */
   void learning_policy_register_sampler(learning_policy_sampler_fn sampler);
   void learning_policy_register_reward_sink(learning_policy_reward_fn sink);

   /* The arms a decision point declares, in declaration order. Writes at most
    * `max` names. Returns the count, or -1 for an unknown point. */
   int learning_policy_arms(const char *decision_point, char (*out)[LEARNING_POLICY_ARM_LEN],
                            int max);

   /* 1 when `arm` is declared for `decision_point`. */
   int learning_policy_arm_is_valid(const char *decision_point, const char *arm);

   /* The current default arm, i.e. what selection falls back to. Returns NULL
    * for an unknown point. */
   const char *learning_policy_default_arm(const char *decision_point);

   /* Choose an arm. Writes the chosen name into `out`. A sampler that declines,
    * errors, or names an arm the point does not declare yields the default —
    * a bad sampler must not be able to inject an undeclared fragment.
    * Returns 0 on success, -1 for an unknown point / bad args. */
   int learning_policy_select(const char *decision_point, char *out, size_t out_len);

   /* Report the measured outcome of an arm. Forwarded to the installed sink;
    * a no-op when none is installed. Refuses an undeclared arm rather than
    * crediting a fragment nobody chose. Returns 0 when forwarded, -1 otherwise. */
   int learning_policy_report(const char *decision_point, const char *arm, double reward);

   /* Make `arm` the default for `decision_point`.
    *
    * Gated: refuses while the endogeneity gate is closed, because a loop whose
    * evidence has become self-referential must not get to rewrite the
    * instructions it will then judge itself by. Refuses an undeclared arm.
    * Returns 0 on success, -1 on refusal. */
   int learning_policy_promote(const char *decision_point, const char *arm);

   /* Reset every decision point to its shipped default and clear the installed
    * sampler/sink. For tests and for an operator undoing a promotion. */
   void learning_policy_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_LEARNING_POLICY_ARMS_H */
