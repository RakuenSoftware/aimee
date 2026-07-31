/* config_server_api.c: own the aimee-server public HTTP API config
 * (aimee.api.*) — the optional localhost TCP listener + bearer for the /v1
 * surface, plus per-client transport (socket|http|auto). Split out of
 * config.c to keep that file within the line budget. */
#include "aimee.h"
#include "config.h"
#include "server.h" /* SERVER_REMOTE_WRITES_* */
#include "runtime_secret.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

void config_parse_server_api(config_t *cfg, const cJSON *root)
{
   if (!cfg)
      return;

   if (root)
   {
      const cJSON *aimee = cJSON_GetObjectItemCaseSensitive(root, "aimee");
      const cJSON *api =
          cJSON_IsObject(aimee) ? cJSON_GetObjectItemCaseSensitive(aimee, "api") : NULL;
      if (cJSON_IsObject(api))
      {
         const cJSON *item = cJSON_GetObjectItemCaseSensitive(api, "http_port");
         if (cJSON_IsNumber(item))
            cfg->server_api_http_port = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(api, "tls_port");
         if (cJSON_IsNumber(item))
            cfg->server_api_tls_port = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(api, "mtls");
         if (cJSON_IsString(item) && item->valuestring)
            cfg->server_api_mtls = strcmp(item->valuestring, "required") == 0   ? 2
                                   : strcmp(item->valuestring, "optional") == 0 ? 1
                                                                                : 0;
         else if (cJSON_IsNumber(item))
            cfg->server_api_mtls = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(api, "mtls_client_ca");
         if (cJSON_IsString(item) && item->valuestring)
            strncpy(cfg->server_api_mtls_client_ca, item->valuestring,
                    sizeof(cfg->server_api_mtls_client_ca) - 1);

         item = cJSON_GetObjectItemCaseSensitive(api, "bearer_token");
         if (cJSON_IsString(item) && item->valuestring)
            strncpy(cfg->server_api_bearer_token, item->valuestring,
                    sizeof(cfg->server_api_bearer_token) - 1);

         /* Additional accepted bearers. Pairing a new client must not revoke
          * the credential every other client is already using. */
         item = cJSON_GetObjectItemCaseSensitive(api, "bearer_tokens_extra");
         if (cJSON_IsArray(item))
         {
            cfg->server_api_bearer_extra_count = 0;
            cJSON *tok = NULL;
            cJSON_ArrayForEach(tok, item)
            {
               if (cfg->server_api_bearer_extra_count >= AIMEE_API_BEARER_EXTRA_MAX)
                  break;
               if (!cJSON_IsString(tok) || !tok->valuestring || !tok->valuestring[0])
                  continue;
               snprintf(cfg->server_api_bearer_extra[cfg->server_api_bearer_extra_count],
                        sizeof(cfg->server_api_bearer_extra[0]), "%s", tok->valuestring);
               cfg->server_api_bearer_extra_count++;
            }
         }

         item = cJSON_GetObjectItemCaseSensitive(api, "rate_limit_per_min");
         if (cJSON_IsNumber(item))
            cfg->server_api_rate_limit_per_min = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(api, "max_event_streams");
         if (cJSON_IsNumber(item) && item->valuedouble > 0)
            cfg->server_api_max_event_streams = (int)item->valuedouble;

         item = cJSON_GetObjectItemCaseSensitive(api, "cli_session_forwarding");
         if (cJSON_IsBool(item))
            cfg->server_api_cli_session_forwarding = cJSON_IsTrue(item) ? 1 : 0;

         item = cJSON_GetObjectItemCaseSensitive(api, "remote_writes");
         if (cJSON_IsString(item) && item->valuestring)
         {
            if (strcmp(item->valuestring, "full") == 0)
               cfg->server_api_remote_writes = SERVER_REMOTE_WRITES_FULL;
            else if (strcmp(item->valuestring, "data") == 0)
               cfg->server_api_remote_writes = SERVER_REMOTE_WRITES_DATA;
            else
               cfg->server_api_remote_writes = SERVER_REMOTE_WRITES_OFF;
         }

         item = cJSON_GetObjectItemCaseSensitive(api, "client_transport");
         if (cJSON_IsString(item) && item->valuestring)
            strncpy(cfg->server_api_client_transport, item->valuestring,
                    sizeof(cfg->server_api_client_transport) - 1);

         /* No shadow_publish knob here on purpose: shadow publishing is armed at
          * runtime (POST /v1/shadow/enable) and never persisted, so it cannot
          * survive a reboot. See shadow_mirror.c. */
      }
   }

   /* Env override (deploy truth, applied even when aimee.api is absent or the
    * config file is read-only / canonical-rewritten — e.g. a containerized
    * server). AIMEE_API_REMOTE_WRITES = off|data|full lets a deploy set the TCP
    * write posture without a writable aimee.yaml. */
   const char *rw_env = getenv("AIMEE_API_REMOTE_WRITES");
   if (rw_env && rw_env[0])
   {
      if (strcmp(rw_env, "full") == 0)
         cfg->server_api_remote_writes = SERVER_REMOTE_WRITES_FULL;
      else if (strcmp(rw_env, "data") == 0)
         cfg->server_api_remote_writes = SERVER_REMOTE_WRITES_DATA;
      else if (strcmp(rw_env, "off") == 0)
         cfg->server_api_remote_writes = SERVER_REMOTE_WRITES_OFF;
   }

   /* AIMEE_API_MTLS = off|optional|required lets the container image request
    * client certificates even when an older persisted aimee.yaml predates the
    * mTLS setting.  This is deploy truth for the same reason as
    * AIMEE_API_REMOTE_WRITES: image upgrades must not leave an enrolled client
    * silently operating as bearer-only. */
   const char *mtls_env = getenv("AIMEE_API_MTLS");
   if (mtls_env && mtls_env[0])
   {
      if (strcmp(mtls_env, "required") == 0)
         cfg->server_api_mtls = 2;
      else if (strcmp(mtls_env, "optional") == 0)
         cfg->server_api_mtls = 1;
      else if (strcmp(mtls_env, "off") == 0)
         cfg->server_api_mtls = 0;
   }

   /* AIMEE_API_BEARER_TOKEN is read only from its hydrated first-boot Vault
    * slot, overriding any legacy config-file value after migration. Providing
    * an explicit token opts out of trust-on-first-use rotation because the
    * operator is managing that primary credential. */
   char bearer_env[sizeof(cfg->server_api_bearer_token)];
   if (runtime_secret_get("AIMEE_API_BEARER_TOKEN", bearer_env, sizeof(bearer_env)))
   {
      /* Extras are credentials enrolled under the primary persisted beside
       * them. A deployment-secret change is an out-of-band rotation and must
       * revoke that old set; keeping it would make changing the secret fail to
       * revoke the clients it was meant to replace. An unchanged env primary
       * preserves enrollments across ordinary container restarts. */
      if (!cfg->server_api_bearer_token[0] || strcmp(cfg->server_api_bearer_token, bearer_env) != 0)
      {
         memset(cfg->server_api_bearer_extra, 0, sizeof(cfg->server_api_bearer_extra));
         cfg->server_api_bearer_extra_count = 0;
      }
      strncpy(cfg->server_api_bearer_token, bearer_env, sizeof(cfg->server_api_bearer_token) - 1);
      cfg->server_api_bearer_token[sizeof(cfg->server_api_bearer_token) - 1] = '\0';
   }
   runtime_secret_wipe(bearer_env, sizeof(bearer_env));
}

int config_server_api_bearer_extra_snapshot(char out[][256], int max)
{
   if (!out || max <= 0)
      return 0;
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   if (config_load(cfg) != 0)
   {
      free(cfg);
      return -1;
   }
   int count = cfg->server_api_bearer_extra_count;
   if (count < 0)
      count = 0;
   if (count > AIMEE_API_BEARER_EXTRA_MAX)
      count = AIMEE_API_BEARER_EXTRA_MAX;
   if (count > max)
      count = max;
   for (int i = 0; i < count; i++)
      snprintf(out[i], sizeof(out[i]), "%s", cfg->server_api_bearer_extra[i]);
   free(cfg);
   return count;
}
