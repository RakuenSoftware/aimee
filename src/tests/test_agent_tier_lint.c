/* test_agent_tier_lint.c: cost_tier vs catalog price consistency. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee.h"
#include "agent_config.h"
#include "agent_tier_lint.h"
#include "model_registry.h"

/* Mirror agent_config.c so this test links only the registry, not the whole
 * agent-config layer. Kept behaviourally identical to the originals. */
const char *agent_catalog_provider(const agent_t *agent)
{
   if (!agent)
      return "";
   return agent->catalog_provider[0] ? agent->catalog_provider : agent->provider;
}

int agent_has_role(const agent_t *agent, const char *role)
{
   for (int i = 0; i < agent->role_count; i++)
      if (strcmp(agent->roles[i], "all") == 0 || strcmp(agent->roles[i], role) == 0)
         return 1;
   return 0;
}

int agent_is_exec_role(const agent_t *agent, const char *role)
{
   /* Same default set as agent_config.c: these 18 roles are granted to every
    * agent regardless of its declared roles list. */
   static const char *defaults[] = {"deploy",  "validate",   "test",  "diagnose",   "execute",
                                    "review",  "code",       "refactor", "draft",   "implement",
                                    "continuity", "prose",   "line-edit", "beat-check",
                                    "lyric",   "hook",       "prosody", "songform"};
   if (agent->exec_role_count > 0)
   {
      for (int i = 0; i < agent->exec_role_count; i++)
         if (strcmp(agent->exec_roles[i], role) == 0)
            return 1;
      return 0;
   }
   for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
      if (strcmp(defaults[i], role) == 0)
         return 1;
   return 0;
}

/* Seed a real priced catalog. Without this the static table prices nothing for
 * the fixture models and every meaningful assertion below SKIPs — a green run
 * that proves nothing. models_dev_cache_lookup() reads $HOME/.cache/aimee on
 * every call, so pointing HOME at a temp dir gives deterministic prices. */
static char g_tmp_home[256];

static void seed_priced_catalog(void)
{
   snprintf(g_tmp_home, sizeof(g_tmp_home), "/tmp/test-tier-lint-XXXXXX");
   assert(mkdtemp(g_tmp_home) != NULL);

   char dir[512], path[600];
   snprintf(dir, sizeof(dir), "%s/.cache", g_tmp_home);
   mkdir(dir, 0755);
   snprintf(dir, sizeof(dir), "%s/.cache/aimee", g_tmp_home);
   mkdir(dir, 0755);
   snprintf(path, sizeof(path), "%s/models_dev.json", dir);

   /* DEAR is more expensive than CHEAP on BOTH axes: the unambiguous case the
    * lint is specified to flag. AMBIG_* invert between axes, which must NOT be
    * flagged (no exchange rate is invented). */
   const char *json = "{"
                      "\"testvendor/dear\":  {\"contextWindow\":100000,\"inputCost\":10.0,"
                      "                      \"outputCost\":50.0,\"tools\":true},"
                      "\"testvendor/cheap\": {\"contextWindow\":100000,\"inputCost\":1.0,"
                      "                      \"outputCost\":5.0,\"tools\":true},"
                      "\"testvendor/ambig_a\":{\"contextWindow\":100000,\"inputCost\":9.0,"
                      "                      \"outputCost\":1.0,\"tools\":true},"
                      "\"testvendor/ambig_b\":{\"contextWindow\":100000,\"inputCost\":1.0,"
                      "                      \"outputCost\":9.0,\"tools\":true},"
                      /* Equal on input, dearer on output: Pareto-dominated. */
                      "\"testvendor/eq_dear\":{\"contextWindow\":100000,\"inputCost\":10.0,"
                      "                      \"outputCost\":50.0,\"tools\":true},"
                      "\"testvendor/eq_cheap\":{\"contextWindow\":100000,\"inputCost\":10.0,"
                      "                      \"outputCost\":5.0,\"tools\":true},"
                      /* Output price ABSENT: partial data, not a known zero. */
                      "\"testvendor/partial\":{\"contextWindow\":100000,\"inputCost\":1.0,"
                      "                      \"tools\":true}"
                      "}";
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(json, f);
   fclose(f);

   setenv("HOME", g_tmp_home, 1);

   /* Fail loudly if the seed did not take: a silent miss would restore exactly
    * the vacuous-skip problem this exists to remove. */
   model_capability_t c;
   assert(model_capability_get("testvendor", "dear", &c) != 0);
   assert(c.cost_in_per_mtok == 10.0 && c.cost_out_per_mtok == 50.0);
   assert(model_capability_get("testvendor", "cheap", &c) != 0);
   assert(c.cost_in_per_mtok == 1.0);
}

/* Prices come from the capability catalog. With a cold models.dev cache the
 * static table is the source, so these fixtures pin models the static table
 * knows and assert on the RELATIVE ordering rather than absolute dollars. */
static void add_agent(agent_config_t *cfg, const char *name, const char *provider,
                      const char *model, int tier)
{
   agent_t *a = &cfg->agents[cfg->agent_count++];
   memset(a, 0, sizeof(*a));
   snprintf(a->name, sizeof(a->name), "%s", name);
   snprintf(a->provider, sizeof(a->provider), "%s", provider);
   snprintf(a->model, sizeof(a->model), "%s", model);
   a->cost_tier = tier;
   a->enabled = 1;
   a->tools_enabled = 1;
   snprintf(a->roles[0], sizeof(a->roles[0]), "%s", "review");
   a->role_count = 1;
}

/* A priced pair whose tier ordering contradicts both price axes is the whole
 * point of the check: routing minimises cost_tier, so it would prefer the more
 * expensive model believing it to be cheaper. */
static void test_detects_inverted_tier(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   /* The expensive model is configured at the CHEAPEST tier. */
   add_agent(&cfg, "expensive_at_tier0", "testvendor", "dear", 0);
   add_agent(&cfg, "cheap_at_tier1", "testvendor", "cheap", 1);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   int n = agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX);
   assert(n == 1);
   assert(strcmp(out[0].cheaper_tier_agent, "expensive_at_tier0") == 0);
   assert(strcmp(out[0].costlier_tier_agent, "cheap_at_tier1") == 0);
   assert(out[0].cheaper_tier == 0 && out[0].costlier_tier == 1);
   assert(out[0].cheaper_tier_in > out[0].costlier_tier_in);

   /* Correcting the tiers clears the finding. */
   cfg.agents[0].cost_tier = 1;
   cfg.agents[1].cost_tier = 0;
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_detects_inverted_tier\n");
}

/* An exemption must be honoured — a subscription seat's marginal cost is not
 * the published per-token price — but it requires a stated reason so it cannot
 * silently mask a genuinely mis-tiered agent. */
static void test_exemption_suppresses(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   add_agent(&cfg, "subscription", "testvendor", "dear", 0);
   add_agent(&cfg, "metered", "testvendor", "cheap", 1);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 1);

   snprintf(cfg.agents[0].tier_price_exempt, sizeof(cfg.agents[0].tier_price_exempt), "%s",
            "flat-rate subscription; marginal token cost is not the API price");
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_exemption_suppresses\n");
}

/* Absent price data is NOT evidence of a wrong tier. A model the catalog does
 * not price must never produce a finding, or the check would fire on every
 * unknown model an operator adds. */
static void test_unpriced_model_is_not_a_finding(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "unknown_a", "anthropic", "totally-unknown-model-aaa", 0);
   add_agent(&cfg, "unknown_b", "anthropic", "totally-unknown-model-bbb", 1);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_unpriced_model_is_not_a_finding\n");
}

/* A disabled agent is not routable, so its tier cannot mis-route anything. */
static void test_disabled_agent_ignored(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   add_agent(&cfg, "expensive_at_tier0", "testvendor", "dear", 0);
   add_agent(&cfg, "cheap_at_tier1", "testvendor", "cheap", 1);
   cfg.agents[0].enabled = 0;

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_disabled_agent_ignored\n");
}

/* Equal tiers express no ordering, so they cannot contradict a price. */
static void test_equal_tiers_never_conflict(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "a", "testvendor", "dear", 2);
   add_agent(&cfg, "b", "testvendor", "cheap", 2);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_equal_tiers_never_conflict\n");
}

/* NULL config and a zero-capacity output buffer must not crash, and the count
 * must still be reported so a caller can size a buffer. */
static void test_guards(void)
{
   agent_tier_conflict_t out[1];
   assert(agent_tier_price_conflicts(NULL, out, 1) == 0);

   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   assert(agent_tier_price_conflicts(&cfg, NULL, 0) == 0);

   printf("  PASS: test_guards\n");
}

/* When input and output prices disagree about which model is cheaper there is
 * no single correct ordering. Flagging it would require inventing an
 * input/output exchange rate — the scalarisation this design rejects — so an
 * ambiguous pair must stay silent regardless of tier. */
static void test_ambiguous_price_axes_not_flagged(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* ambig_a: $9 in / $1 out. ambig_b: $1 in / $9 out. */
   add_agent(&cfg, "a_at_tier0", "testvendor", "ambig_a", 0);
   add_agent(&cfg, "b_at_tier1", "testvendor", "ambig_b", 1);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   /* ...and the reverse tier assignment is equally not a defect. */
   cfg.agents[0].cost_tier = 1;
   cfg.agents[1].cost_tier = 0;
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_ambiguous_price_axes_not_flagged\n");
}

/* Equality on one axis with a strictly dearer other axis IS unambiguous Pareto
 * dominance and needs no exchange rate: $10/$50 is never cheaper than $10/$5.
 * A strict-on-both-axes test missed this. */
static void test_equal_axis_dominance_is_flagged(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "dear_at_tier0", "testvendor", "eq_dear", 0);
   add_agent(&cfg, "cheap_at_tier1", "testvendor", "eq_cheap", 1);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 1);
   assert(strcmp(out[0].cheaper_tier_agent, "dear_at_tier0") == 0);

   printf("  PASS: test_equal_axis_dominance_is_flagged\n");
}

/* A model with only one published price axis must not be compared as though the
 * missing axis were known to be zero — that would manufacture a conflict from
 * absent data, contradicting "absent is no evidence". */
static void test_partial_price_data_is_not_compared(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "dear_at_tier0", "testvendor", "dear", 0);      /* 10 / 50 */
   add_agent(&cfg, "partial_at_tier1", "testvendor", "partial", 1); /* 1 / absent */

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_partial_price_data_is_not_compared\n");
}

/* Agents that can never enter the same candidate set are not substitutes, so
 * their relative tiers cannot mis-route anything. Reporting them would tell the
 * operator routing prefers the dearer model when routing never chooses between
 * them at all. */
static void test_non_competing_roles_not_flagged(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "dear_at_tier0", "testvendor", "dear", 0);
   add_agent(&cfg, "cheap_at_tier1", "testvendor", "cheap", 1);
   /* Disjoint, NON-exec roles: neither can serve the other's work. */
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "%s", "explain");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "%s", "summarize");

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   /* Sharing a role restores the finding. */
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "%s", "explain");
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 1);

   printf("  PASS: test_non_competing_roles_not_flagged\n");
}

int main(void)
{
   printf("agent_tier_lint:\n");
   seed_priced_catalog();
   test_detects_inverted_tier();
   test_exemption_suppresses();
   test_unpriced_model_is_not_a_finding();
   test_disabled_agent_ignored();
   test_equal_tiers_never_conflict();
   test_ambiguous_price_axes_not_flagged();
   test_equal_axis_dominance_is_flagged();
   test_partial_price_data_is_not_compared();
   test_non_competing_roles_not_flagged();
   test_guards();
   printf("agent_tier_lint: all tests passed\n");
   return 0;
}
