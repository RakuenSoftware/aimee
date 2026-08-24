/* kb_curator_provider.h: map a curator pipeline stage to its configured LLM
 * provider (curator-llm-backend §2). Pure resolution over legacy_config_record — given a
 * stage, classify it Tier-A (mechanical extract/index) or Tier-B (reasoning /
 * judge) and return the provider def for that tier, or report "idle" when the
 * tier is unconfigured. Tier-B has NO weak fallback to the Tier-A default: a
 * small model must never run the reasoning stages (it would poison the graph). */
#ifndef DEC_KB_CURATOR_PROVIDER_H
#define DEC_KB_CURATOR_PROVIDER_H 1

#include "config.h"          /* legacy_config_record */
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
      /* Idle-time reflection synthesis (kb_reflection.c). Distinct from SYNTHESIZE
       * so any future calibration / bandit telemetry stays isolated from the
       * curator entity-synthesis stream (which writes a different artifact kind on
       * a different surface). */
      KB_CURATOR_STAGE_SYNTHESIZE_REFLECTION,
   } kb_curator_stage_t;

   /* Fill *out with the provider def for `stage`. Resolution, the same for every
    * stage:
    *   1. `provider.*` config;
    *   2. else the configured synthesis endpoint (SYNTHESIS_ENDPOINT, ingested by
    *      config and normalized in config_synth_chat_endpoint_current());
    *   3. else idle.
    *
    * `stage` NO LONGER SELECTS A PROVIDER. It used to: `provider.*` served the
    * mechanical stages and `tier_b.*` the reasoning ones, which could not fall back
    * to the mechanical provider because a weak model on the reasoning stages
    * poisons the graph. There is one synthesis provider now, so every stage
    * resolves identically and the parameter is kept only so callers still say which
    * stage they are, for logging and future per-stage policy.
    *
    * Returns 1 when configured (out filled; base_url non-empty), 0 when idle
    * (out zeroed — the stage must not run).
    *
    * Lifetime: *out OWNS its strings (see provider_def_owned_t) — nothing external
    * has to be kept alive, and mutating the environment afterwards cannot change
    * what was resolved. Pass &out->def wherever a provider_def_t is wanted, and do
    * not copy the struct by value (def would still point into the original).
    * out->def.api_key is NULL when no key applies (keyless local endpoint);
    * provider_client treats NULL as "no bearer". */
   int kb_curator_provider_for_stage(kb_curator_stage_t stage, provider_def_owned_t *out);

   /* True when an error means the configured provider would not serve work now
    * (as opposed to rejecting one malformed job). Recognizes both the direct
    * provider-client wording and the bundled llm-chat.py sidecar envelope. */
   int kb_curator_error_is_provider_unavailable(const char *error_msg);

   /* Process-wide outage gate shared by every curator LLM stage. A per-job
    * next_attempt_at prevents one row from spinning, but it does not prevent a
    * large queue from walking fresh rows while the provider circuit is open.
    * note() applies a bounded exponential cooldown, active() lets workers avoid
    * claiming during it, and recovered() resets the curve after a successful
    * provider response. */
   int kb_curator_provider_backoff_active(void);
   void kb_curator_provider_backoff_note(void);
   void kb_curator_provider_backoff_recovered(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CURATOR_PROVIDER_H */
