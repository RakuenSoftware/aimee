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
   /* Tier-A unconfigured -> idle. */
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &def) == 0);
   assert(def.def.base_url == NULL);
   /* Tier-B unconfigured -> idle. */
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_JUDGE, &def) == 0);
   printf("kb_curator_provider: unconfigured tiers idle ok\n");
}

static void test_provider_resolves(void)
{
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://curator:8080/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "gemma-4-e4b");
   /* no api_key -> keyless */

   provider_def_owned_t def;
   assert(kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &def) == 1);
   assert(strcmp(def.def.base_url, "http://curator:8080/v1") == 0);
   assert(strcmp(def.def.model, "gemma-4-e4b") == 0);
   assert(def.def.api_key == NULL); /* empty key => no bearer */
   assert(def.def.wire == PROVIDER_WIRE_OPENAI_CHAT);
   assert(def.def.disable_thinking == 1); /* Tier-A skips the reasoning pass */

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
   printf("kb_curator_provider: one endpoint from env drives every stage ok" "\n");
}

/* SYNTHESIS_ENDPOINT — the single "capable container" knob — drives BOTH tiers via
 * {SYNTHESIS_ENDPOINT}/v1, deriving the chat endpoint + a default model. It
 * is the only env fallback Tier-B accepts. A config provider still wins. */
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

   /* The configured field alone resolves both tiers — no env var involved. */
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
   printf("kb_curator_provider: SYNTHESIS_ENDPOINT drives both tiers ok\n");
}

int main(void)
{
   clear_llm_env();
   test_one_provider_for_every_stage();
   test_unconfigured_idle();
   test_provider_resolves();
   test_stage_families_share_one_provider();
   test_env_bridge();
   test_aimee_llm_url();
   printf("kb_curator_provider: all tests passed\n");
   return 0;
}
