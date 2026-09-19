#include "json_fluent.h"
#include "module_commands.h"
/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* agent_eval_memory_support.c: live-corpus host adapters and benchmark transport.
 * Split from agent_eval.c to keep the core eval harness under the lint cap. */
#include "aimee.h"
#include "agent_eval.h"
#include "agent_eval_internal.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "config.h"
#include "config_database.h"
#include "memory.h"
#include <math.h>
#include "lifecycle.h"
#include "eval_support.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- Corpus-based memory retrieval eval --- */

/* Internal: read entire file into a heap-allocated string. Caller frees. */
char *slurp_file_eval(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t nread = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[nread] = '\0';
   return buf;
}

void mem_eval_support_case_add(mem_eval_support_case_t *scase, int64_t id)
{
   if (!scase || id <= 0 ||
       scase->n_relevant >= (int)(sizeof(scase->relevant_ids) / sizeof(scase->relevant_ids[0])))
      return;
   for (int i = 0; i < scase->n_relevant; i++)
      if (scase->relevant_ids[i] == id)
         return;
   scase->relevant_ids[scase->n_relevant++] = id;
}

int mem_eval_run_support_with_latency(mem_eval_support_case_t *cases, int n_cases,
                                      mem_eval_scores_t *out, mem_eval_latency_t *latency_out)
{
   if (!cases || !out || n_cases <= 0)
      return -1;

   memset(out, 0, sizeof(*out));
   out->n_cases = n_cases;
   if (latency_out)
      memset(latency_out, 0, sizeof(*latency_out));

   double total_mrr = 0.0, total_ndcg5 = 0.0, total_ndcg10 = 0.0;
   double total_recall5 = 0.0, total_recall10 = 0.0;
   double *latencies = latency_out ? calloc((size_t)n_cases, sizeof(double)) : NULL;
   if (latency_out && !latencies)
      return -1;

   for (int c = 0; c < n_cases; c++)
   {
      mem_eval_direct_trace_t trace;
      if (mem_eval_score_retrieval(cases[c].query, cases[c].relevant_ids, cases[c].n_relevant,
                                   &trace) != 0)
      {
         free(latencies);
         return -1;
      }
      if (latency_out)
         latencies[c] = trace.latency_ms;
      total_mrr += trace.mrr;
      total_ndcg5 += trace.ndcg_5;
      total_ndcg10 += trace.ndcg_10;
      total_recall5 += trace.recall_5;
      total_recall10 += trace.recall_10;
   }

   out->mrr = total_mrr / n_cases;
   out->ndcg_5 = total_ndcg5 / n_cases;
   out->ndcg_10 = total_ndcg10 / n_cases;
   out->recall_5 = total_recall5 / n_cases;
   out->recall_10 = total_recall10 / n_cases;
   if (latency_out)
      mem_eval_latency_finalize(latencies, n_cases, latency_out);
   free(latencies);
   return 0;
}

int mem_eval_score_retrieval(const char *query, const int64_t *expected_ids, int n_expected,
                             mem_eval_direct_trace_t *trace_out)
{
   if (!trace_out)
      return -1;
   memset(trace_out, 0, sizeof(*trace_out));
   if (!query || n_expected < 0 || n_expected > 128 || (n_expected && !expected_ids))
      return -1;
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   cJSON *ids = args ? cJSON_AddArrayToObject(args, "expected_ids") : NULL;
   if (!ids || !cJSON_AddStringToObject(args, "operation", "benchmark-score") ||
       !cJSON_AddStringToObject(args, "query", query))
   {
      cJSON_Delete(args);
      return -1;
   }
   for (int i = 0; i < n_expected; i++)
   {
      char id[32];
      snprintf(id, sizeof(id), "%lld", (long long)expected_ids[i]);
      cJSON *item = cJSON_CreateString(id);
      if (!item || !cJSON_AddItemToArray(ids, item))
      {
         cJSON_Delete(item);
         cJSON_Delete(args);
         return -1;
      }
   }
   struct timespec start, end;
   clock_gettime(CLOCK_MONOTONIC, &start);
   int rc = mem_eval_dispatch_diagnostic(args, &reply);
   clock_gettime(CLOCK_MONOTONIC, &end);
   cJSON_Delete(args);
   const cJSON *retrieved = cJSON_GetObjectItemCaseSensitive(reply, "retrieved_ids");
   mem_eval_direct_trace_t trace = {0};
   const char *names[] = {"mrr", "ndcg_5", "ndcg_10", "recall_5", "recall_10"};
   double *values[] = {&trace.mrr, &trace.ndcg_5, &trace.ndcg_10, &trace.recall_5,
                       &trace.recall_10};
   if (rc <= 0 || strcmp(jo_cstr(reply, "status"), "ok") || !cJSON_IsArray(retrieved) ||
       cJSON_GetArraySize(retrieved) > 20)
      goto failed;
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(reply, names[i]);
      if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0 ||
          value->valuedouble > (i == 0 ? 1 : 20))
         goto failed;
      *values[i] = value->valuedouble;
   }
   trace.n_retrieved = cJSON_GetArraySize(retrieved);
   for (int i = 0; i < trace.n_retrieved; i++)
   {
      const cJSON *value = cJSON_GetArrayItem(retrieved, i);
      if (!cJSON_IsString(value) || !value->valuestring[0])
         goto failed;
      char *tail = NULL, canonical[32];
      errno = 0;
      long long id = strtoll(value->valuestring, &tail, 10);
      if (errno || !tail || *tail || id <= 0)
         goto failed;
      snprintf(canonical, sizeof(canonical), "%lld", id);
      if (strcmp(canonical, value->valuestring))
         goto failed;
      trace.retrieved_ids[i] = (int64_t)id;
   }
   trace.latency_ms = elapsed_ms(&start, &end);
   cJSON_Delete(reply);
   *trace_out = trace;
   return 0;
failed:
   cJSON_Delete(reply);
   return -1;
}

int mem_eval_score_case(const mem_eval_case_t *ecase, mem_eval_direct_trace_t *trace_out)
{
   if (!ecase || ecase->n_expected < 0 || ecase->n_expected > 20)
   {
      if (trace_out)
         memset(trace_out, 0, sizeof(*trace_out));
      return -1;
   }
   return mem_eval_score_retrieval(ecase->query, ecase->expected_ids, ecase->n_expected, trace_out);
}

FILE *mem_eval_open_progress_file(const char *path)
{
   if (!path || !path[0])
      return NULL;
   FILE *fp = fopen(path, "w");
   if (!fp)
      return NULL;
   setvbuf(fp, NULL, _IOLBF, 0);
   return fp;
}

FILE *mem_eval_open_append_progress_file(const char *path)
{
   if (!path || !path[0])
      return NULL;
   FILE *fp = fopen(path, "a");
   if (!fp)
      return NULL;
   setvbuf(fp, NULL, _IOLBF, 0);
   return fp;
}

void mem_eval_append_direct_progress_row(FILE *fp, const char *dataset, const char *question_id,
                                         const char *label_key, const char *label_value,
                                         const char *question, const char *gold_answer,
                                         const mem_eval_direct_trace_t *trace)
{
   if (!fp || !dataset || !question_id || !label_key || !label_value || !question || !gold_answer ||
       !trace)
      return;

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "dataset", dataset);
   cJSON_AddStringToObject(root, "track", "direct");
   cJSON_AddStringToObject(root, "question_id", question_id);
   cJSON_AddStringToObject(root, label_key, label_value);
   cJSON_AddStringToObject(root, "question", question);
   cJSON_AddStringToObject(root, "gold_answer", gold_answer);
   cJSON_AddStringToObject(root, "verdict", trace->recall_5 > 0.0 ? "CORRECT" : "WRONG");
   cJSON_AddNumberToObject(root, "retrieval_latency_ms", trace->latency_ms);
   cJSON_AddNumberToObject(root, "mrr", trace->mrr);
   cJSON_AddNumberToObject(root, "ndcg_5", trace->ndcg_5);
   cJSON_AddNumberToObject(root, "ndcg_10", trace->ndcg_10);
   cJSON_AddNumberToObject(root, "recall_5", trace->recall_5);
   cJSON_AddNumberToObject(root, "recall_10", trace->recall_10);

   cJSON *retrieved = cJSON_AddArrayToObject(root, "retrieved_ids");
   for (int i = 0; i < trace->n_retrieved; i++)
   {
      /* Preserve Go's exact IDs through the remaining progress-file transport. */
      char id[32];
      snprintf(id, sizeof(id), "%lld", (long long)trace->retrieved_ids[i]);
      cJSON_AddItemToArray(retrieved, cJSON_CreateRaw(id));
   }

   char *line = cJSON_PrintUnformatted(root);
   if (line)
   {
      fprintf(fp, "%s\n", line);
      fflush(fp);
      free(line);
   }
   cJSON_Delete(root);
}

void mem_eval_append_miss_setup_progress_row(FILE *fp, const char *dataset, const char *unit_kind,
                                             int setup_completed, int setup_expected)
{
   if (!fp || !dataset)
      return;

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "dataset", dataset);
   cJSON_AddStringToObject(root, "phase", "miss_report_setup");
   cJSON_AddStringToObject(root, "unit_kind", unit_kind ? unit_kind : "item");
   cJSON_AddNumberToObject(root, "setup_completed", setup_completed);
   cJSON_AddNumberToObject(root, "setup_expected", setup_expected);

   char *line = cJSON_PrintUnformatted(root);
   if (line)
   {
      fprintf(fp, "%s\n", line);
      fflush(fp);
      free(line);
   }
   cJSON_Delete(root);
}

/* Temporary host transport while the remaining benchmark runner migrates. */
int mem_eval_dispatch_diagnostic(cJSON *args, cJSON **reply)
{
   *reply = NULL;
   return aimee_module_commands_dispatch_internal("memory.runtime", args, reply);
}

int mem_eval_write_hard_negative(FILE *fp, const char *suite, const eval_task_t *task,
                                 const agent_result_t *result)
{
   if (!fp || !task || !result)
      return -1;
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   if (!args || !cJSON_AddStringToObject(args, "operation", "benchmark-hard-negative") ||
       !cJSON_AddStringToObject(args, "suite", suite ? suite : "") ||
       !cJSON_AddStringToObject(args, "task", task->name) ||
       !cJSON_AddStringToObject(args, "query", task->prompt) ||
       !cJSON_AddStringToObject(args, "expected", task->success_check_value) ||
       !cJSON_AddStringToObject(args, "got", result->response ? result->response : "") ||
       !cJSON_AddStringToObject(args, "error", result->error))
   {
      cJSON_Delete(args);
      return -1;
   }
   int rc = mem_eval_dispatch_diagnostic(args, &reply);
   cJSON_Delete(args);
   const cJSON *line = cJSON_GetObjectItemCaseSensitive(reply, "line");
   if (rc <= 0 || strcmp(jo_cstr(reply, "status"), "ok") || !cJSON_IsString(line) ||
       !line->valuestring[0] || strlen(line->valuestring) > 1048576 ||
       strchr(line->valuestring, '\n') || strchr(line->valuestring, '\r'))
   {
      cJSON_Delete(reply);
      return -1;
   }
   rc = fprintf(fp, "%s\n", line->valuestring) < 0 || fflush(fp) != 0 ? -1 : 0;
   cJSON_Delete(reply);
   return rc;
}

int mem_eval_build_retrieval_context(const char *query, int top_k, int token_budget,
                                     char *context_out, size_t context_len,
                                     int *retrieved_tokens_out)
{
   if (retrieved_tokens_out)
      *retrieved_tokens_out = 0;
   if (!context_out || context_len == 0)
      return -1;
   context_out[0] = '\0';
   if (!query)
      return -1;
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   if (!args || !cJSON_AddStringToObject(args, "operation", "benchmark-context") ||
       !cJSON_AddStringToObject(args, "query", query) ||
       !cJSON_AddNumberToObject(args, "top_k", top_k) ||
       !cJSON_AddNumberToObject(args, "token_budget", token_budget) ||
       !cJSON_AddNumberToObject(args, "capacity", context_len))
   {
      cJSON_Delete(args);
      return -1;
   }
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", args, &reply);
   cJSON_Delete(args);
   const cJSON *text = cJSON_GetObjectItemCaseSensitive(reply, "context");
   const cJSON *tokens = cJSON_GetObjectItemCaseSensitive(reply, "tokens");
   const cJSON *kept = cJSON_GetObjectItemCaseSensitive(reply, "kept");
   if (rc <= 0 || strcmp(jo_cstr(reply, "status"), "ok") || !cJSON_IsString(text) ||
       strlen(text->valuestring) >= context_len || !cJSON_IsNumber(tokens) ||
       !isfinite(tokens->valuedouble) || tokens->valuedouble < 0 || tokens->valuedouble > 131072 ||
       floor(tokens->valuedouble) != tokens->valuedouble ||
       (token_budget > 0 && tokens->valuedouble > token_budget) || !cJSON_IsNumber(kept) ||
       !isfinite(kept->valuedouble) || kept->valuedouble < 0 || kept->valuedouble > 32 ||
       floor(kept->valuedouble) != kept->valuedouble || (top_k > 0 && kept->valuedouble > top_k) ||
       ((kept->valuedouble == 0) != (text->valuestring[0] == '\0')) ||
       ((kept->valuedouble == 0) != (tokens->valuedouble == 0)))
   {
      cJSON_Delete(reply);
      return -1;
   }
   memcpy(context_out, text->valuestring, strlen(text->valuestring) + 1);
   if (retrieved_tokens_out)
      *retrieved_tokens_out = (int)tokens->valuedouble;
   int count = (int)kept->valuedouble;
   cJSON_Delete(reply);
   return count;
}

int append_latency_sample(double **samples, int *count, int *cap, double value)
{
   if (!samples || !count || !cap)
      return -1;
   if (*count >= *cap)
   {
      int new_cap = (*cap == 0) ? 128 : (*cap * 2);
      double *grown = realloc(*samples, (size_t)new_cap * sizeof(double));
      if (!grown)
         return -1;
      *samples = grown;
      *cap = new_cap;
   }
   (*samples)[(*count)++] = value;
   return 0;
}
