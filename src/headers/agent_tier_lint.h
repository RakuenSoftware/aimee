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

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AGENT_TIER_LINT_H */
