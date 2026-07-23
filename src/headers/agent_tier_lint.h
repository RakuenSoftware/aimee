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

   /* Effective $/Mtok for an agent across the three billed axes — cached read,
    * input, output — resolving the operator override first and the model catalog
    * second. This is the single source of truth for "what does this agent cost
    * us": the catalog publishes a LIST price, which is not what every deployment
    * pays (subscription seats, committed-use discounts, self-hosted compute,
    * reselling gateways).
    *
    * Each axis resolves independently, so an operator may pin only one. Returns
    * 1 when input AND output are known, 0 otherwise; a caller must treat 0 as
    * "no price evidence" and never as free.
    *
    * `cached_per_mtok` is OPTIONAL and set to 0 when neither the operator nor the
    * catalog publishes one — many providers do not. It is reported separately
    * rather than folded into input because cache reads are typically an order of
    * magnitude cheaper, so a caching workload's real cost is not approximated by
    * the input rate. Any parameter may be NULL. */
   int agent_resolved_price(const agent_t *agent, double *in_per_mtok, double *out_per_mtok,
                            double *cached_per_mtok);

   /* Same, but for a request of `context_tokens`. Several providers charge more
    * above a context threshold (gpt-5.6-sol doubles above 272k), so the BASE
    * rate returned by agent_resolved_price() is only correct for requests below
    * the first band. Use this wherever the request size is known; use the base
    * form only for a headline figure, and label it as such.
    *
    * An operator price override, when present, applies at EVERY band: the
    * operator is stating what they pay, which supersedes the vendor's schedule
    * rather than being scaled by it. context_tokens <= 0 means "unknown", which
    * resolves to the base rate. */
   int agent_resolved_price_at_context(const agent_t *agent, int context_tokens,
                                       double *in_per_mtok, double *out_per_mtok,
                                       double *cached_per_mtok);

   /* Does this agent have context-band pricing that its configured context
    * ceiling can actually reach? Used to avoid asserting a definitive price
    * ordering for an agent whose applicable rate depends on request size. */
   int agent_has_reachable_price_band(const agent_t *agent);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AGENT_TIER_LINT_H */
