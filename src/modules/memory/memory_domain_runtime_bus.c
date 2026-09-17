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

cJSON *memory_maintenance_summary_to_json(const memory_maintenance_summary_t *summary)
{
   if (!summary)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   cJSON_AddNumberToObject(out, "modes_run", summary->modes_run);
   cJSON_AddBoolToObject(out, "skipped", summary->skipped != 0);
   cJSON_AddBoolToObject(out, "dry_run", summary->dry_run != 0);
   cJSON_AddNumberToObject(out, "promoted", summary->promoted);
   cJSON_AddNumberToObject(out, "demoted", summary->demoted);
   cJSON_AddNumberToObject(out, "expired", summary->expired);
   cJSON_AddNumberToObject(out, "lifecycle_archived", summary->lifecycle_archived);
   cJSON_AddNumberToObject(out, "reminders_expired", summary->reminders_expired);
   cJSON_AddNumberToObject(out, "directives_expired", summary->directives_expired);
   cJSON_AddNumberToObject(out, "rescored", summary->rescored);
   cJSON_AddNumberToObject(out, "profile_cards_refreshed", summary->profile_cards_refreshed);
   cJSON_AddNumberToObject(out, "merged", summary->merged);
   cJSON_AddNumberToObject(out, "summarized", summary->summarized);
   cJSON_AddNumberToObject(out, "drift_candidates", summary->drift_candidates);
   cJSON_AddNumberToObject(out, "drift_requeued", summary->drift_requeued);
   cJSON_AddNumberToObject(out, "elapsed_ms", summary->elapsed_ms);
   cJSON_AddNumberToObject(out, "memory_count_before", (double)summary->memory_count_before);
   cJSON_AddNumberToObject(out, "memory_count_after", (double)summary->memory_count_after);
   return out;
}

static int fusion_state_call(const char *operation, const char *state)
{
   cJSON *request = domain_request(operation);
   if (!request || (state && !cJSON_AddStringToObject(request, "state", state)))
   {
      cJSON_Delete(request);
      return 0;
   }
   cJSON *response = domain_call(request);
   int enabled = domain_bool(response, "allowed");
   cJSON_Delete(response);
   return enabled;
}

/* Kept for ABI compatibility with older recall adapters. Request fields no
 * longer change fusion: the memory owner reads the instance configuration. */

int memory_fusion_state_is_on(void)
{
   return fusion_state_call("fusion-state-get", NULL);
}

static cJSON *runtime_metrics_call(const char *operation)
{
   cJSON *response = domain_call(domain_request(operation));
   cJSON *metrics = response ? cJSON_DetachItemFromObjectCaseSensitive(response, "metrics") : NULL;
   cJSON_Delete(response);
   return metrics;
}

static int64_t metric_i64(const cJSON *metrics, const char *key)
{
   const cJSON *value = metrics ? cJSON_GetObjectItemCaseSensitive(metrics, key) : NULL;
   return cJSON_IsNumber(value) ? (int64_t)value->valuedouble : 0;
}

static double metric_double(const cJSON *metrics, const char *key)
{
   const cJSON *value = metrics ? cJSON_GetObjectItemCaseSensitive(metrics, key) : NULL;
   return cJSON_IsNumber(value) ? value->valuedouble : 0.0;
}

void memory_recall_metrics(int64_t *assemblies, int64_t *starts, double *average, double *maximum)
{
   cJSON *metrics = runtime_metrics_call("recall-metrics");
   if (assemblies)
      *assemblies = metric_i64(metrics, "assemblies");
   if (starts)
      *starts = metric_i64(metrics, "starts");
   if (average)
      *average = metric_double(metrics, "average_ms");
   if (maximum)
      *maximum = metric_double(metrics, "maximum_ms");
   cJSON_Delete(metrics);
}

static void recall_trace_event(const char *operation)
{
   cJSON *response = domain_call(domain_request(operation));
   cJSON_Delete(response);
}

void memory_recall_trace_capture_begin(void)
{
   recall_trace_event("recall-trace-begin");
}
void memory_recall_trace_capture_end(void)
{
   recall_trace_event("recall-trace-end");
}

int memory_recall_trace_rejections(memory_recall_rejection_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *response = domain_call(domain_request("recall-trace-list"));
   const cJSON *items =
       response ? cJSON_GetObjectItemCaseSensitive(response, "recall_rejections") : NULL;
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
      if (!cJSON_IsNumber(id) || domain_copy(out[i].lane, sizeof(out[i].lane), item, "lane") ||
          domain_copy(out[i].gate, sizeof(out[i].gate), item, "gate"))
      {
         cJSON_Delete(response);
         return 0;
      }
      out[i].memory_id = (int64_t)id->valuedouble;
   }
   cJSON_Delete(response);
   return n;
}

const char *memory_answer_evidence_decision_str(const memory_answer_evidence_t *trace)
{
   if (!trace)
      return "abstain";
   switch (trace->decision)
   {
   case MEMORY_ANSWER_DECISION_ANSWERABLE:
      return "answerable";
   case MEMORY_ANSWER_DECISION_EXEMPT:
      return "exempt";
   default:
      return "abstain";
   }
}

const char *memory_answer_evidence_reason_str(const memory_answer_evidence_t *trace)
{
   if (!trace)
      return "db_unavailable";
   switch (trace->reason)
   {
   case MEMORY_ANSWER_REASON_OK:
      return "ok";
   case MEMORY_ANSWER_REASON_STRUCTURAL_EMPTY:
      return "structural_empty";
   case MEMORY_ANSWER_REASON_STRUCTURAL_NO_EXTRACT:
      return "structural_no_extract";
   case MEMORY_ANSWER_REASON_CITATION_REQUIRED:
      return "citation_required";
   case MEMORY_ANSWER_REASON_GROUNDING_LOW:
      return "grounding_low";
   case MEMORY_ANSWER_REASON_CHUNK_FLOOR:
      return "chunk_floor";
   case MEMORY_ANSWER_REASON_CURATED_EXEMPT:
      return "curated_exempt";
   default:
      return "db_unavailable";
   }
}

static int domain_count_operation(const char *operation, const char *string_key,
                                  const char *string_value, int number, const char *number_key)
{
   cJSON *request = domain_request(operation);
   if (!request ||
       (string_key &&
        !cJSON_AddStringToObject(request, string_key, string_value ? string_value : "")) ||
       (number_key && !cJSON_AddNumberToObject(request, number_key, number)))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int count = -1;
   if (domain_number(response, "count", &count) != 0)
      count = -1;
   cJSON_Delete(response);
   return count;
}

int memory_rebuild_derived_indexes(int limit)
{
   cJSON *request = domain_request("rebuild-derived");
   if (!request || !cJSON_AddNumberToObject(request, "limit", limit > 0 ? limit : 100000))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call_with_timeout(request, DOMAIN_MAINTENANCE_TIMEOUT_MS);
   int count = -1;
   (void)domain_number(response, "count", &count);
   cJSON_Delete(response);
   return count;
}

int memory_scan_conversations(char dirs[][MAX_PATH_LEN], int dir_count)
{
   if (dir_count < 0 || dir_count > 8)
      return -1;
   cJSON *request = domain_request("scan-conversations");
   cJSON *array = request ? cJSON_AddArrayToObject(request, "directories") : NULL;
   if (!array)
   {
      cJSON_Delete(request);
      return -1;
   }
   for (int i = 0; i < dir_count; ++i)
      if (dirs[i][0])
         cJSON_AddItemToArray(array, cJSON_CreateString(dirs[i]));
   cJSON *response = domain_call_with_timeout(request, DOMAIN_MAINTENANCE_TIMEOUT_MS);
   int count = -1;
   (void)domain_number(response, "count", &count);
   cJSON_Delete(response);
   return count;
}

int anti_pattern_extract_from_feedback(void)
{
   return domain_count_operation("anti-pattern-feedback", NULL, NULL, 0, NULL);
}

int anti_pattern_extract_from_failures(void)
{
   return domain_count_operation("anti-pattern-failures", NULL, NULL, 0, NULL);
}

int anti_pattern_escalate(int hit_threshold)
{
   return domain_count_operation("anti-pattern-escalate", NULL, NULL, hit_threshold,
                                 "hit_threshold");
}

int memory_learn_style(void)
{
   return domain_count_operation("learn-style", NULL, NULL, 0, NULL);
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

int memory_rebuild_vector_index_for_version(const char *version, int *failed_out)
{
   if (failed_out)
      *failed_out = 0;
   if (!version || !version[0])
      return -1;
   cJSON *request = domain_request("vector-rebuild");
   if (!request || !cJSON_AddStringToObject(request, "version", version))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int rebuilt = -1, failed = 0;
   (void)domain_number(response, "count", &rebuilt);
   (void)domain_number(response, "failed", &failed);
   cJSON_Delete(response);
   if (failed_out)
      *failed_out = failed;
   return rebuilt;
}
