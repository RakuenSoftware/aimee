#include "aimee.h"
#include "config_database.h"
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

   /* Remote aimee-kb client pointer (set when this host uses a remote kb
    * instead of a local sidecar). aimee-server exports these into its env at
    * startup; the AIMEE_KB_API_URL / AIMEE_KB_API_BEARER_TOKEN env vars still
    * win when present. */
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
   const char *env_url = getenv("AIMEE_DB2_URL");
   if (env_url && env_url[0])
   {
      snprintf(cfg->db2_url, sizeof(cfg->db2_url), "%s", env_url);
      return 1;
   }
   return 0;
}

/* Effective embedding dimension: the AIMEE_EMBEDDING_DIM env override when set
 * and valid (1..EMBED_MAX_DIM), else cfg->embedding_dim. The env lets a
 * containerized deploy set the dim without a writable aimee.yaml — it must match
 * the running embedder model (1024 for pplx-embed-v1-0.6b, 2560 for the default
 * pplx-embed-v1-4b). Non-mutating so const callers can use it. */
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
