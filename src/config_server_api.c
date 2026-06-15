/* config_server_api.c: parse the aimee-server public HTTP API config
 * (aimee.api.*) — the optional localhost TCP listener + bearer for the /v1
 * surface, plus per-client transport (socket|http|auto). Split out of
 * config.c to keep that file within the line budget. Parse-only (no save
 * round-trip), mirroring kb.api.* handling. */
#include "aimee.h"
#include "config.h"
#include "server.h" /* SERVER_REMOTE_WRITES_* */
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

         item = cJSON_GetObjectItemCaseSensitive(api, "bearer_token");
         if (cJSON_IsString(item) && item->valuestring)
            strncpy(cfg->server_api_bearer_token, item->valuestring,
                    sizeof(cfg->server_api_bearer_token) - 1);

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
}
