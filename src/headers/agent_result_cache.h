#ifndef AIMEE_AGENT_RESULT_CACHE_H
#define AIMEE_AGENT_RESULT_CACHE_H
#include "agent_types.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include <string.h>
/* Cache identity includes the exact admitted output-boundary request, rather
 * than a prompt before system/context/model/profile transforms. No credential
 * is accepted by this helper. The execution owner gates eligibility separately. */
static inline int agent_result_cache_key(const char *principal, const char *role,
                                         const agent_t *agent, const char *body, char key[65])
{
   if (!principal || !principal[0] || !role || !role[0] || !agent || !body || !key)
      return -1;
   char request[65];
   if (aimee_sha256_hex(body, strlen(body), request))
      return -1;
   cJSON *identity = cJSON_CreateArray();
   const char *parts[] = {"canonical-result-v1", principal,       role,         agent->name,
                          agent->provider,       agent->endpoint, agent->model, request};
   if (!identity)
      return -1;
   for (size_t i = 0; i < sizeof parts / sizeof *parts; i++)
   {
      cJSON *part = cJSON_CreateString(parts[i]);
      if (!part || !cJSON_AddItemToArray(identity, part))
      {
         cJSON_Delete(part);
         cJSON_Delete(identity);
         return -1;
      }
   }
   char *wire = cJSON_PrintUnformatted(identity);
   cJSON_Delete(identity);
   int rc = wire ? aimee_sha256_hex(wire, strlen(wire), key) : -1;
   free(wire);
   return rc;
}
/* Internal execution owner: role eligibility is resolved by the Go policy;
 * these calls never expose a transport-selectable cache or session history. */
int agent_result_cache_open(const char *role, const agent_t *agent, const char *body,
                            const char *auth, const char *extra, char key[65]);
int agent_result_cache_read(const char *role, const char *key, agent_result_t *out);
void agent_result_cache_write(const char *role, const char *key, const agent_result_t *out);
#endif
