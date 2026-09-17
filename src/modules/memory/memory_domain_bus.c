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
#include "memory_query.h"
#include "memory_scenes.h"
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

const char *memory_ontology_node_kind_to_text(memory_node_kind_t kind)
{
   return domain_policy_name("ontology-node-name", NULL, NULL, "subject_kind", (int)kind);
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

void db2_memory_scope_tag_insert(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   (void)memory_tag_scope(memory_id, scope_type, scope_value);
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
