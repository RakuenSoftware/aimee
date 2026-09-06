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
#include "memory_ontology.h"
#include "memory_lint.h"
#include "memory_export.h"
#include "memory_query.h"
#include "memory_scenes.h"
#include "memory_scope_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOMAIN_TIMEOUT_MS             5000
#define DOMAIN_MAINTENANCE_TIMEOUT_MS 120000

static cJSON *domain_call_with_timeout(cJSON *request, int timeout_ms)
{
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

int memory_embed(int64_t memory_id, const char *command)
{
   if (memory_id <= 0 || !command || !command[0])
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "record") ||
       !cJSON_AddNumberToObject(request, "memory_id", (double)memory_id) ||
       !cJSON_AddStringToObject(request, "base_url", command) ||
       !cJSON_AddNumberToObject(request, "max_dim", EMBED_MAX_DIM))
   {
      cJSON_Delete(request);
      return -1;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response = aimee_module_json_call(AIMEE_MEMORY_EVENT_EMBED, AIMEE_MEMORY_STAGE_EMBED,
                                            request, AIMEE_MODULE_MESSAGE_MAX_BODY, 25000, &result);
   const cJSON *embedded = response ? cJSON_GetObjectItemCaseSensitive(response, "embedded") : NULL;
   int ok = cJSON_IsBool(embedded) && cJSON_IsTrue(embedded);
   cJSON_Delete(response);
   return ok ? 0 : -1;
}

static int domain_copy(char *out, size_t cap, const cJSON *obj, const char *key)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!out || cap == 0 || !cJSON_IsString(value) || !value->valuestring)
      return -1;
   snprintf(out, cap, "%s", value->valuestring);
   return 0;
}

static int domain_bool(const cJSON *response, const char *key)
{
   const cJSON *value = response ? cJSON_GetObjectItemCaseSensitive(response, key) : NULL;
   return cJSON_IsBool(value) && cJSON_IsTrue(value);
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

static int domain_memory_from_json(const cJSON *obj, memory_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(obj, "id");
   const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(obj, "confidence");
   if (!out || !cJSON_IsObject(obj) || !cJSON_IsNumber(id) || !cJSON_IsNumber(confidence))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->confidence = confidence->valuedouble;
   return domain_copy(out->tier, sizeof(out->tier), obj, "tier") ||
                  domain_copy(out->kind, sizeof(out->kind), obj, "kind") ||
                  domain_copy(out->key, sizeof(out->key), obj, "key") ||
                  domain_copy(out->content, sizeof(out->content), obj, "content")
              ? -1
              : 0;
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

int memory_touch_many(const int64_t *ids, int n)
{
   if (!ids || n <= 0 || n > 256)
      return -1;
   cJSON *request = domain_request("touch");
   cJSON *array = request ? cJSON_AddArrayToObject(request, "ids") : NULL;
   if (!array)
   {
      cJSON_Delete(request);
      return -1;
   }
   for (int i = 0; i < n; ++i)
      if (ids[i] > 0)
         cJSON_AddItemToArray(array, cJSON_CreateNumber((double)ids[i]));
   cJSON *response = domain_call(request);
   int changed = 0;
   int rc = domain_number(response, "count", &changed);
   cJSON_Delete(response);
   return rc == 0 ? 0 : -1;
}

int memory_touch(int64_t id)
{
   return memory_touch_many(&id, 1);
}

int memory_update_content(int64_t id, const char *content)
{
   return domain_id_update("update-content", id, "content", content);
}

int memory_reject(int64_t id, const char *reason)
{
   int result = domain_id_update("reject", id, "reason", reason);
   if (result == 0)
      memory_audit_emit("memory.reject", id, NULL, NULL, NULL, 0.0, NULL);
   return result;
}

static int link_from_json(const cJSON *obj, memory_link_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(obj, "id");
   const cJSON *source = cJSON_GetObjectItemCaseSensitive(obj, "source_id");
   const cJSON *target = cJSON_GetObjectItemCaseSensitive(obj, "target_id");
   if (!out || !cJSON_IsNumber(id) || !cJSON_IsNumber(source) || !cJSON_IsNumber(target))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->source_id = (int64_t)source->valuedouble;
   out->target_id = (int64_t)target->valuedouble;
   return domain_copy(out->relation, sizeof(out->relation), obj, "relation") ||
                  domain_copy(out->created_at, sizeof(out->created_at), obj, "created_at")
              ? -1
              : 0;
}

int memory_link_create(int64_t source_id, int64_t target_id, const char *relation)
{
   cJSON *request = domain_request("link-create");
   if (!request || source_id <= 0 || target_id <= 0 || !relation || !relation[0] ||
       !cJSON_AddNumberToObject(request, "source_id", (double)source_id) ||
       !cJSON_AddNumberToObject(request, "target_id", (double)target_id) ||
       !cJSON_AddStringToObject(request, "relation", relation))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "links") : NULL;
   int rc = cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 1 ? 0 : -1;
   cJSON_Delete(response);
   return rc;
}

int memory_link_query(int64_t memory_id, memory_link_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return -1;
   cJSON *request = domain_request("link-query");
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "links") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (link_from_json(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
      {
         cJSON_Delete(response);
         return -1;
      }
   cJSON_Delete(response);
   return n;
}

int memory_link_delete(int64_t link_id)
{
   cJSON *request = domain_request("link-delete");
   if (!request || link_id <= 0 || !cJSON_AddNumberToObject(request, "id", (double)link_id))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int ok = domain_bool(response, "deleted");
   cJSON_Delete(response);
   return ok ? 0 : -1;
}

int memory_get_provenance(int64_t memory_id, provenance_entry_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return -1;
   cJSON *request = domain_request("provenance-list");
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "provenance") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *row = cJSON_GetArrayItem(rows, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
      const cJSON *mid = cJSON_GetObjectItemCaseSensitive(row, "memory_id");
      if (!cJSON_IsNumber(id) || !cJSON_IsNumber(mid))
      {
         cJSON_Delete(response);
         return -1;
      }
      memset(&out[i], 0, sizeof(out[i]));
      out[i].id = (int64_t)id->valuedouble;
      out[i].memory_id = (int64_t)mid->valuedouble;
      domain_copy(out[i].session_id, sizeof(out[i].session_id), row, "session_id");
      domain_copy(out[i].action, sizeof(out[i].action), row, "action");
      domain_copy(out[i].details, sizeof(out[i].details), row, "details");
      domain_copy(out[i].created_at, sizeof(out[i].created_at), row, "created_at");
   }
   cJSON_Delete(response);
   return n;
}

void add_provenance(int64_t memory_id, const char *session_id, const char *action,
                    const char *details)
{
   cJSON *request = domain_request("provenance-add");
   if (!request || memory_id <= 0 || !action || !action[0] ||
       !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddStringToObject(request, "session_id", session_id ? session_id : "") ||
       !cJSON_AddStringToObject(request, "action_text", action) ||
       !cJSON_AddStringToObject(request, "details", details ? details : ""))
   {
      cJSON_Delete(request);
      return;
   }
   cJSON_Delete(domain_call(request));
}

static int conflict_from_json(const cJSON *row, conflict_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
   const cJSON *a = cJSON_GetObjectItemCaseSensitive(row, "memory_a_id");
   const cJSON *b = cJSON_GetObjectItemCaseSensitive(row, "memory_b_id");
   const cJSON *resolved = cJSON_GetObjectItemCaseSensitive(row, "resolved");
   if (!out || !cJSON_IsNumber(id) || !cJSON_IsNumber(a) || !cJSON_IsNumber(b))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->memory_a = (int64_t)a->valuedouble;
   out->memory_b = (int64_t)b->valuedouble;
   out->resolved = cJSON_IsTrue(resolved);
   domain_copy(out->detected_at, sizeof(out->detected_at), row, "detected_at");
   domain_copy(out->resolution, sizeof(out->resolution), row, "resolution");
   return 0;
}

int memory_list_conflicts(conflict_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   cJSON *request = domain_request("conflict-list");
   if (!request || !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "conflicts") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (conflict_from_json(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
      {
         cJSON_Delete(response);
         return -1;
      }
   cJSON_Delete(response);
   return n;
}

int memory_record_conflict(int64_t mem_a, int64_t mem_b)
{
   cJSON *request = domain_request("conflict-record");
   if (!request || !cJSON_AddNumberToObject(request, "source_id", (double)mem_a) ||
       !cJSON_AddNumberToObject(request, "target_id", (double)mem_b))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "conflicts") : NULL;
   int rc = cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 1 ? 0 : -1;
   cJSON_Delete(response);
   return rc;
}

int memory_resolve_conflict(int64_t conflict_id, const char *resolution)
{
   return domain_id_update("conflict-resolve", conflict_id, "resolution", resolution);
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

int memory_tag_global(int64_t id)
{
   return memory_tag_scope(id, "global", "_global");
}
int memory_tag_project(int64_t id, const char *project)
{
   return memory_tag_scope(id, "project", project);
}
int memory_tag_workspace(int64_t id, const char *workspace)
{
   return memory_tag_scope(id, "workspace", workspace);
}

int memory_collect_scopes(int64_t memory_id, memory_scope_tag_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return -1;
   cJSON *request = domain_request("scope-collect");
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)memory_id))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "scopes") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *row = cJSON_GetArrayItem(rows, i);
      memset(&out[i], 0, sizeof(out[i]));
      domain_copy(out[i].type, sizeof(out[i].type), row, "type");
      domain_copy(out[i].value, sizeof(out[i].value), row, "value");
   }
   cJSON_Delete(response);
   return n;
}

int memory_scope_visibility_rank(int64_t memory_id, const char *workspace, const char *project)
{
   cJSON *request = domain_request("scope-rank");
   if (!request || memory_id <= 0 || !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddStringToObject(request, "workspace", workspace ? workspace : "") ||
       !cJSON_AddStringToObject(request, "project", project ? project : ""))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "scope_ranks") : NULL;
   const cJSON *row = cJSON_IsArray(rows) ? cJSON_GetArrayItem(rows, 0) : NULL;
   const cJSON *rank = row ? cJSON_GetObjectItemCaseSensitive(row, "rank") : NULL;
   int result = cJSON_IsNumber(rank) ? rank->valueint : 0;
   cJSON_Delete(response);
   return result;
}

memory_scope_level_t memory_primary_scope(int64_t memory_id, char *value, size_t value_len)
{
   cJSON *request = domain_request("scope-primary");
   if (!request || memory_id <= 0 || !cJSON_AddNumberToObject(request, "id", (double)memory_id))
   {
      cJSON_Delete(request);
      return MEMORY_SCOPE_NONE;
   }
   cJSON *response = domain_call(request);
   const cJSON *scopes = response ? cJSON_GetObjectItemCaseSensitive(response, "scopes") : NULL;
   const cJSON *scope = cJSON_IsArray(scopes) ? cJSON_GetArrayItem(scopes, 0) : NULL;
   const cJSON *type = scope ? cJSON_GetObjectItemCaseSensitive(scope, "type") : NULL;
   const cJSON *scope_value = scope ? cJSON_GetObjectItemCaseSensitive(scope, "value") : NULL;
   if (value && value_len)
      snprintf(value, value_len, "%s",
               cJSON_IsString(scope_value) && scope_value->valuestring ? scope_value->valuestring
                                                                       : "");
   memory_scope_level_t level = MEMORY_SCOPE_NONE;
   if (cJSON_IsString(type) && type->valuestring)
      level = strcmp(type->valuestring, "project") == 0     ? MEMORY_SCOPE_PROJECT
              : strcmp(type->valuestring, "workspace") == 0 ? MEMORY_SCOPE_WORKSPACE
              : strcmp(type->valuestring, "global") == 0    ? MEMORY_SCOPE_GLOBAL
                                                            : MEMORY_SCOPE_NONE;
   cJSON_Delete(response);
   return level;
}

static const char *domain_policy_name(const char *operation, const char *text_key, const char *text,
                                      const char *number_key, int number)
{
   static __thread char name[64];
   cJSON *request = domain_request(operation);
   if (!request || (text_key && !cJSON_AddStringToObject(request, text_key, text ? text : "")) ||
       (number_key && !cJSON_AddNumberToObject(request, number_key, number)))
   {
      cJSON_Delete(request);
      return "other";
   }
   cJSON *response = domain_call(request);
   const cJSON *value = response ? cJSON_GetObjectItemCaseSensitive(response, "name") : NULL;
   snprintf(name, sizeof(name), "%s",
            cJSON_IsString(value) && value->valuestring ? value->valuestring : "other");
   cJSON_Delete(response);
   return name;
}

const char *memory_scope_level_name(memory_scope_level_t level)
{
   return domain_policy_name("scope-level-name", NULL, NULL, "level", (int)level);
}

const char *memory_functional_tier_name(const char *tier)
{
   return domain_policy_name("tier-name", "tier", tier, NULL, 0);
}

int memory_stats(memory_stats_t *out)
{
   if (!out)
      return -1;
   cJSON *response = domain_call(domain_request("stats"));
   const cJSON *stats = response ? cJSON_GetObjectItemCaseSensitive(response, "stats") : NULL;
   const cJSON *tiers = stats ? cJSON_GetObjectItemCaseSensitive(stats, "tier_counts") : NULL;
   const cJSON *kinds = stats ? cJSON_GetObjectItemCaseSensitive(stats, "kind_counts") : NULL;
   const char *kind_names[KIND_COUNT] = {
       KIND_FACT,    KIND_PREFERENCE, KIND_DECISION, KIND_EPISODE,  KIND_TASK,
       KIND_SCRATCH, KIND_PROCEDURE,  KIND_POLICY,   KIND_WORKFLOW, KIND_OPINION};
   if (!cJSON_IsObject(stats) || !cJSON_IsObject(tiers) || !cJSON_IsObject(kinds))
   {
      cJSON_Delete(response);
      return -1;
   }
   memset(out, 0, sizeof(*out));
   for (int i = 0; i < 6; ++i)
   {
      char tier[4];
      snprintf(tier, sizeof(tier), "L%d", i);
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(tiers, tier);
      out->tier_counts[i] = cJSON_IsNumber(value) ? value->valueint : 0;
   }
   for (int i = 0; i < KIND_COUNT; ++i)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(kinds, kind_names[i]);
      out->kind_counts[i] = cJSON_IsNumber(value) ? value->valueint : 0;
   }
   domain_number(stats, "total", &out->total);
   domain_number(stats, "conflicts", &out->conflicts);
   cJSON_Delete(response);
   return 0;
}

int memory_query_health(memory_health_t *out)
{
   if (!out)
      return -1;
   cJSON *response = domain_call(domain_request("health"));
   const cJSON *health = response ? cJSON_GetObjectItemCaseSensitive(response, "health") : NULL;
   if (!cJSON_IsObject(health))
   {
      cJSON_Delete(response);
      return -1;
   }
   memset(out, 0, sizeof(*out));
#define HEALTH_DOUBLE(field, key)                                                                  \
   do                                                                                              \
   {                                                                                               \
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(health, key);                              \
      if (cJSON_IsNumber(v))                                                                       \
         out->field = v->valuedouble;                                                              \
   } while (0)
#define HEALTH_INT(field, key)                                                                     \
   do                                                                                              \
   {                                                                                               \
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(health, key);                              \
      if (cJSON_IsNumber(v))                                                                       \
         out->field = v->valueint;                                                                 \
   } while (0)
   HEALTH_DOUBLE(contradiction_rate, "contradiction_rate");
   HEALTH_DOUBLE(promotion_rate, "promotion_rate");
   HEALTH_DOUBLE(demotion_rate, "demotion_rate");
   HEALTH_DOUBLE(staleness, "staleness");
   HEALTH_INT(total_contradictions, "total_contradictions");
   HEALTH_INT(total_promotions, "total_promotions");
   HEALTH_INT(total_demotions, "total_demotions");
   HEALTH_INT(total_expirations, "total_expirations");
   HEALTH_INT(cycles, "cycles");
#undef HEALTH_DOUBLE
#undef HEALTH_INT
   cJSON_Delete(response);
   return 0;
}

int memory_fact_history(const char *key, memory_t *out, int max)
{
   if (!key || !key[0] || !out || max <= 0)
      return -1;
   cJSON *request = domain_request("fact-history");
   if (!request || !cJSON_AddStringToObject(request, "key", key) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "records") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (domain_memory_from_json(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
      {
         cJSON_Delete(response);
         return -1;
      }
   cJSON_Delete(response);
   return n;
}

static int prospective_from_json(const cJSON *row, memory_prospective_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
   const cJSON *count = cJSON_GetObjectItemCaseSensitive(row, "trigger_count");
   if (!out || !cJSON_IsNumber(id) || !cJSON_IsNumber(count))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->trigger_count = count->valueint;
#define PROSPECTIVE_COPY(field, key)                                                               \
   if (domain_copy(out->field, sizeof(out->field), row, key) != 0)                                 \
   return -1
   PROSPECTIVE_COPY(trigger_text, "trigger_text");
   PROSPECTIVE_COPY(action_text, "action_text");
   PROSPECTIVE_COPY(anchor_entity, "anchor_entity");
   PROSPECTIVE_COPY(anchor_file, "anchor_file");
   PROSPECTIVE_COPY(recurrence, "recurrence");
   PROSPECTIVE_COPY(state, "state");
   PROSPECTIVE_COPY(valid_until, "valid_until");
   PROSPECTIVE_COPY(source_session, "source_session");
   PROSPECTIVE_COPY(last_triggered_at, "last_triggered_at");
   PROSPECTIVE_COPY(created_at, "created_at");
   PROSPECTIVE_COPY(updated_at, "updated_at");
#undef PROSPECTIVE_COPY
   return 0;
}

static int prospective_rows(cJSON *response, memory_prospective_t *out, int max)
{
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "prospectives") : NULL;
   if (!cJSON_IsArray(rows) || !out || max <= 0)
      return -1;
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (prospective_from_json(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
         return -1;
   return n;
}

int memory_prospective_create(const char *trigger_text, const char *action_text,
                              const char *anchor_entity, const char *anchor_file,
                              const char *recurrence, const char *valid_until,
                              const char *source_session, memory_prospective_t *out)
{
   cJSON *request = domain_request("prospective-create");
   if (!request || !trigger_text || !trigger_text[0] || !action_text || !action_text[0] ||
       !cJSON_AddStringToObject(request, "trigger_text", trigger_text) ||
       !cJSON_AddStringToObject(request, "action_text", action_text) ||
       !cJSON_AddStringToObject(request, "anchor_entity", anchor_entity ? anchor_entity : "") ||
       !cJSON_AddStringToObject(request, "anchor_file", anchor_file ? anchor_file : "") ||
       !cJSON_AddStringToObject(request, "recurrence", recurrence ? recurrence : "") ||
       !cJSON_AddStringToObject(request, "valid_until", valid_until ? valid_until : "") ||
       !cJSON_AddStringToObject(request, "session_id", source_session ? source_session : ""))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   memory_prospective_t ignored;
   int n = prospective_rows(response, out ? out : &ignored, 1);
   cJSON_Delete(response);
   return n == 1 ? 0 : -1;
}

static int prospective_query(const char *operation, int64_t id, const char *state, const char *turn,
                             const char *entity, const char *file, memory_prospective_t *out,
                             int max)
{
   cJSON *request = domain_request(operation);
   if (!request || (id > 0 && !cJSON_AddNumberToObject(request, "id", (double)id)) ||
       (state && !cJSON_AddStringToObject(request, "state", state)) ||
       (turn && !cJSON_AddStringToObject(request, "query", turn)) ||
       (entity && !cJSON_AddStringToObject(request, "anchor_entity", entity)) ||
       (file && !cJSON_AddStringToObject(request, "anchor_file", file)) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int n = prospective_rows(response, out, max);
   cJSON_Delete(response);
   return n;
}

int memory_prospective_list(const char *state, memory_prospective_t *out, int max)
{
   return prospective_query("prospective-list", 0, state ? state : "", NULL, NULL, NULL, out, max);
}

int memory_prospective_get(int64_t id, memory_prospective_t *out)
{
   return prospective_query("prospective-get", id, NULL, NULL, NULL, NULL, out, 1) == 1 ? 0 : -1;
}

int memory_prospective_match(const char *turn_text, const char *active_entity,
                             const char *active_file, memory_prospective_t *out, int max)
{
   return prospective_query("prospective-match", 0, NULL, turn_text ? turn_text : "",
                            active_entity ? active_entity : "", active_file ? active_file : "", out,
                            max);
}

int memory_prospective_complete(int64_t id)
{
   return domain_id_update("prospective-complete", id, NULL, NULL);
}

int memory_prospective_mark_triggered(int64_t id)
{
   return domain_id_update("prospective-mark-triggered", id, NULL, NULL);
}

int memory_prospective_sweep_expired(void)
{
   cJSON *response = domain_call(domain_request("prospective-sweep"));
   int count = -1;
   domain_number(response, "prospective_expired", &count);
   cJSON_Delete(response);
   return count;
}

static int directive_from_json_object(const cJSON *row, memory_directive_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
   const cJSON *priority = cJSON_GetObjectItemCaseSensitive(row, "priority");
   const cJSON *a = cJSON_GetObjectItemCaseSensitive(row, "memory_a_id");
   const cJSON *b = cJSON_GetObjectItemCaseSensitive(row, "memory_b_id");
   const cJSON *resolution = cJSON_GetObjectItemCaseSensitive(row, "resolution_memory_id");
   const cJSON *surfaced = cJSON_GetObjectItemCaseSensitive(row, "surfaced_count");
   if (!out || !cJSON_IsNumber(id) || !cJSON_IsNumber(priority) || !cJSON_IsNumber(a) ||
       !cJSON_IsNumber(b) || !cJSON_IsNumber(resolution) || !cJSON_IsNumber(surfaced))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->priority = priority->valueint;
   out->memory_a_id = (int64_t)a->valuedouble;
   out->memory_b_id = (int64_t)b->valuedouble;
   out->resolution_memory_id = (int64_t)resolution->valuedouble;
   out->surfaced_count = surfaced->valueint;
#define DIRECTIVE_COPY(field, key)                                                                 \
   if (domain_copy(out->field, sizeof(out->field), row, key) != 0)                                 \
   return -1
   DIRECTIVE_COPY(question, "question");
   DIRECTIVE_COPY(topic, "topic");
   DIRECTIVE_COPY(anchor_entity, "anchor_entity");
   DIRECTIVE_COPY(anchor_file, "anchor_file");
   DIRECTIVE_COPY(cause, "cause");
   DIRECTIVE_COPY(state, "state");
   DIRECTIVE_COPY(evidence, "evidence");
   DIRECTIVE_COPY(source_session, "source_session");
   DIRECTIVE_COPY(last_surfaced_at, "last_surfaced_at");
   DIRECTIVE_COPY(resolved_at, "resolved_at");
   DIRECTIVE_COPY(valid_until, "valid_until");
   DIRECTIVE_COPY(created_at, "created_at");
   DIRECTIVE_COPY(updated_at, "updated_at");
#undef DIRECTIVE_COPY
   return 0;
}

int memory_directive_from_json(const cJSON *obj, memory_directive_t *out)
{
   return directive_from_json_object(obj, out);
}

cJSON *memory_directive_to_json(const memory_directive_t *d)
{
   if (!d)
      return NULL;
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
#define DIRECTIVE_STRING(field) cJSON_AddStringToObject(obj, #field, d->field)
   cJSON_AddNumberToObject(obj, "id", (double)d->id);
   DIRECTIVE_STRING(question);
   DIRECTIVE_STRING(topic);
   DIRECTIVE_STRING(anchor_entity);
   DIRECTIVE_STRING(anchor_file);
   DIRECTIVE_STRING(cause);
   cJSON_AddNumberToObject(obj, "priority", d->priority);
   DIRECTIVE_STRING(state);
   cJSON_AddNumberToObject(obj, "memory_a_id", (double)d->memory_a_id);
   cJSON_AddNumberToObject(obj, "memory_b_id", (double)d->memory_b_id);
   cJSON_AddNumberToObject(obj, "resolution_memory_id", (double)d->resolution_memory_id);
   DIRECTIVE_STRING(evidence);
   DIRECTIVE_STRING(source_session);
   cJSON_AddNumberToObject(obj, "surfaced_count", d->surfaced_count);
   DIRECTIVE_STRING(last_surfaced_at);
   DIRECTIVE_STRING(resolved_at);
   DIRECTIVE_STRING(valid_until);
   DIRECTIVE_STRING(created_at);
   DIRECTIVE_STRING(updated_at);
#undef DIRECTIVE_STRING
   return obj;
}

static int directive_rows(cJSON *response, memory_directive_t *out, int max)
{
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "directives") : NULL;
   if (!cJSON_IsArray(rows) || !out || max <= 0)
      return -1;
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (directive_from_json_object(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
         return -1;
   return n;
}

int memory_directive_create(const char *question, const char *topic, const char *anchor_entity,
                            const char *anchor_file, const char *cause, int priority,
                            int64_t memory_a_id, int64_t memory_b_id, const char *evidence,
                            const char *source_session, const char *valid_until,
                            memory_directive_t *out)
{
   cJSON *request = domain_request("directive-create");
   if (!request || !question || !question[0] || !cause || !cause[0] ||
       !cJSON_AddStringToObject(request, "question", question) ||
       !cJSON_AddStringToObject(request, "topic", topic ? topic : "") ||
       !cJSON_AddStringToObject(request, "anchor_entity", anchor_entity ? anchor_entity : "") ||
       !cJSON_AddStringToObject(request, "anchor_file", anchor_file ? anchor_file : "") ||
       !cJSON_AddStringToObject(request, "cause", cause) ||
       !cJSON_AddNumberToObject(request, "priority", priority) ||
       !cJSON_AddNumberToObject(request, "memory_a_id", (double)memory_a_id) ||
       !cJSON_AddNumberToObject(request, "memory_b_id", (double)memory_b_id) ||
       !cJSON_AddStringToObject(request, "evidence", evidence ? evidence : "") ||
       !cJSON_AddStringToObject(request, "session_id", source_session ? source_session : "") ||
       !cJSON_AddStringToObject(request, "valid_until", valid_until ? valid_until : ""))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   memory_directive_t ignored;
   int n = directive_rows(response, out ? out : &ignored, 1);
   cJSON_Delete(response);
   return n == 1 ? 0 : -1;
}

static int directive_query(const char *operation, int64_t id, const char *state, const char *cause,
                           const char *turn, const char *entity, const char *file,
                           memory_directive_t *out, int max)
{
   cJSON *request = domain_request(operation);
   if (!request || (id > 0 && !cJSON_AddNumberToObject(request, "id", (double)id)) ||
       (state && !cJSON_AddStringToObject(request, "state", state)) ||
       (cause && !cJSON_AddStringToObject(request, "cause", cause)) ||
       (turn && !cJSON_AddStringToObject(request, "query", turn)) ||
       (entity && !cJSON_AddStringToObject(request, "anchor_entity", entity)) ||
       (file && !cJSON_AddStringToObject(request, "anchor_file", file)) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int n = directive_rows(response, out, max);
   cJSON_Delete(response);
   return n;
}

int memory_directive_list(const char *state, const char *cause, memory_directive_t *out, int max)
{
   return directive_query("directive-list", 0, state ? state : "", cause ? cause : "", NULL, NULL,
                          NULL, out, max);
}

int memory_directive_get(int64_t id, memory_directive_t *out)
{
   return directive_query("directive-get", id, NULL, NULL, NULL, NULL, NULL, out, 1) == 1 ? 0 : -1;
}

int memory_directive_match(const char *turn_text, const char *active_entity,
                           const char *active_file, memory_directive_t *out, int max)
{
   return directive_query("directive-match", 0, NULL, NULL, turn_text ? turn_text : "",
                          active_entity ? active_entity : "", active_file ? active_file : "", out,
                          max);
}

int memory_directive_resolve(int64_t id, int64_t resolution_memory_id, const char *note)
{
   cJSON *request = domain_request("directive-resolve");
   if (!request || id <= 0 || !cJSON_AddNumberToObject(request, "id", (double)id) ||
       !cJSON_AddNumberToObject(request, "resolution_memory_id", (double)resolution_memory_id) ||
       !cJSON_AddStringToObject(request, "note", note ? note : ""))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int ok = domain_bool(response, "updated");
   cJSON_Delete(response);
   return ok ? 0 : -1;
}

int memory_directive_suppress(int64_t id)
{
   return domain_id_update("directive-suppress", id, NULL, NULL);
}

int memory_directive_mark_surfaced(int64_t id)
{
   return domain_id_update("directive-mark-surfaced", id, NULL, NULL);
}

int memory_directive_sweep_expired(void)
{
   cJSON *response = domain_call(domain_request("directive-sweep"));
   int count = -1;
   domain_number(response, "expired", &count);
   cJSON_Delete(response);
   return count;
}

int memory_directive_counts(memory_directive_counts_t *out)
{
   if (!out)
      return -1;
   cJSON *response = domain_call(domain_request("directive-count"));
   const cJSON *counts =
       response ? cJSON_GetObjectItemCaseSensitive(response, "directive_counts") : NULL;
   if (!cJSON_IsObject(counts))
   {
      cJSON_Delete(response);
      return -1;
   }
   const cJSON *open = cJSON_GetObjectItemCaseSensitive(counts, "open");
   const cJSON *suppressed = cJSON_GetObjectItemCaseSensitive(counts, "suppressed");
   const cJSON *resolved = cJSON_GetObjectItemCaseSensitive(counts, "resolved");
   const cJSON *expired = cJSON_GetObjectItemCaseSensitive(counts, "expired");
   if (!cJSON_IsNumber(open) || !cJSON_IsNumber(suppressed) || !cJSON_IsNumber(resolved) ||
       !cJSON_IsNumber(expired))
   {
      cJSON_Delete(response);
      return -1;
   }
   out->open = (int64_t)open->valuedouble;
   out->suppressed = (int64_t)suppressed->valuedouble;
   out->resolved = (int64_t)resolved->valuedouble;
   out->expired = (int64_t)expired->valuedouble;
   cJSON_Delete(response);
   return 0;
}

static int episode_from_json(const cJSON *row, memory_episode_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
   const cJSON *memory_id = cJSON_GetObjectItemCaseSensitive(row, "memory_id");
   if (!out || !cJSON_IsNumber(id) || !cJSON_IsNumber(memory_id))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->memory_id = (int64_t)memory_id->valuedouble;
   return domain_copy(out->episode_key, sizeof(out->episode_key), row, "episode_key") ||
                  domain_copy(out->episode_text, sizeof(out->episode_text), row, "episode_text") ||
                  domain_copy(out->source_session, sizeof(out->source_session), row,
                              "source_session") ||
                  domain_copy(out->reference_time, sizeof(out->reference_time), row,
                              "reference_time") ||
                  domain_copy(out->created_at, sizeof(out->created_at), row, "created_at")
              ? -1
              : 0;
}

static int episode_query(const char *operation, const char *key, const char *query,
                         memory_episode_t *out, int max)
{
   cJSON *request = domain_request(operation);
   if (!request || (key && !cJSON_AddStringToObject(request, "key", key)) ||
       (query && !cJSON_AddStringToObject(request, "query", query)) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "episodes") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (episode_from_json(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
      {
         cJSON_Delete(response);
         return -1;
      }
   cJSON_Delete(response);
   return n;
}

int memory_list_episodes(const char *query, int limit, memory_episode_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;
   return episode_query("episode-list", NULL, query ? query : "", out, limit);
}

int memory_get_episode(const char *episode_key, memory_episode_t *out)
{
   if (!episode_key || !episode_key[0] || !out)
      return -1;
   return episode_query("episode-get", episode_key, NULL, out, 1) == 1 ? 0 : -1;
}

static int relation_from_json(const cJSON *row, memory_relation_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
   const cJSON *memory_id = cJSON_GetObjectItemCaseSensitive(row, "memory_id");
   const cJSON *episode_id = cJSON_GetObjectItemCaseSensitive(row, "episode_id");
   const cJSON *weight = cJSON_GetObjectItemCaseSensitive(row, "weight");
   if (!out || !cJSON_IsNumber(id) || !cJSON_IsNumber(memory_id) || !cJSON_IsNumber(episode_id) ||
       !cJSON_IsNumber(weight))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->memory_id = (int64_t)memory_id->valuedouble;
   out->episode_id = (int64_t)episode_id->valuedouble;
   out->weight = weight->valuedouble;
   return domain_copy(out->src_entity, sizeof(out->src_entity), row, "source") ||
                  domain_copy(out->relation, sizeof(out->relation), row, "relation") ||
                  domain_copy(out->dst_entity, sizeof(out->dst_entity), row, "target") ||
                  domain_copy(out->fact_text, sizeof(out->fact_text), row, "fact") ||
                  domain_copy(out->valid_at, sizeof(out->valid_at), row, "valid_at") ||
                  domain_copy(out->invalid_at, sizeof(out->invalid_at), row, "invalid_at") ||
                  domain_copy(out->created_at, sizeof(out->created_at), row, "created_at")
              ? -1
              : 0;
}

static int relation_query(const char *operation, const char *query, const char *as_of,
                          const char *entity, int limit, memory_relation_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;
   cJSON *request = domain_request(operation);
   if (!request || (query && !cJSON_AddStringToObject(request, "query", query)) ||
       (as_of && !cJSON_AddStringToObject(request, "as_of", as_of)) ||
       (entity && !cJSON_AddStringToObject(request, "entity", entity)) ||
       !cJSON_AddNumberToObject(request, "limit", limit))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "relations") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (relation_from_json(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
      {
         cJSON_Delete(response);
         return -1;
      }
   cJSON_Delete(response);
   return n;
}

int memory_search_graph(const char *query, int limit, memory_relation_t *out, int max)
{
   return relation_query("relation-search", query ? query : "", NULL, NULL, limit, out, max);
}

int memory_search_graph_as_of(const char *query, const char *as_of, int limit,
                              memory_relation_t *out, int max)
{
   return relation_query("relation-search", query ? query : "", as_of ? as_of : "", NULL, limit,
                         out, max);
}

int memory_get_entity_edges(const char *entity, int limit, memory_relation_t *out, int max)
{
   if (!entity || !entity[0])
      return -1;
   return relation_query("entity-edges", NULL, NULL, entity, limit, out, max);
}

int memory_get_entity_profile(const char *entity, memory_entity_profile_t *out)
{
   if (!entity || !entity[0] || !out)
      return -1;
   cJSON *request = domain_request("entity-profile");
   if (!request || !cJSON_AddStringToObject(request, "entity", entity))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *profile =
       response ? cJSON_GetObjectItemCaseSensitive(response, "entity_profile") : NULL;
   const cJSON *mentions =
       profile ? cJSON_GetObjectItemCaseSensitive(profile, "mention_count") : NULL;
   const cJSON *relations =
       profile ? cJSON_GetObjectItemCaseSensitive(profile, "relation_count") : NULL;
   if (!cJSON_IsObject(profile) || !cJSON_IsNumber(mentions) || !cJSON_IsNumber(relations))
   {
      cJSON_Delete(response);
      return -1;
   }
   memset(out, 0, sizeof(*out));
   out->mention_count = mentions->valueint;
   out->relation_count = relations->valueint;
   domain_copy(out->entity, sizeof(out->entity), profile, "entity");
   domain_copy(out->latest_episode, sizeof(out->latest_episode), profile, "latest_episode");
   domain_copy(out->summary, sizeof(out->summary), profile, "summary");
   cJSON_Delete(response);
   return 0;
}

int memory_transition_lifecycle(int64_t memory_id, const char *new_state,
                                const char *archive_reason)
{
   cJSON *request = domain_request("lifecycle-transition");
   if (!request || memory_id <= 0 || !new_state || !new_state[0] ||
       !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddStringToObject(request, "lifecycle_state", new_state) ||
       !cJSON_AddStringToObject(request, "archive_reason", archive_reason ? archive_reason : ""))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int ok = domain_bool(response, "updated");
   cJSON_Delete(response);
   return ok ? 0 : -1;
}

int memory_mark_pending(int64_t memory_id, int ttl_days)
{
   cJSON *request = domain_request("lifecycle-pending");
   if (!request || memory_id <= 0 || ttl_days <= 0 ||
       !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddNumberToObject(request, "ttl_days", ttl_days))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int ok = domain_bool(response, "updated");
   cJSON_Delete(response);
   return ok ? 0 : -1;
}

int memory_lifecycle_sweep_expired(void)
{
   cJSON *response = domain_call(domain_request("lifecycle-sweep"));
   int count = -1;
   domain_number(response, "count", &count);
   cJSON_Delete(response);
   return count;
}

int memory_lifecycle_counts(memory_lifecycle_counts_t *out)
{
   if (!out)
      return -1;
   cJSON *response = domain_call(domain_request("lifecycle-count"));
   const cJSON *counts = response ? cJSON_GetObjectItemCaseSensitive(response, "lifecycle") : NULL;
   if (!cJSON_IsObject(counts))
   {
      cJSON_Delete(response);
      return -1;
   }
   const char *names[] = {"active", "pending", "fulfilled", "superseded", "archived"};
   int64_t *targets[] = {&out->active, &out->pending, &out->fulfilled, &out->superseded,
                         &out->archived};
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(counts, names[i]);
      if (!cJSON_IsNumber(value))
      {
         cJSON_Delete(response);
         return -1;
      }
      *targets[i] = (int64_t)value->valuedouble;
   }
   cJSON_Delete(response);
   return 0;
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

cJSON *memory_recall_activated(const char *task_hint, int limit_tokens, int session_start,
                               const struct memory_activation *activation)
{
   (void)activation;
   return memory_recall(task_hint, limit_tokens, session_start);
}

cJSON *memory_briefing(int limit_tokens)
{
   return domain_payload_call("briefing-bundle", NULL, NULL, limit_tokens, 0);
}

cJSON *memory_alerts(const char *since)
{
   return domain_payload_call("alerts-bundle", NULL, since ? since : "", 0, 0);
}

static char *domain_context_call(const char *query, const char *block_type, int limit)
{
   cJSON *request = domain_request(block_type ? "context-block" : "assemble-context");
   if (!request || !cJSON_AddStringToObject(request, "query", query ? query : "") ||
       (block_type && !cJSON_AddStringToObject(request, "block_type", block_type)) ||
       !cJSON_AddNumberToObject(request, "limit", limit > 0 ? limit : 12))
   {
      cJSON_Delete(request);
      return NULL;
   }
   cJSON *response = domain_call(request);
   const cJSON *block = response ? cJSON_GetObjectItemCaseSensitive(response, "block") : NULL;
   char *result = cJSON_IsString(block) && block->valuestring ? strdup(block->valuestring) : NULL;
   cJSON_Delete(response);
   return result;
}

char *memory_assemble_context(const char *task_hint)
{
   return domain_context_call(task_hint ? task_hint : "", NULL, 12);
}

char *memory_assemble_context_ws(const char *task_hint, const char *workspace)
{
   (void)workspace;
   return memory_assemble_context(task_hint);
}

char *memory_get_context_block(const char *query, const char *block_type, int limit)
{
   return domain_context_call(query ? query : "", block_type ? block_type : "general", limit);
}

static int diagnostic_from_json(const cJSON *row, memory_diagnostic_t *out)
{
   const cJSON *memory = cJSON_GetObjectItemCaseSensitive(row, "memory");
   const cJSON *parts = cJSON_GetObjectItemCaseSensitive(row, "parts");
   if (!out || domain_memory_from_json(memory, &out->memory) != 0 || !cJSON_IsObject(parts))
      return -1;
   memset(&out->parts, 0, sizeof(out->parts));
#define DIAG_PART(field)                                                                           \
   do                                                                                              \
   {                                                                                               \
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(parts, #field);                        \
      if (cJSON_IsNumber(value))                                                                   \
         out->parts.field = value->valuedouble;                                                    \
   } while (0)
   DIAG_PART(lexical);
   DIAG_PART(coverage);
   DIAG_PART(confidence);
   DIAG_PART(salience);
   DIAG_PART(hybrid_total);
   DIAG_PART(blended_total);
   DIAG_PART(total);
#undef DIAG_PART
   return 0;
}

static int diagnostic_call(const char *operation, const char *query, const char *scope_type,
                           const char *scope_value, int64_t id, int limit, memory_diagnostic_t *out,
                           int max)
{
   if (!out || max <= 0)
      return -1;
   cJSON *request = domain_request(operation);
   cJSON *scope =
       request && scope_type && scope_type[0] ? cJSON_AddObjectToObject(request, "scope") : NULL;
   if (!request || !cJSON_AddStringToObject(request, "query", query ? query : "") ||
       !cJSON_AddNumberToObject(request, "limit", limit > 0 ? limit : max) ||
       (id > 0 && !cJSON_AddNumberToObject(request, "id", (double)id)) ||
       (scope && (!cJSON_AddStringToObject(scope, "type", scope_type) ||
                  !cJSON_AddStringToObject(scope, "value", scope_value ? scope_value : ""))))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "diagnostics") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (diagnostic_from_json(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
      {
         cJSON_Delete(response);
         return -1;
      }
   cJSON_Delete(response);
   return n;
}

int memory_diagnose_scoped(const char *query, const char *scope_type, const char *scope_value,
                           int limit, memory_diagnostic_t *out, int max)
{
   return diagnostic_call("diagnose", query, scope_type, scope_value, 0, limit, out, max);
}

int memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max)
{
   return memory_diagnose_scoped(query, NULL, NULL, limit, out, max);
}

int memory_explain_match(const char *query, int64_t memory_id, memory_diagnostic_t *out)
{
   return diagnostic_call("explain", query, NULL, NULL, memory_id, 1, out, 1) == 1 ? 0 : -1;
}

static int domain_ask(const char *query, const char *scope_type, const char *scope_value, int limit,
                      memory_answer_result_t *out)
{
   if (!out)
      return -1;
   cJSON *request = domain_request("ask");
   cJSON *scope =
       request && scope_type && scope_type[0] ? cJSON_AddObjectToObject(request, "scope") : NULL;
   if (!request || !cJSON_AddStringToObject(request, "query", query ? query : "") ||
       !cJSON_AddNumberToObject(request, "limit", limit > 0 ? limit : 5) ||
       (scope && (!cJSON_AddStringToObject(scope, "type", scope_type) ||
                  !cJSON_AddStringToObject(scope, "value", scope_value ? scope_value : ""))))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *answer = response ? cJSON_GetObjectItemCaseSensitive(response, "answer") : NULL;
   const cJSON *confidence = answer ? cJSON_GetObjectItemCaseSensitive(answer, "confidence") : NULL;
   const cJSON *citations =
       answer ? cJSON_GetObjectItemCaseSensitive(answer, "citation_ids") : NULL;
   if (!cJSON_IsObject(answer) || !cJSON_IsNumber(confidence) || !cJSON_IsArray(citations))
   {
      cJSON_Delete(response);
      return -1;
   }
   memset(out, 0, sizeof(*out));
   out->confidence = confidence->valuedouble;
   out->no_answer = domain_bool(answer, "no_answer");
   out->low_confidence = domain_bool(answer, "low_confidence");
   domain_copy(out->answer, sizeof(out->answer), answer, "answer");
   domain_copy(out->evidence_mode, sizeof(out->evidence_mode), answer, "evidence_mode");
   domain_copy(out->error, sizeof(out->error), answer, "error");
   domain_number(answer, "retrieval_count", &out->retrieval_count);
   int n = cJSON_GetArraySize(citations);
   if (n > MEMORY_ANSWER_MAX_CITATIONS)
      n = MEMORY_ANSWER_MAX_CITATIONS;
   out->citation_count = n;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *id = cJSON_GetArrayItem(citations, i);
      if (cJSON_IsNumber(id))
         out->citation_ids[i] = (int64_t)id->valuedouble;
   }
   out->evidence.decision =
       out->no_answer ? MEMORY_ANSWER_DECISION_ABSTAIN : MEMORY_ANSWER_DECISION_ANSWERABLE;
   out->evidence.reason =
       out->no_answer ? MEMORY_ANSWER_REASON_STRUCTURAL_EMPTY : MEMORY_ANSWER_REASON_OK;
   out->evidence.ranked_count = out->retrieval_count;
   out->evidence.topk_grounding = out->confidence;
   cJSON_Delete(response);
   return 0;
}

int memory_ask_query(const char *query, int limit, memory_answer_result_t *out)
{
   return domain_ask(query, NULL, NULL, limit, out);
}

int memory_ask_query_scoped(const char *query, const char *scope_type, const char *scope_value,
                            int limit, memory_answer_result_t *out)
{
   return domain_ask(query, scope_type, scope_value, limit, out);
}

static int domain_policy_code(const char *operation, const char *key, const char *value)
{
   cJSON *request = domain_request(operation);
   if (!request || !cJSON_AddStringToObject(request, key, value ? value : ""))
   {
      cJSON_Delete(request);
      return 99;
   }
   cJSON *response = domain_call(request);
   const cJSON *code = response ? cJSON_GetObjectItemCaseSensitive(response, "code") : NULL;
   int result = cJSON_IsNumber(code) ? code->valueint : 99;
   cJSON_Delete(response);
   return result;
}

memory_relation_kind_t memory_ontology_relation_from_text(const char *label)
{
   return (memory_relation_kind_t)domain_policy_code("ontology-relation-code", "relation", label);
}

const char *memory_ontology_relation_to_text(memory_relation_kind_t relation)
{
   return domain_policy_name("ontology-relation-name", NULL, NULL, "relation_code", (int)relation);
}

memory_node_kind_t memory_ontology_node_kind_from_text(const char *label)
{
   return (memory_node_kind_t)domain_policy_code("ontology-node-code", "kind", label);
}

const char *memory_ontology_node_kind_to_text(memory_node_kind_t kind)
{
   return domain_policy_name("ontology-node-name", NULL, NULL, "subject_kind", (int)kind);
}

int memory_ontology_validate(memory_node_kind_t subject_kind, memory_relation_kind_t relation,
                             memory_node_kind_t object_kind)
{
   cJSON *request = domain_request("ontology-validate");
   if (!request || !cJSON_AddNumberToObject(request, "subject_kind", (int)subject_kind) ||
       !cJSON_AddNumberToObject(request, "relation_code", (int)relation) ||
       !cJSON_AddNumberToObject(request, "object_kind", (int)object_kind))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   int allowed = domain_bool(response, "allowed");
   cJSON_Delete(response);
   return allowed;
}

int memory_ontology_rules(const memory_ontology_rule_t **out)
{
   static __thread memory_ontology_rule_t rules[512];
   if (!out)
      return -1;
   *out = NULL;
   cJSON *response = domain_call(domain_request("ontology-rules"));
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "rules") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > (int)(sizeof(rules) / sizeof(rules[0])))
      n = (int)(sizeof(rules) / sizeof(rules[0]));
   for (int i = 0; i < n; ++i)
   {
      const cJSON *row = cJSON_GetArrayItem(rows, i);
      const cJSON *subject = cJSON_GetObjectItemCaseSensitive(row, "subject_kind");
      const cJSON *relation = cJSON_GetObjectItemCaseSensitive(row, "relation");
      const cJSON *object = cJSON_GetObjectItemCaseSensitive(row, "object_kind");
      if (!cJSON_IsNumber(subject) || !cJSON_IsNumber(relation) || !cJSON_IsNumber(object))
      {
         cJSON_Delete(response);
         return -1;
      }
      rules[i].sk = (memory_node_kind_t)subject->valueint;
      rules[i].rel = (memory_relation_kind_t)relation->valueint;
      rules[i].ok = (memory_node_kind_t)object->valueint;
   }
   cJSON_Delete(response);
   *out = rules;
   return n;
}

static int domain_query_records(const char *mode, const char *pattern, int days, memory_t *out,
                                int max)
{
   if (!out || max <= 0)
      return -1;
   cJSON *request = domain_request("query-records");
   if (!request || !cJSON_AddStringToObject(request, "mode", mode) ||
       !cJSON_AddStringToObject(request, "pattern", pattern ? pattern : "") ||
       !cJSON_AddNumberToObject(request, "days", days) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *rows = response ? cJSON_GetObjectItemCaseSensitive(response, "records") : NULL;
   if (!cJSON_IsArray(rows))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(rows);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
      if (domain_memory_from_json(cJSON_GetArrayItem(rows, i), &out[i]) != 0)
      {
         cJSON_Delete(response);
         return -1;
      }
   cJSON_Delete(response);
   return n;
}

int db2_memory_find_facts_like(const char *query, int limit, memory_t *out, int max)
{
   if (limit > 0 && limit < max)
      max = limit;
   return domain_query_records("like", query ? query : "", 0, out, max);
}

int db2_memory_top_l2_facts(memory_t *out, int max)
{
   return domain_query_records("top-l2", "", 0, out, max);
}

int db2_memory_list_session_scope_priority(memory_t *out, int max)
{
   return domain_query_records("session-priority", "", 0, out, max);
}

int db2_memory_list_session_scope_priority_like(const char *pattern, memory_t *out, int max)
{
   return domain_query_records("session-priority", pattern ? pattern : "", 0, out, max);
}

int db2_memory_search_facts_patterns_by_keyword(const char *keyword, memory_t *out, int max)
{
   return domain_query_records("facts-patterns", keyword ? keyword : "", 0, out, max);
}

int db2_memory_load_eval_corpus(memory_t *out, int max, char *label_out, size_t label_len)
{
   int n = domain_query_records("eval", "", 0, out, max);
   if (label_out && label_len)
      snprintf(label_out, label_len, "%s", n > 0 ? "durable L1-L3" : "");
   return n < 0 ? 0 : n;
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

int64_t db2_memory_find_id_by_key_kind(const char *key, const char *kind)
{
   cJSON *request = domain_request("find-id");
   if (!request || !key || !key[0] || !cJSON_AddStringToObject(request, "key", key) ||
       !cJSON_AddStringToObject(request, "kind", kind ? kind : ""))
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

int db2_memory_epistemic_kind(int64_t memory_id, char *out, size_t out_cap)
{
   if (memory_id <= 0 || !out || !out_cap)
      return -1;
   cJSON *request = domain_request("epistemic-kind");
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)memory_id))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int rc = domain_copy(out, out_cap, response, "name");
   cJSON_Delete(response);
   return rc;
}

int db2_memory_conflict_list(conflict_t *out, int max)
{
   return memory_list_conflicts(out, max);
}

void db2_memory_scope_tag_insert(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   (void)memory_tag_scope(memory_id, scope_type, scope_value);
}

void db2_memory_workspace_tag_insert(int64_t memory_id, const char *workspace)
{
   (void)memory_tag_workspace(memory_id, workspace);
}

int db2_memory_promotion_demote_id(int64_t memory_id)
{
   return domain_id_update("demote-confidence", memory_id, NULL, NULL) == 0 ? 1 : 0;
}

int db2_memory_reject(int64_t memory_id, const char *reason)
{
   (void)reason;
   return domain_id_update("demote-confidence", memory_id, NULL, NULL);
}

int db2_memory_list_low_effectiveness(double threshold, int limit, db2_memory_low_eff_row_t *rows,
                                      int max)
{
   if (!rows || max <= 0)
      return 0;
   if (limit <= 0 || limit > max)
      limit = max;
   cJSON *request = domain_request("low-effectiveness");
   if (!request || !cJSON_AddNumberToObject(request, "confidence", threshold) ||
       !cJSON_AddNumberToObject(request, "limit", limit))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   const cJSON *items =
       response ? cJSON_GetObjectItemCaseSensitive(response, "low_effectiveness") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return 0;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
      const cJSON *effectiveness = cJSON_GetObjectItemCaseSensitive(item, "effectiveness");
      const cJSON *uses = cJSON_GetObjectItemCaseSensitive(item, "use_count");
      if (!cJSON_IsNumber(id) || !cJSON_IsNumber(effectiveness) || !cJSON_IsNumber(uses))
      {
         cJSON_Delete(response);
         return 0;
      }
      memset(&rows[i], 0, sizeof(rows[i]));
      rows[i].id = (int64_t)id->valuedouble;
      rows[i].effectiveness = effectiveness->valuedouble;
      rows[i].use_count = uses->valueint;
      domain_copy(rows[i].tier, sizeof(rows[i].tier), item, "tier");
      domain_copy(rows[i].kind, sizeof(rows[i].kind), item, "kind");
      domain_copy(rows[i].key, sizeof(rows[i].key), item, "key");
   }
   cJSON_Delete(response);
   return n;
}

int db2_memory_list_unused_l2(int days, db2_memory_unused_l2_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   cJSON *request = domain_request("unused-l2");
   if (!request || !cJSON_AddNumberToObject(request, "days", days) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   const cJSON *items = response ? cJSON_GetObjectItemCaseSensitive(response, "records") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return 0;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      memory_t memory;
      if (domain_memory_from_json(cJSON_GetArrayItem(items, i), &memory) != 0)
      {
         cJSON_Delete(response);
         return 0;
      }
      memset(&rows[i], 0, sizeof(rows[i]));
      rows[i].id = memory.id;
      rows[i].confidence = memory.confidence;
      snprintf(rows[i].key, sizeof(rows[i].key), "%s", memory.key);
      snprintf(rows[i].tier, sizeof(rows[i].tier), "%s", memory.tier);
      snprintf(rows[i].kind, sizeof(rows[i].kind), "%s", memory.kind);
   }
   cJSON_Delete(response);
   return n;
}

int db2_memory_list_superseded_keys(int min_versions, db2_memory_superseded_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   cJSON *request = domain_request("superseded-keys");
   if (!request || !cJSON_AddNumberToObject(request, "min_versions", min_versions) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   const cJSON *items =
       response ? cJSON_GetObjectItemCaseSensitive(response, "superseded_keys") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return 0;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      const cJSON *versions = cJSON_GetObjectItemCaseSensitive(item, "versions");
      memset(&rows[i], 0, sizeof(rows[i]));
      domain_copy(rows[i].base_key, sizeof(rows[i].base_key), item, "base_key");
      rows[i].versions = cJSON_IsNumber(versions) ? versions->valueint : 0;
   }
   cJSON_Delete(response);
   return n;
}

int db2_memory_set_artifact(int64_t memory_id, const char *artifact_type, const char *artifact_ref,
                            const char *artifact_hash)
{
   cJSON *request = domain_request("set-artifact");
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddStringToObject(request, "artifact_type", artifact_type ? artifact_type : "") ||
       !cJSON_AddStringToObject(request, "artifact_ref", artifact_ref ? artifact_ref : "") ||
       !cJSON_AddStringToObject(request, "artifact_hash", artifact_hash ? artifact_hash : ""))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   int updated = domain_bool(response, "updated");
   cJSON_Delete(response);
   return updated;
}

int db2_memory_review_list(const char *state, int limit, db2_memory_review_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;
   cJSON *request = domain_request("review-list");
   if (!request || !cJSON_AddStringToObject(request, "state", state ? state : "") ||
       !cJSON_AddNumberToObject(request, "limit", limit))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *items = response ? cJSON_GetObjectItemCaseSensitive(response, "reviews") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
      const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(item, "confidence");
      if (!cJSON_IsNumber(id) || !cJSON_IsNumber(confidence))
      {
         cJSON_Delete(response);
         return -1;
      }
      memset(&out[i], 0, sizeof(out[i]));
      out[i].id = (int64_t)id->valuedouble;
      out[i].confidence = confidence->valuedouble;
#define REVIEW_COPY(field) domain_copy(out[i].field, sizeof(out[i].field), item, #field)
      REVIEW_COPY(tier);
      REVIEW_COPY(kind);
      REVIEW_COPY(key);
      REVIEW_COPY(content);
      REVIEW_COPY(lifecycle_state);
      REVIEW_COPY(review_reason);
      REVIEW_COPY(scope_type);
      REVIEW_COPY(scope_value);
      REVIEW_COPY(created_at);
      REVIEW_COPY(updated_at);
#undef REVIEW_COPY
   }
   cJSON_Delete(response);
   return n;
}

int db2_memory_restore(int64_t memory_id, const char *actor)
{
   cJSON *request = domain_request("restore");
   if (!request || memory_id <= 0 || !actor || !actor[0] ||
       !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddStringToObject(request, "actor", actor))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int updated = domain_bool(response, "updated");
   cJSON_Delete(response);
   return updated ? 0 : -1;
}

int db2_memory_summaries_list(int64_t memory_id, int limit, db2_memory_summary_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;
   cJSON *request = domain_request("summaries");
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddNumberToObject(request, "limit", limit))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *items = response ? cJSON_GetObjectItemCaseSensitive(response, "summaries") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      memset(&out[i], 0, sizeof(out[i]));
      domain_copy(out[i].scope, sizeof(out[i].scope), item, "scope");
      domain_copy(out[i].summary, sizeof(out[i].summary), item, "summary");
   }
   cJSON_Delete(response);
   return n;
}

int db2_memory_scenes_list_recent(db2_memory_scene_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return -1;
   cJSON *request = domain_request("scenes");
   if (!request || !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *items = response ? cJSON_GetObjectItemCaseSensitive(response, "scenes") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
      const cJSON *turns = cJSON_GetObjectItemCaseSensitive(item, "turn_count");
      if (!cJSON_IsNumber(id) || !cJSON_IsNumber(turns))
      {
         cJSON_Delete(response);
         return -1;
      }
      memset(&rows[i], 0, sizeof(rows[i]));
      rows[i].id = (int64_t)id->valuedouble;
      rows[i].turn_count = turns->valueint;
      domain_copy(rows[i].workspace_id, sizeof(rows[i].workspace_id), item, "workspace_id");
      domain_copy(rows[i].created_at, sizeof(rows[i].created_at), item, "created_at");
   }
   cJSON_Delete(response);
   return n;
}

int db2_memory_scene_members(int64_t scene_id, db2_memory_scene_member_t *rows, int max)
{
   if (!rows || max <= 0)
      return -1;
   cJSON *request = domain_request("scene-members");
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)scene_id) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *items =
       response ? cJSON_GetObjectItemCaseSensitive(response, "scene_members") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "memory_id");
      const cJSON *strength = cJSON_GetObjectItemCaseSensitive(item, "membership_strength");
      if (!cJSON_IsNumber(id) || !cJSON_IsNumber(strength))
      {
         cJSON_Delete(response);
         return -1;
      }
      memset(&rows[i], 0, sizeof(rows[i]));
      rows[i].memory_id = (int64_t)id->valuedouble;
      rows[i].membership_strength = strength->valuedouble;
      domain_copy(rows[i].key, sizeof(rows[i].key), item, "key");
   }
   cJSON_Delete(response);
   return n;
}

int db2_memory_alloc_all_ids(int64_t **out_ids, size_t *out_count)
{
   if (!out_ids || !out_count)
      return -1;
   *out_ids = NULL;
   *out_count = 0;
   cJSON *response = domain_call(domain_request("all-ids"));
   const cJSON *ids = response ? cJSON_GetObjectItemCaseSensitive(response, "ids") : NULL;
   if (!cJSON_IsArray(ids))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(ids);
   if (n > 0)
   {
      *out_ids = calloc((size_t)n, sizeof(**out_ids));
      if (!*out_ids)
      {
         cJSON_Delete(response);
         return -1;
      }
      for (int i = 0; i < n; ++i)
      {
         const cJSON *id = cJSON_GetArrayItem(ids, i);
         if (!cJSON_IsNumber(id))
         {
            free(*out_ids);
            *out_ids = NULL;
            cJSON_Delete(response);
            return -1;
         }
         (*out_ids)[i] = (int64_t)id->valuedouble;
      }
   }
   *out_count = (size_t)n;
   cJSON_Delete(response);
   return 0;
}

int db2_memory_count_by_tier_kind(db2_memory_tier_kind_count_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   cJSON *request = domain_request("tier-kind-counts");
   if (!request || !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *items =
       response ? cJSON_GetObjectItemCaseSensitive(response, "tier_kind_counts") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return -1;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      const cJSON *count = cJSON_GetObjectItemCaseSensitive(item, "count");
      memset(&out[i], 0, sizeof(out[i]));
      if (!cJSON_IsObject(item) || !cJSON_IsNumber(count) ||
          domain_copy(out[i].tier, sizeof(out[i].tier), item, "tier") != 0 ||
          domain_copy(out[i].kind, sizeof(out[i].kind), item, "kind") != 0)
      {
         cJSON_Delete(response);
         return -1;
      }
      out[i].count = count->valueint;
   }
   cJSON_Delete(response);
   return n;
}

int memory_effectiveness_stats(effectiveness_stats_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *response = domain_call(domain_request("effectiveness-stats"));
   const cJSON *stats =
       response ? cJSON_GetObjectItemCaseSensitive(response, "effectiveness") : NULL;
   const cJSON *average = stats ? cJSON_GetObjectItemCaseSensitive(stats, "average") : NULL;
   const cJSON *low = stats ? cJSON_GetObjectItemCaseSensitive(stats, "low_count") : NULL;
   const cJSON *high = stats ? cJSON_GetObjectItemCaseSensitive(stats, "high_impact_count") : NULL;
   const cJSON *never = stats ? cJSON_GetObjectItemCaseSensitive(stats, "never_surfaced_l2") : NULL;
   if (!cJSON_IsNumber(average) || !cJSON_IsNumber(low) || !cJSON_IsNumber(high) ||
       !cJSON_IsNumber(never))
   {
      cJSON_Delete(response);
      return -1;
   }
   out->avg_effectiveness = average->valuedouble;
   out->low_effectiveness_count = low->valueint;
   out->high_impact_count = high->valueint;
   out->never_surfaced_l2 = never->valueint;
   cJSON_Delete(response);
   return 0;
}

int memory_lint_run(memory_lint_issue_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *request = domain_request("lint");
   if (!request || !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   const cJSON *items = response ? cJSON_GetObjectItemCaseSensitive(response, "lint_issues") : NULL;
   if (!cJSON_IsArray(items))
   {
      cJSON_Delete(response);
      return 0;
   }
   int n = cJSON_GetArraySize(items);
   if (n > max)
      n = max;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "memory_id");
      memset(&out[i], 0, sizeof(out[i]));
      if (!cJSON_IsObject(item) ||
          domain_copy(out[i].type, sizeof(out[i].type), item, "type") != 0 ||
          domain_copy(out[i].message, sizeof(out[i].message), item, "message") != 0)
      {
         cJSON_Delete(response);
         return 0;
      }
      if (cJSON_IsNumber(id))
         out[i].memory_id = (int64_t)id->valuedouble;
      const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
      if (cJSON_IsString(key) && key->valuestring)
         snprintf(out[i].key, sizeof(out[i].key), "%s", key->valuestring);
   }
   cJSON_Delete(response);
   return n;
}
