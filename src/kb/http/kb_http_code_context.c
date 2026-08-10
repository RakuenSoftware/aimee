/* kb_http_code_context.c: bounded task-conditioned code context (E4).
 *
 * The existing hybrid route owns candidate generation and RRF. This wrapper
 * turns that ranking into the stricter ingress contract: an active project is
 * mandatory, current-generation identity is attached to every item, exact and
 * structural evidence leads, weak vector-only rows are rejected, and memory is
 * additive only after code evidence made the task answerable. */
#include "kb_http_code.h"
#include "kb_http_code_vector_status.h"
#include "canonical_index.h"
#include "code_index.h"
#include "memory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_CONTEXT_MAX_ITEMS  4
#define CODE_CONTEXT_MAX_TOKENS 1200
#define CODE_CONTEXT_HYBRID_BUF (256 * 1024)
#define CODE_CONTEXT_VECTOR_MIN 0.70
#define CODE_CONTEXT_ENRICH_MAX 25

static int context_signal(const cJSON *row, const char *wanted)
{
   const cJSON *signals = cJSON_GetObjectItemCaseSensitive(row, "signals");
   const cJSON *signal = NULL;
   cJSON_ArrayForEach(signal, signals) if (cJSON_IsString(signal) &&
                                           strcmp(signal->valuestring, wanted) == 0) return 1;
   return 0;
}

/* Lower class sorts first. -1 is not answerable code evidence. */
static int context_row_class(const cJSON *row, double *confidence)
{
   int code = context_signal(row, "code");
   int graph = context_signal(row, "graph");
   const cJSON *vector_score = cJSON_GetObjectItemCaseSensitive(row, "vector_score");
   if (code || graph)
   {
      if (confidence)
         *confidence = code && graph ? 0.95 : (code ? 0.90 : 0.85);
      return 0;
   }
   if (context_signal(row, "vector") && cJSON_IsNumber(vector_score) &&
       vector_score->valuedouble >= CODE_CONTEXT_VECTOR_MIN)
   {
      if (confidence)
         *confidence = vector_score->valuedouble > 1.0 ? 1.0 : vector_score->valuedouble;
      return 1;
   }
   return -1;
}

static int context_enriched_line(const code_search_hit_t *hits, int count, const char *project,
                                 const char *path)
{
   for (int i = 0; i < count; i++)
      if (strcmp(hits[i].project, project) == 0 && strcmp(hits[i].file_path, path) == 0)
         return hits[i].line;
   return 0;
}

static cJSON *context_result_copy(const cJSON *row, const char *project, int64_t generation,
                                  const code_search_hit_t *enriched, int enriched_count,
                                  double confidence)
{
   const cJSON *path = cJSON_GetObjectItemCaseSensitive(row, "file_path");
   if (!cJSON_IsString(path) || !path->valuestring[0])
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   cJSON_AddStringToObject(out, "project", project);
   cJSON_AddStringToObject(out, "file_path", path->valuestring);
   cJSON_AddNumberToObject(out, "generation", (double)generation);
   cJSON_AddStringToObject(out, "freshness", "current");
   cJSON_AddNumberToObject(out, "confidence", confidence);
   cJSON_AddBoolToObject(out, "accepted", 1);

   const char *copy_fields[] = {"score",   "signal_hits",  "structural_weight",
                                "snippet", "content_hash", "vector_score",
                                NULL};
   for (int i = 0; copy_fields[i]; i++)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, copy_fields[i]);
      if (value)
         cJSON_AddItemToObject(out, copy_fields[i], cJSON_Duplicate(value, 1));
   }
   const cJSON *signals = cJSON_GetObjectItemCaseSensitive(row, "signals");
   cJSON_AddItemToObject(out, "provenance",
                         cJSON_IsArray(signals) ? cJSON_Duplicate(signals, 1)
                                                : cJSON_CreateArray());

   int line = context_enriched_line(enriched, enriched_count, project, path->valuestring);
   const cJSON *caller_line = cJSON_GetObjectItemCaseSensitive(row, "caller_line");
   if (line <= 0 && cJSON_IsNumber(caller_line) && caller_line->valueint > 0)
      line = caller_line->valueint;
   cJSON *span = cJSON_AddObjectToObject(out, "span");
   cJSON_AddStringToObject(span, "kind", line > 0 ? "line" : "file");
   cJSON_AddNumberToObject(span, "line_start", line);
   cJSON_AddNumberToObject(span, "line_end", line);
   const cJSON *caller = cJSON_GetObjectItemCaseSensitive(row, "caller");
   if (cJSON_IsString(caller) && caller->valuestring[0])
      cJSON_AddStringToObject(span, "symbol", caller->valuestring);
   return out;
}

static int context_memory_kind(const char *kind)
{
   return kind && (strcmp(kind, "decision") == 0 || strcmp(kind, "preference") == 0 ||
                   strcmp(kind, "constraint") == 0 || strcmp(kind, "policy") == 0 ||
                   strcmp(kind, "requirement") == 0);
}

static int context_memory_duplicates_code(const memory_t *memory, const char *snippet)
{
   if (!memory || !snippet || !snippet[0])
      return 0;
   const char *text = memory->headline[0] ? memory->headline : memory->content;
   size_t n = strlen(text);
   return n >= 24 && (strstr(snippet, text) != NULL || strstr(text, snippet) != NULL);
}

static int context_identifier_char(unsigned char c)
{
   return isalnum(c) || c == '_';
}

static int context_text_has_anchor(const char *text, const char *path, const char *symbol)
{
   if (!text)
      return 0;
   if (path && path[0] && strstr(text, path))
      return 1;
   if (!symbol || strlen(symbol) < 3)
      return 0;
   size_t symbol_n = strlen(symbol);
   const char *match = text;
   while ((match = strstr(match, symbol)) != NULL)
   {
      unsigned char before = match == text ? 0 : (unsigned char)match[-1];
      unsigned char after = (unsigned char)match[symbol_n];
      if ((!before || !context_identifier_char(before)) &&
          (!after || !context_identifier_char(after)))
         return 1;
      match += symbol_n;
   }
   return 0;
}

/* A scoped memory is still only explanatory context. Require an explicit file
 * path or symbol mention before attaching it to accepted code; query similarity
 * and project scope alone do not prove that relationship. */
static int context_memory_anchors_code(const memory_t *memory, const char *path, const char *symbol)
{
   return memory && (context_text_has_anchor(memory->key, path, symbol) ||
                     context_text_has_anchor(memory->headline, path, symbol) ||
                     context_text_has_anchor(memory->content, path, symbol) ||
                     context_text_has_anchor(memory->use_cases, path, symbol));
}

static const cJSON *context_memory_code_anchor(const memory_t *memory, const cJSON *results)
{
   const cJSON *row = NULL;
   cJSON_ArrayForEach(row, results)
   {
      const cJSON *path = cJSON_GetObjectItemCaseSensitive(row, "file_path");
      const cJSON *span = cJSON_GetObjectItemCaseSensitive(row, "span");
      const cJSON *symbol = cJSON_GetObjectItemCaseSensitive(span, "symbol");
      if (cJSON_IsString(path) &&
          context_memory_anchors_code(memory, path->valuestring,
                                      cJSON_IsString(symbol) ? symbol->valuestring : NULL))
         return row;
   }
   return NULL;
}

int handle_get_code_context(const char *query_string, char *out_buf, int out_cap)
{
   char query[512] = "";
   char symbol[256] = "";
   char project[256] = "";
   if (!code_qparam(query_string, "query", query, sizeof(query)) || !query[0])
      return code_scan_write_error(out_buf, out_cap, "missing query");
   code_qparam(query_string, "symbol", symbol, sizeof(symbol));
   int scope_status =
       code_request_project(query_string, project, sizeof(project), 0, NULL, out_buf, out_cap);
   if (scope_status)
      return scope_status;

   int64_t generation = 0;
   if (db2_code_index_project_current_generation(project, &generation) != 0 || generation <= 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":{\"type\":\"project_not_current\",\"message\":\"active project "
               "has no current generation\"}}");
      return 404;
   }

   char *hybrid = malloc(CODE_CONTEXT_HYBRID_BUF);
   if (!hybrid)
      return code_scan_write_error(out_buf, out_cap, "oom");
   int hybrid_status = handle_get_code_hybrid(query_string, hybrid, CODE_CONTEXT_HYBRID_BUF);
   if (hybrid_status != 200)
   {
      snprintf(out_buf, (size_t)out_cap, "%s", hybrid);
      free(hybrid);
      return hybrid_status;
   }
   cJSON *source = cJSON_Parse(hybrid);
   free(hybrid);
   if (!source)
      return code_scan_write_error(out_buf, out_cap, "hybrid response was invalid");

   code_search_hit_t enriched[CODE_CONTEXT_ENRICH_MAX];
   memset(enriched, 0, sizeof(enriched));
   int enriched_count =
       canonical_index_code_search(query, project, enriched, CODE_CONTEXT_ENRICH_MAX, 1);
   if (enriched_count < 0)
      enriched_count = 0;

   cJSON *response = cJSON_CreateObject();
   cJSON *results = response ? cJSON_AddArrayToObject(response, "results") : NULL;
   cJSON *why = response ? cJSON_AddArrayToObject(response, "why") : NULL;
   if (!response || !results || !why)
   {
      cJSON_Delete(source);
      cJSON_Delete(response);
      return code_scan_write_error(out_buf, out_cap, "oom");
   }
   cJSON_AddStringToObject(response, "query", query);
   if (symbol[0])
      cJSON_AddStringToObject(response, "symbol", symbol);
   cJSON_AddStringToObject(response, "project", project);
   cJSON_AddNumberToObject(response, "generation", (double)generation);
   cJSON_AddStringToObject(response, "freshness", "current");
   cJSON_AddBoolToObject(response, "resolved", 1);
   cJSON_AddNumberToObject(response, "max_results", CODE_CONTEXT_MAX_ITEMS);
   cJSON_AddNumberToObject(response, "max_tokens", CODE_CONTEXT_MAX_TOKENS);

   const cJSON *source_results = cJSON_GetObjectItemCaseSensitive(source, "results");
   int candidate_count = cJSON_IsArray(source_results) ? cJSON_GetArraySize(source_results) : 0;
   int accepted = 0;
   double top_confidence = 0.0;
   for (int cls = 0; cls <= 1 && accepted < CODE_CONTEXT_MAX_ITEMS; cls++)
   {
      const cJSON *row = NULL;
      cJSON_ArrayForEach(row, source_results)
      {
         if (accepted >= CODE_CONTEXT_MAX_ITEMS)
            break;
         const cJSON *row_project = cJSON_GetObjectItemCaseSensitive(row, "project");
         if (!cJSON_IsString(row_project) || strcmp(row_project->valuestring, project) != 0)
            continue;
         double confidence = 0.0;
         if (context_row_class(row, &confidence) != cls)
            continue;
         cJSON *copy =
             context_result_copy(row, project, generation, enriched, enriched_count, confidence);
         if (!copy)
            continue;
         cJSON_AddItemToArray(results, copy);
         if (confidence > top_confidence)
            top_confidence = confidence;
         accepted++;
      }
   }

   /* Memory can explain accepted code, never make an otherwise-unanswerable code
    * task look answerable. The visible reader hard-orders project, workspace,
    * then shared/global before this final packet limit. */
   if (accepted > 0 && accepted < CODE_CONTEXT_MAX_ITEMS)
   {
      memory_t memories[8];
      int memory_count = memory_find_facts_visible_ex(query, NULL, project, 0, 8, memories, 8);
      if (memory_count < 0)
         memory_count = 0;
      for (int i = 0; i < memory_count && accepted < CODE_CONTEXT_MAX_ITEMS; i++)
      {
         const cJSON *code_anchor = context_memory_code_anchor(&memories[i], results);
         const cJSON *anchor_path =
             code_anchor ? cJSON_GetObjectItemCaseSensitive(code_anchor, "file_path") : NULL;
         const cJSON *anchor_snippet =
             code_anchor ? cJSON_GetObjectItemCaseSensitive(code_anchor, "snippet") : NULL;
         if (!context_memory_kind(memories[i].kind) || !cJSON_IsString(anchor_path) ||
             (cJSON_IsString(anchor_snippet) &&
              context_memory_duplicates_code(&memories[i], anchor_snippet->valuestring)))
            continue;
         int scope_rank = memory_scope_visibility_rank(memories[i].id, NULL, project);
         /* The task packet is stricter than ordinary local-first recall: only
          * exact active-project memory can enter it. Workspace/global memory
          * remains available through explicit recall but cannot substitute. */
         if (scope_rank < 3)
            continue;
         cJSON *item = cJSON_CreateObject();
         if (!item)
            continue;
         cJSON_AddNumberToObject(item, "memory_id", (double)memories[i].id);
         cJSON_AddStringToObject(item, "kind", memories[i].kind);
         if (memories[i].headline[0])
            cJSON_AddStringToObject(item, "headline", memories[i].headline);
         cJSON_AddStringToObject(item, "content", memories[i].content);
         cJSON_AddStringToObject(item, "scope", "project");
         cJSON_AddNumberToObject(item, "scope_rank", scope_rank);
         cJSON_AddStringToObject(item, "provenance", "memory");
         cJSON_AddNumberToObject(item, "confidence", memories[i].confidence);
         cJSON *anchor = cJSON_AddObjectToObject(item, "anchor");
         cJSON_AddStringToObject(anchor, "project", project);
         cJSON_AddStringToObject(anchor, "file_path", anchor_path->valuestring);
         cJSON_AddNumberToObject(anchor, "generation", (double)generation);
         cJSON_AddStringToObject(anchor, "freshness", "current");
         cJSON_AddItemToArray(why, item);
         accepted++;
      }
   }

   int answerable = cJSON_GetArraySize(results) > 0;
   const cJSON *vector_status = cJSON_GetObjectItemCaseSensitive(source, "vector_status");
   if (!answerable && cJSON_IsString(vector_status) &&
       (strcmp(vector_status->valuestring, "unavailable") == 0 ||
        strcmp(vector_status->valuestring, "stale") == 0 ||
        strcmp(vector_status->valuestring, "unauthorized") == 0))
   {
      const char *status = vector_status->valuestring;
      int stale = strcmp(status, "stale") == 0;
      int unauthorized = strcmp(status, "unauthorized") == 0;
      const cJSON *vector_dependency =
          cJSON_GetObjectItemCaseSensitive(source, "vector_dependency");
      const char *dependency =
          cJSON_IsString(vector_dependency) ? vector_dependency->valuestring : "embedder";
      cJSON *failure = cJSON_CreateObject();
      if (!failure)
      {
         cJSON_Delete(response);
         cJSON_Delete(source);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"status\":\"unavailable\",\"dependency\":\"embedder\","
                  "\"retryable\":true,\"retry_after_ms\":%d}",
                  KB_CODE_VECTOR_RETRY_AFTER_MS);
         return 503;
      }
      cJSON_AddStringToObject(failure, "status", status);
      cJSON_AddStringToObject(failure, "dependency", dependency);
      cJSON_AddBoolToObject(failure, "retryable", !stale && !unauthorized);
      if (!stale && !unauthorized)
      {
         const cJSON *retry_after =
             cJSON_GetObjectItemCaseSensitive(source, "vector_retry_after_ms");
         int retry_after_ms = cJSON_IsNumber(retry_after) && retry_after->valuedouble > 0
                                  ? (int)retry_after->valuedouble
                                  : KB_CODE_VECTOR_RETRY_AFTER_MS;
         cJSON_AddNumberToObject(failure, "retry_after_ms", retry_after_ms);
      }
      cJSON_AddStringToObject(failure, "project", project);
      cJSON_AddNumberToObject(failure, "generation", (double)generation);
      const cJSON *observed_dim =
          cJSON_GetObjectItemCaseSensitive(source, "vector_observed_dimension");
      const cJSON *current_dim =
          cJSON_GetObjectItemCaseSensitive(source, "vector_current_dimension");
      if (cJSON_IsNumber(observed_dim))
         cJSON_AddNumberToObject(failure, "observed_dimension", observed_dim->valuedouble);
      if (cJSON_IsNumber(current_dim))
         cJSON_AddNumberToObject(failure, "current_dimension", current_dim->valuedouble);
      char *failure_json = cJSON_PrintUnformatted(failure);
      snprintf(out_buf, (size_t)out_cap, "%s", failure_json ? failure_json : "{}");
      free(failure_json);
      cJSON_Delete(failure);
      cJSON_Delete(response);
      cJSON_Delete(source);
      return stale ? 409 : (unauthorized ? 401 : 503);
   }
   cJSON_AddStringToObject(response, "status", answerable ? "ok" : "abstained");
   cJSON_AddNumberToObject(response, "item_count", accepted);
   cJSON *answerability = cJSON_AddObjectToObject(response, "answerability");
   cJSON_AddStringToObject(answerability, "decision", answerable ? "answerable" : "no_answer");
   cJSON_AddStringToObject(answerability, "reason",
                           answerable ? "current_project_evidence" : "no_evidence_above_floor");
   cJSON_AddNumberToObject(answerability, "candidate_count", candidate_count);
   cJSON_AddNumberToObject(answerability, "accepted_code_count", cJSON_GetArraySize(results));
   cJSON_AddNumberToObject(answerability, "top_confidence", top_confidence);
   cJSON_AddNumberToObject(answerability, "vector_floor", CODE_CONTEXT_VECTOR_MIN);

   /* An abstention that names nothing is indistinguishable from a broken index, and
    * the caller acts accordingly. Measured on the Ponytail benchmark: every cell got
    * status=abstained with results=[] because the fixture's best vector evidence
    * scores ~0.61 against a 0.70 floor and a natural-language question matches no
    * `code`/`graph` signal. Two of three agents then said in their own words that the
    * index was unavailable and fell back to shell -- 16 shell commands per cell
    * against a toolless baseline's 9.3. The tool taught them not to trust it.
    *
    * So when nothing clears the floor, say what was NEAR it. The decision is
    * unchanged -- status stays "abstained", results stays empty, nothing sub-floor is
    * ever presented as evidence -- but the caller can now tell "I looked and the best
    * was 0.61" from "I am not working", and can go read that file itself. */
   if (!answerable && candidate_count > 0)
   {
      cJSON *near = cJSON_AddArrayToObject(answerability, "near_misses");
      int listed = 0;
      const cJSON *row = NULL;
      cJSON_ArrayForEach(row, source_results)
      {
         if (listed >= CODE_CONTEXT_MAX_ITEMS)
            break;
         const cJSON *row_project = cJSON_GetObjectItemCaseSensitive(row, "project");
         if (!cJSON_IsString(row_project) || strcmp(row_project->valuestring, project) != 0)
            continue;
         const cJSON *path = cJSON_GetObjectItemCaseSensitive(row, "file_path");
         if (!cJSON_IsString(path) || !path->valuestring[0])
            continue;
         const cJSON *vscore = cJSON_GetObjectItemCaseSensitive(row, "vector_score");
         cJSON *entry = cJSON_CreateObject();
         if (!entry)
            break;
         cJSON_AddStringToObject(entry, "file_path", path->valuestring);
         if (cJSON_IsNumber(vscore))
            cJSON_AddNumberToObject(entry, "vector_score", vscore->valuedouble);
         cJSON_AddItemToArray(near, entry);
         listed++;
      }
      if (listed > 0)
         cJSON_AddStringToObject(
             answerability, "hint",
             "no candidate cleared the vector floor; these files were the closest matches "
             "and are worth reading directly");
   }

   char *json = cJSON_PrintUnformatted(response);
   int status = 200;
   if (!json)
      status = code_scan_write_error(out_buf, out_cap, "oom");
   else if (strlen(json) >= (size_t)out_cap)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"context packet exceeds response capacity\"}");
      status = 413;
   }
   else
      snprintf(out_buf, (size_t)out_cap, "%s", json);
   free(json);
   cJSON_Delete(response);
   cJSON_Delete(source);
   return status;
}

int handle_get_code_context_route(const char *method, const char *query_string, char *out_buf,
                                  int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_context(query_string, out_buf, out_cap);
}
