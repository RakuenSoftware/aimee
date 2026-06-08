/* db2/bandit.h: DB2 accessors for contextual bandit decision logs and arm stats.
 * See
 * docs/proposals/accepted/contextual-bandits-and-counterfactual-replay.md
 */
#ifndef DEC_DB2_BANDIT_H
#define DEC_DB2_BANDIT_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Record a new arm selection.
    * id:             pre-generated UUID (>= 37 bytes) from db2_artifact_gen_id.
    * decision_point: named decision surface.
    * arm_id:         selected arm.
    * context_hash:   hash of context features (for replay; may be empty).
    * propensity:     π(arm | context) at selection time.
    * is_exploration: 1 if the sample was an exploration pull (Thompson over
    *                 posterior), 0 if it was the MAP exploit. Used by the
    *                 exploration-budget Gate.
    * Returns 0 on success, -1 on error. */
   int db2_bandit_decision_insert(const char *id, const char *decision_point, const char *arm_id,
                                  const char *context_hash, double propensity, int is_exploration);

   /* Close a decision with its observed reward.
    * Returns 0 on success, -1 on error / not found. */
   int db2_bandit_decision_close(const char *id, double reward);

   /* Read or initialize arm stats for a (decision_point, arm_id) pair.
    * Populates n_decisions, n_rewards, posterior_alpha, posterior_beta on success.
    * Returns 0 on success, -1 on error. */
   typedef struct
   {
      long long n_decisions;
      long long n_rewards;
      double sum_reward;
      double posterior_alpha;
      double posterior_beta;
   } db2_bandit_arm_stats_t;

   int db2_bandit_arm_stats_read(const char *decision_point, const char *arm_id,
                                 db2_bandit_arm_stats_t *out);

   /* Update arm stats after a reward is observed (upsert).
    * Returns 0 on success, -1 on error. */
   int db2_bandit_arm_stats_update(const char *decision_point, const char *arm_id,
                                   double reward_delta, double posterior_alpha,
                                   double posterior_beta);

   /* Read closed decisions for offline replay.
    * Writes JSON array of {id, arm_id, context_hash, propensity, reward, decided_at}
    * into buf.  Returns 0 on success, -1 on error. */
   int db2_bandit_decisions_export(const char *decision_point, int limit, char *buf, size_t len);

   /* Count decisions in a time window per decision_point, split into exploration
    * and total. window_seconds <= 0 means "any time".
    * Either out pointer may be NULL.
    * Returns 0 on success (even if the table is empty), -1 on error. */
   int db2_bandit_explore_stats(const char *decision_point, int window_seconds,
                                long long *n_explore_out, long long *n_total_out);

   /* List the distinct decision points that actually have logged decisions, as a
    * JSON array of strings ordered most-recent-first.  Writes "[]" when empty.
    * Used by the bandit export so introspection reflects the points that are
    * really sampled, rather than a hard-coded literal.
    * Returns 0 on success, -1 on error. */
   int db2_bandit_decision_points_list(char *buf, size_t len);

   /* List the distinct arm ids observed for a decision point in the decision log,
    * as a JSON array of strings ordered by arm id.  Writes "[]" when empty.
    * Discovered from the log (not arm-stats), so arms are visible even before any
    * reward has been observed.  Returns 0 on success, -1 on error. */
   int db2_bandit_arms_list(const char *decision_point, char *buf, size_t len);

   /* Promoted (production-default) arm per decision point. get writes the arm into
    * arm_out and returns 0, or -1 when no promotion exists. set upserts the
    * promotion (recording rollback_arm, the prior default). Returns 0 on success. */
   int db2_bandit_promotion_get(const char *decision_point, char *arm_out, size_t arm_out_len);
   int db2_bandit_promotion_set(const char *decision_point, const char *arm_id,
                                const char *rollback_arm);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_BANDIT_H */
