/* kb_curator_provider.c: stage -> configured curator LLM provider. See header. */

#include "kb_curator_provider.h"

#include <stdio.h>  /* snprintf — AIMEE_LLM_URL -> {base}/v1 */
#include <stdlib.h> /* getenv */
#include <string.h>

/* Model name sent to the unified aimee-llm gateway when none is configured. The
 * gateway serves a single baked model and ignores the request's model field, so
 * this is a label; an operator can override it with AIMEE_LLM_MODEL. */
#define AIMEE_LLM_DEFAULT_MODEL "aimee-synth"

kb_curator_tier_t kb_curator_stage_tier(kb_curator_stage_t stage)
{
   switch (stage)
   {
   /* Tier-A: mechanical, grammar-constrained extract/index. */
   case KB_CURATOR_STAGE_EXTRACT_DOCS:
   case KB_CURATOR_STAGE_EXTRACT_CODE:
   case KB_CURATOR_STAGE_INDEX_NARRATIVE:
   case KB_CURATOR_STAGE_INDEX_CLAIMS:
   case KB_CURATOR_STAGE_INDEX_CODE_UNIT:
   case KB_CURATOR_STAGE_LINK_ARTIFACTS:
      return KB_CURATOR_TIER_A;
   /* Tier-B: reasoning / judge. */
   case KB_CURATOR_STAGE_JUDGE:
   case KB_CURATOR_STAGE_RESOLVE_ENTITIES:
   case KB_CURATOR_STAGE_DETECT_CONTRADICTIONS:
   case KB_CURATOR_STAGE_SYNTHESIZE:
   case KB_CURATOR_STAGE_PROMOTE_ENTITY:
      return KB_CURATOR_TIER_B;
   }
   /* Fail safe: an unclassified / future stage routes to the capable tier (which
    * simply idles when tier_b is unconfigured), NEVER silently to the small
    * Tier-A model — a weak model on a reasoning stage poisons the graph. */
   return KB_CURATOR_TIER_B;
}

int kb_curator_provider_for_stage(const config_t *cfg, kb_curator_stage_t stage,
                                  provider_def_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!cfg || !out)
      return 0;

   const char *base_url;
   const char *model;
   const char *api_key;
   if (kb_curator_stage_tier(stage) == KB_CURATOR_TIER_B)
   {
      base_url = cfg->kb_curator_tier_b_base_url;
      model = cfg->kb_curator_tier_b_model;
      api_key = cfg->kb_curator_tier_b_api_key;
   }
   else
   {
      base_url = cfg->kb_curator_provider_base_url;
      model = cfg->kb_curator_provider_model;
      api_key = cfg->kb_curator_provider_api_key;
   }

   /* Env fallback when a tier has no config provider, in precedence order:
    *
    *  1. AIMEE_LLM_URL — the single "point me at the unified aimee-llm container"
    *     knob. It is the operator explicitly designating one CAPABLE container
    *     for the whole LLM surface, so it drives BOTH tiers via that container's
    *     OpenAI chat endpoint ({AIMEE_LLM_URL}/v1). This is the only env fallback
    *     Tier-B accepts — see (2).
    *  2. LLM_ENDPOINT — TIER-A ONLY. It is the small-model interface (the
    *     zero-config CPU sibling points Tier-A here); letting a small model serve
    *     the reasoning stages is the weak-model-poisons-the-graph case the tier
    *     split guards against, so Tier-B never falls back to it.
    *
    * A config provider for the tier (checked above) still wins. Whole-provider
    * fallback: base+model+key move as a unit. */
   static __thread char synth_url[512];
   if (!base_url[0])
   {
      const char *llm_url = getenv("AIMEE_LLM_URL");
      if (llm_url && llm_url[0])
      {
         /* AIMEE_LLM_URL is the container base (no /v1); derive the chat endpoint
          * unless the operator already included it. */
         size_t n = strlen(llm_url);
         while (n > 0 && llm_url[n - 1] == '/')
            n--;
         int has_v1 = (n >= 3 && strncmp(llm_url + n - 3, "/v1", 3) == 0);
         snprintf(synth_url, sizeof(synth_url), "%.*s%s", (int)n, llm_url, has_v1 ? "" : "/v1");
         const char *env_model = getenv("AIMEE_LLM_MODEL");
         base_url = synth_url;
         model = (env_model && env_model[0]) ? env_model : AIMEE_LLM_DEFAULT_MODEL;
         api_key = ""; /* keyless container on a trusted network */
      }
      else if (kb_curator_stage_tier(stage) == KB_CURATOR_TIER_A)
      {
         const char *env_ep = getenv("LLM_ENDPOINT");
         if (!env_ep || !env_ep[0])
            return 0; /* neither config nor env — the stage stays idle */
         const char *env_model = getenv("LLM_MODEL");
         const char *env_key = getenv("LLM_API_KEY");
         base_url = env_ep;
         model = (env_model && env_model[0]) ? env_model : "";
         api_key = (env_key && env_key[0]) ? env_key : "";
      }
      else
      {
         return 0; /* Tier-B with no config and no AIMEE_LLM_URL — stays idle */
      }
   }

   out->base_url = base_url;
   out->model = model;
   out->api_key = api_key[0] ? api_key : NULL; /* keyless local endpoint => no bearer */
   out->wire = PROVIDER_WIRE_OPENAI_CHAT;
   out->temperature = -1.0; /* let the provider default */
   /* Tier-A is mechanical, grammar-constrained extraction/indexing — it does not
    * need (and is hurt by) a reasoning model's chain-of-thought: the reasoning pass
    * adds latency at drain volume and, worse, can consume the output budget so the
    * JSON answer comes back truncated/empty (observed: memory-fact extraction landed
    * 0 facts). Skip thinking for Tier-A; Tier-B (judge/synthesize) keeps it. */
   out->disable_thinking = (kb_curator_stage_tier(stage) == KB_CURATOR_TIER_A);
   return 1;
}
