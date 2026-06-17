/* kb_curator_provider.c: stage -> configured curator LLM provider. See header. */

#include "kb_curator_provider.h"

#include <string.h>

kb_curator_tier_t kb_curator_stage_tier(kb_curator_stage_t stage)
{
   switch (stage)
   {
   case KB_CURATOR_STAGE_JUDGE:
   case KB_CURATOR_STAGE_RESOLVE_ENTITIES:
   case KB_CURATOR_STAGE_DETECT_CONTRADICTIONS:
   case KB_CURATOR_STAGE_SYNTHESIZE:
   case KB_CURATOR_STAGE_PROMOTE_ENTITY:
      return KB_CURATOR_TIER_B;
   default:
      return KB_CURATOR_TIER_A;
   }
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

   /* An empty base_url means that tier is unconfigured — the stage stays idle.
    * Tier-B intentionally does NOT fall back to the Tier-A default. */
   if (!base_url[0])
      return 0;

   out->base_url = base_url;
   out->model = model;
   out->api_key = api_key[0] ? api_key : NULL; /* keyless local endpoint => no bearer */
   out->wire = PROVIDER_WIRE_OPENAI_CHAT;
   out->temperature = -1.0; /* let the provider default */
   return 1;
}
