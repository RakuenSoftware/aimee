/* model_provider.h: provider profile registry.
 * Implements the model_provider_t ABI from
 * docs/proposals/pending/provider-profile-registry-and-auxiliary-models.md. */
#ifndef DEC_MODEL_PROVIDER_H
#define DEC_MODEL_PROVIDER_H 1

#include "failover.h"
#include "provider_model.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      API_MODE_CHAT_COMPLETIONS = 0, /* OpenAI /chat/completions */
      API_MODE_CODEX_RESPONSES,      /* OpenAI /responses */
      API_MODE_ANTHROPIC_MESSAGES,   /* Anthropic /messages */
      API_MODE_LLAMA_NATIVE,         /* llama.cpp native */
   } api_mode_t;

   typedef struct model_provider model_provider_t;

   struct model_provider
   {
      const char *name; /* "openai", "openrouter", "anthropic", ... */
      const char *display_name;
      const char *description;

      const char *base_url;   /* primary API base, e.g. https://openrouter.ai/api/v1 */
      const char *models_url; /* endpoint for GET model list */
      const char *signup_url;

      const char *auth_type; /* "api_key" | "oauth_pkce" | "none" */
      const char **env_vars; /* NULL-terminated list of env var names */

      api_mode_t api_mode;

      const char *default_model;
      const char *default_aux_model; /* cheap model for auxiliary tasks */
      const char **fallback_models;  /* NULL-terminated */

      /* Curated set of models this provider EXPOSES as routable, NULL-terminated.
       * Distinct from fallback_models, which describes retry ORDER and may omit
       * the primary. This is exposure POLICY: a provider's catalog is discovery
       * data, and a model appearing in it does not prove it is intended for this
       * product, has complete capability metadata, or is enabled for the
       * configured account. Lets an operator register a provider without naming
       * models ("models": "auto"). NULL means the provider offers no curated
       * set, so "auto" is rejected rather than guessed at.
       *
       * This list is the DEFAULT, not a ceiling: an explicit "models" array is an
       * operator OVERRIDE and is expanded without membership checking. An
       * operator who names a model deliberately is not second-guessed — the
       * curated set exists so they do not have to name any. */
      const char **routable_models;

      int fixed_temperature; /* -1 = caller chooses; >= 0 = pinned */
      int default_max_tokens;

      const char **default_headers; /* NULL-terminated key,val,key,val,... pairs */

      /* Fetch live model catalog. Returns 0 on success; caller frees
       * *models_out with free() (one contiguous block, no owned pointers).
       *
       * Populate every field the provider actually publishes and LEAVE THE REST
       * ZERO. 0 means "not published", not zero-capacity — most
       * OpenAI-compatible endpoints return ids and nothing else, and a fetch
       * that invented a limit would overwrite the operator's declared value
       * with a guess. See provider_model.h. */
      int (*fetch_models)(model_provider_t *p, provider_model_t **models_out, int *n_out);

      /* Optional provider-owned failure classifier. Return 1 and set *out when
       * the provider recognizes an error body/status; return 0 to fall back to
       * generic classification. */
      int (*classify_body)(model_provider_t *p, int http_status, const char *body,
                           failover_reason_t *out);
   };

   /* Register a provider profile (idempotent on duplicate names). */
   void model_provider_register(model_provider_t *p);

   /* Look up by name. NULL if not found. Triggers lazy built-in init. */
   model_provider_t *model_provider_get(const char *name);

   /* List all registered providers. Returns count; fills up to max. */
   int model_provider_list(model_provider_t **out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MODEL_PROVIDER_H */
