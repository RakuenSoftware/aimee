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
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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
   static const char *defaults[] = {
       "deploy",    "validate",   "test",  "diagnose",  "execute",    "review",
       "code",      "refactor",   "draft", "implement", "continuity", "prose",
       "line-edit", "beat-check", "lyric", "hook",      "prosody",    "songform"};
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
   snprintf(g_tmp_home, sizeof(g_tmp_home), "%s/test-tier-lint-XXXXXX", platform_tmpdir());
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
                      "                      \"outputCost\":5.0,\"cacheReadCost\":0.1,"
                      "                      \"tools\":true},"
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
                      "                      \"tools\":true},"
                      /* Banded models use the NESTED schema, the only form that
                       * carries cost.tiers. Mirrors gpt-5.6-sol: base $5/$30,
                       * doubling above 272k. */
                      "\"banded\": {\"models\": {"
                      "  \"sol_like\": {\"limit\":{\"context\":1050000},"
                      "     \"cost\":{\"input\":5.0,\"output\":30.0,\"cache_read\":0.5,"
                      "       \"tiers\":[{\"input\":10.0,\"output\":45.0,\"cache_read\":1.0,"
                      "         \"tier\":{\"type\":\"context\",\"size\":272000}}]},"
                      "     \"tool_call\":true},"
                      /* Flat-priced peer, dearer than sol_like's BASE band but
                       * cheaper than its top band: the ordering flips. */
                      "  \"flat_mid\": {\"limit\":{\"context\":1050000},"
                      "     \"cost\":{\"input\":7.0,\"output\":35.0},\"tool_call\":true},"
                      /* Dearer than sol_like at EVERY band. */
                      "  \"flat_dear\": {\"limit\":{\"context\":1050000},"
                      "     \"cost\":{\"input\":20.0,\"output\":90.0},\"tool_call\":true},"
                      /* Two bands, to prove ascending selection. */
                      /* Flips dear -> cheap -> dear across two boundaries: a
                       * base+top comparison alone would wrongly report a
                       * conflict that does not hold in the middle. */
                      "  \"flipflop\": {\"limit\":{\"context\":1000000},"
                      "     \"cost\":{\"input\":10.0,\"output\":10.0,"
                      "       \"tiers\":[{\"input\":1.0,\"output\":1.0,"
                      "         \"tier\":{\"type\":\"context\",\"size\":100000}},"
                      "        {\"input\":20.0,\"output\":20.0,"
                      "         \"tier\":{\"type\":\"context\",\"size\":200000}}]},"
                      "     \"tool_call\":true},"
                      "  \"flat_five\": {\"limit\":{\"context\":1000000},"
                      "     \"cost\":{\"input\":5.0,\"output\":5.0},\"tool_call\":true},"
                      "  \"two_band\": {\"limit\":{\"context\":1000000},"
                      "     \"cost\":{\"input\":1.0,\"output\":2.0,"
                      "       \"tiers\":[{\"input\":4.0,\"output\":8.0,"
                      "         \"tier\":{\"type\":\"context\",\"size\":500000}},"
                      "        {\"input\":2.0,\"output\":4.0,"
                      "         \"tier\":{\"type\":\"context\",\"size\":100000}}]},"
                      "     \"tool_call\":true}"
                      "}}"
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
   add_agent(&cfg, "dear_at_tier0", "testvendor", "dear", 0);       /* 10 / 50 */
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
   /* Disjoint declared roles AND disjoint exec sets: only then do they truly
    * never compete. Constraining exec_roles is essential — with the DEFAULT exec
    * set both agents are routable for review/code/test and so DO compete, which
    * an earlier version of this test wrongly encoded as "no competition". */
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "%s", "explain");
   snprintf(cfg.agents[0].exec_roles[0], sizeof(cfg.agents[0].exec_roles[0]), "%s", "explain");
   cfg.agents[0].exec_role_count = 1;
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "%s", "summarize");
   snprintf(cfg.agents[1].exec_roles[0], sizeof(cfg.agents[1].exec_roles[0]), "%s", "summarize");
   cfg.agents[1].exec_role_count = 1;

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   /* Sharing a declared role restores the finding. */
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "%s", "explain");
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 1);

   printf("  PASS: test_non_competing_roles_not_flagged\n");
}

/* Operator pricing beats the catalog. The published rate is a LIST price and is
 * not what every deployment pays: a subscription seat, committed-use discount,
 * self-hosted compute, or a reselling gateway all differ. Overriding is the
 * mechanism for saying what this deployment actually pays. */
static void test_operator_price_override_wins(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "dear_at_tier0", "testvendor", "dear", 0);   /* catalog 10 / 50 */
   add_agent(&cfg, "cheap_at_tier1", "testvendor", "cheap", 1); /* catalog 1 / 5  */

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   /* On catalog prices this is a genuine contradiction. */
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 1);

   /* The operator declares the tier-0 agent is actually near-free to them (a
    * flat-rate seat). The tiers now agree with real cost, so no finding. */
   cfg.agents[0].price_in_per_mtok = 0.10;
   cfg.agents[0].price_out_per_mtok = 0.20;
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   double pin = 0.0, pout = 0.0, pcached = 0.0;
   assert(agent_resolved_price(&cfg.agents[0], &pin, &pout, &pcached) == 1);
   assert(pin == 0.10 && pout == 0.20);

   /* An override can also CREATE a finding the catalog would not: a gateway
    * reselling the cheap model at a markup. */
   memset(&cfg.agents[0].price_in_per_mtok, 0, sizeof(double));
   cfg.agents[0].price_in_per_mtok = 0.0;
   cfg.agents[0].price_out_per_mtok = 0.0;
   cfg.agents[1].price_in_per_mtok = 100.0;
   cfg.agents[1].price_out_per_mtok = 500.0;
   /* Now tier-1 is dearer, which is CONSISTENT with its higher tier: no finding. */
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_operator_price_override_wins\n");
}

/* Each axis resolves independently: an operator may pin only one and let the
 * catalog answer the other. */
static void test_price_override_per_axis(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "a", "testvendor", "dear", 0); /* catalog 10 / 50 */

   double pin = 0.0, pout = 0.0, pcached = 0.0;
   cfg.agents[0].price_in_per_mtok = 2.0; /* pin input only */
   assert(agent_resolved_price(&cfg.agents[0], &pin, &pout, &pcached) == 1);
   assert(pin == 2.0);   /* operator */
   assert(pout == 50.0); /* catalog  */

   /* An unpriced model with only one axis pinned is still "no evidence": the
    * catalog cannot supply the other, so both axes are not known. */
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "unknown", "anthropic", "totally-unknown-model-zzz", 0);
   cfg.agents[0].price_in_per_mtok = 3.0;
   assert(agent_resolved_price(&cfg.agents[0], &pin, &pout, &pcached) == 0);

   /* Pinning both makes an otherwise unpriced model fully priced — the path a
    * self-hosted or gateway model takes. */
   cfg.agents[0].price_out_per_mtok = 9.0;
   assert(agent_resolved_price(&cfg.agents[0], &pin, &pout, &pcached) == 1);
   assert(pin == 3.0 && pout == 9.0);

   printf("  PASS: test_price_override_per_axis\n");
}

/* Cache-read price is a THIRD billed axis and is reported separately: it is
 * typically an order of magnitude below input, so a caching workload's real
 * cost is not approximated by the input rate. It is optional — many providers
 * publish none — and its absence must not make an otherwise-priced agent look
 * unpriced. */
static void test_cached_price_axis(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "cheap", "testvendor", "cheap", 0); /* catalog publishes cache_read */
   add_agent(&cfg, "dear", "testvendor", "dear", 1);   /* catalog publishes none */

   double pin = 0.0, pout = 0.0, pcached = -1.0;
   assert(agent_resolved_price(&cfg.agents[0], &pin, &pout, &pcached) == 1);
   assert(pin == 1.0 && pout == 5.0);
   assert(pcached == 0.1);

   /* No published cache rate -> 0, and the agent is still fully priced. */
   pcached = -1.0;
   assert(agent_resolved_price(&cfg.agents[1], &pin, &pout, &pcached) == 1);
   assert(pcached == 0.0);

   /* An operator may pin the cache rate alone; input/output still resolve from
    * the catalog. */
   cfg.agents[1].price_cached_per_mtok = 0.5;
   assert(agent_resolved_price(&cfg.agents[1], &pin, &pout, &pcached) == 1);
   assert(pcached == 0.5 && pin == 10.0 && pout == 50.0);

   /* Every out-parameter is optional. */
   assert(agent_resolved_price(&cfg.agents[0], NULL, NULL, NULL) == 1);

   printf("  PASS: test_cached_price_axis\n");
}

/* Two agents with entirely disjoint DECLARED roles do NOT compete for any route.
 * Selection is declared-role only — there is no exec-role fallback that would
 * make every agent routable for review/code/test alike — so pricing them at
 * different tiers is not a conflict: routing never chooses between them. */
static void test_disjoint_declared_roles_do_not_compete(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "dear_at_tier0", "testvendor", "dear", 0);
   add_agent(&cfg, "cheap_at_tier1", "testvendor", "cheap", 1);
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "%s", "explain");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "%s", "summarize");

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   /* A shared declared role brings them back into competition. */
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "%s", "explain");
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 1);

   printf("  PASS: test_disjoint_declared_roles_do_not_compete\n");
}

/* Context-band pricing. Several providers charge more once a request exceeds a
 * threshold, so the base rate is only correct below the first band. The
 * authoritative threshold is cost.tiers[].tier.size — the registry's legacy
 * `context_over_200k` key name does NOT encode it (the real boundary is 272000
 * for gpt-5.6-sol and 512000 for MiniMax-M3), which is why that alias is
 * ignored entirely. */
static void test_price_bands_resolve_by_context(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "sol_like", "banded", "sol_like", 0);

   double in = 0.0, out = 0.0, cached = 0.0;

   /* Below the threshold: base band. */
   assert(agent_resolved_price_at_context(&cfg.agents[0], 1000, &in, &out, &cached) == 1);
   assert(in == 5.0 && out == 30.0 && cached == 0.5);

   /* Exactly AT the threshold is still base: the band applies ABOVE it. */
   assert(agent_resolved_price_at_context(&cfg.agents[0], 272000, &in, &out, &cached) == 1);
   assert(in == 5.0 && out == 30.0);

   /* One token above flips to the band. */
   assert(agent_resolved_price_at_context(&cfg.agents[0], 272001, &in, &out, &cached) == 1);
   assert(in == 10.0 && out == 45.0 && cached == 1.0);

   /* Unknown context resolves to base, as does the plain accessor. */
   assert(agent_resolved_price_at_context(&cfg.agents[0], 0, &in, &out, NULL) == 1);
   assert(in == 5.0);
   assert(agent_resolved_price(&cfg.agents[0], &in, &out, NULL) == 1);
   assert(in == 5.0 && out == 30.0);

   printf("  PASS: test_price_bands_resolve_by_context\n");
}

/* Bands are stored ascending regardless of the order the registry lists them,
 * so a lookup can take the last threshold the context exceeds. */
static void test_price_bands_sorted_ascending(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "two_band", "banded", "two_band", 0);

   model_capability_t cap;
   assert(model_capability_get("banded", "two_band", &cap) != 0);
   assert(cap.price_band_count == 2);
   /* Declared 500000 first, then 100000 — must be stored ascending. */
   assert(cap.price_bands[0].above_tokens == 100000);
   assert(cap.price_bands[1].above_tokens == 500000);

   double in = 0.0, out = 0.0;
   assert(agent_resolved_price_at_context(&cfg.agents[0], 50000, &in, &out, NULL) == 1);
   assert(in == 1.0);
   assert(agent_resolved_price_at_context(&cfg.agents[0], 200000, &in, &out, NULL) == 1);
   assert(in == 2.0);
   assert(agent_resolved_price_at_context(&cfg.agents[0], 900000, &in, &out, NULL) == 1);
   assert(in == 4.0);

   printf("  PASS: test_price_bands_sorted_ascending\n");
}

/* The lint must not issue definitive advice when the ordering DEPENDS on request
 * size: sol_like is cheaper than flat_mid at base ($5/$30 vs $7/$35) but dearer
 * above 272k ($10/$45). Telling the operator to re-tier would be wrong for part
 * of the operating range, so the pair is left alone. */
static void test_band_dependent_ordering_is_not_flagged(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* flat_mid at the CHEAPER tier while dearer at base => a base-only
    * comparison would report a conflict. */
   add_agent(&cfg, "flat_mid_t0", "banded", "flat_mid", 0);
   add_agent(&cfg, "sol_like_t1", "banded", "sol_like", 1);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_band_dependent_ordering_is_not_flagged\n");
}

/* When the dominance holds at EVERY band the finding is still reported: banded
 * pricing must not become a blanket excuse to say nothing. */
static void test_band_stable_ordering_is_still_flagged(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* flat_dear ($20/$90) is dearer than sol_like at base AND above 272k. */
   add_agent(&cfg, "flat_dear_t0", "banded", "flat_dear", 0);
   add_agent(&cfg, "sol_like_t1", "banded", "sol_like", 1);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 1);
   assert(strcmp(out[0].cheaper_tier_agent, "flat_dear_t0") == 0);

   printf("  PASS: test_band_stable_ordering_is_still_flagged\n");
}

/* A policy ceiling at or below the first threshold makes the band unreachable,
 * so the agent's price is effectively flat and definitive advice is fine. */
static void test_unreachable_band_is_ignored(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "capped", "banded", "sol_like", 0);
   cfg.agents[0].middleware.context_window = 272000; /* cannot exceed the band */
   assert(agent_has_reachable_price_band(&cfg.agents[0]) == 0);

   cfg.agents[0].middleware.context_window = 400000; /* can exceed it */
   assert(agent_has_reachable_price_band(&cfg.agents[0]) == 1);

   /* A full operator override makes the vendor schedule irrelevant. */
   cfg.agents[0].price_in_per_mtok = 1.0;
   cfg.agents[0].price_out_per_mtok = 2.0;
   assert(agent_has_reachable_price_band(&cfg.agents[0]) == 0);
   /* ...and that override applies at every band, not just the base. */
   double in = 0.0, out = 0.0;
   assert(agent_resolved_price_at_context(&cfg.agents[0], 900000, &in, &out, NULL) == 1);
   assert(in == 1.0 && out == 2.0);

   printf("  PASS: test_unreachable_band_is_ignored\n");
}

/* An ordering that flips and flips BACK must not be reported. flipflop is
 * $10 at base, $1 above 100k, $20 above 200k; flat_five is $5 everywhere. A
 * comparison of base and top alone sees "dearer at both ends" and would report
 * a conflict, but flipflop is CHEAPER between 100k and 200k, so the advice
 * would be wrong across part of the operating range. */
static void test_ordering_that_flips_back_is_not_flagged(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "flipflop_t0", "banded", "flipflop", 0);
   add_agent(&cfg, "flat_five_t1", "banded", "flat_five", 1);

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_ordering_that_flips_back_is_not_flagged\n");
}

/* An UNREACHABLE band must not suppress a conflict that holds everywhere the
 * agents can actually operate. Capping flipflop below its second boundary makes
 * the range 1..200000, where it is dearer at base and cheaper above 100k — still
 * not a clean dominance. Capping below the FIRST boundary makes it flat $10 vs
 * $5, which is a real conflict and must be reported. */
static void test_unreachable_band_does_not_suppress_valid_conflict(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "flipflop_t0", "banded", "flipflop", 0);
   add_agent(&cfg, "flat_five_t1", "banded", "flat_five", 1);
   /* Ceiling below the first boundary: only the base rate is reachable. */
   cfg.agents[0].middleware.context_window = 100000;
   cfg.agents[1].middleware.context_window = 100000;

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 1);
   assert(strcmp(out[0].cheaper_tier_agent, "flipflop_t0") == 0);

   printf("  PASS: test_unreachable_band_does_not_suppress_valid_conflict\n");
}

/* exec_roles govern tool exposure at execution time, not selection: two agents
 * that share only an exec role (custom or built-in) but declare disjoint roles
 * are never routed against each other, so pricing them at different tiers is not
 * a conflict. A shared exec role alone must NOT be reported. */
static void test_shared_exec_role_alone_does_not_compete(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   add_agent(&cfg, "dear_at_tier0", "testvendor", "dear", 0);
   add_agent(&cfg, "cheap_at_tier1", "testvendor", "cheap", 1);

   /* Disjoint declared roles, disjoint built-in exec roles, but a SHARED
    * custom exec role that both are routable for. */
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "%s", "explain");
   snprintf(cfg.agents[0].exec_roles[0], sizeof(cfg.agents[0].exec_roles[0]), "%s",
            "custom-migration");
   cfg.agents[0].exec_role_count = 1;
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "%s", "summarize");
   snprintf(cfg.agents[1].exec_roles[0], sizeof(cfg.agents[1].exec_roles[0]), "%s",
            "custom-migration");
   cfg.agents[1].exec_role_count = 1;

   agent_tier_conflict_t out[AGENT_TIER_LINT_MAX];
   assert(agent_tier_price_conflicts(&cfg, out, AGENT_TIER_LINT_MAX) == 0);

   printf("  PASS: test_shared_exec_role_alone_does_not_compete\n");
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
   test_disjoint_declared_roles_do_not_compete();
   test_operator_price_override_wins();
   test_price_override_per_axis();
   test_cached_price_axis();
   test_price_bands_resolve_by_context();
   test_price_bands_sorted_ascending();
   test_band_dependent_ordering_is_not_flagged();
   test_band_stable_ordering_is_still_flagged();
   test_unreachable_band_is_ignored();
   test_ordering_that_flips_back_is_not_flagged();
   test_unreachable_band_does_not_suppress_valid_conflict();
   test_shared_exec_role_alone_does_not_compete();
   test_guards();
   printf("agent_tier_lint: all tests passed\n");
   return 0;
}
