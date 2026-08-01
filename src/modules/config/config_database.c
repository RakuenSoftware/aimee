#include "aimee.h"
#include "config_database.h"
#include "config_embedder_dims.h" /* CONFIG_EMBEDDER_DIMS_DEFAULT — the one declaration */
#include "runtime_secret.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Applies DB2 connection defaults, then overlays values from the parsed config
 * JSON. memset in config_load already zeroed the fields so the URL stays empty
 * unless set. */
void config_parse_database(config_t *cfg, cJSON *root)
{
   if (cfg->db2_pool_size <= 0)
      cfg->db2_pool_size = 8;

   if (!root)
      return;

   cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "db2_url");
   if (cJSON_IsString(url))
      snprintf(cfg->db2_url, sizeof(cfg->db2_url), "%s", url->valuestring);

   /* Remote aimee-kb client pointer. Legacy bearer values are parsed only so
    * the boot migration can seal and remove them; runtime auth comes from Vault. */
   cJSON *kb_url = cJSON_GetObjectItemCaseSensitive(root, "kb_client_url");
   if (cJSON_IsString(kb_url))
      snprintf(cfg->kb_client_url, sizeof(cfg->kb_client_url), "%s", kb_url->valuestring);

   cJSON *kb_tok = cJSON_GetObjectItemCaseSensitive(root, "kb_client_bearer_token");
   if (cJSON_IsString(kb_tok))
      snprintf(cfg->kb_client_bearer_token, sizeof(cfg->kb_client_bearer_token), "%s",
               kb_tok->valuestring);

   cJSON *pool = cJSON_GetObjectItemCaseSensitive(root, "db2_pool_size");
   if (cJSON_IsNumber(pool))
   {
      int n = (int)pool->valuedouble;
      if (n >= 1 && n <= 256)
         cfg->db2_pool_size = n;
      else
         fprintf(stderr,
                 "aimee: config warning: \"db2_pool_size\" must be 1..256, got "
                 "%d\n",
                 n);
   }
}

int config_apply_db2_url_env_override(config_t *cfg)
{
   if (!cfg)
      return 0;
   char env_url[sizeof(cfg->db2_url)];
   if (runtime_secret_get("AIMEE_DB2_URL", env_url, sizeof(env_url)))
   {
      snprintf(cfg->db2_url, sizeof(cfg->db2_url), "%s", env_url);
      runtime_secret_wipe(env_url, sizeof(env_url));
      return 1;
   }
   runtime_secret_wipe(env_url, sizeof(env_url));
   return 0;
}

/* The db2 URL this process should dial: the AIMEE_DB2_URL runtime secret when
 * present, else the stored db2_url. Same precedence
 * config_apply_db2_url_env_override applied to a caller's struct, without the
 * caller holding one and without mutating anything -- the env value is
 * authoritative per boot and is only re-persisted by a SUCCESSFUL bootstrap.
 *
 * Writes into caller-supplied storage rather than returning a pointer: the value
 * is a credential-bearing URL, and callers hold it across db2_init and a retry
 * loop, well past the life of any shared buffer. Returns 1 when non-empty. */
int config_db2_url_effective(char *out, size_t n)
{
   if (!out || n == 0)
      return 0;
   out[0] = '\0';
   if (runtime_secret_get("AIMEE_DB2_URL", out, n) && out[0])
      return 1;
   snprintf(out, n, "%s", config_db2_url());
   return out[0] ? 1 : 0;
}

int config_embedder_dims_default(void)
{
   return CONFIG_EMBEDDER_DIMS_DEFAULT;
}

/* Effective embedding dimension: the EMBEDDER_DIMS env override when set
 * and valid (1..EMBED_MAX_DIM), else cfg->embedder_dims. The env lets a
 * containerized deploy set the dim without a writable aimee.yaml — it must match
 * the running embedder model. Normally it should be left UNSET so the dim is
 * derived (pinned > recorded > probed > default); setting it is an operator pin.
 * Returns 0 for "nothing pinned" — see config_embedder_dims_effective() for a
 * width you can use. Non-mutating so const callers can use it. */
/* No-arg form: same value against the loaded config. Deliberately mirrors
 * config_resolve_embedder_dims (the PIN signal, 0 when nothing is pinned) rather
 * than config_embedder_dims_current (the effective width) -- the two callers of
 * this are pinning db2, and collapsing them would silently turn "unpinned" into
 * the default. */
int config_resolve_embedder_dims_current(void)
{
   int dim = config_embedder_dims();
   const char *env = getenv("EMBEDDER_DIMS");
   if (env && env[0])
   {
      char *end = NULL;
      long v = strtol(env, &end, 10);
      if (end && *end == '\0' && v >= 1 && v <= EMBED_MAX_DIM)
         return (int)v;
      fprintf(stderr, "aimee: config warning: EMBEDDER_DIMS must be 1..%d, got \"%s\"\n",
              EMBED_MAX_DIM, env);
   }
   return dim;
}

int config_resolve_embedder_dims(const config_t *cfg)
{
   int dim = cfg ? cfg->embedder_dims : 0;
   const char *env = getenv("EMBEDDER_DIMS");
   if (env && env[0])
   {
      char *end = NULL;
      long v = strtol(env, &end, 10);
      if (end && *end == '\0' && v >= 1 && v <= EMBED_MAX_DIM)
         return (int)v;
      fprintf(stderr, "aimee: config warning: EMBEDDER_DIMS must be 1..%d, got \"%s\"\n",
              EMBED_MAX_DIM, env);
   }
   return dim;
}

/* The width to embed and size columns with: the pin when there is one, else the
 * declared default. Every caller that used to keep its own fallback literal calls
 * this instead. */
int config_embedder_dims_effective(const config_t *cfg)
{
   int dim = config_resolve_embedder_dims(cfg);
   return dim > 0 ? dim : CONFIG_EMBEDDER_DIMS_DEFAULT;
}

/* No-arg form, for callers that want the deployment's width and hold no config_t
 * (the CLI doctor, reporting what it expects the embedder to return). Same answer
 * as config_embedder_dims_effective against the loaded config. */
int config_embedder_dims_current(void)
{
   int pinned = config_embedder_dims();
   const char *env = getenv("EMBEDDER_DIMS");
   if (env && env[0])
   {
      char *end = NULL;
      long v = strtol(env, &end, 10);
      if (end && *end == '\0' && v >= 1 && v <= EMBED_MAX_DIM)
         return (int)v;
   }
   return pinned > 0 ? pinned : CONFIG_EMBEDDER_DIMS_DEFAULT;
}

/* Trailing-slash trim + /v1 suffix, shared by both resolvers below so they cannot
 * disagree about what an operator's value means: "http://h:8742/" and
 * "http://h:8742/v1/" both normalize to the same address. */
static int config_synth_chat_endpoint_normalize(const char *endpoint, char *out, size_t out_len)
{
   size_t n = strlen(endpoint);
   while (n > 0 && endpoint[n - 1] == '/')
      n--;
   if (n == 0)
      return 0; /* "/" or "///" names nothing */
   int has_v1 = (n >= 3 && strncmp(endpoint + n - 3, "/v1", 3) == 0);
   int wrote = snprintf(out, out_len, "%.*s%s", (int)n, endpoint, has_v1 ? "" : "/v1");
   if (wrote < 0 || (size_t)wrote >= out_len)
   {
      out[0] = '\0'; /* never hand back a truncated URL */
      return 0;
   }
   return 1;
}

/* The synthesis endpoint — see config_database.h. SYNTHESIS_ENDPOINT outranks the stored
 * field for the same reason it does for the embedder: a containerized deploy sets
 * the environment, not a writable aimee.yaml. */
int config_synth_chat_endpoint(const config_t *cfg, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';

   const char *endpoint = getenv("SYNTHESIS_ENDPOINT");
   if (!endpoint || !endpoint[0])
      endpoint = cfg ? cfg->synthesis_endpoint : NULL;
   if (!endpoint || !endpoint[0])
      return 0;
   return config_synth_chat_endpoint_normalize(endpoint, out, out_len);
}

/* Same resolver, without the caller holding a config_t: reads llm_synth_endpoint
 * through its accessor. SYNTHESIS_ENDPOINT still outranks it, and the normalization is
 * the shared one above, so no caller can disagree about what an operator's value
 * means. */
int config_synth_chat_endpoint_current(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';

   const char *endpoint = getenv("SYNTHESIS_ENDPOINT");
   if (!endpoint || !endpoint[0])
      endpoint = config_synthesis_endpoint();
   if (!endpoint || !endpoint[0])
      return 0;
   return config_synth_chat_endpoint_normalize(endpoint, out, out_len);
}

/* §2a: pinned iff the resolved operator dim is positive. Keeping this defined in
 * terms of config_resolve_embedder_dims guarantees the pin flag agrees with the
 * dim that was actually set (env "0"/non-numeric/empty and an unset cfg both
 * resolve to 0 → not pinned), so the recorded-dim override fires on exactly the
 * deployments that did not pin. */
int config_embedder_dims_is_pinned(const config_t *cfg)
{
   return config_resolve_embedder_dims(cfg) > 0;
}

/* No-arg form for callers that hold no config_t, matching
 * config_embedder_dims_current. The config_t form stays: it is a pure function
 * of one field plus EMBEDDER_DIMS, and test_config walks the whole
 * pinned/not-pinned table with hand-built configs and no I/O. */
int config_embedder_dims_pinned_current(void)
{
   /* Mirrors config_resolve_embedder_dims: a VALID env override is a pin, else
    * the configured field is a pin when positive. Deliberately not
    * config_embedder_dims_current() > 0 -- that form substitutes the declared
    * default when nothing is pinned, so it is true even for an unpinned deploy. */
   const char *env = getenv("EMBEDDER_DIMS");
   if (env && env[0])
   {
      char *end = NULL;
      long v = strtol(env, &end, 10);
      if (end && *end == '\0' && v >= 1 && v <= EMBED_MAX_DIM)
         return 1;
   }
   return config_embedder_dims() > 0;
}

/* deploy_role_mode() lived here: it mapped a role backend string to the retired
 * plugin's AIMEE_LLM_<ROLE>_MODE value. With llm_embed_backend/llm_synth_backend
 * gone there is no backend string to map — a non-empty endpoint IS the mode. */

/* Same emitter without the caller holding a config_t. */
void config_emit_deploy_env_current(char *buf, size_t n)
{
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
   {
      if (buf && n)
         buf[0] = '\0';
      return;
   }
   (void)config_load(cfg);
   config_emit_deploy_env(cfg, buf, n);
   free(cfg);
}

void config_emit_deploy_env(const config_t *cfg, char *buf, size_t n)
{
   if (!buf || n == 0)
      return;
   buf[0] = '\0';
   size_t pos = 0;
/* Append a line, never overflowing buf. */
#define EMITF(...)                                                                                 \
   do                                                                                              \
   {                                                                                               \
      if (pos < n)                                                                                 \
         pos += (size_t)snprintf(buf + pos, n - pos, __VA_ARGS__);                                 \
   } while (0)

   const int remote_kb = strcmp(cfg->kb_mode, "remote") == 0;

   /* COMPOSE_PROFILES: a remote kb deploys nothing; a local kb runs the "kb" service
    * and that is all. There is no longer an inference service to gate a profile on —
    * the kb embeds in-container from baked weights and the reranker is gone, so the
    * "llm" profile has nothing behind it. Synthesis resolves an external endpoint,
    * which is configuration rather than a deployed service. */
   char profiles[64] = "";
   if (!remote_kb)
      snprintf(profiles, sizeof(profiles), "kb");
   EMITF("COMPOSE_PROFILES=%s\n", profiles);

   if (remote_kb)
   {
      if (cfg->kb_client_url[0])
         EMITF("AIMEE_KB_API_URL=%s\n", cfg->kb_client_url);
      return; /* connect to the existing kb; nothing else is deployed */
   }

   /* The embedder. There is no per-role container to size or place any more: the kb
    * serves the selected model itself, so all the deploy layer passes on is WHICH model
    * (the wizard's choice, which the kb resolves from its registry) and, for an external
    * embedder, the endpoint to use instead. */
   if (cfg->embedder_model[0])
      EMITF("EMBEDDER_MODEL=%s\n", cfg->embedder_model);
   /* A non-empty URL IS the external embedder — there is no separate backend
    * selector to agree with any more. The llm_embed_backend/llm_synth_backend pair
    * used to gate these emissions and could disagree with the fields it gated:
    * "external" with an empty URL emitted nothing and failed silently. */
   if (cfg->embedder_url[0])
      EMITF("EMBEDDER_URL=%s\n", cfg->embedder_url);
   /* SYNTHESIS_ENDPOINT, not AIMEE_LLM_SYNTH_URL. The latter was the retired gateway's
    * own variable — it told aimee-llm where to proxy synth — and with the gateway
    * gone NOTHING read it, so a wizard-configured external synth endpoint was dead
    * end to end: written to config, emitted into the environment, consumed by no
    * one. SYNTHESIS_ENDPOINT is the variable config_synth_chat_endpoint() honours, which
    * is what a containerized kb needs when it has no writable aimee.yaml. */
   if (cfg->synthesis_endpoint[0])
      EMITF("SYNTHESIS_ENDPOINT=%s\n", cfg->synthesis_endpoint);
   if (cfg->synthesis_model[0])
      EMITF("SYNTHESIS_MODEL=%s\n", cfg->synthesis_model);
   /* embedder_api_key and synthesis_api_key are deliberately NOT emitted. A
    * credential in a long-lived service environment is exactly what
    * check-vault-only-container-env forbids: Config.Env persists, so anything
    * written there outlives the boot that set it and shows up in `docker inspect`.
    * They are sealed into Vault by the disposable bootstrap helper and read back
    * with runtime_secret_get(). */

   /* Only a pinned dim (external embedder) is emitted; an in-container embedder's
    * width is derived from the selected model at runtime. */
   if (config_embedder_dims_is_pinned(cfg) && cfg->embedder_dims > 0)
      EMITF("EMBEDDER_DIMS=%d\n", cfg->embedder_dims);
#undef EMITF
}
