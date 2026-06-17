/* kb_curator_provider.c: stage -> configured curator LLM provider. See header. */

#include "kb_curator_provider.h"

#include <stdlib.h> /* getenv */
#include <string.h>

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

   /* Env bridge: when a tier has no config provider, fall back to the curator's
    * LLM_ENDPOINT/LLM_MODEL/LLM_API_KEY env (the interface the bundled-Gemma
    * deployment and the python sidecars use). This applies to TIER-A ONLY: the
    * bundled model (Gemma 3n E4B) is a Tier-A model, so letting it serve the
    * reasoning stages would be exactly the weak-model-poisons-the-graph case §2a
    * guards against. Tier-B takes a capable provider from tier_b.* config or it
    * stays idle — no env fallback. A config provider for the tier still takes
    * precedence. Whole-provider fallback (base+model+key as a unit). */
   if (!base_url[0])
   {
      if (kb_curator_stage_tier(stage) != KB_CURATOR_TIER_A)
         return 0; /* Tier-B: capable provider via tier_b.* config, or idle */
      const char *env_ep = getenv("LLM_ENDPOINT");
      if (!env_ep || !env_ep[0])
         return 0; /* neither config nor env — the stage stays idle */
      const char *env_model = getenv("LLM_MODEL");
      const char *env_key = getenv("LLM_API_KEY");
      base_url = env_ep;
      model = (env_model && env_model[0]) ? env_model : "";
      api_key = (env_key && env_key[0]) ? env_key : "";
   }

   out->base_url = base_url;
   out->model = model;
   out->api_key = api_key[0] ? api_key : NULL; /* keyless local endpoint => no bearer */
   out->wire = PROVIDER_WIRE_OPENAI_CHAT;
   out->temperature = -1.0; /* let the provider default */
   return 1;
}
