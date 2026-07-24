/* agent_tier_lint.c: cost_tier vs catalog price consistency check. */
#include "aimee.h" /* MAX_PATH_LEN, used by agent_types.h */
#include "agent_tier_lint.h"
#include "agent_config.h"
#include "model_registry.h"
#include <limits.h>
#include <string.h>

/* Effective $/Mtok for an agent: operator override first, catalog second. The
 * catalog publishes a LIST price, which is not what every deployment pays —
 * subscription seats, committed-use discounts, self-hosted compute, and
 * reselling gateways all diverge from it. Each axis resolves independently so an
 * operator may pin only one. Returns 1 only when BOTH axes are known; the
 * capability struct cannot tell "free" from "absent", so a partially known price
 * must never enter a comparison as though the missing half were zero. */
int agent_resolved_price(const agent_t *agent, double *in_per_mtok, double *out_per_mtok,
                         double *cached_per_mtok)
{
   if (!agent)
      return 0;

   double in_price = agent->price_in_per_mtok;
   double out_price = agent->price_out_per_mtok;
   double cached_price = agent->price_cached_per_mtok;

   if (in_price <= 0.0 || out_price <= 0.0 || cached_price <= 0.0)
   {
      model_capability_t cap;
      if (agent->model[0] &&
          model_capability_get(agent_catalog_provider(agent), agent->model, &cap))
      {
         if (in_price <= 0.0)
            in_price = cap.cost_in_per_mtok;
         if (out_price <= 0.0)
            out_price = cap.cost_out_per_mtok;
         if (cached_price <= 0.0)
            cached_price = cap.cost_cache_read_per_mtok;
      }
   }

   /* Cached is optional: many providers publish none, and its absence must not
    * make an otherwise-priced agent look unpriced. */
   if (cached_per_mtok)
      *cached_per_mtok = cached_price > 0.0 ? cached_price : 0.0;
   if (in_price <= 0.0 || out_price <= 0.0)
      return 0;
   if (in_per_mtok)
      *in_per_mtok = in_price;
   if (out_per_mtok)
      *out_per_mtok = out_price;
   return 1;
}

/* Effective context ceiling: the operator's policy window when set, else the
 * model's catalogued capability. This is how far a request can actually go, and
 * therefore which bands are reachable. */
static int agent_effective_context_ceiling(const agent_t *agent, const model_capability_t *cap)
{
   if (agent->middleware.context_window > 0)
      return agent->middleware.context_window;
   return cap ? cap->context_window : 0;
}

int agent_resolved_price_at_context(const agent_t *agent, int context_tokens, double *in_per_mtok,
                                    double *out_per_mtok, double *cached_per_mtok)
{
   if (!agent)
      return 0;

   model_capability_t cap;
   int have_cap =
       agent->model[0] && model_capability_get(agent_catalog_provider(agent), agent->model, &cap);

   double in_price = have_cap ? cap.cost_in_per_mtok : 0.0;
   double out_price = have_cap ? cap.cost_out_per_mtok : 0.0;
   double cached_price = have_cap ? cap.cost_cache_read_per_mtok : 0.0;

   /* Highest band whose threshold this request exceeds. Bands are ascending. */
   if (have_cap && context_tokens > 0)
   {
      for (int i = 0; i < cap.price_band_count; i++)
      {
         if (context_tokens > cap.price_bands[i].above_tokens)
         {
            in_price = cap.price_bands[i].in_per_mtok;
            out_price = cap.price_bands[i].out_per_mtok;
            cached_price = cap.price_bands[i].cache_read_per_mtok;
         }
      }
   }

   /* The operator override states what THEY pay and so wins at every band. */
   if (agent->price_in_per_mtok > 0.0)
      in_price = agent->price_in_per_mtok;
   if (agent->price_out_per_mtok > 0.0)
      out_price = agent->price_out_per_mtok;
   if (agent->price_cached_per_mtok > 0.0)
      cached_price = agent->price_cached_per_mtok;

   if (cached_per_mtok)
      *cached_per_mtok = cached_price > 0.0 ? cached_price : 0.0;
   if (in_price <= 0.0 || out_price <= 0.0)
      return 0;
   if (in_per_mtok)
      *in_per_mtok = in_price;
   if (out_per_mtok)
      *out_per_mtok = out_price;
   return 1;
}

int agent_has_reachable_price_band(const agent_t *agent)
{
   if (!agent || !agent->model[0])
      return 0;
   /* A full operator override makes the vendor schedule irrelevant. */
   if (agent->price_in_per_mtok > 0.0 && agent->price_out_per_mtok > 0.0)
      return 0;
   model_capability_t cap;
   if (!model_capability_get(agent_catalog_provider(agent), agent->model, &cap))
      return 0;
   int ceiling = agent_effective_context_ceiling(agent, &cap);
   for (int i = 0; i < cap.price_band_count; i++)
   {
      if (ceiling <= 0 || ceiling > cap.price_bands[i].above_tokens)
         return 1;
   }
   return 0;
}

/* Does `a` cost at least as much as `b` at EVERY context size both can serve?
 *
 * Checking only the base rate and a notional "top" rate is not sufficient: with
 * two or more bands an ordering can flip and flip back (dearer at base, cheaper
 * mid-range, dearer again at the top), and a base+top test would wrongly report
 * a conflict that does not hold across the whole range. Nor can the top be
 * probed with INT_MAX — that evaluates bands the agents' context ceilings cannot
 * reach, which suppresses conflicts that DO hold everywhere usable.
 *
 * So evaluate at the base plus every band boundary either agent publishes, and
 * only up to the smaller of the two ceilings — beyond that the agents are not
 * substitutable and the comparison is moot. A truncated schedule is treated as
 * unknown: the dropped bands could reverse the ordering. */
static int price_dominates_across_reachable_bands(const agent_t *a, const agent_t *b)
{
   model_capability_t cap_a, cap_b;
   int have_a = a->model[0] && model_capability_get(agent_catalog_provider(a), a->model, &cap_a);
   int have_b = b->model[0] && model_capability_get(agent_catalog_provider(b), b->model, &cap_b);
   if ((have_a && cap_a.price_bands_truncated) || (have_b && cap_b.price_bands_truncated))
      return 0;

   int ceil_a = agent_effective_context_ceiling(a, have_a ? &cap_a : NULL);
   int ceil_b = agent_effective_context_ceiling(b, have_b ? &cap_b : NULL);
   /* 0 means unknown, i.e. unbounded for this purpose. */
   int limit = 0;
   if (ceil_a > 0 && ceil_b > 0)
      limit = ceil_a < ceil_b ? ceil_a : ceil_b;
   else if (ceil_a > 0)
      limit = ceil_a;
   else if (ceil_b > 0)
      limit = ceil_b;

   /* Probe points: the base band, then just past each published boundary. */
   int points[2 * MODEL_PRICE_BANDS_MAX + 1];
   int n = 0;
   points[n++] = 1;
   for (int pass = 0; pass < 2; pass++)
   {
      const model_capability_t *cap = pass == 0 ? &cap_a : &cap_b;
      if (!(pass == 0 ? have_a : have_b))
         continue;
      for (int i = 0; i < cap->price_band_count; i++)
      {
         int at = cap->price_bands[i].above_tokens;
         if (at <= 0 || at == INT_MAX)
            continue;
         if (limit > 0 && at >= limit)
            continue; /* neither agent can reach past this boundary */
         if (n < (int)(sizeof(points) / sizeof(points[0])))
            points[n++] = at + 1;
      }
   }

   for (int i = 0; i < n; i++)
   {
      double a_in = 0.0, a_out = 0.0, b_in = 0.0, b_out = 0.0;
      if (!agent_resolved_price_at_context(a, points[i], &a_in, &a_out, NULL) ||
          !agent_resolved_price_at_context(b, points[i], &b_in, &b_out, NULL))
         return 0;
      if (!(a_in >= b_in && a_out >= b_out && (a_in > b_in || a_out > b_out)))
         return 0;
   }
   return 1;
}

/* Could these two agents ever be candidates for the SAME route? Comparing tiers
 * across agents that never compete is a false positive: routing never chooses
 * between them, so "routing prefers the more expensive model" would be untrue
 * and the advice to re-tier them misleading.
 *
 * Selection is declared-role membership only (agent_has_role: the role itself or
 * the `all` wildcard) — there is no exec-role fallback, so two agents compete iff
 * they share a declared role. exec_roles[] governs tool exposure at execution
 * time, not who is picked, so it no longer creates route competition. */
static int agents_compete_for_a_role(const agent_t *a, const agent_t *b)
{
   for (int i = 0; i < a->role_count; i++)
   {
      const char *role = a->roles[i];
      if (role[0] && (strcmp(role, "all") == 0 || agent_has_role(b, role)))
         return 1;
   }
   for (int i = 0; i < b->role_count; i++)
   {
      const char *role = b->roles[i];
      if (role[0] && (strcmp(role, "all") == 0 || agent_has_role(a, role)))
         return 1;
   }
   return 0;
}

static int agent_is_price_exempt(const agent_t *ag)
{
   return ag && ag->tier_price_exempt[0] != '\0';
}

static int agent_price_eligible(const agent_t *ag, double *in_out, double *out_out)
{
   if (!ag || !ag->enabled || agent_is_price_exempt(ag))
      return 0;
   /* Operator override first, then catalog: the lint must judge tiers against
    * what this deployment actually pays, not a list price it may not be on. */
   return agent_resolved_price(ag, in_out, out_out, NULL);
}

int agent_tier_price_conflicts(const agent_config_t *cfg, agent_tier_conflict_t *out, int max)
{
   if (!cfg)
      return 0;

   int found = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      const agent_t *a = &cfg->agents[i];
      double a_in = 0.0, a_out = 0.0;
      if (!agent_price_eligible(a, &a_in, &a_out))
         continue;

      for (int j = 0; j < cfg->agent_count; j++)
      {
         if (i == j)
            continue;
         const agent_t *b = &cfg->agents[j];
         double b_in = 0.0, b_out = 0.0;
         if (!agent_price_eligible(b, &b_in, &b_out))
            continue;

         /* a is configured cheaper than b ... */
         if (a->cost_tier >= b->cost_tier)
            continue;
         /* ... but is PARETO-DOMINATED on price: no cheaper on either axis, and
          * strictly dearer on at least one. This needs no input/output exchange
          * rate, and unlike a strict-on-both test it still catches equality on
          * one axis ($10/$50 vs $10/$5 — never cheaper, dearer whenever output
          * is nonzero). A pair whose axes DISAGREE has no single correct
          * ordering and is deliberately left alone. */
         if (!(a_in >= b_in && a_out >= b_out && (a_in > b_in || a_out > b_out)))
            continue;
         /* ... and they actually compete for the same work. */
         if (!agents_compete_for_a_role(a, b))
            continue;

         /* ... and the ordering does not depend on request size. When either
          * agent can reach a context band its applicable rate changes with the
          * request, so one comparison cannot support the definitive advice
          * "routing prefers the more expensive model". */
         if (!price_dominates_across_reachable_bands(a, b))
            continue;

         if (out && found < max)
         {
            agent_tier_conflict_t *c = &out[found];
            memset(c, 0, sizeof(*c));
            snprintf(c->cheaper_tier_agent, sizeof(c->cheaper_tier_agent), "%s", a->name);
            snprintf(c->costlier_tier_agent, sizeof(c->costlier_tier_agent), "%s", b->name);
            c->cheaper_tier = a->cost_tier;
            c->costlier_tier = b->cost_tier;
            c->cheaper_tier_in = a_in;
            c->cheaper_tier_out = a_out;
            c->costlier_tier_in = b_in;
            c->costlier_tier_out = b_out;
         }
         found++;
      }
   }
   return found;
}
