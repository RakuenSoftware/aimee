/* kb_bandit.h: Contextual bandit arm selection and reward feedback.
 * See docs/proposals/accepted/contextual-bandits-and-counterfactual-replay.md
 */
#ifndef DEC_KB_BANDIT_H
#define DEC_KB_BANDIT_H 1

#include "headers/config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define KB_BANDIT_MAX_ARM_ID   64
#define KB_BANDIT_MAX_ARMS     32
#define KB_BANDIT_MAX_DECISION 64

   /* Register or update a policy_arm artifact for a decision point.
    * variant_json:       arm-specific knobs as JSON (or NULL = "{}").
    * reward_schema_json: reward component weights as JSON (or NULL = "{}").
    * Returns 0 on success, -1 on error. */
   int kb_bandit_arm_register(const char *decision_point, const char *arm_id,
                              const char *variant_json, const char *reward_schema_json);

   /* Sample an arm using Thompson sampling (via bandit_optimize_command sidecar).
    * context_json:    feature-row subset as JSON (or NULL = "{}").
    * arm_ids:         array of n_arms arm IDs to choose from.
    * decision_id_out: receives UUID of the logged decision (>= KB_BANDIT_MAX_DECISION bytes);
    *                  may be NULL.
    * Returns index of selected arm on success, or -1 if disabled / error
    * (caller should fall back to arm 0). */
   int kb_bandit_sample(const config_t *cfg, const char *decision_point, const char *context_json,
                        const char (*arm_ids)[KB_BANDIT_MAX_ARM_ID], int n_arms,
                        char *decision_id_out);

   /* Close a decision with its observed reward and update the arm posterior.
    * decision_point: named decision surface (needed to key the arm stats).
    * decision_id:    UUID from kb_bandit_sample.
    * arm_id:         arm that was actually applied (may differ from sampled).
    * reward:         normalized reward; [0,1] for Beta-Bernoulli.
    * Returns 0 on success, -1 on error. */
   int kb_bandit_reward(const config_t *cfg, const char *decision_point, const char *decision_id,
                        const char *arm_id, double reward);

   /* Reward (v1) for the kb_memory_retrieval_limit decision: did the chosen
    * limit return a sufficient, non-truncated recall set?
    *   n_results == 0           -> 0.0  (empty recall is bad at any limit)
    *   n_results >= limit       -> 0.5  (hit the cap; a larger limit might help)
    *   0 < n_results < limit    -> 1.0  (limit was sufficient, no truncation)
    * Inspectable proxy; not trivially biased toward the larger arm (an arm that
    * returns everything relevant without truncation scores 1.0 regardless of
    * limit).  Pure function so it can be unit-tested in isolation. */
   double kb_bandit_recall_sufficiency_reward(int n_results, int limit);

   /* Record offline-replay attribution as a benchmark_trace artifact.
    *
    * Used by `aimee kb bandit-replay`: the Python replay tool emits a JSON
    * result (IPW or synthetic-control); this function lifts that result into
    * the charter's evidence stream so the Gate / promotion pipeline can see it.
    *
    * decision_point: scope_id on the artifact.
    * result_json:    output of tools/bandit_replay.py (object).
    * id_out / id_out_len: filled with the new artifact id (>= 33 bytes
    *                 recommended; may be NULL to skip).
    * Returns 0 on success, -1 on error (DB unavailable, bad input). */
   int kb_bandit_record_replay_evidence(const char *decision_point, const char *result_json,
                                        char *id_out, size_t id_out_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_BANDIT_H */
