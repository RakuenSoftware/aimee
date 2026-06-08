/* kb_bandit_registry.h: declarative registry of bandit decision points.
 *
 * Single source of truth for which decisions the optimizer knows about, their
 * arm sets, and the reward function each uses.  Consumed by the live sampler
 * (kb_service_memory.c) and the intelligence export (kb_intel_payload.c), and
 * surfaced to operators via `aimee optimize`.  See
 * docs/proposals/pending/optimization-surface.md (P1).
 */
#ifndef DEC_KB_BANDIT_REGISTRY_H
#define DEC_KB_BANDIT_REGISTRY_H 1

#include "kb_bandit.h" /* KB_BANDIT_MAX_ARMS, KB_BANDIT_MAX_ARM_ID */

#ifdef __cplusplus
extern "C"
{
#endif

   /* A declared decision point.  All fields are static string literals owned by
    * the registry table; callers must not free them. */
   typedef struct
   {
      const char *id;          /* decision_point key (matches the bandit log) */
      const char *description; /* one-line human summary */
      const char *arms[KB_BANDIT_MAX_ARMS];
      int n_arms;
      const char *reward_fn; /* reward function id, e.g. "recall_sufficiency_v1" */
      const char *status;    /* "live" (sampled now) | "static" (knob, not yet sampled)
                              * | "planned" (declared, not wired) */
   } kb_bandit_decision_point_t;

   /* Number of registered decision points. */
   int kb_bandit_registry_count(void);

   /* Registered point by index in [0, count), or NULL if out of range. */
   const kb_bandit_decision_point_t *kb_bandit_registry_at(int i);

   /* Registered point by id, or NULL if not registered. */
   const kb_bandit_decision_point_t *kb_bandit_registry_get(const char *id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_BANDIT_REGISTRY_H */
