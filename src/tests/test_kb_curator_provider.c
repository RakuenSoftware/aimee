/* test_kb_curator_provider.c: stage->provider resolution (curator-llm-backend §2). */
#include "kb_curator_provider.h"
#include "support/curator_config_stub.h"
#include "runtime_secret.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The resolver falls back to SYNTHESIS_ENDPOINT/LLM_MODEL/LLM_API_KEY env; clear them
 * so the config-path tests are deterministic regardless of the ambient env. */
static void clear_llm_env(void)
{
   unsetenv("SYNTHESIS_ENDPOINT");
   unsetenv("SYNTHESIS_MODEL");
   unsetenv("SYNTHESIS_API_KEY");
   unsetenv("SYNTHESIS_ENDPOINT");
   unsetenv("SYNTHESIS_MODEL");
   runtime_secret_remove("SYNTHESIS_API_KEY");
   unsetenv("SYNTHESIS_AUTH_REQUIRED");
}

static void test_one_provider_for_every_stage(void)
{
   /* This asserted a Tier-A/Tier-B classification per stage. There is one synthesis
    * role now, so the property worth pinning is that a MECHANICAL stage and a
    * REASONING stage resolve the SAME provider. */
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "https://api.one/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "one-model");
   provider_def_owned_t a, b;
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_JUDGE, &b) == 1);
   assert(strcmp(a.def.base_url, b.def.base_url) == 0);
   assert(strcmp(a.def.model, b.def.model) == 0);
   /* Thinking is a global operator switch, never implied by the stage. */
   assert(a.def.disable_thinking == b.def.disable_thinking);
   printf("kb_curator_provider: one provider for every stage ok\n");
}

static void test_unconfigured_idle(void)
{
   memset(&cfg, 0, sizeof(cfg)); /* all providers empty */
   provider_def_owned_t def;
   /* A mechanical stage, unconfigured -> idle. */
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &def) == 0);
   assert(def.def.base_url == NULL);
   /* A reasoning stage, unconfigured -> idle. */
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_JUDGE, &def) == 0);
   printf("kb_curator_provider: an unconfigured provider leaves every stage idle ok\n");
}

static void test_provider_resolves(void)
{
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://curator:8080/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "gemma-4-e4b");
   /* no api_key -> keyless */
   /* The stub zeroes cfg, so it must opt in to the shipped default explicitly:
    * synthesis_thinking is "true" in the config defaults table. */
   cfg.synthesis_thinking = 1;

   provider_def_owned_t def;
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &def) == 1);
   assert(strcmp(def.def.base_url, "http://curator:8080/v1") == 0);
   assert(strcmp(def.def.model, "gemma-4-e4b") == 0);
   assert(def.def.api_key == NULL); /* empty key => no bearer */
   assert(def.def.wire == PROVIDER_WIRE_OPENAI_CHAT);
   /* Nothing about the stage suppresses thinking any more: the flag tracks the
    * operator switch alone, and on the default it is off. Suppressing it measured
    * a 0.09 F1 loss for gemma-4-E4B by degrading output-contract adherence — see
    * the note in kb_curator_provider_for_stage. */
   assert(def.def.disable_thinking == 0);

   /* Tier-B still idle (no weak fallback to the Tier-A default). */
   /* A reasoning stage takes the SAME provider. It used to stay idle here, because
    * it refused to fall back to what was then the small Tier-A model. */
   provider_def_owned_t reasoning;
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &reasoning) == 1);
   assert(strcmp(reasoning.def.base_url, def.def.base_url) == 0);
   printf("kb_curator_provider: every stage resolves the one provider ok\n");
}

static void test_stage_families_share_one_provider(void)
{
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://small:8080/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "small");
   provider_def_owned_t a, b;
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_CODE, &a) == 1);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_JUDGE, &b) == 1);
   /* This asserted two DISTINCT endpoints from provider.* and tier_b.* — exactly
    * the split that was removed. */
   assert(strcmp(a.def.base_url, b.def.base_url) == 0);
   assert(strcmp(a.def.model, b.def.model) == 0);
   printf("kb_curator_provider: mechanical and reasoning stages share one provider ok\n");
}

/* Thinking is one global switch the operator owns, shipped on (synthesis_thinking
 * defaults to "true"). Nothing about the stage may influence it: an earlier design
 * suppressed thinking for the mechanical stages, which measured worse. Both states
 * are pinned here because only the operator may turn it off. */
static void test_thinking_is_one_global_operator_switch(void)
{
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://curator:8080/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "gemma-4-e4b");

   const kb_curator_stage_t stages[] = {KB_CURATOR_STAGE_EXTRACT_DOCS,
                                        KB_CURATOR_STAGE_EXTRACT_CODE, KB_CURATOR_STAGE_JUDGE,
                                        KB_CURATOR_STAGE_SYNTHESIZE};

   /* On (the shipped default): no stage suppresses thinking. */
   cfg.synthesis_thinking = 1;
   for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
   {
      provider_def_owned_t def;
      assert(kb_curator_provider_for_stage(stages[i], &def) == 1);
      assert(def.def.disable_thinking == 0);
   }

   /* Off: the operator's choice reaches every stage, mechanical and reasoning alike. */
   cfg.synthesis_thinking = 0;
   for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
   {
      provider_def_owned_t def;
      assert(kb_curator_provider_for_stage(stages[i], &def) == 1);
      assert(def.def.disable_thinking == 1);
   }

   printf("kb_curator_provider: thinking is one global operator switch, on by default ok\n");
}

/* SYNTHESIS_ENDPOINT is ingested into config and drives EVERY stage. It used to be
 * a Tier-A-only env bridge that the reasoning stages deliberately refused, so the
 * bundled small model could not serve them. */
static void test_env_bridge(void)
{
   clear_llm_env(); /* start from a known-clean env, not just clean up at the end */
   memset(&cfg, 0, sizeof(cfg));
   setenv("SYNTHESIS_ENDPOINT", "http://bundled:8080/v1", 1);
   setenv("SYNTHESIS_MODEL", "gemma-3n-e4b", 1);
   setenv("SYNTHESIS_API_KEY", "", 1); /* keyless local */

   provider_def_owned_t a, b;
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.def.base_url, "http://bundled:8080/v1") == 0);
   assert(strcmp(a.def.model, "gemma-3n-e4b") == 0);
   assert(a.def.api_key == NULL); /* empty env key => keyless */

   /* A reasoning stage now takes the same endpoint instead of staying idle. */
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(strcmp(b.def.base_url, a.def.base_url) == 0);
   assert(strcmp(b.def.model, a.def.model) == 0);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_JUDGE, &b) == 1);
   assert(strcmp(b.def.base_url, a.def.base_url) == 0);

   /* provider.* config still outranks the environment, for every stage. */
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "https://api.configured/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "configured");
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(strcmp(b.def.base_url, "https://api.configured/v1") == 0);

   /* No env, no config => idle. */
   clear_llm_env();
   memset(&cfg, 0, sizeof(cfg));
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 0);
   printf("kb_curator_provider: one endpoint from env drives every stage ok"
          "\n");
}

/* SYNTHESIS_ENDPOINT drives every stage via {SYNTHESIS_ENDPOINT}/v1, deriving the
 * chat endpoint + a default model. A config provider still wins. */
static void test_aimee_llm_url(void)
{
   clear_llm_env();
   memset(&cfg, 0, sizeof(cfg));
   setenv("SYNTHESIS_ENDPOINT", "http://10.100.0.1:8742", 1);

   provider_def_owned_t a, b;
   /* Tier-A derives {url}/v1 + default model, keyless. */
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.def.base_url, "http://10.100.0.1:8742/v1") == 0);
   assert(strcmp(a.def.model, "aimee-synth") == 0);
   assert(a.def.api_key == NULL); /* keyless container => no bearer */
   /* Tier-B also resolves to the same capable container (the one env fallback
    * Tier-B accepts). */
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(strcmp(b.def.base_url, "http://10.100.0.1:8742/v1") == 0);
   assert(strcmp(b.def.model, "aimee-synth") == 0);

   /* A managed KB authenticates every synth request with its service identity. */
   assert(runtime_secret_store("SYNTHESIS_API_KEY", "kb-to-llm-service-token") == 0);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(b.def.api_key && strcmp(b.def.api_key, "kb-to-llm-service-token") == 0);

   /* Managed mode must not silently downgrade the unified gateway to keyless. */
   runtime_secret_remove("SYNTHESIS_API_KEY");
   setenv("SYNTHESIS_AUTH_REQUIRED", "1", 1);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 0);
   assert(runtime_secret_store("SYNTHESIS_API_KEY", "kb-to-llm-service-token") == 0);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);

   /* Trailing slash and an already-/v1 URL both normalize to exactly one /v1. */
   setenv("SYNTHESIS_ENDPOINT", "http://host:8742/", 1);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_JUDGE, &b) == 1);
   assert(strcmp(b.def.base_url, "http://host:8742/v1") == 0);
   setenv("SYNTHESIS_ENDPOINT", "http://host:8742/v1", 1);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_JUDGE, &b) == 1);
   assert(strcmp(b.def.base_url, "http://host:8742/v1") == 0);

   /* SYNTHESIS_MODEL overrides the default model label. */
   setenv("SYNTHESIS_ENDPOINT", "http://host:8742", 1);
   setenv("SYNTHESIS_MODEL", "gemma-4-12b", 1);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.def.model, "gemma-4-12b") == 0);

   /* A config provider still wins over SYNTHESIS_ENDPOINT. */
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://pinned:9000/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "pinned");
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.def.base_url, "http://pinned:9000/v1") == 0);

   clear_llm_env();
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.synthesis_endpoint, sizeof(cfg.synthesis_endpoint), "http://synth.internal:9100");

   /* The configured field alone resolves every stage — no env var involved. */
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &a) == 1);
   assert(strcmp(a.def.base_url, "http://synth.internal:9100/v1") == 0);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(strcmp(b.def.base_url, "http://synth.internal:9100/v1") == 0);

   /* The same normalization applies to the field, not just the env var. */
   snprintf(cfg.synthesis_endpoint, sizeof(cfg.synthesis_endpoint),
            "http://synth.internal:9100/v1/");
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(strcmp(b.def.base_url, "http://synth.internal:9100/v1") == 0);

   /* SYNTHESIS_ENDPOINT outranks the stored field: a containerized deploy sets the
    * environment, not a writable aimee.yaml. */
   snprintf(cfg.synthesis_endpoint, sizeof(cfg.synthesis_endpoint), "http://from-config:9100");
   setenv("SYNTHESIS_ENDPOINT", "http://from-env:8742", 1);
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 1);
   assert(strcmp(b.def.base_url, "http://from-env:8742/v1") == 0);

   /* A value that names nothing must not resolve to a bare "/v1". */
   unsetenv("SYNTHESIS_ENDPOINT");
   snprintf(cfg.synthesis_endpoint, sizeof(cfg.synthesis_endpoint), "///");
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_SYNTHESIZE, &b) == 0);

   clear_llm_env();
   printf("kb_curator_provider: synth endpoint resolves from config ok\n");

   clear_llm_env();
   printf("kb_curator_provider: SYNTHESIS_ENDPOINT drives every stage ok\n");
}

int main(void)
{
   clear_llm_env();
   test_one_provider_for_every_stage();
   test_unconfigured_idle();
   test_provider_resolves();
   test_stage_families_share_one_provider();
   test_thinking_is_one_global_operator_switch();
   test_env_bridge();
   test_aimee_llm_url();
   printf("kb_curator_provider: all tests passed\n");
   return 0;
}
