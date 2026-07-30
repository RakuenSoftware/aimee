#include "aimee.h"
#include "config_database.h"
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

/* Effective embedding dimension: the AIMEE_EMBEDDING_DIM env override when set
 * and valid (1..EMBED_MAX_DIM), else cfg->embedding_dim. The env lets a
 * containerized deploy set the dim without a writable aimee.yaml — it must match
 * the running embedder model (EMBED_DEFAULT_DIM for the bundled one on
 * every tier; 1024/2560 for the legacy pplx-embed 0.6b/4b). Normally it should be
 * left UNSET so the dim is derived (pinned > recorded > probed); setting it is an
 * operator pin. Non-mutating so const callers can use it. */
int config_resolve_embedding_dim(const config_t *cfg)
{
   int dim = cfg ? cfg->embedding_dim : 0;
   const char *env = getenv("AIMEE_EMBEDDING_DIM");
   if (env && env[0])
   {
      char *end = NULL;
      long v = strtol(env, &end, 10);
      if (end && *end == '\0' && v >= 1 && v <= EMBED_MAX_DIM)
         return (int)v;
      fprintf(stderr, "aimee: config warning: AIMEE_EMBEDDING_DIM must be 1..%d, got \"%s\"\n",
              EMBED_MAX_DIM, env);
   }
   return dim;
}

/* §2a: pinned iff the resolved operator dim is positive. Keeping this defined in
 * terms of config_resolve_embedding_dim guarantees the pin flag agrees with the
 * dim that was actually set (env "0"/non-numeric/empty and an unset cfg both
 * resolve to 0 → not pinned), so the recorded-dim override fires on exactly the
 * deployments that did not pin. */
int config_embedding_dim_is_pinned(const config_t *cfg)
{
   return config_resolve_embedding_dim(cfg) > 0;
}

/* Map a role backend string to the plugin's AIMEE_LLM_<ROLE>_MODE value. Empty
 * (unconfigured) yields "" so the caller can skip emitting it. */
static const char *deploy_role_mode(const char *backend)
{
   if (strcmp(backend, "local") == 0)
      return "local";
   if (strcmp(backend, "external") == 0)
      return "external";
   if (strcmp(backend, "off") == 0)
      return "off";
   return "";
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
   const char *eb = cfg->llm_embed_backend, *sb = cfg->llm_synth_backend;

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
   if (cfg->embedding_model[0])
      EMITF("EMBEDDER_MODEL=%s\n", cfg->embedding_model);
   if (strcmp(eb, "external") == 0 && cfg->embedding_endpoint[0])
      EMITF("AIMEE_EMBEDDER_URL=%s\n", cfg->embedding_endpoint);

   if (deploy_role_mode(sb)[0])
      EMITF("AIMEE_LLM_SYNTH_MODE=%s\n", deploy_role_mode(sb));
   if (strcmp(sb, "local") == 0 && cfg->llm_synth_tier[0])
      EMITF("AIMEE_LLM_SYNTH_TIER=%s\n", cfg->llm_synth_tier);
   if (strcmp(sb, "external") == 0 && cfg->llm_synth_endpoint[0])
      EMITF("AIMEE_LLM_SYNTH_URL=%s\n", cfg->llm_synth_endpoint);

   /* No AIMEE_LLM_URL is synthesised any more: it used to name the co-deployed
    * aimee-llm service, and that service is gone. Synthesis now takes whatever
    * endpoint the operator configures, which is llm_synth_endpoint above.
    *
    * Only a pinned dim (external embedder) is emitted; an in-container embedder's
    * width is derived from the selected model at runtime. */
   if (config_embedding_dim_is_pinned(cfg) && cfg->embedding_dim > 0)
      EMITF("AIMEE_EMBEDDING_DIM=%d\n", cfg->embedding_dim);
#undef EMITF
}
