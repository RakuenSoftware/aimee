/* kb_curator_provider.c: stage -> configured curator LLM provider. See header. */

#include "kb_curator_provider.h"
#include "config_database.h" /* config_synth_chat_endpoint — the one synth-address resolver */
#include "runtime_secret.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h> /* getenv */
#include <string.h>
#include <time.h>

/* Model name sent to the unified aimee-llm gateway when none is configured. The
 * gateway serves a single baked model and ignores the request's model field, so
 * this is a label; an operator can override it with AIMEE_LLM_MODEL. */
#define AIMEE_LLM_DEFAULT_MODEL "aimee-synth"

/* An outage is global to the provider, not local to one queue row. Without a
 * process-wide gate the per-row retry timestamp merely makes a large backlog
 * advance to the next fresh row while the provider circuit is open. */
#define KB_CURATOR_PROVIDER_BACKOFF_MIN_S 30
#define KB_CURATOR_PROVIDER_BACKOFF_MAX_S 300

static pthread_mutex_t provider_backoff_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t provider_backoff_until = 0;
static unsigned int provider_backoff_failures = 0;

int kb_curator_error_is_provider_unavailable(const char *error_msg)
{
   if (!error_msg || !error_msg[0])
      return 0;
   /* The bundled llm-chat.py reports transport timeouts as
    * "request to <url> failed after ...: timed out", without an HTTP status.
    * That is an availability failure, not evidence that this queue row is
    * poisonous. Keep the match contextual so an arbitrary local sidecar that
    * merely says "timed out" still consumes its normal attempt budget. */
   int bundled_transport_timeout =
       strstr(error_msg, "request to ") != NULL && strstr(error_msg, "timed out") != NULL;
   return strstr(error_msg, "provider HTTP 503") != NULL ||
          strstr(error_msg, "provider HTTP 429") != NULL ||
          strstr(error_msg, "provider HTTP -1") != NULL ||
          strstr(error_msg, "HTTP 503 from") != NULL ||
          strstr(error_msg, "HTTP 429 from") != NULL ||
          strstr(error_msg, "\"code\": \"provider_unavailable\"") != NULL ||
          strstr(error_msg, "\"code\":\"provider_unavailable\"") != NULL ||
          strstr(error_msg, "upstream circuit is open") != NULL || bundled_transport_timeout;
}

int kb_curator_provider_backoff_active(void)
{
   pthread_mutex_lock(&provider_backoff_lock);
   int active = provider_backoff_until > time(NULL);
   pthread_mutex_unlock(&provider_backoff_lock);
   return active;
}

void kb_curator_provider_backoff_note(void)
{
   pthread_mutex_lock(&provider_backoff_lock);
   time_t now = time(NULL);
   /* Several workers can observe the same outage concurrently. They are one
    * failure epoch, not several exponential steps; only a failed probe after a
    * cooldown expires lengthens the next cooldown. */
   if (provider_backoff_until > now)
   {
      pthread_mutex_unlock(&provider_backoff_lock);
      return;
   }
   if (provider_backoff_failures < 32)
      provider_backoff_failures++;
   unsigned int shift = provider_backoff_failures > 1 ? provider_backoff_failures - 1 : 0;
   if (shift > 4)
      shift = 4;
   int delay = KB_CURATOR_PROVIDER_BACKOFF_MIN_S << shift;
   if (delay > KB_CURATOR_PROVIDER_BACKOFF_MAX_S)
      delay = KB_CURATOR_PROVIDER_BACKOFF_MAX_S;
   provider_backoff_until = now + delay;
   pthread_mutex_unlock(&provider_backoff_lock);
}

void kb_curator_provider_backoff_recovered(void)
{
   pthread_mutex_lock(&provider_backoff_lock);
   provider_backoff_until = 0;
   provider_backoff_failures = 0;
   pthread_mutex_unlock(&provider_backoff_lock);
}

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
   case KB_CURATOR_STAGE_SYNTHESIZE_REFLECTION:
      return KB_CURATOR_TIER_B;
   }
   /* Fail safe: an unclassified / future stage routes to the capable tier (which
    * simply idles when tier_b is unconfigured), NEVER silently to the small
    * Tier-A model — a weak model on a reasoning stage poisons the graph. */
   return KB_CURATOR_TIER_B;
}

int kb_curator_provider_for_stage(kb_curator_stage_t stage, provider_def_owned_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));

   /* Each accessor hands back its own thread-local buffer, so the six reads below
    * are copied into *out's storage immediately rather than aliased -- that is the
    * whole reason this returns an owned def. */
   if (kb_curator_stage_tier(stage) == KB_CURATOR_TIER_B)
   {
      snprintf(out->base_url, sizeof(out->base_url), "%s", config_kb_curator_tier_b_base_url());
      snprintf(out->model, sizeof(out->model), "%s", config_kb_curator_tier_b_model());
      snprintf(out->api_key, sizeof(out->api_key), "%s", config_kb_curator_tier_b_api_key());
   }
   else
   {
      snprintf(out->base_url, sizeof(out->base_url), "%s", config_kb_curator_provider_base_url());
      snprintf(out->model, sizeof(out->model), "%s", config_kb_curator_provider_model());
      snprintf(out->api_key, sizeof(out->api_key), "%s", config_kb_curator_provider_api_key());
   }

   /* Env fallback when a tier has no config provider, in precedence order:
    *
    *  1. The configured SYNTHESIS endpoint, resolved by config — one field
    *     (llm_synth_endpoint) with the AIMEE_LLM_URL override applied inside
    *     config_synth_chat_endpoint_current(), never here. It no longer names a
    *     co-deployed container: aimee-llm is retired and the kb embeds in-container,
    *     so this is synthesis-only and external-only, and it drives BOTH tiers via
    *     that endpoint's OpenAI chat API. It is the only fallback Tier-B accepts —
    *     see (2).
    *
    *     The URL is normalized (trailing slashes, /v1 suffix) in that one resolver,
    *     so this file cannot disagree with any other caller about what an operator's
    *     value means.
    *  2. LLM_ENDPOINT — TIER-A ONLY. It is the small-model interface (the
    *     zero-config CPU sibling points Tier-A here); letting a small model serve
    *     the reasoning stages is the weak-model-poisons-the-graph case the tier
    *     split guards against, so Tier-B never falls back to it.
    *
    * A config provider for the tier (checked above) still wins. Whole-provider
    * fallback: base+model+key move as a unit. */
   if (!out->base_url[0])
   {
      if (config_synth_chat_endpoint_current(out->base_url, sizeof(out->base_url)))
      {
         const char *env_model = getenv("AIMEE_LLM_MODEL");
         snprintf(out->model, sizeof(out->model), "%s",
                  (env_model && env_model[0]) ? env_model : AIMEE_LLM_DEFAULT_MODEL);
         int have_service_token =
             runtime_secret_get("AIMEE_LLM_AUTH_TOKEN", out->api_key, sizeof(out->api_key));
         const char *auth_required = getenv("AIMEE_LLM_AUTH_REQUIRED");
         if (auth_required && strcmp(auth_required, "1") == 0 && !have_service_token)
         {
            /* Managed unified gateway must never receive a keyless request. Wipe
             * rather than just returning: out->api_key may hold a partial secret. */
            runtime_secret_wipe(out->api_key, sizeof(out->api_key));
            memset(out, 0, sizeof(*out));
            return 0;
         }
         if (!have_service_token)
            out->api_key[0] = '\0';
      }
      else if (kb_curator_stage_tier(stage) == KB_CURATOR_TIER_A)
      {
         const char *env_ep = getenv("LLM_ENDPOINT");
         if (!env_ep || !env_ep[0])
         {
            memset(out, 0, sizeof(*out));
            return 0; /* neither config nor env — the stage stays idle */
         }
         const char *env_model = getenv("LLM_MODEL");
         snprintf(out->base_url, sizeof(out->base_url), "%s", env_ep);
         snprintf(out->model, sizeof(out->model), "%s",
                  (env_model && env_model[0]) ? env_model : "");
         if (!runtime_secret_get("LLM_API_KEY", out->api_key, sizeof(out->api_key)))
            out->api_key[0] = '\0';
      }
      else
      {
         memset(out, 0, sizeof(*out));
         return 0; /* Tier-B with no config and no AIMEE_LLM_URL — stays idle */
      }
   }

   out->def.base_url = out->base_url;
   out->def.model = out->model;
   /* keyless local endpoint => no bearer */
   out->def.api_key = out->api_key[0] ? out->api_key : NULL;
   out->def.wire = PROVIDER_WIRE_OPENAI_CHAT;
   out->def.temperature = -1.0; /* let the provider default */
   /* Tier-A is mechanical, grammar-constrained extraction/indexing — it does not
    * need (and is hurt by) a reasoning model's chain-of-thought: the reasoning pass
    * adds latency at drain volume and, worse, can consume the output budget so the
    * JSON answer comes back truncated/empty (observed: memory-fact extraction landed
    * 0 facts). Skip thinking for Tier-A; Tier-B (judge/synthesize) keeps it. */
   out->def.disable_thinking = (kb_curator_stage_tier(stage) == KB_CURATOR_TIER_A);
   return 1;
}
