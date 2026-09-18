/* Legacy C ABI adapter for the Go-owned memory domain.
 *
 * This translation unit is deliberately transport-only: it bounds and encodes
 * requests for stage 7 and copies typed replies into the structs still used by
 * the CLI/KB edge. Persistence, lifecycle, matching, ranking and scope policy
 * all live in server-go/modules/memory.
 */
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

#define DOMAIN_TIMEOUT_MS             5000
#define DOMAIN_MAINTENANCE_TIMEOUT_MS 120000

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

static int domain_bool(const cJSON *response, const char *key)
{
   const cJSON *value = response ? cJSON_GetObjectItemCaseSensitive(response, key) : NULL;
   return cJSON_IsBool(value) && cJSON_IsTrue(value);
}

static int domain_id_update(const char *operation, int64_t id, const char *key, const char *value)
{
   if (id <= 0)
      return -1;
   cJSON *request = domain_request(operation);
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)id) ||
       (key && !cJSON_AddStringToObject(request, key, value ? value : "")))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int ok = domain_bool(response, "updated");
   cJSON_Delete(response);
   return ok ? 0 : -1;
}

int memory_reject(int64_t id, const char *reason)
{
   int result = domain_id_update("reject", id, "reason", reason);
   if (result == 0)
      memory_audit_emit("memory.reject", id, NULL, NULL, NULL, 0.0, NULL);
   return result;
}

int memory_tag_scope(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   cJSON *request = domain_request("scope-tag");
   cJSON *scope = request ? cJSON_AddObjectToObject(request, "scope") : NULL;
   if (!scope || memory_id <= 0 || !scope_type || !scope_type[0] ||
       !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddStringToObject(scope, "type", scope_type) ||
       !cJSON_AddStringToObject(scope, "value", scope_value ? scope_value : ""))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int ok = domain_bool(response, "updated");
   cJSON_Delete(response);
   return ok ? 0 : -1;
}

int memory_tag_workspace(int64_t id, const char *workspace)
{
   return memory_tag_scope(id, "workspace", workspace);
}

static cJSON *domain_payload_call(const char *operation, const char *query, const char *as_of,
                                  int limit_tokens, int session_start)
{
   cJSON *request = domain_request(operation);
   if (!request || (query && !cJSON_AddStringToObject(request, "query", query)) ||
       (as_of && !cJSON_AddStringToObject(request, "as_of", as_of)) ||
       (limit_tokens > 0 && !cJSON_AddNumberToObject(request, "limit_tokens", limit_tokens)) ||
       !cJSON_AddBoolToObject(request, "session_start", session_start != 0))
   {
      cJSON_Delete(request);
      return NULL;
   }
   cJSON *response = domain_call(request);
   cJSON *payload = response ? cJSON_DetachItemFromObjectCaseSensitive(response, "payload") : NULL;
   cJSON_Delete(response);
   return payload;
}

cJSON *memory_recall(const char *task_hint, int limit_tokens, int session_start)
{
   return domain_payload_call("recall-bundle", task_hint ? task_hint : "", NULL, limit_tokens,
                              session_start);
}

int db2_memory_key_exists(const char *key)
{
   if (!key || !key[0])
      return 0;
   cJSON *request = domain_request("key-exists");
   if (!request || !cJSON_AddStringToObject(request, "key", key))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *allowed = response ? cJSON_GetObjectItemCaseSensitive(response, "allowed") : NULL;
   int result = cJSON_IsBool(allowed) ? cJSON_IsTrue(allowed) : -1;
   cJSON_Delete(response);
   return result;
}

void db2_memory_scope_tag_insert(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   (void)memory_tag_scope(memory_id, scope_type, scope_value);
}
