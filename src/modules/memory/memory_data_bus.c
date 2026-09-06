/* Legacy ABI translation for the Go memory data stage.
 *
 * This file is intentionally limited to event-bus connection work: build a
 * bounded JSON request, invoke memory:7, and copy the reply into the existing C
 * structs while callers migrate. No persistence, ranking, lifecycle, or scope
 * policy is implemented here.
 */
#include "aimee.h"
#include "headers/module_json_call.h"

#include <aimee/memory/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>

#include "cJSON.h"
#include "memory_bus_context.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MEMORY_DATA_TIMEOUT_MS 5000

static memory_audit_hook_fn memory_audit_hook;
static void (*memory_context_reader)(db2_memory_scope_context_t *);

void memory_bus_set_context_reader(void (*reader)(db2_memory_scope_context_t *))
{
   memory_context_reader = reader;
}

void memory_bus_read_context(db2_memory_scope_context_t *context)
{
   memset(context, 0, sizeof(*context));
   if (memory_context_reader)
      memory_context_reader(context);
}

void memory_set_audit_hook(memory_audit_hook_fn hook)
{
   memory_audit_hook = hook;
}

void memory_audit_emit(const char *op, int64_t id, const char *tier, const char *kind,
                       const char *key, double confidence, const char *session_id)
{
   if (memory_audit_hook)
      memory_audit_hook(op, id, tier, kind, key, confidence, session_id);
}

static cJSON *memory_data_call(cJSON *request)
{
   if (memory_bus_add_context(request) != 0)
   {
      cJSON_Delete(request);
      return NULL;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   return aimee_module_json_call(AIMEE_MEMORY_EVENT_DATA, AIMEE_MEMORY_STAGE_DATA, request,
                                 AIMEE_MODULE_MESSAGE_MAX_BODY, MEMORY_DATA_TIMEOUT_MS, &result);
}

static int copy_string(char *out, size_t cap, const cJSON *object, const char *name)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
   if (!cJSON_IsString(value) || !value->valuestring || strlen(value->valuestring) >= cap)
      return -1;
   memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
   return 0;
}

static int memory_from_json(const cJSON *object, memory_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
   const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(object, "confidence");
   if (!out || !cJSON_IsObject(object) || !cJSON_IsNumber(id) || id->valuedouble <= 0 ||
       !cJSON_IsNumber(confidence))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->confidence = confidence->valuedouble;
   const cJSON *content = cJSON_GetObjectItemCaseSensitive(object, "content");
   if (!cJSON_IsString(content))
      return -1;
   size_t length = strlen(content->valuestring);
   if (length >= sizeof(out->content))
   {
      length = sizeof(out->content) - 1;
      while (length && ((unsigned char)content->valuestring[length] & 0xc0) == 0x80)
         --length;
   }
   memcpy(out->content, content->valuestring, length);
   out->content[length] = '\0';
   return copy_string(out->tier, sizeof(out->tier), object, "tier") == 0 &&
                  copy_string(out->kind, sizeof(out->kind), object, "kind") == 0 &&
                  copy_string(out->key, sizeof(out->key), object, "key") == 0
              ? 0
              : -1;
}

static int records_from_response(cJSON *response, memory_t *out, int max)
{
   const cJSON *records = response ? cJSON_GetObjectItemCaseSensitive(response, "records") : NULL;
   if (!cJSON_IsArray(records) || !out || max <= 0)
      return -1;
   int count = cJSON_GetArraySize(records);
   if (count > max)
      count = max;
   for (int i = 0; i < count; ++i)
      if (memory_from_json(cJSON_GetArrayItem(records, i), &out[i]) != 0)
         return -1;
   return count;
}

static int add_scope(cJSON *request, const char *type, const char *value)
{
   if (!type || !type[0])
      return 0;
   cJSON *scope = cJSON_AddObjectToObject(request, "scope");
   return scope && cJSON_AddStringToObject(scope, "type", type) &&
                  (!value || !value[0] || cJSON_AddStringToObject(scope, "value", value))
              ? 0
              : -1;
}

static int search_bus(const char *query, const char *scope_type, const char *scope_value,
                      const char *tier, const char *kind, int limit, memory_t *out, int max)
{
   if (!out || max <= 0 || limit <= 0)
      return -1;
   if (limit > max)
      limit = max;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "search") ||
       !cJSON_AddStringToObject(request, "query", query ? query : "") ||
       !cJSON_AddNumberToObject(request, "limit", limit) ||
       (tier && tier[0] && !cJSON_AddStringToObject(request, "tier", tier)) ||
       (kind && kind[0] && !cJSON_AddStringToObject(request, "kind", kind)) ||
       add_scope(request, scope_type, scope_value) != 0)
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   int count = records_from_response(response, out, max);
   cJSON_Delete(response);
   return count;
}

int memory_find_facts(const char *query, int limit, memory_t *out, int max)
{
   return search_bus(query, NULL, NULL, NULL, NULL, limit, out, max);
}

int memory_find_facts_scoped(const char *query, const char *scope_type, const char *scope_value,
                             int limit, memory_t *out, int max)
{
   return search_bus(query, scope_type, scope_value, NULL, NULL, limit, out, max);
}

int memory_find_facts_visible(const char *query, const char *workspace, const char *project,
                              int limit, memory_t *out, int max)
{
   return memory_find_facts_visible_ex(query, workspace, project, 0, limit, out, max);
}

int memory_find_facts_visible_ex(const char *query, const char *workspace, const char *project,
                                 int include_all, int limit, memory_t *out, int max)
{
   if (!out || max <= 0 || limit <= 0)
      return -1;
   if (limit > max)
      limit = max;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "visible-search") ||
       !cJSON_AddStringToObject(request, "query", query ? query : "") ||
       !cJSON_AddNumberToObject(request, "limit", limit) ||
       !cJSON_AddBoolToObject(request, "include_all", include_all != 0) ||
       (workspace && workspace[0] && !cJSON_AddStringToObject(request, "workspace", workspace)) ||
       (project && project[0] && !cJSON_AddStringToObject(request, "project", project)))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   int count = records_from_response(response, out, max);
   cJSON_Delete(response);
   return count;
}

int memory_list(const char *tier, const char *kind, int limit, memory_t *out, int max)
{
   return search_bus("", NULL, NULL, tier, kind, limit, out, max);
}

int memory_get_result(int64_t id, memory_t *out)
{
   if (id <= 0 || !out)
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "get") ||
       !cJSON_AddNumberToObject(request, "id", (double)id))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   int count = records_from_response(response, out, 1);
   cJSON_Delete(response);
   return count == 1 ? 0 : count == 0 ? 1 : -1;
}

int memory_get(int64_t id, memory_t *out)
{
   return memory_get_result(id, out) == 0 ? 0 : -1;
}

int db2_memory_get(int64_t id, memory_t *out)
{
   return memory_get(id, out);
}

char *memory_content_dup(int64_t memory_id)
{
   if (memory_id <= 0)
      return NULL;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "get") ||
       !cJSON_AddNumberToObject(request, "id", (double)memory_id))
   {
      cJSON_Delete(request);
      return NULL;
   }
   cJSON *response = memory_data_call(request);
   const cJSON *records = response ? cJSON_GetObjectItemCaseSensitive(response, "records") : NULL;
   const cJSON *record = cJSON_IsArray(records) ? cJSON_GetArrayItem(records, 0) : NULL;
   const cJSON *content = record ? cJSON_GetObjectItemCaseSensitive(record, "content") : NULL;
   char *copy =
       cJSON_IsString(content) && content->valuestring ? strdup(content->valuestring) : NULL;
   cJSON_Delete(response);
   return copy;
}

int memory_valid_at(int64_t memory_id, const char *as_of)
{
   if (memory_id <= 0 || !as_of || !as_of[0])
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "valid-at") ||
       !cJSON_AddNumberToObject(request, "id", (double)memory_id) ||
       !cJSON_AddStringToObject(request, "as_of", as_of))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   const cJSON *valid = response ? cJSON_GetObjectItemCaseSensitive(response, "valid_at") : NULL;
   int result = cJSON_IsBool(valid) ? cJSON_IsTrue(valid) : -1;
   cJSON_Delete(response);
   return result;
}

int db2_memory_provenance_by_id(int64_t memory_id, char *kind_out, int kind_len, char *source_out,
                                int source_len, char *version_out, int version_len)
{
   memory_t record;
   if (memory_get(memory_id, &record) != 0)
      return 0;
   if (kind_out && kind_len > 0)
      snprintf(kind_out, (size_t)kind_len, "%s", record.kind);
   if (source_out && source_len > 0)
      source_out[0] = '\0';
   if (version_out && version_len > 0)
      version_out[0] = '\0';
   return 1;
}

int memory_supersede(int64_t old_id, const char *new_content, double confidence,
                     const char *session_id, memory_t *out)
{
   (void)session_id;
   if (old_id <= 0 || !new_content || !new_content[0])
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "supersede") ||
       !cJSON_AddNumberToObject(request, "id", (double)old_id) ||
       !cJSON_AddStringToObject(request, "content", new_content) ||
       !cJSON_AddNumberToObject(request, "confidence", confidence))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   memory_t ignored;
   int count = records_from_response(response, out ? out : &ignored, 1);
   cJSON_Delete(response);
   if (count == 1)
   {
      const memory_t *record = out ? out : &ignored;
      memory_audit_emit("memory.supersede", record->id, record->tier, record->kind, record->key,
                        record->confidence, session_id ? session_id : "");
   }
   return count == 1 ? 0 : -1;
}

int64_t memory_upsert_workflow(const char *workspace, const char *signal_type, const char *rule,
                               double observed_confidence, const char *session_id)
{
   if (!workspace || !workspace[0] || !signal_type || !signal_type[0] || !rule || !rule[0])
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "upsert-workflow") ||
       !cJSON_AddStringToObject(request, "workspace", workspace) ||
       !cJSON_AddStringToObject(request, "signal_type", signal_type) ||
       !cJSON_AddStringToObject(request, "rule", rule) ||
       !cJSON_AddNumberToObject(request, "confidence", observed_confidence) ||
       (session_id && session_id[0] && !cJSON_AddStringToObject(request, "session_id", session_id)))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   memory_t result;
   int count = records_from_response(response, &result, 1);
   cJSON_Delete(response);
   return count == 1 ? result.id : -1;
}

int memory_apply_feedback(int success, int64_t *citation_ids, int citation_count)
{
   if (!citation_ids || citation_count <= 0 || citation_count > 64)
      return -1;
   cJSON *request = cJSON_CreateObject();
   cJSON *ids = request ? cJSON_AddArrayToObject(request, "ids") : NULL;
   if (!request || !ids || !cJSON_AddStringToObject(request, "operation", "feedback") ||
       !cJSON_AddBoolToObject(request, "success", success != 0))
   {
      cJSON_Delete(request);
      return -1;
   }
   for (int i = 0; i < citation_count; ++i)
      if (citation_ids[i] > 0)
         cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)citation_ids[i]));
   cJSON *response = memory_data_call(request);
   if (!response)
      return -1;
   cJSON_Delete(response);
   return 0;
}

int memory_run_maintenance(int *promoted, int *demoted, int *expired)
{
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "maintenance"))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   const cJSON *p = response ? cJSON_GetObjectItemCaseSensitive(response, "promoted") : NULL;
   const cJSON *d = response ? cJSON_GetObjectItemCaseSensitive(response, "demoted") : NULL;
   const cJSON *e = response ? cJSON_GetObjectItemCaseSensitive(response, "expired") : NULL;
   if (!cJSON_IsNumber(p) || !cJSON_IsNumber(d) || !cJSON_IsNumber(e))
   {
      cJSON_Delete(response);
      return -1;
   }
   if (promoted)
      *promoted = p->valueint;
   if (demoted)
      *demoted = d->valueint;
   if (expired)
      *expired = e->valueint;
   cJSON_Delete(response);
   return 0;
}

int memory_prospective_count_by_state(int *armed, int *triggered, int *completed, int *expired)
{
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "prospective-count"))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   const cJSON *a = response ? cJSON_GetObjectItemCaseSensitive(response, "armed") : NULL;
   const cJSON *t = response ? cJSON_GetObjectItemCaseSensitive(response, "triggered") : NULL;
   const cJSON *c = response ? cJSON_GetObjectItemCaseSensitive(response, "completed") : NULL;
   const cJSON *e =
       response ? cJSON_GetObjectItemCaseSensitive(response, "prospective_expired") : NULL;
   if (!cJSON_IsNumber(a) || !cJSON_IsNumber(t) || !cJSON_IsNumber(c) || !cJSON_IsNumber(e))
   {
      cJSON_Delete(response);
      return -1;
   }
   if (armed)
      *armed = a->valueint;
   if (triggered)
      *triggered = t->valueint;
   if (completed)
      *completed = c->valueint;
   if (expired)
      *expired = e->valueint;
   cJSON_Delete(response);
   return 0;
}

int memory_insert_epistemic_ex(const char *tier, const char *kind, const char *epistemic_kind,
                               const char *key, const char *content, const char *use_cases,
                               double confidence, const char *session_id,
                               memory_authority_t authority, memory_t *out)
{
   if (!tier || !tier[0] || !kind || !kind[0] || !key || !key[0] || !content || !content[0] ||
       confidence < 0.0 || confidence > 1.0 ||
       (authority != MEMORY_AUTHORITY_MODEL && authority != MEMORY_AUTHORITY_USER))
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "insert-epistemic") ||
       !cJSON_AddStringToObject(request, "tier", tier) ||
       !cJSON_AddStringToObject(request, "kind", kind) ||
       !cJSON_AddStringToObject(request, "epistemic_kind",
                                epistemic_kind && epistemic_kind[0] ? epistemic_kind
                                                                    : "world_fact") ||
       !cJSON_AddStringToObject(request, "key", key) ||
       !cJSON_AddStringToObject(request, "content", content) ||
       !cJSON_AddStringToObject(request, "use_cases", use_cases ? use_cases : "") ||
       !cJSON_AddNumberToObject(request, "confidence", confidence) ||
       !cJSON_AddNumberToObject(request, "authority", authority) ||
       (session_id && session_id[0] && !cJSON_AddStringToObject(request, "session_id", session_id)))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   memory_t ignored;
   int count = records_from_response(response, out ? out : &ignored, 1);
   cJSON_Delete(response);
   return count == 1 ? 0 : -1;
}

int memory_insert_ex(const char *tier, const char *kind, const char *key, const char *content,
                     const char *use_cases, double confidence, const char *session_id,
                     memory_authority_t authority, memory_t *out)
{
   return memory_insert_epistemic_ex(tier, kind, "world_fact", key, content, use_cases, confidence,
                                     session_id, authority, out);
}

int memory_insert(const char *tier, const char *kind, const char *key, const char *content,
                  double confidence, const char *session_id, memory_t *out)
{
   return memory_insert_ex(tier, kind, key, content, "", confidence, session_id,
                           MEMORY_AUTHORITY_MODEL, out);
}

int memory_update_content_as(int64_t id, const char *content, memory_authority_t authority,
                             int64_t *new_id_out)
{
   if (id <= 0 || !content || !content[0] ||
       (authority != MEMORY_AUTHORITY_MODEL && authority != MEMORY_AUTHORITY_USER))
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "update-as") ||
       !cJSON_AddNumberToObject(request, "id", (double)id) ||
       !cJSON_AddStringToObject(request, "content", content) ||
       !cJSON_AddNumberToObject(request, "authority", authority))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   const cJSON *code = response ? cJSON_GetObjectItemCaseSensitive(response, "code") : NULL;
   const cJSON *ids = response ? cJSON_GetObjectItemCaseSensitive(response, "ids") : NULL;
   const cJSON *new_id = cJSON_IsArray(ids) ? cJSON_GetArrayItem(ids, 0) : NULL;
   if (!cJSON_IsNumber(code) || !cJSON_IsNumber(new_id))
   {
      cJSON_Delete(response);
      return -1;
   }
   if (new_id_out)
      *new_id_out = (int64_t)new_id->valuedouble;
   int result = code->valueint;
   cJSON_Delete(response);
   if (result == 0)
      memory_audit_emit("memory.update", id, NULL, NULL, NULL, 0.0, NULL);
   return result;
}

int memory_delete_as(int64_t id, memory_authority_t authority)
{
   if (id <= 0 || (authority != MEMORY_AUTHORITY_MODEL && authority != MEMORY_AUTHORITY_USER))
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "delete-as") ||
       !cJSON_AddNumberToObject(request, "id", (double)id) ||
       !cJSON_AddNumberToObject(request, "authority", authority))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   const cJSON *deleted = response ? cJSON_GetObjectItemCaseSensitive(response, "deleted") : NULL;
   int result = cJSON_IsBool(deleted) && cJSON_IsTrue(deleted) ? 0 : -1;
   cJSON_Delete(response);
   if (result == 0)
      memory_audit_emit("memory.delete", id, NULL, NULL, NULL, 0.0, NULL);
   return result;
}

int memory_delete(int64_t id)
{
   return memory_delete_as(id, MEMORY_AUTHORITY_USER);
}

int memory_fold_session(const char *session_id, char *summary_out, size_t summary_out_len)
{
   if (summary_out && summary_out_len > 0)
      summary_out[0] = '\0';
   if (!session_id || !session_id[0])
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "fold-session") ||
       !cJSON_AddStringToObject(request, "session_id", session_id))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   const cJSON *count = response ? cJSON_GetObjectItemCaseSensitive(response, "count") : NULL;
   const cJSON *block = response ? cJSON_GetObjectItemCaseSensitive(response, "block") : NULL;
   if (!cJSON_IsNumber(count) || !cJSON_IsString(block) || !block->valuestring)
   {
      cJSON_Delete(response);
      return -1;
   }
   int result = count->valueint;
   if (result >= 0 && summary_out && summary_out_len > 0)
      snprintf(summary_out, summary_out_len, "%s", block->valuestring);
   cJSON_Delete(response);
   return result;
}
