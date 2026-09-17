/* Runtime, maintenance and vector portion of the legacy C ABI adapter for the
 * Go-owned memory domain. This file remains transport-only. */
#include "aimee.h"
#include "headers/module_json_call.h"

#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/memory/module_api.h>

#include "cJSON.h"
#include "memory_query.h"
#include "memory_scope_query.h"
#include "memory_bus_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOMAIN_TIMEOUT_MS 5000

static cJSON *domain_call_with_timeout(cJSON *request, int timeout_ms)
{
   if (memory_bus_add_context(request) != 0)
   {
      cJSON_Delete(request);
      return NULL;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   return aimee_module_json_call(AIMEE_MEMORY_EVENT_DATA, AIMEE_MEMORY_STAGE_DATA, request,
                                 AIMEE_MODULE_MESSAGE_MAX_BODY, timeout_ms, &result);
}

static cJSON *domain_call(cJSON *request)
{
   return domain_call_with_timeout(request, DOMAIN_TIMEOUT_MS);
}

static cJSON *domain_request(const char *operation)
{
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", operation))
   {
      cJSON_Delete(request);
      return NULL;
   }
   return request;
}

static int domain_number(const cJSON *response, const char *key, int *out)
{
   const cJSON *value = response ? cJSON_GetObjectItemCaseSensitive(response, key) : NULL;
   if (!cJSON_IsNumber(value))
      return -1;
   if (out)
      *out = value->valueint;
   return 0;
}

int64_t memory_episode_card_generate(const char *source_session)
{
   if (!source_session || !source_session[0])
      return 0;
   cJSON *request = domain_request("episode-card-generate");
   if (!request || !cJSON_AddStringToObject(request, "session_id", source_session))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   const cJSON *ids = response ? cJSON_GetObjectItemCaseSensitive(response, "ids") : NULL;
   const cJSON *id = cJSON_IsArray(ids) ? cJSON_GetArrayItem(ids, 0) : NULL;
   int64_t result = cJSON_IsNumber(id) ? (int64_t)id->valuedouble : 0;
   cJSON_Delete(response);
   return result;
}

int pgvec_memory_vector_search_record_type(const char *record_type, const float *vec, int dim,
                                           int limit, int64_t *ids, double *scores, int max)
{
   if (!record_type || !record_type[0] || !vec || dim <= 0 || dim > EMBED_MAX_DIM || !ids ||
       !scores || max <= 0)
      return -1;
   if (limit > max)
      limit = max;
   if (limit > 256)
      limit = 256;
   db2_memory_scope_context_t scope;
   memset(&scope, 0, sizeof(scope));
   db2_memory_scope_context_get(&scope);
   cJSON *request = domain_request("vector-search");
   cJSON *vector = request ? cJSON_AddArrayToObject(request, "vector") : NULL;
   if (!vector || !cJSON_AddStringToObject(request, "record_type", record_type) ||
       !cJSON_AddStringToObject(request, "workspace", scope.workspace) ||
       !cJSON_AddStringToObject(request, "project", scope.project) ||
       !cJSON_AddBoolToObject(request, "include_all", scope.include_all) ||
       !cJSON_AddNumberToObject(request, "max_results", limit))
   {
      cJSON_Delete(request);
      return -1;
   }
   for (int i = 0; i < dim; ++i)
      cJSON_AddItemToArray(vector, cJSON_CreateNumber(vec[i]));
   cJSON *response = domain_call(request);
   const cJSON *hits = response ? cJSON_GetObjectItemCaseSensitive(response, "vector_hits") : NULL;
   if (!cJSON_IsArray(hits))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(hits);
   if (n > limit)
      n = limit;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *hit = cJSON_GetArrayItem(hits, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(hit, "id");
      const cJSON *score = cJSON_GetObjectItemCaseSensitive(hit, "score");
      if (!cJSON_IsNumber(id) || !cJSON_IsNumber(score))
      {
         cJSON_Delete(response);
         return -1;
      }
      ids[i] = (int64_t)id->valuedouble;
      scores[i] = score->valuedouble;
   }
   cJSON_Delete(response);
   return n;
}

int memory_repair_vector_index(int64_t memory_id, const char *command)
{
   return memory_embed(memory_id, command);
}

int memory_repair_vector_index_failed_only(const char *command, int limit, int *failed_out)
{
   if (failed_out)
      *failed_out = 0;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "repair-failed") ||
       !cJSON_AddStringToObject(request, "base_url", command ? command : "") ||
       !cJSON_AddNumberToObject(request, "max_dim", EMBED_MAX_DIM) ||
       !cJSON_AddNumberToObject(request, "limit", limit > 0 ? limit : 256))
   {
      cJSON_Delete(request);
      return -1;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response =
       aimee_module_json_call(AIMEE_MEMORY_EVENT_EMBED, AIMEE_MEMORY_STAGE_EMBED, request,
                              AIMEE_MODULE_MESSAGE_MAX_BODY, 120000, &result);
   int repaired = -1, failed = 0;
   (void)domain_number(response, "repaired", &repaired);
   (void)domain_number(response, "failed", &failed);
   cJSON_Delete(response);
   if (failed_out)
      *failed_out = failed;
   return repaired;
}
