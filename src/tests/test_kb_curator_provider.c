/* test_kb_curator_provider.c: stage->provider resolution (curator-llm-backend §2). */
#include "kb_curator_provider.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The resolver falls back to LLM_ENDPOINT/LLM_MODEL/LLM_API_KEY env; clear them
 * so the config-path tests are deterministic regardless of the ambient env. */
static void clear_llm_env(void)
{
   unsetenv("LLM_ENDPOINT");
   unsetenv("LLM_MODEL");
   unsetenv("LLM_API_KEY");
   unsetenv("AIMEE_LLM_URL");
   unsetenv("AIMEE_LLM_MODEL");
}

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
   assert(def.disable_thinking == 1); /* Tier-A skips the reasoning pass */

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
   assert(a.disable_thinking == 1); /* Tier-A: reasoning off */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_JUDGE, &b) == 1);
   assert(strcmp(b.base_url, "https://api.big/v1") == 0);
   assert(strcmp(b.model, "big-32b") == 0);
   assert(b.api_key && strcmp(b.api_key, "sk-secret") == 0);
   assert(b.disable_thinking == 0); /* Tier-B keeps its reasoning pass */
   printf("kb_curator_provider: tier-A and tier-B resolve independently ok\n");
}

/* LLM_ENDPOINT/LLM_MODEL/LLM_API_KEY drive TIER-A only (the bundled Gemma E4B is
 * a Tier-A model). Tier-B has no env fallback — it needs tier_b.* config or stays
 * idle, so a Tier-A model never runs the reasoning stages. */
static void test_env_bridge(void)
{
   clear_llm_env(); /* start from a known-clean env, not just clean up at the end */
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   setenv("LLM_ENDPOINT", "http://bundled:8080/v1", 1);
   setenv("LLM_MODEL", "gemma-3n-e4b", 1);
   setenv("LLM_API_KEY", "", 1); /* keyless local */

   provider_def_t a, b;
   /* Tier-A from env. */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.base_url, "http://bundled:8080/v1") == 0);
   assert(strcmp(a.model, "gemma-3n-e4b") == 0);
   assert(a.api_key == NULL); /* empty env key => keyless */
   /* Tier-B does NOT take the Tier-A env endpoint — it stays idle (out zeroed).
    * Check two distinct Tier-B stages, not just one. */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_SYNTHESIZE, &b) == 0);
   assert(b.base_url == NULL && b.model == NULL); /* idle => out zeroed */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_JUDGE, &b) == 0);

   /* A config tier_b enables Tier-B (capable model); Tier-A still uses env. */
   snprintf(cfg.kb_curator_tier_b_base_url, sizeof(cfg.kb_curator_tier_b_base_url),
            "https://api.big/v1");
   snprintf(cfg.kb_curator_tier_b_model, sizeof(cfg.kb_curator_tier_b_model), "big-32b");
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(strcmp(b.base_url, "https://api.big/v1") == 0 && strcmp(b.model, "big-32b") == 0);
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.base_url, "http://bundled:8080/v1") == 0); /* env still */

   /* No env, no config => idle. */
   clear_llm_env();
   memset(&cfg, 0, sizeof(cfg));
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 0);
   printf("kb_curator_provider: env bridge (Tier-A only; Tier-B needs config) ok\n");
}

/* AIMEE_LLM_URL — the single "capable container" knob — drives BOTH tiers via
 * {AIMEE_LLM_URL}/v1, deriving the chat endpoint + a default model (keyless). It
 * is the only env fallback Tier-B accepts. A config provider still wins. */
static void test_aimee_llm_url(void)
{
   clear_llm_env();
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   setenv("AIMEE_LLM_URL", "http://10.100.0.1:8742", 1);

   provider_def_t a, b;
   /* Tier-A derives {url}/v1 + default model, keyless. */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.base_url, "http://10.100.0.1:8742/v1") == 0);
   assert(strcmp(a.model, "aimee-synth") == 0);
   assert(a.api_key == NULL); /* keyless container => no bearer */
   /* Tier-B also resolves to the same capable container (the one env fallback
    * Tier-B accepts). */
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(strcmp(b.base_url, "http://10.100.0.1:8742/v1") == 0);
   assert(strcmp(b.model, "aimee-synth") == 0);

   /* Trailing slash and an already-/v1 URL both normalize to exactly one /v1. */
   setenv("AIMEE_LLM_URL", "http://host:8742/", 1);
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_JUDGE, &b) == 1);
   assert(strcmp(b.base_url, "http://host:8742/v1") == 0);
   setenv("AIMEE_LLM_URL", "http://host:8742/v1", 1);
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_JUDGE, &b) == 1);
   assert(strcmp(b.base_url, "http://host:8742/v1") == 0);

   /* AIMEE_LLM_MODEL overrides the default model label. */
   setenv("AIMEE_LLM_URL", "http://host:8742", 1);
   setenv("AIMEE_LLM_MODEL", "gemma-4-12b", 1);
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.model, "gemma-4-12b") == 0);

   /* A config provider still wins over AIMEE_LLM_URL. */
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://pinned:9000/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "pinned");
   assert(kb_curator_provider_for_stage(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.base_url, "http://pinned:9000/v1") == 0);

   clear_llm_env();
   printf("kb_curator_provider: AIMEE_LLM_URL drives both tiers ok\n");
}

int main(void)
{
   clear_llm_env();
   test_tier_classification();
   test_unconfigured_idle();
   test_tier_a_resolves();
   test_tier_b_resolves();
   test_env_bridge();
   test_aimee_llm_url();
   printf("kb_curator_provider: all tests passed\n");
   return 0;
}
