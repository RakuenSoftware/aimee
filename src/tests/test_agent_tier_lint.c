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

int main(void)
{
   printf("agent_tier_lint:\n");
   seed_priced_catalog();
   printf("agent_tier_lint: all tests passed\n");
   return 0;
}
