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
   } kb_curator_stage_t;

   typedef enum
   {
      KB_CURATOR_TIER_A = 0,
      KB_CURATOR_TIER_B = 1,
   } kb_curator_tier_t;

   /* Which tier a stage belongs to. */
   kb_curator_tier_t kb_curator_stage_tier(kb_curator_stage_t stage);

   /* Fill *out with the provider def for `stage`: Tier-A stages use the default
    * `provider.*` config, Tier-B stages use `tier_b.*` (never the Tier-A default).
    * Returns 1 when that tier is configured (out filled; base_url non-empty), 0
    * when it is unconfigured (out zeroed — the stage must stay idle). The const
    * char* fields in *out point into *cfg, so keep cfg alive while using *out. */
   int kb_curator_provider_for_stage(const config_t *cfg, kb_curator_stage_t stage,
                                     provider_def_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CURATOR_PROVIDER_H */
