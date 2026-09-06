/* Runtime, maintenance and vector portion of the legacy C ABI adapter for the
 * Go-owned memory domain. This file remains transport-only. */
#include "aimee.h"
#include "headers/module_json_call.h"

#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/memory/module_api.h>

#include "cJSON.h"
#include "memory_export.h"
#include "memory_lint.h"
#include "memory_query.h"
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

static int maintenance_copy_int(const cJSON *obj, const char *key, int *out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!cJSON_IsNumber(value))
      return -1;
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

int memory_maintenance_run(unsigned int modes, int force, int dry_run,
                           memory_maintenance_summary_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *request = domain_request("scheduled-maintenance");
   if (!request || !cJSON_AddNumberToObject(request, "modes", modes) ||
       !cJSON_AddBoolToObject(request, "force", force != 0) ||
       !cJSON_AddBoolToObject(request, "dry_run", dry_run != 0))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *item = response ? cJSON_GetObjectItemCaseSensitive(response, "maintenance") : NULL;
   const cJSON *skipped = item ? cJSON_GetObjectItemCaseSensitive(item, "skipped") : NULL;
   const cJSON *dry = item ? cJSON_GetObjectItemCaseSensitive(item, "dry_run") : NULL;
   const cJSON *elapsed = item ? cJSON_GetObjectItemCaseSensitive(item, "elapsed_ms") : NULL;
   const cJSON *before =
       item ? cJSON_GetObjectItemCaseSensitive(item, "memory_count_before") : NULL;
   const cJSON *after = item ? cJSON_GetObjectItemCaseSensitive(item, "memory_count_after") : NULL;
   if (!cJSON_IsObject(item) || !cJSON_IsBool(skipped) || !cJSON_IsBool(dry) ||
       !cJSON_IsNumber(elapsed) || !cJSON_IsNumber(before) || !cJSON_IsNumber(after) ||
       maintenance_copy_int(item, "modes_run", &out->modes_run) ||
       maintenance_copy_int(item, "promoted", &out->promoted) ||
       maintenance_copy_int(item, "demoted", &out->demoted) ||
       maintenance_copy_int(item, "expired", &out->expired) ||
       maintenance_copy_int(item, "lifecycle_archived", &out->lifecycle_archived) ||
       maintenance_copy_int(item, "reminders_expired", &out->reminders_expired) ||
       maintenance_copy_int(item, "directives_expired", &out->directives_expired) ||
       maintenance_copy_int(item, "rescored", &out->rescored) ||
       maintenance_copy_int(item, "profile_cards_refreshed", &out->profile_cards_refreshed) ||
       maintenance_copy_int(item, "merged", &out->merged) ||
       maintenance_copy_int(item, "summarized", &out->summarized) ||
       maintenance_copy_int(item, "drift_candidates", &out->drift_candidates) ||
       maintenance_copy_int(item, "drift_requeued", &out->drift_requeued))
   {
      cJSON_Delete(response);
      return -1;
   }
   out->skipped = cJSON_IsTrue(skipped);
   out->dry_run = cJSON_IsTrue(dry);
   out->elapsed_ms = elapsed->valuedouble;
   out->memory_count_before = (int64_t)before->valuedouble;
   out->memory_count_after = (int64_t)after->valuedouble;
   cJSON *summary = memory_maintenance_summary_to_json(out);
   char *encoded = summary ? cJSON_PrintUnformatted(summary) : NULL;
   if (encoded)
      snprintf(out->summary_json, sizeof(out->summary_json), "%s", encoded);
   free(encoded);
   cJSON_Delete(summary);
   cJSON_Delete(response);
   return 0;
}

void db2_memory_export_row_free(db2_memory_export_row_t *row)
{
   if (!row)
      return;
   free(row->key);
   free(row->content);
   free(row->source_session);
   free(row->created_at);
   free(row->updated_at);
   memset(row, 0, sizeof(*row));
}

static int export_string_dup(const cJSON *item, const char *key, char **out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, key);
   if (!cJSON_IsString(value) || !value->valuestring)
      return -1;
   *out = strdup(value->valuestring);
   return *out ? 0 : -1;
}

int db2_memory_export_alloc_all(db2_memory_export_row_t **out, size_t *count)
{
   if (!out || !count)
      return -1;
   *out = NULL;
   *count = 0;
   int64_t after_id = 0;
   for (;;)
   {
      cJSON *request = domain_request("export-records");
      if (!request || !cJSON_AddNumberToObject(request, "after_id", (double)after_id) ||
          !cJSON_AddNumberToObject(request, "limit", 1))
      {
         cJSON_Delete(request);
         goto fail;
      }
      cJSON *response = domain_call(request);
      const cJSON *items =
          response ? cJSON_GetObjectItemCaseSensitive(response, "export_records") : NULL;
      const cJSON *item = cJSON_IsArray(items) ? cJSON_GetArrayItem(items, 0) : NULL;
      if (!cJSON_IsArray(items))
      {
         cJSON_Delete(response);
         goto fail;
      }
      if (!item)
      {
         cJSON_Delete(response);
         return 0;
      }
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
      const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(item, "confidence");
      const cJSON *use_count = cJSON_GetObjectItemCaseSensitive(item, "use_count");
      if (!cJSON_IsNumber(id) || !cJSON_IsNumber(confidence) || !cJSON_IsNumber(use_count) ||
          id->valuedouble <= after_id)
      {
         cJSON_Delete(response);
         goto fail;
      }
      db2_memory_export_row_t *grown = realloc(*out, (*count + 1) * sizeof(**out));
      if (!grown)
      {
         cJSON_Delete(response);
         goto fail;
      }
      *out = grown;
      db2_memory_export_row_t *row = &grown[*count];
      memset(row, 0, sizeof(*row));
      row->id = (int64_t)id->valuedouble;
      row->confidence = confidence->valuedouble;
      row->use_count = use_count->valueint;
      if (domain_copy(row->tier, sizeof(row->tier), item, "tier") ||
          domain_copy(row->kind, sizeof(row->kind), item, "kind") ||
          export_string_dup(item, "key", &row->key) ||
          export_string_dup(item, "content", &row->content) ||
          export_string_dup(item, "source_session", &row->source_session) ||
          export_string_dup(item, "created_at", &row->created_at) ||
          export_string_dup(item, "updated_at", &row->updated_at))
      {
         db2_memory_export_row_free(row);
         cJSON_Delete(response);
         goto fail;
      }
      after_id = row->id;
      (*count)++;
      cJSON_Delete(response);
   }

fail:
   for (size_t i = 0; i < *count; ++i)
      db2_memory_export_row_free(&(*out)[i]);
   free(*out);
   *out = NULL;
   *count = 0;
   return -1;
}

int db2_memory_decisions_export_jsonl(const char *path)
{
   if (!path || !path[0])
      return -1;
   cJSON *request = domain_request("export-decisions-jsonl");
   if (!request || !cJSON_AddStringToObject(request, "path", path))
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

int memory_query_edges(const char *entity, edge_t *out, int max)
{
   if (!entity || !entity[0] || !out || max <= 0)
      return -1;
   cJSON *request = domain_request("entity-edges");
   if (!request || !cJSON_AddStringToObject(request, "entity", entity) ||
       !cJSON_AddNumberToObject(request, "limit", max))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *items = response ? cJSON_GetObjectItemCaseSensitive(response, "relations") : NULL;
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
      const cJSON *weight = cJSON_GetObjectItemCaseSensitive(item, "weight");
      memset(&out[i], 0, sizeof(out[i]));
      if (!cJSON_IsNumber(id) || !cJSON_IsNumber(weight) ||
          domain_copy(out[i].source, sizeof(out[i].source), item, "source") ||
          domain_copy(out[i].relation, sizeof(out[i].relation), item, "relation") ||
          domain_copy(out[i].target, sizeof(out[i].target), item, "target"))
      {
         cJSON_Delete(response);
         return -1;
      }
      out[i].id = (int64_t)id->valuedouble;
      out[i].weight = (int)weight->valuedouble;
   }
   cJSON_Delete(response);
   return n;
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

void memory_fusion_state_set(const char *state)
{
   (void)fusion_state_call("fusion-state-set", state ? state : "");
}

int memory_fusion_state_is_on(void)
{
   return fusion_state_call("fusion-state-get", NULL);
}

void memory_fusion_state_clear(void)
{
   (void)fusion_state_call("fusion-state-clear", NULL);
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

void memory_directive_metrics(int64_t *created, int64_t *resolved, int64_t *expired,
                              int64_t *surfaced, int64_t *calls, double *average, double *maximum)
{
   cJSON *metrics = runtime_metrics_call("directive-metrics");
   if (created)
      *created = metric_i64(metrics, "created");
   if (resolved)
      *resolved = metric_i64(metrics, "resolved");
   if (expired)
      *expired = metric_i64(metrics, "expired");
   if (surfaced)
      *surfaced = metric_i64(metrics, "surfaced");
   if (calls)
      *calls = metric_i64(metrics, "calls");
   if (average)
      *average = metric_double(metrics, "average_ms");
   if (maximum)
      *maximum = metric_double(metrics, "maximum_ms");
   cJSON_Delete(metrics);
}

void memory_prospective_metrics(int64_t *triggered, int64_t *completed, int64_t *expired,
                                int64_t *calls, double *average, double *maximum)
{
   cJSON *metrics = runtime_metrics_call("prospective-metrics");
   if (triggered)
      *triggered = metric_i64(metrics, "triggered");
   if (completed)
      *completed = metric_i64(metrics, "completed");
   if (expired)
      *expired = metric_i64(metrics, "expired");
   if (calls)
      *calls = metric_i64(metrics, "calls");
   if (average)
      *average = metric_double(metrics, "average_ms");
   if (maximum)
      *maximum = metric_double(metrics, "maximum_ms");
   cJSON_Delete(metrics);
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
void memory_recall_trace_capture_reset(void)
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

int memory_search(char **clusters, int cluster_count, int limit, search_result_t *out, int max)
{
   if (!out || max <= 0 || cluster_count < 0)
      return 0;
   if (limit <= 0 || limit > max)
      limit = max;
   if (limit > 64)
      limit = 64;
   cJSON *request = domain_request("legacy-search");
   cJSON *items = request ? cJSON_AddArrayToObject(request, "clusters") : NULL;
   if (!items || !cJSON_AddNumberToObject(request, "limit", limit))
   {
      cJSON_Delete(request);
      return 0;
   }
   for (int i = 0; i < cluster_count && i < 64; ++i)
      if (clusters && clusters[i] && clusters[i][0])
         cJSON_AddItemToArray(items, cJSON_CreateString(clusters[i]));
   cJSON *response = domain_call(request);
   const cJSON *results =
       response ? cJSON_GetObjectItemCaseSensitive(response, "legacy_results") : NULL;
   if (!cJSON_IsArray(results))
   {
      cJSON_Delete(response);
      return 0;
   }
   int n = cJSON_GetArraySize(results);
   if (n > limit)
      n = limit;
   for (int i = 0; i < n; ++i)
   {
      const cJSON *row = cJSON_GetArrayItem(results, i);
      const cJSON *seq = cJSON_GetObjectItemCaseSensitive(row, "seq");
      const cJSON *start = cJSON_GetObjectItemCaseSensitive(row, "start_line");
      const cJSON *end = cJSON_GetObjectItemCaseSensitive(row, "end_line");
      const cJSON *score = cJSON_GetObjectItemCaseSensitive(row, "score");
      const cJSON *files = cJSON_GetObjectItemCaseSensitive(row, "files");
      memset(&out[i], 0, sizeof(out[i]));
      (void)domain_copy(out[i].session_id, sizeof(out[i].session_id), row, "session_id");
      (void)domain_copy(out[i].file_path, sizeof(out[i].file_path), row, "file_path");
      (void)domain_copy(out[i].summary, sizeof(out[i].summary), row, "summary");
      out[i].seq = cJSON_IsNumber(seq) ? seq->valueint : 0;
      out[i].start_line = cJSON_IsNumber(start) ? start->valueint : 0;
      out[i].end_line = cJSON_IsNumber(end) ? end->valueint : 0;
      out[i].score = cJSON_IsNumber(score) ? score->valuedouble : 0.0;
      if (cJSON_IsArray(files))
      {
         int nf = cJSON_GetArraySize(files);
         if (nf > 32)
            nf = 32;
         for (int f = 0; f < nf; ++f)
         {
            const cJSON *file = cJSON_GetArrayItem(files, f);
            if (cJSON_IsString(file) && file->valuestring)
               snprintf(out[i].files[out[i].file_count++], MAX_PATH_LEN, "%s", file->valuestring);
         }
      }
   }
   cJSON_Delete(response);
   return n;
}

int memory_compact_windows(int *summary_count, int *fact_count)
{
   cJSON *response =
       domain_call_with_timeout(domain_request("compact-legacy"), DOMAIN_MAINTENANCE_TIMEOUT_MS);
   int summaries = 0, facts = 0;
   int valid = domain_number(response, "summary_count", &summaries) == 0 &&
               domain_number(response, "fact_count", &facts) == 0;
   cJSON_Delete(response);
   if (!valid)
      return -1;
   if (summary_count)
      *summary_count = summaries;
   if (fact_count)
      *fact_count = facts;
   return 0;
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

int memory_check_drift(int64_t task_id, const char *file_path, const char *command,
                       drift_result_t *out)
{
   if (!out || task_id <= 0)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *request = domain_request("check-drift");
   if (!request || !cJSON_AddNumberToObject(request, "id", (double)task_id) ||
       !cJSON_AddStringToObject(request, "path", file_path ? file_path : "") ||
       !cJSON_AddStringToObject(request, "command", command ? command : ""))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *drift = response ? cJSON_GetObjectItemCaseSensitive(response, "drift") : NULL;
   const cJSON *drifted = drift ? cJSON_GetObjectItemCaseSensitive(drift, "drifted") : NULL;
   const cJSON *id = drift ? cJSON_GetObjectItemCaseSensitive(drift, "task_id") : NULL;
   if (!cJSON_IsObject(drift) || !cJSON_IsBool(drifted) || !cJSON_IsNumber(id))
   {
      cJSON_Delete(response);
      return -1;
   }
   out->drifted = cJSON_IsTrue(drifted);
   out->task_id = (int64_t)id->valuedouble;
   (void)domain_copy(out->task_title, sizeof(out->task_title), drift, "task_title");
   (void)domain_copy(out->message, sizeof(out->message), drift, "message");
   cJSON_Delete(response);
   return 0;
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

int pgvec_memory_vector_collection_exists(void)
{
   cJSON *response = domain_call(domain_request("vector-collection-exists"));
   int exists = domain_bool(response, "allowed");
   cJSON_Delete(response);
   return exists;
}

int pgvec_memory_vector_collection_recreate(int dim)
{
   cJSON *request = domain_request("vector-collection-recreate");
   if (!request || !cJSON_AddNumberToObject(request, "dimension", dim))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   int updated = domain_bool(response, "updated");
   cJSON_Delete(response);
   return updated ? 0 : -1;
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
