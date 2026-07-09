/* kb_curator_provider.h: map a curator pipeline stage to its configured LLM
 * provider (curator-llm-backend §2). Pure resolution over config_t — given a
 * stage, classify it Tier-A (mechanical extract/index) or Tier-B (reasoning /
 * judge) and return the provider def for that tier, or report "idle" when the
 * tier is unconfigured. Tier-B has NO weak fallback to the Tier-A default: a
 * small model must never run the reasoning stages (it would poison the graph). */
#ifndef DEC_KB_CURATOR_PROVIDER_H
#define DEC_KB_CURATOR_PROVIDER_H 1

#include "config.h"          /* config_t */
#include "provider_client.h" /* provider_def_t */

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      /* Tier-A: mechanical, grammar-constrained, high-volume. */
      KB_CURATOR_STAGE_EXTRACT_DOCS = 0,
      KB_CURATOR_STAGE_EXTRACT_CODE,
      KB_CURATOR_STAGE_INDEX_NARRATIVE,
      KB_CURATOR_STAGE_INDEX_CLAIMS,
      KB_CURATOR_STAGE_INDEX_CODE_UNIT,
      KB_CURATOR_STAGE_LINK_ARTIFACTS,
      /* Tier-B: reasoning over extracted content; needs a capable model. */
      KB_CURATOR_STAGE_JUDGE,
      KB_CURATOR_STAGE_RESOLVE_ENTITIES,
      KB_CURATOR_STAGE_DETECT_CONTRADICTIONS,
      KB_CURATOR_STAGE_SYNTHESIZE,
      KB_CURATOR_STAGE_PROMOTE_ENTITY,
      /* Idle-time reflection synthesis (kb_reflection.c). A distinct Tier-B stage
       * from SYNTHESIZE so its provider resolution, and any future calibration /
       * bandit telemetry, stay isolated from the curator entity-synthesis stream
       * (which writes a different artifact kind on a different surface). */
      KB_CURATOR_STAGE_SYNTHESIZE_REFLECTION,
   } kb_curator_stage_t;

   typedef enum
   {
      KB_CURATOR_TIER_A = 0,
      KB_CURATOR_TIER_B = 1,
   } kb_curator_tier_t;

   /* Which tier a stage belongs to. */
   kb_curator_tier_t kb_curator_stage_tier(kb_curator_stage_t stage);

   /* Fill *out with the provider def for `stage`. Resolution per tier:
    *   1. tier config — Tier-A uses `provider.*`, Tier-B uses `tier_b.*`
    *      (never the Tier-A config; no weak fallback between config tiers);
    *   2. else, for TIER-A ONLY, the curator env LLM_ENDPOINT/LLM_MODEL/
    *      LLM_API_KEY (the bundled-model deployment — Gemma 3n E4B is a Tier-A
    *      model, so it must not serve the reasoning stages);
    *   3. else idle. Tier-B has NO env fallback: it needs a capable provider via
    *      tier_b.* config, or it stays idle.
    * Returns 1 when configured (out filled; base_url non-empty), 0 when idle
    * (out zeroed — the stage must not run).
    *
    * Lifetime: out->base_url/model/api_key alias strings owned elsewhere — either
    * inside *cfg (config path) or the process environment (env path). Keep *cfg
    * alive and don't mutate the environment (setenv/putenv) while using *out; the
    * def is a borrowed view, not a copy. out->api_key is NULL when no key applies
    * (keyless local endpoint); provider_client treats NULL as "no bearer". */
   int kb_curator_provider_for_stage(const config_t *cfg, kb_curator_stage_t stage,
                                     provider_def_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CURATOR_PROVIDER_H */
