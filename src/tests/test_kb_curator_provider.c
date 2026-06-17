/* test_kb_curator_provider.c: stage->provider resolution (curator-llm-backend §2). */
#include "kb_curator_provider.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_tier_classification(void)
{
   /* Tier-A: mechanical extract/index. */
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_EXTRACT_DOCS) == KB_CURATOR_TIER_A);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_EXTRACT_CODE) == KB_CURATOR_TIER_A);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_INDEX_NARRATIVE) == KB_CURATOR_TIER_A);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_INDEX_CLAIMS) == KB_CURATOR_TIER_A);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_INDEX_CODE_UNIT) == KB_CURATOR_TIER_A);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_LINK_ARTIFACTS) == KB_CURATOR_TIER_A);
   /* Tier-B: reasoning / judge. */
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_JUDGE) == KB_CURATOR_TIER_B);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_RESOLVE_ENTITIES) == KB_CURATOR_TIER_B);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_DETECT_CONTRADICTIONS) == KB_CURATOR_TIER_B);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_SYNTHESIZE) == KB_CURATOR_TIER_B);
   assert(kb_curator_stage_tier(KB_CURATOR_STAGE_PROMOTE_ENTITY) == KB_CURATOR_TIER_B);
   printf("kb_curator_provider: tier classification ok\n");
}

static void test_unconfigured_idle(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg)); /* all providers empty */
   provider_def_t def;
   /* Tier-A unconfigured -> idle. */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &def) == 0);
   assert(def.base_url == NULL);
   /* Tier-B unconfigured -> idle. */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_JUDGE, &def) == 0);
   printf("kb_curator_provider: unconfigured tiers idle ok\n");
}

static void test_tier_a_resolves(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://curator:8080/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "gemma-4-e4b");
   /* no api_key -> keyless */

   provider_def_t def;
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &def) == 1);
   assert(strcmp(def.base_url, "http://curator:8080/v1") == 0);
   assert(strcmp(def.model, "gemma-4-e4b") == 0);
   assert(def.api_key == NULL); /* empty key => no bearer */
   assert(def.wire == PROVIDER_WIRE_OPENAI_CHAT);

   /* Tier-B still idle (no weak fallback to the Tier-A default). */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_SYNTHESIZE, &def) == 0);
   printf("kb_curator_provider: tier-A resolves, tier-B no fallback ok\n");
}

static void test_tier_b_resolves(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* both tiers configured, distinct providers + a tier-B key */
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://small:8080/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "small");
   snprintf(cfg.kb_curator_tier_b_base_url, sizeof(cfg.kb_curator_tier_b_base_url),
            "https://api.big/v1");
   snprintf(cfg.kb_curator_tier_b_model, sizeof(cfg.kb_curator_tier_b_model), "big-32b");
   snprintf(cfg.kb_curator_tier_b_api_key, sizeof(cfg.kb_curator_tier_b_api_key), "sk-secret");

   provider_def_t a, b;
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_CODE, &a) == 1);
   assert(strcmp(a.model, "small") == 0 && a.api_key == NULL);
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_JUDGE, &b) == 1);
   assert(strcmp(b.base_url, "https://api.big/v1") == 0);
   assert(strcmp(b.model, "big-32b") == 0);
   assert(b.api_key && strcmp(b.api_key, "sk-secret") == 0);
   printf("kb_curator_provider: tier-A and tier-B resolve independently ok\n");
}

int main(void)
{
   test_tier_classification();
   test_unconfigured_idle();
   test_tier_a_resolves();
   test_tier_b_resolves();
   printf("kb_curator_provider: all tests passed\n");
   return 0;
}
