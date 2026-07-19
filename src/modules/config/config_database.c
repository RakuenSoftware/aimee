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

/* A GPU-class model tier — one served by the model-less aimee-llm image, which
 * downloads the tier on first boot. The absence of any GPU tier (cpu, or an unset
 * tier that resolves to cpu) selects the pre-baked aimee-llm-cpu image instead. */
static int deploy_tier_is_gpu(const char *tier)
{
   return tier &&
          (strcmp(tier, "small") == 0 || strcmp(tier, "mid") == 0 || strcmp(tier, "large") == 0);
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
   const char *eb = cfg->llm_embed_backend, *rb = cfg->llm_rerank_backend,
              *sb = cfg->llm_synth_backend;
   const int any_local =
       strcmp(eb, "local") == 0 || strcmp(rb, "local") == 0 || strcmp(sb, "local") == 0;

   /* The locally-served LLM image depends on the tier. Any GPU tier (small/mid/
    * large) on a local role selects the model-less aimee-llm image (profile "llm"),
    * which downloads that tier on first boot and persists it in the /models volume.
    * A pure-CPU stack (no GPU tier) selects the pre-baked aimee-llm-cpu image
    * (profile "llm-cpu"), which ships the cpu GGUFs in the image and mounts no
    * volume — the offline appliance LLM that, with aimee-kb, retires aimee-combined.
    * The two profiles are mutually exclusive; both answer to the host "aimee-llm". */
   const int gpu_local = (strcmp(eb, "local") == 0 && deploy_tier_is_gpu(cfg->llm_embed_tier)) ||
                         (strcmp(rb, "local") == 0 && deploy_tier_is_gpu(cfg->llm_rerank_tier)) ||
                         (strcmp(sb, "local") == 0 && deploy_tier_is_gpu(cfg->llm_synth_tier));

   /* COMPOSE_PROFILES: a remote kb deploys nothing; a local kb runs the "kb"
    * service, plus the LLM profile whenever any role is served locally here. */
   char profiles[64] = "";
   if (!remote_kb)
   {
      snprintf(profiles, sizeof(profiles), "kb%s",
               any_local ? (gpu_local ? ",llm" : ",llm-cpu") : "");
   }
   EMITF("COMPOSE_PROFILES=%s\n", profiles);

   if (remote_kb)
   {
      if (cfg->kb_client_url[0])
         EMITF("AIMEE_KB_API_URL=%s\n", cfg->kb_client_url);
      if (cfg->kb_client_bearer_token[0])
         EMITF("AIMEE_KB_API_BEARER_TOKEN=%s\n", cfg->kb_client_bearer_token);
      return; /* connect to the existing kb; nothing else is deployed */
   }

   /* Per-role plugin env (Phase-0 AIMEE_LLM_<ROLE>_MODE/TIER/URL). */
   if (deploy_role_mode(eb)[0])
      EMITF("AIMEE_LLM_EMBED_MODE=%s\n", deploy_role_mode(eb));
   if (strcmp(eb, "local") == 0 && cfg->llm_embed_tier[0])
      EMITF("AIMEE_LLM_EMBED_TIER=%s\n", cfg->llm_embed_tier);
   if (strcmp(eb, "external") == 0 && cfg->embedding_endpoint[0])
      EMITF("AIMEE_LLM_EMBED_URL=%s\n", cfg->embedding_endpoint);

   if (deploy_role_mode(rb)[0])
      EMITF("AIMEE_LLM_RERANK_MODE=%s\n", deploy_role_mode(rb));
   if (strcmp(rb, "local") == 0 && cfg->llm_rerank_tier[0])
      EMITF("AIMEE_LLM_RERANK_TIER=%s\n", cfg->llm_rerank_tier);
   if (strcmp(rb, "external") == 0 && cfg->llm_rerank_endpoint[0])
      EMITF("AIMEE_LLM_RERANK_URL=%s\n", cfg->llm_rerank_endpoint);

   if (deploy_role_mode(sb)[0])
      EMITF("AIMEE_LLM_SYNTH_MODE=%s\n", deploy_role_mode(sb));
   if (strcmp(sb, "local") == 0 && cfg->llm_synth_tier[0])
      EMITF("AIMEE_LLM_SYNTH_TIER=%s\n", cfg->llm_synth_tier);
   if (strcmp(sb, "external") == 0 && cfg->llm_synth_endpoint[0])
      EMITF("AIMEE_LLM_SYNTH_URL=%s\n", cfg->llm_synth_endpoint);

   /* When any role is local, aimee-server reaches the co-deployed aimee-llm
    * compose service by name; an all-external stack keeps its per-role URLs. */
   if (any_local)
      EMITF("AIMEE_LLM_URL=http://aimee-llm:8742\n");

   /* Only a pinned dim (external embedder) is emitted; a local/unset dim is
    * derived from the embedder /health probe at runtime. */
   if (config_embedding_dim_is_pinned(cfg) && cfg->embedding_dim > 0)
      EMITF("AIMEE_EMBEDDING_DIM=%d\n", cfg->embedding_dim);
#undef EMITF
}
