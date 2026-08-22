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
 * this is a label; an operator can override it with SYNTHESIS_MODEL. */
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
   /* An unconfigured endpoint is the most global condition there is: no row can
    * ever succeed until an operator acts, so spending a per-row attempt budget on
    * it is pure waste. Left unmatched, this burned 14,705 sidecar invocations in
    * one run — a python process forked per symbol, three attempts each, ~51% CPU
    * held for hours, starving the box the queue was meant to serve. It is not a
    * poisonous row: the very next row fails identically. */
   int endpoint_unconfigured = strstr(error_msg, "no synthesis endpoint configured") != NULL;
   return strstr(error_msg, "provider HTTP 503") != NULL ||
          strstr(error_msg, "provider HTTP 429") != NULL ||
          strstr(error_msg, "provider HTTP -1") != NULL ||
          strstr(error_msg, "HTTP 503 from") != NULL ||
          strstr(error_msg, "HTTP 429 from") != NULL ||
          strstr(error_msg, "\"code\": \"provider_unavailable\"") != NULL ||
          strstr(error_msg, "\"code\":\"provider_unavailable\"") != NULL ||
          strstr(error_msg, "upstream circuit is open") != NULL || bundled_transport_timeout ||
          endpoint_unconfigured;
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

/* kb_curator_stage_tier() lived here, mapping each stage to Tier-A (mechanical
 * extract/index) or Tier-B (reasoning/judge) so the two could be served by
 * DIFFERENT MODELS. That split is gone: there is one synthesis role and one model
 * behind it, because measurement did not support running a cheaper one on the
 * mechanical stages.
 *
 * Thinking is NOT suppressed for any stage. It measured positive-to-neutral
 * everywhere it was tried — on the 69-note extraction set the default model gains
 * 0.084 F1 with it (95% CI [+0.0094,+0.1712], paired bootstrap; see
 * docs/SYNTHESIS_MODELS.md). An earlier per-tier suppression existed because a
 * reasoning pass can consume the output budget and truncate the JSON answer; that
 * is bounded by MF_LLM_OUT_CAP rather than by refusing to think. */

int kb_curator_provider_for_stage(kb_curator_stage_t stage, provider_def_owned_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));

   /* Each accessor hands back its own thread-local buffer, so the six reads below
    * are copied into *out's storage immediately rather than aliased -- that is the
    * whole reason this returns an owned def. */
   /* One provider for every stage. This used to read kb_curator_tier_b_* for the
    * reasoning stages and kb_curator_provider_* for the mechanical ones. */
   snprintf(out->base_url, sizeof(out->base_url), "%s", config_kb_curator_provider_base_url());
   snprintf(out->model, sizeof(out->model), "%s", config_kb_curator_provider_model());
   (void)runtime_secret_get("AIMEE_KB_CURATOR_PROVIDER_API_KEY", out->api_key,
                            sizeof(out->api_key));

   /* Fallback when no config provider is set: the configured SYNTHESIS endpoint,
    * resolved by config — one field (llm_synth_endpoint) with the
    * SYNTHESIS_ENDPOINT override applied inside
    * config_synth_chat_endpoint_current(), never here. It no longer names a
    * co-deployed container: aimee-llm is retired and the kb embeds in-container,
    * so this is synthesis-only and external-only, reached through that endpoint's
    * OpenAI chat API.
    *
    * The URL is normalized (trailing slashes, /v1 suffix) in that one resolver, so
    * this file cannot disagree with any other caller about what an operator's
    * value means.
    *
    * There was a second, tier-dependent fallback here: the mechanical stages could
    * also read the endpoint straight from the environment while the reasoning
    * stages were forbidden to, on the theory that a small model serving the
    * reasoning stages poisons the graph. There are no tiers to distinguish now, and
    * config ingests SYNTHESIS_ENDPOINT itself, so one path serves every stage.
    *
    * A config provider (checked above) still wins. Whole-provider fallback:
    * base+model+key move as a unit. */
   if (!out->base_url[0])
   {
      if (config_synth_chat_endpoint_current(out->base_url, sizeof(out->base_url)))
      {
         const char *env_model = getenv("SYNTHESIS_MODEL");
         snprintf(out->model, sizeof(out->model), "%s",
                  (env_model && env_model[0]) ? env_model : AIMEE_LLM_DEFAULT_MODEL);
         int have_service_token =
             runtime_secret_get("SYNTHESIS_API_KEY", out->api_key, sizeof(out->api_key));
         const char *auth_required = getenv("SYNTHESIS_AUTH_REQUIRED");
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
      else
      {
         /* No configured endpoint — the stage stays idle, which is supported. */
         memset(out, 0, sizeof(*out));
         return 0;
      }
   }

   out->def.base_url = out->base_url;
   out->def.model = out->model;
   /* keyless local endpoint => no bearer */
   out->def.api_key = out->api_key[0] ? out->api_key : NULL;
   out->def.wire = PROVIDER_WIRE_OPENAI_CHAT;
   out->def.temperature = -1.0; /* let the provider default */
   /* One global, operator-owned switch (synthesis_thinking, default on) rather
    * than a rule implied by the stage. See the note above the resolver. */
   out->def.disable_thinking = !config_synthesis_thinking();
   return 1;
}
