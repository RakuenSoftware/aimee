/* agent_tier_lint.c: cost_tier vs catalog price consistency check. */
#include "aimee.h" /* MAX_PATH_LEN, used by agent_types.h */
#include "agent_tier_lint.h"
#include "agent_config.h"
#include "model_registry.h"
#include <string.h>

/* Resolve an agent's published price. Returns 0 when the catalog has no price,
 * which is the normal case for a model the registry does not know — treated as
 * "no evidence", never as "free". */
static int agent_catalog_price(const agent_t *ag, double *in_out, double *out_out)
{
   if (!ag || !ag->model[0])
      return 0;
   model_capability_t cap;
   if (!model_capability_get(agent_catalog_provider(ag), ag->model, &cap))
      return 0;
   /* BOTH axes must be present. The capability struct cannot distinguish "free"
    * from "absent" — a missing, null, or non-numeric price simply stays 0.0 — so
    * a partially published entry would otherwise enter the comparison with its
    * missing axis treated as known-zero and manufacture a conflict. Requiring
    * both keeps the stated policy that absent data is no evidence; the cost is
    * that a genuinely zero-priced model is skipped, which is the safe direction. */
   if (cap.cost_in_per_mtok <= 0.0 || cap.cost_out_per_mtok <= 0.0)
      return 0;
   *in_out = cap.cost_in_per_mtok;
   *out_out = cap.cost_out_per_mtok;
   return 1;
}

/* Could these two agents ever be candidates for the SAME route? Comparing tiers
 * across agents that never compete is a false positive: routing never chooses
 * between them, so "routing prefers the more expensive model" would be untrue
 * and the advice to re-tier them misleading. Substitutability here means sharing
 * at least one role — including the default exec roles, which agent_supports_role
 * grants to every agent regardless of its declared list. */
static int agents_compete_for_a_role(const agent_t *a, const agent_t *b)
{
   for (int i = 0; i < a->role_count; i++)
   {
      const char *role = a->roles[i];
      if (!role[0])
         continue;
      if (strcmp(role, "all") == 0 || agent_has_role(b, role) || agent_is_exec_role(b, role))
         return 1;
   }
   for (int i = 0; i < b->role_count; i++)
   {
      if (b->roles[i][0] && strcmp(b->roles[i], "all") == 0)
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
   return agent_catalog_price(ag, in_out, out_out);
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
