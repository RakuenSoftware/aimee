/* agent_tier_lint.h: detect agents whose configured cost_tier contradicts the
 * catalog's published price.
 *
 * agent_route() minimises cost_tier, so that integer IS the cheapness ordering
 * the router optimises. It is hand-entered per agent and nothing has ever
 * checked it against what the models actually cost. A tier that disagrees with
 * price means "cheapest-first" routing is not minimising cost at all — it is
 * minimising an unverified integer. */
#ifndef AIMEE_AGENT_TIER_LINT_H
#define AIMEE_AGENT_TIER_LINT_H 1

#include "agent_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AGENT_TIER_LINT_MAX 32

   typedef struct
   {
      char cheaper_tier_agent[MAX_AGENT_NAME]; /* lower cost_tier ... */
      char costlier_tier_agent[MAX_AGENT_NAME];
      int cheaper_tier;
      int costlier_tier;
      /* ... but these prices say the opposite ($/Mtok). */
      double cheaper_tier_in, cheaper_tier_out;
      double costlier_tier_in, costlier_tier_out;
   } agent_tier_conflict_t;

   /* Find enabled agent pairs where the configured tier ordering contradicts the
    * catalog price ordering.
    *
    * A pair is reported ONLY when the contradiction is unambiguous: the
    * lower-tier agent is more expensive on BOTH input and output price. If input
    * and output disagree about which model is cheaper there is no single correct
    * ordering, and inventing an exchange rate between them would be exactly the
    * scalarisation this design rejects — so those pairs are left alone.
    *
    * Agents with no resolvable catalog price (price 0) are skipped: absent data
    * is not evidence of a wrong tier. Agents carrying a tier_price_exempt reason
    * are skipped too — a subscription or flat-rate plan can make per-token price
    * the wrong basis for that agent.
    *
    * Returns the number of conflicts found (may exceed `max`; at most `max` are
    * written). Never mutates config: this reports, it does not re-tier. */
   int agent_tier_price_conflicts(const agent_config_t *cfg, agent_tier_conflict_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AGENT_TIER_LINT_H */
