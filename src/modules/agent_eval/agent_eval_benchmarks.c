/* agent_eval_benchmarks.c: LoCoMo and LongMemEval dataset runners, extracted
 * from agent_eval.c to keep it tractable. All of the shared eval machinery
 * (case scoring, latency buckets, temp-db bootstrap, progress files) lives
 * in agent_eval.c and is exposed via agent_eval_internal.h. */
#include "aimee.h"
#include "agent_eval.h"
#include "agent_eval_internal.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "config.h"
#include "db2/kb_service_backend.h"
#include "db2/pgvec_kb_service.h"
#include "kb.h"
#include "memory.h"
#include "kb_vectors.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
   int enabled;
   int calls;
   int fully_drained_calls;
   double elapsed_ms;
   int queue_processed;
   int queue_pending;
   int queue_running;
   int queue_failed;
   int cognify_processed;
   int cognify_pending;
   int cognify_running;
   int cognify_failed;
   int cognify_retried;
} mem_eval_async_drain_totals_t;

static int mem_eval_vector_upsert_document(int64_t document_id, const float *vec, int dim,
                                           const char *payload_json, void *ctx)
{
   (void)ctx;
   return pgvec_kb_service_upsert_document_point(document_id, vec, dim, payload_json);
}

static double mem_eval_elapsed_ms_local(const struct timespec *start, const struct timespec *end)
{
   long sec = end->tv_sec - start->tv_sec;
   long nsec = end->tv_nsec - start->tv_nsec;
   if (nsec < 0)
   {
      sec--;
      nsec += 1000000000L;
   }
   return (double)sec * 1000.0 + (double)nsec / 1000000.0;
}

static int mem_eval_route_bucket_index(memory_query_route_t route)
{
   switch (route)
   {
   case MEM_ROUTE_LEXICAL:
      return 0;
   case MEM_ROUTE_SEMANTIC:
      return 1;
   case MEM_ROUTE_GRAPH:
      return 2;
   case MEM_ROUTE_HYBRID:
   default:
      return 3;
   }
}

static int mem_eval_shape_bucket_index(memory_query_shape_t shape)
{
   switch (shape)
   {
   case MEM_SHAPE_UNKNOWN:
      return 0;
   case MEM_SHAPE_FACTOID:
      return 1;
   case MEM_SHAPE_LIST:
      return 2;
   case MEM_SHAPE_YES_NO:
      return 3;
   case MEM_SHAPE_WHEN:
      return 4;
   case MEM_SHAPE_HOW:
      return 5;
   case MEM_SHAPE_WHY:
      return 6;
   case MEM_SHAPE_QUANTITATIVE:
      return 7;
   case MEM_SHAPE_TEMPORAL_INTERVAL:
      return 8;
   default:
      return 0;
   }
}

static void mem_eval_bucket_add(mem_eval_bucket_scores_t *bucket, double metric_a, double metric_b)
{
   if (!bucket)
      return;
   bucket->cases++;
   bucket->metric_a += metric_a;
   bucket->metric_b += metric_b;
}

static void mem_eval_finalize_bucket_array(mem_eval_bucket_scores_t *buckets, int count)
{
   if (!buckets || count <= 0)
      return;
   for (int i = 0; i < count; i++)
   {
      if (buckets[i].cases <= 0)
         continue;
      buckets[i].metric_a /= buckets[i].cases;
      buckets[i].metric_b /= buckets[i].cases;
   }
}

static int mem_eval_drain_async_queues(mem_eval_async_drain_totals_t *totals)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return -1;

   int cognify_enabled = cfg.memory_cognify_enabled && cfg.memory_cognify_command[0];
   if (!cfg.memory_cognify_async_enabled && !cognify_enabled)
      return 0;

   const char *embed_cmd = config_embedding_command(&cfg, NULL);
   db2_kb_service_async_queue_stats_t queue_stats;
   memory_cognify_queue_stats_t cog_stats;
   memset(&queue_stats, 0, sizeof(queue_stats));
   memset(&cog_stats, 0, sizeof(cog_stats));

   struct timespec ts0, ts1;
   clock_gettime(CLOCK_MONOTONIC, &ts0);
   if (db2_kb_service_async_queue_drain(embed_cmd, 0, pgvec_kb_vector_collection_name(),
                                        mem_eval_vector_upsert_document, NULL, &queue_stats) != 0)
      return -1;
   if (cognify_enabled && memory_cognify_drain(&cfg, 0, &cog_stats) != 0)
      return -1;
   clock_gettime(CLOCK_MONOTONIC, &ts1);

   if (totals)
   {
      totals->enabled = 1;
      totals->calls++;
      totals->elapsed_ms += mem_eval_elapsed_ms_local(&ts0, &ts1);
      totals->queue_processed += queue_stats.processed;
      totals->queue_pending += queue_stats.pending;
      totals->queue_running += queue_stats.running;
      totals->queue_failed += queue_stats.failed;
      totals->cognify_processed += cog_stats.processed;
      totals->cognify_pending += cog_stats.pending;
      totals->cognify_running += cog_stats.running;
      totals->cognify_failed += cog_stats.failed;
      totals->cognify_retried += cog_stats.retried;
      if (queue_stats.pending == 0 && queue_stats.running == 0 && cog_stats.pending == 0 &&
          cog_stats.running == 0)
         totals->fully_drained_calls++;
   }

   return 0;
}

static void mem_eval_emit_async_drain_summary(FILE *fp, const mem_eval_async_drain_totals_t *totals)
{
   if (!fp || !totals || !totals->enabled || totals->calls <= 0)
      return;

   fprintf(fp,
           "Async drain summary: enabled=1 calls=%d fully_drained=%d/%d elapsed_ms=%.3f "
           "queue_processed=%d queue_pending=%d queue_running=%d queue_failed=%d "
           "cognify_processed=%d cognify_pending=%d cognify_running=%d cognify_failed=%d "
           "cognify_retried=%d\n",
           totals->calls, totals->fully_drained_calls, totals->calls, totals->elapsed_ms,
           totals->queue_processed, totals->queue_pending, totals->queue_running,
           totals->queue_failed, totals->cognify_processed, totals->cognify_pending,
           totals->cognify_running, totals->cognify_failed, totals->cognify_retried);
}

int mem_eval_run_locomo(const char *dataset_path, int max_samples, mem_eval_scores_t *out,
                        mem_eval_latency_t *latency_out, int *samples_out,
                        const char *progress_path)
{
   if (!dataset_path || !out)
      return -1;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
   {
      fprintf(stderr, "mem_eval_run_locomo: cannot read %s\n", dataset_path);
      return -1;
   }

   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      fprintf(stderr, "mem_eval_run_locomo: invalid JSON in %s\n", dataset_path);
      return -1;
   }

   mem_eval_scores_t totals;
   memset(&totals, 0, sizeof(totals));
   double *latencies = NULL;
   int latency_count = 0;
   int latency_cap = 0;
   int sample_count = 0;
   FILE *progress_fp = mem_eval_open_progress_file(progress_path);
   mem_eval_async_drain_totals_t drain_totals;
   memset(&drain_totals, 0, sizeof(drain_totals));

   cJSON *sample = NULL;
   cJSON_ArrayForEach(sample, root)
   {
      if (max_samples > 0 && sample_count >= max_samples)
         break;

      cJSON *conversation = cJSON_GetObjectItem(sample, "conversation");
      cJSON *qa = cJSON_GetObjectItem(sample, "qa");
      if (!conversation || !cJSON_IsObject(conversation) || !qa || !cJSON_IsArray(qa))
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;

      corpus_fid_entry_t *fid_map = calloc(2048, sizeof(corpus_fid_entry_t));
      int fid_count = 0;
      int fid_cap = 2048;
      if (!fid_map)
      {
         mem_eval_close_temp_db();
         continue;
      }

      cJSON *field = NULL;
      cJSON_ArrayForEach(field, conversation)
      {
         if (!field->string || strncmp(field->string, "session_", 8) != 0 ||
             strstr(field->string, "_date_time") != NULL || !cJSON_IsArray(field))
            continue;

         char date_key[64];
         snprintf(date_key, sizeof(date_key), "%s_date_time", field->string);
         cJSON *date = cJSON_GetObjectItem(conversation, date_key);
         const char *date_str = (date && cJSON_IsString(date)) ? date->valuestring : "";

         cJSON *turn = NULL;
         cJSON_ArrayForEach(turn, field)
         {
            cJSON *dia_id = cJSON_GetObjectItem(turn, "dia_id");
            cJSON *speaker = cJSON_GetObjectItem(turn, "speaker");
            cJSON *text = cJSON_GetObjectItem(turn, "text");
            if (!dia_id || !cJSON_IsString(dia_id) || !text || !cJSON_IsString(text))
               continue;

            if (fid_count >= fid_cap)
               break;

            char key[512];
            char content[2048];
            snprintf(key, sizeof(key), "locomo %s %s",
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     dia_id->valuestring);
            snprintf(content, sizeof(content), "[%s] %s: %s", date_str,
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     text->valuestring);

            memory_t mem;
            if (memory_insert(TIER_L2, KIND_FACT, key, content, 0.9, "locomo", &mem) == 0)
            {
               snprintf(fid_map[fid_count].fid, sizeof(fid_map[fid_count].fid), "%s",
                        dia_id->valuestring);
               fid_map[fid_count].db_id = mem.id;
               fid_count++;
            }
         }
      }

      mem_eval_case_t *cases = calloc((size_t)cJSON_GetArraySize(qa), sizeof(mem_eval_case_t));
      int n_cases = 0;
      if (!cases)
      {
         free(fid_map);
         mem_eval_close_temp_db();
         continue;
      }

      cJSON *item = NULL;
      cJSON_ArrayForEach(item, qa)
      {
         cJSON *question = cJSON_GetObjectItem(item, "question");
         cJSON *evidence = cJSON_GetObjectItem(item, "evidence");
         if (!question || !cJSON_IsString(question) || !evidence || !cJSON_IsArray(evidence))
            continue;

         mem_eval_case_t *ec = &cases[n_cases];
         memset(ec, 0, sizeof(*ec));
         mem_eval_normalize_question(question->valuestring, ec->query, sizeof(ec->query));

         cJSON *ev = NULL;
         cJSON_ArrayForEach(ev, evidence)
         {
            if (!cJSON_IsString(ev))
               continue;
            int64_t db_id = fid_lookup(fid_map, fid_count, ev->valuestring);
            if (db_id > 0 && ec->n_expected < 20)
               ec->expected_ids[ec->n_expected++] = db_id;
         }
         if (ec->n_expected > 0)
            n_cases++;
      }

      if (n_cases > 0 && mem_eval_drain_async_queues(&drain_totals) != 0)
      {
         free(cases);
         free(fid_map);
         mem_eval_close_temp_db();
         cJSON_Delete(root);
         free(latencies);
         if (progress_fp)
            fclose(progress_fp);
         fprintf(stderr, "mem_eval_run_locomo: async drain failed\n");
         return -1;
      }

      if (n_cases > 0)
      {
         for (int i = 0; i < n_cases; i++)
         {
            mem_eval_direct_trace_t trace;
            if (mem_eval_score_case(&cases[i], &trace) == 0)
            {
               totals.mrr += trace.mrr;
               totals.ndcg_5 += trace.ndcg_5;
               totals.ndcg_10 += trace.ndcg_10;
               totals.recall_5 += trace.recall_5;
               totals.recall_10 += trace.recall_10;
               totals.n_cases += 1;
               append_latency_sample(&latencies, &latency_count, &latency_cap, trace.latency_ms);

               cJSON *question = cJSON_GetObjectItem(cJSON_GetArrayItem(qa, i), "question");
               cJSON *answer = cJSON_GetObjectItem(cJSON_GetArrayItem(qa, i), "answer");
               cJSON *question_id = cJSON_GetObjectItem(cJSON_GetArrayItem(qa, i), "question_id");
               cJSON *category = cJSON_GetObjectItem(cJSON_GetArrayItem(qa, i), "category");
               char category_buf[16];
               if (cJSON_IsNumber(category))
                  snprintf(category_buf, sizeof(category_buf), "%.0f", category->valuedouble);
               else if (cJSON_IsString(category))
                  snprintf(category_buf, sizeof(category_buf), "%s", category->valuestring);
               else
                  snprintf(category_buf, sizeof(category_buf), "unknown");
               mem_eval_append_direct_progress_row(
                   progress_fp, "locomo",
                   (question_id && cJSON_IsString(question_id)) ? question_id->valuestring
                                                                : cases[i].query,
                   "category", category_buf,
                   (question && cJSON_IsString(question)) ? question->valuestring : cases[i].query,
                   (answer && cJSON_IsString(answer)) ? answer->valuestring : "", &trace);
            }
         }
         sample_count++;
      }

      free(cases);
      free(fid_map);
      mem_eval_close_temp_db();
   }

   cJSON_Delete(root);

   if (totals.n_cases <= 0)
      return -1;

   out->mrr = totals.mrr / totals.n_cases;
   out->ndcg_5 = totals.ndcg_5 / totals.n_cases;
   out->ndcg_10 = totals.ndcg_10 / totals.n_cases;
   out->recall_5 = totals.recall_5 / totals.n_cases;
   out->recall_10 = totals.recall_10 / totals.n_cases;
   out->n_cases = totals.n_cases;
   if (latency_out)
      mem_eval_latency_finalize(latencies, latency_count, latency_out);
   if (samples_out)
      *samples_out = sample_count;
   free(latencies);
   if (progress_fp)
      fclose(progress_fp);
   mem_eval_emit_async_drain_summary(stderr, &drain_totals);
   return 0;
}

int mem_eval_run_locomo_session_support(const char *dataset_path, int max_samples,
                                        mem_eval_scores_t *out, mem_eval_latency_t *latency_out,
                                        int *samples_out)
{
   if (!dataset_path || !out)
      return -1;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
      return -1;

   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   mem_eval_scores_t totals;
   memset(&totals, 0, sizeof(totals));
   double *latencies = NULL;
   int latency_count = 0;
   int latency_cap = 0;
   int sample_count = 0;
   mem_eval_async_drain_totals_t drain_totals;
   memset(&drain_totals, 0, sizeof(drain_totals));

   cJSON *sample = NULL;
   cJSON_ArrayForEach(sample, root)
   {
      if (max_samples > 0 && sample_count >= max_samples)
         break;

      cJSON *conversation = cJSON_GetObjectItem(sample, "conversation");
      cJSON *qa = cJSON_GetObjectItem(sample, "qa");
      if (!conversation || !cJSON_IsObject(conversation) || !qa || !cJSON_IsArray(qa))
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;

      locomo_fid_entry_t *fid_map = calloc(2048, sizeof(locomo_fid_entry_t));
      mem_eval_scope_bucket_t scope_buckets[64];
      int fid_count = 0;
      int fid_cap = 2048;
      int scope_count = 0;
      if (!fid_map)
      {
         mem_eval_close_temp_db();
         continue;
      }

      cJSON *field = NULL;
      cJSON_ArrayForEach(field, conversation)
      {
         if (!field->string || strncmp(field->string, "session_", 8) != 0 ||
             strstr(field->string, "_date_time") != NULL || !cJSON_IsArray(field))
            continue;

         char date_key[64];
         snprintf(date_key, sizeof(date_key), "%s_date_time", field->string);
         cJSON *date = cJSON_GetObjectItem(conversation, date_key);
         const char *date_str = (date && cJSON_IsString(date)) ? date->valuestring : "";
         mem_eval_scope_bucket_t *bucket =
             mem_eval_get_scope_bucket(scope_buckets, &scope_count, 64, field->string);

         cJSON *turn = NULL;
         cJSON_ArrayForEach(turn, field)
         {
            cJSON *dia_id = cJSON_GetObjectItem(turn, "dia_id");
            cJSON *speaker = cJSON_GetObjectItem(turn, "speaker");
            cJSON *text = cJSON_GetObjectItem(turn, "text");
            if (!dia_id || !cJSON_IsString(dia_id) || !text || !cJSON_IsString(text))
               continue;
            if (fid_count >= fid_cap)
               break;

            char key[512];
            char content[2048];
            snprintf(key, sizeof(key), "locomo %s %s",
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     dia_id->valuestring);
            snprintf(content, sizeof(content), "[%s] %s: %s", date_str,
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     text->valuestring);

            memory_t mem;
            if (memory_insert(TIER_L2, KIND_FACT, key, content, 0.9, "locomo", &mem) == 0)
            {
               snprintf(fid_map[fid_count].fid, sizeof(fid_map[fid_count].fid), "%s",
                        dia_id->valuestring);
               fid_map[fid_count].db_id = mem.id;
               snprintf(fid_map[fid_count].scope, sizeof(fid_map[fid_count].scope), "%s",
                        field->string);
               fid_count++;
               if (bucket && bucket->count < (int)(sizeof(bucket->ids) / sizeof(bucket->ids[0])))
                  bucket->ids[bucket->count++] = mem.id;
            }
         }
      }

      mem_eval_support_case_t *cases =
          calloc((size_t)cJSON_GetArraySize(qa), sizeof(mem_eval_support_case_t));
      int n_cases = 0;
      if (!cases)
      {
         free(fid_map);
         mem_eval_close_temp_db();
         continue;
      }

      cJSON *item = NULL;
      cJSON_ArrayForEach(item, qa)
      {
         cJSON *question = cJSON_GetObjectItem(item, "question");
         cJSON *evidence = cJSON_GetObjectItem(item, "evidence");
         if (!question || !cJSON_IsString(question) || !evidence || !cJSON_IsArray(evidence))
            continue;

         mem_eval_support_case_t *ec = &cases[n_cases];
         memset(ec, 0, sizeof(*ec));
         mem_eval_normalize_question(question->valuestring, ec->query, sizeof(ec->query));

         cJSON *ev = NULL;
         cJSON_ArrayForEach(ev, evidence)
         {
            int64_t db_id = 0;
            char scope[64];
            scope[0] = '\0';
            if (!cJSON_IsString(ev) || !locomo_fid_lookup(fid_map, fid_count, ev->valuestring,
                                                          &db_id, scope, sizeof(scope)))
               continue;

            for (int s = 0; s < scope_count; s++)
            {
               if (strcmp(scope_buckets[s].scope, scope) != 0)
                  continue;
               for (int k = 0; k < scope_buckets[s].count; k++)
                  mem_eval_support_case_add(ec, scope_buckets[s].ids[k]);
               break;
            }
         }
         if (ec->n_relevant > 0)
            n_cases++;
      }

      if (n_cases > 0 && mem_eval_drain_async_queues(&drain_totals) != 0)
      {
         free(cases);
         free(fid_map);
         mem_eval_close_temp_db();
         cJSON_Delete(root);
         free(latencies);
         fprintf(stderr, "mem_eval_run_locomo_session_support: async drain failed\n");
         return -1;
      }

      if (n_cases > 0)
      {
         mem_eval_scores_t convo_scores;
         mem_eval_latency_t convo_latency;
         if (mem_eval_run_support_with_latency(cases, n_cases, &convo_scores, &convo_latency) == 0)
         {
            totals.mrr += convo_scores.mrr * n_cases;
            totals.ndcg_5 += convo_scores.ndcg_5 * n_cases;
            totals.ndcg_10 += convo_scores.ndcg_10 * n_cases;
            totals.recall_5 += convo_scores.recall_5 * n_cases;
            totals.recall_10 += convo_scores.recall_10 * n_cases;
            totals.n_cases += n_cases;
            sample_count++;

            memory_t results[20];
            for (int i = 0; i < n_cases; i++)
            {
               struct timespec ts0, ts1;
               clock_gettime(CLOCK_MONOTONIC, &ts0);
               if (memory_find_facts(cases[i].query, 20, results, 20) < 0)
                  fatal("memory retrieval index unavailable; server-side maintenance is required");
               clock_gettime(CLOCK_MONOTONIC, &ts1);
               append_latency_sample(&latencies, &latency_count, &latency_cap,
                                     elapsed_ms(&ts0, &ts1));
            }
         }
      }

      free(cases);
      free(fid_map);
      mem_eval_close_temp_db();
   }

   cJSON_Delete(root);
   if (totals.n_cases <= 0)
      return -1;

   out->mrr = totals.mrr / totals.n_cases;
   out->ndcg_5 = totals.ndcg_5 / totals.n_cases;
   out->ndcg_10 = totals.ndcg_10 / totals.n_cases;
   out->recall_5 = totals.recall_5 / totals.n_cases;
   out->recall_10 = totals.recall_10 / totals.n_cases;
   out->n_cases = totals.n_cases;
   if (latency_out)
      mem_eval_latency_finalize(latencies, latency_count, latency_out);
   if (samples_out)
      *samples_out = sample_count;
   free(latencies);
   mem_eval_emit_async_drain_summary(stderr, &drain_totals);
   return 0;
}

int mem_eval_run_locomo_qa(const char *dataset_path, int max_samples, int top_k, int token_budget,
                           mem_eval_qa_scores_t *out, mem_eval_latency_t *latency_out,
                           int *samples_out)
{
   if (!dataset_path || !out)
      return -1;

   agent_config_t cfg;
   if (mem_eval_load_benchmark_agent_config(&cfg) != 0 || cfg.agent_count == 0)
      return -1;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
      return -1;

   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   agent_http_init();

   mem_eval_qa_scores_t totals;
   memset(&totals, 0, sizeof(totals));
   double *latencies = NULL;
   int latency_count = 0;
   int latency_cap = 0;
   int sample_count = 0;

   cJSON *sample = NULL;
   cJSON_ArrayForEach(sample, root)
   {
      if (max_samples > 0 && sample_count >= max_samples)
         break;

      cJSON *conversation = cJSON_GetObjectItem(sample, "conversation");
      cJSON *qa = cJSON_GetObjectItem(sample, "qa");
      if (!conversation || !cJSON_IsObject(conversation) || !qa || !cJSON_IsArray(qa))
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;

      cJSON *field = NULL;
      cJSON_ArrayForEach(field, conversation)
      {
         if (!field->string || strncmp(field->string, "session_", 8) != 0 ||
             strstr(field->string, "_date_time") != NULL || !cJSON_IsArray(field))
            continue;

         char date_key[64];
         snprintf(date_key, sizeof(date_key), "%s_date_time", field->string);
         cJSON *date = cJSON_GetObjectItem(conversation, date_key);
         const char *date_str = (date && cJSON_IsString(date)) ? date->valuestring : "";

         cJSON *turn = NULL;
         cJSON_ArrayForEach(turn, field)
         {
            cJSON *dia_id = cJSON_GetObjectItem(turn, "dia_id");
            cJSON *speaker = cJSON_GetObjectItem(turn, "speaker");
            cJSON *text = cJSON_GetObjectItem(turn, "text");
            if (!dia_id || !cJSON_IsString(dia_id) || !text || !cJSON_IsString(text))
               continue;
            char key[512];
            char content[2048];
            snprintf(key, sizeof(key), "locomo %s %s",
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     dia_id->valuestring);
            snprintf(content, sizeof(content), "[%s] %s: %s", date_str,
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     text->valuestring);
            (void)memory_insert(TIER_L2, KIND_FACT, key, content, 0.9, "locomo", NULL);
         }
      }

      mem_eval_qa_case_t *cases =
          calloc((size_t)cJSON_GetArraySize(qa), sizeof(mem_eval_qa_case_t));
      int n_cases = 0;
      if (!cases)
      {
         mem_eval_close_temp_db();
         continue;
      }

      cJSON *item = NULL;
      cJSON_ArrayForEach(item, qa)
      {
         cJSON *question = cJSON_GetObjectItem(item, "question");
         cJSON *answer = cJSON_GetObjectItem(item, "answer");
         if (!question || !cJSON_IsString(question) || !answer)
            continue;

         snprintf(cases[n_cases].question, sizeof(cases[n_cases].question), "%s",
                  question->valuestring);
         if (cJSON_IsString(answer))
            snprintf(cases[n_cases].answer, sizeof(cases[n_cases].answer), "%s",
                     answer->valuestring);
         else if (cJSON_IsNumber(answer))
            snprintf(cases[n_cases].answer, sizeof(cases[n_cases].answer), "%.0f",
                     answer->valuedouble);
         else
            continue;
         n_cases++;
      }

      if (n_cases > 0)
      {
         for (int i = 0; i < n_cases; i++)
         {
            memory_query_plan_t plan;
            memset(&plan, 0, sizeof(plan));
            (void)memory_query_plan(cases[i].question, top_k, 96, &plan);
            int route_bucket = mem_eval_route_bucket_index(plan.route);
            int shape_bucket = mem_eval_shape_bucket_index(plan.shape);
            mem_eval_qa_scores_t case_scores;
            mem_eval_latency_t case_latency;
            if (mem_eval_run_qa_with_latency(&cfg, &cases[i], 1, top_k, token_budget, &case_scores,
                                             &case_latency) != 0)
               continue;
            totals.accuracy += case_scores.accuracy;
            totals.exact_match += case_scores.exact_match;
            totals.avg_retrieved_tokens += case_scores.avg_retrieved_tokens;
            totals.n_cases++;
            totals.answered_cases += case_scores.answered_cases;
            totals.judged_cases += case_scores.judged_cases;
            totals.cited_answers += case_scores.cited_answers;
            totals.uncited_answers += case_scores.uncited_answers;
            totals.low_confidence_answers += case_scores.low_confidence_answers;
            totals.total_answer_prompt_tokens += case_scores.total_answer_prompt_tokens;
            totals.total_answer_completion_tokens += case_scores.total_answer_completion_tokens;
            totals.total_judge_prompt_tokens += case_scores.total_judge_prompt_tokens;
            totals.total_judge_completion_tokens += case_scores.total_judge_completion_tokens;
            if (route_bucket >= 0 && route_bucket < MEM_EVAL_ROUTE_BUCKET_COUNT)
               mem_eval_bucket_add(&totals.route_buckets[route_bucket], case_scores.accuracy,
                                   case_scores.exact_match);
            if (shape_bucket >= 0 && shape_bucket < MEM_EVAL_SHAPE_BUCKET_COUNT)
               mem_eval_bucket_add(&totals.shape_buckets[shape_bucket], case_scores.accuracy,
                                   case_scores.exact_match);
            if (case_latency.n_queries > 0)
               append_latency_sample(&latencies, &latency_count, &latency_cap, case_latency.p50_ms);
         }
         if (totals.n_cases > 0)
            sample_count++;
      }

      free(cases);
      mem_eval_close_temp_db();
   }

   agent_http_cleanup();
   cJSON_Delete(root);

   if (totals.n_cases <= 0)
      return -1;

   *out = totals;
   out->accuracy /= totals.n_cases;
   out->exact_match /= totals.n_cases;
   out->avg_retrieved_tokens /= totals.n_cases;
   out->citation_coverage =
       out->answered_cases > 0 ? (double)out->cited_answers / out->answered_cases : 0.0;
   out->citation_miss_rate =
       out->answered_cases > 0 ? (double)out->uncited_answers / out->answered_cases : 0.0;
   out->hallucination_rate = 1.0 - out->accuracy;
   mem_eval_finalize_bucket_array(out->route_buckets, MEM_EVAL_ROUTE_BUCKET_COUNT);
   mem_eval_finalize_bucket_array(out->shape_buckets, MEM_EVAL_SHAPE_BUCKET_COUNT);
   if (latency_out)
      mem_eval_latency_finalize(latencies, latency_count, latency_out);
   if (samples_out)
      *samples_out = sample_count;
   free(latencies);
   return 0;
}

int mem_eval_report_locomo_qa_failures(const char *dataset_path, int max_samples, int top_k,
                                       int token_budget, int max_failures, FILE *fp)
{
   if (!dataset_path || !fp)
      return -1;
   if (max_failures <= 0)
      max_failures = 5;

   agent_config_t cfg;
   if (mem_eval_load_benchmark_agent_config(&cfg) != 0 || cfg.agent_count == 0)
      return -1;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
      return -1;
   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   agent_http_init();
   int reported = 0;
   cJSON *sample = NULL;
   cJSON_ArrayForEach(sample, root)
   {
      if (max_samples > 0 && reported >= max_samples)
         break;
      cJSON *conversation = cJSON_GetObjectItem(sample, "conversation");
      cJSON *qa = cJSON_GetObjectItem(sample, "qa");
      if (!conversation || !cJSON_IsObject(conversation) || !qa || !cJSON_IsArray(qa))
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;

      cJSON *field = NULL;
      cJSON_ArrayForEach(field, conversation)
      {
         if (!field->string || strncmp(field->string, "session_", 8) != 0 ||
             strstr(field->string, "_date_time") != NULL || !cJSON_IsArray(field))
            continue;
         char date_key[64];
         snprintf(date_key, sizeof(date_key), "%s_date_time", field->string);
         cJSON *date = cJSON_GetObjectItem(conversation, date_key);
         const char *date_str = (date && cJSON_IsString(date)) ? date->valuestring : "";
         cJSON *turn = NULL;
         cJSON_ArrayForEach(turn, field)
         {
            cJSON *dia_id = cJSON_GetObjectItem(turn, "dia_id");
            cJSON *speaker = cJSON_GetObjectItem(turn, "speaker");
            cJSON *text = cJSON_GetObjectItem(turn, "text");
            if (!dia_id || !cJSON_IsString(dia_id) || !text || !cJSON_IsString(text))
               continue;
            char key[512];
            char content[2048];
            snprintf(key, sizeof(key), "locomo %s %s",
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     dia_id->valuestring);
            snprintf(content, sizeof(content), "[%s] %s: %s", date_str,
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     text->valuestring);
            (void)memory_insert(TIER_L2, KIND_FACT, key, content, 0.9, "locomo", NULL);
         }
      }

      cJSON *item = NULL;
      cJSON_ArrayForEach(item, qa)
      {
         if (reported >= max_failures)
            break;
         cJSON *question = cJSON_GetObjectItem(item, "question");
         cJSON *answer = cJSON_GetObjectItem(item, "answer");
         if (!question || !cJSON_IsString(question) || !answer)
            continue;
         mem_eval_qa_case_t qcase;
         memset(&qcase, 0, sizeof(qcase));
         snprintf(qcase.question, sizeof(qcase.question), "%s", question->valuestring);
         if (cJSON_IsString(answer))
            snprintf(qcase.answer, sizeof(qcase.answer), "%s", answer->valuestring);
         else if (cJSON_IsNumber(answer))
            snprintf(qcase.answer, sizeof(qcase.answer), "%.0f", answer->valuedouble);
         else
            continue;

         mem_eval_qa_trace_t trace;
         if (mem_eval_trace_qa_case(&cfg, &qcase, top_k, token_budget, &trace) != 0)
            continue;
         if (trace.judged_correct)
            continue;
         mem_eval_print_qa_failure(fp, &trace);
         reported++;
      }

      mem_eval_close_temp_db();
      if (reported >= max_failures)
         break;
   }

   agent_http_cleanup();
   cJSON_Delete(root);
   return 0;
}

int mem_eval_report_locomo_misses(const char *dataset_path, int max_samples, int limit,
                                  int max_misses, FILE *fp, const char *progress_path)
{
   if (!dataset_path || !fp)
      return -1;
   if (limit <= 0)
      limit = 5;
   if (max_misses <= 0)
      max_misses = 20;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
      return -1;
   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   int reported = 0;
   int misses = 0;
   int cases_scanned = 0;
   int setup_completed = 0;
   int temporal_miss = 0, entity_miss = 0, lexical_gap = 0, semantic_gap = 0;
   int state_penalty = 0, granularity_gap = 0, ranking_gap = 0, missing = 0;
   FILE *progress_fp = mem_eval_open_append_progress_file(progress_path);
   int setup_expected = cJSON_GetArraySize(root);
   if (max_samples > 0 && max_samples < setup_expected)
      setup_expected = max_samples;
   mem_eval_async_drain_totals_t drain_totals;
   memset(&drain_totals, 0, sizeof(drain_totals));

   cJSON *sample = NULL;
   cJSON_ArrayForEach(sample, root)
   {
      if (max_samples > 0 && setup_completed >= max_samples)
         break;
      setup_completed++;
      mem_eval_append_miss_setup_progress_row(progress_fp, "locomo", "conversation",
                                              setup_completed, setup_expected);
      cJSON *conversation = cJSON_GetObjectItem(sample, "conversation");
      cJSON *qa = cJSON_GetObjectItem(sample, "qa");
      if (!conversation || !cJSON_IsObject(conversation) || !qa || !cJSON_IsArray(qa))
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;

      corpus_fid_entry_t *fid_map = calloc(2048, sizeof(corpus_fid_entry_t));
      int fid_count = 0;
      int fid_cap = 2048;
      if (!fid_map)
      {
         mem_eval_close_temp_db();
         continue;
      }

      cJSON *field = NULL;
      cJSON_ArrayForEach(field, conversation)
      {
         if (!field->string || strncmp(field->string, "session_", 8) != 0 ||
             strstr(field->string, "_date_time") != NULL || !cJSON_IsArray(field))
            continue;

         char date_key[64];
         snprintf(date_key, sizeof(date_key), "%s_date_time", field->string);
         cJSON *date = cJSON_GetObjectItem(conversation, date_key);
         const char *date_str = (date && cJSON_IsString(date)) ? date->valuestring : "";

         cJSON *turn = NULL;
         cJSON_ArrayForEach(turn, field)
         {
            cJSON *dia_id = cJSON_GetObjectItem(turn, "dia_id");
            cJSON *speaker = cJSON_GetObjectItem(turn, "speaker");
            cJSON *text = cJSON_GetObjectItem(turn, "text");
            if (!dia_id || !cJSON_IsString(dia_id) || !text || !cJSON_IsString(text))
               continue;
            if (fid_count >= fid_cap)
               break;

            char key[512];
            char content[2048];
            snprintf(key, sizeof(key), "locomo %s %s",
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     dia_id->valuestring);
            snprintf(content, sizeof(content), "[%s] %s: %s", date_str,
                     (speaker && cJSON_IsString(speaker)) ? speaker->valuestring : "speaker",
                     text->valuestring);

            memory_t mem;
            if (memory_insert(TIER_L2, KIND_FACT, key, content, 0.9, "locomo", &mem) == 0)
            {
               snprintf(fid_map[fid_count].fid, sizeof(fid_map[fid_count].fid), "%s",
                        dia_id->valuestring);
               fid_map[fid_count].db_id = mem.id;
               fid_count++;
            }
         }
      }

      mem_eval_case_t *cases = calloc((size_t)cJSON_GetArraySize(qa), sizeof(mem_eval_case_t));
      int n_cases = 0;
      if (!cases)
      {
         free(fid_map);
         mem_eval_close_temp_db();
         continue;
      }

      cJSON *item = NULL;
      cJSON_ArrayForEach(item, qa)
      {
         cJSON *question = cJSON_GetObjectItem(item, "question");
         cJSON *evidence = cJSON_GetObjectItem(item, "evidence");
         if (!question || !cJSON_IsString(question) || !evidence || !cJSON_IsArray(evidence))
            continue;

         mem_eval_case_t *ec = &cases[n_cases];
         memset(ec, 0, sizeof(*ec));
         mem_eval_normalize_question(question->valuestring, ec->query, sizeof(ec->query));

         cJSON *ev = NULL;
         cJSON_ArrayForEach(ev, evidence)
         {
            if (!cJSON_IsString(ev))
               continue;
            int64_t db_id = fid_lookup(fid_map, fid_count, ev->valuestring);
            if (db_id > 0 && ec->n_expected < 20)
               ec->expected_ids[ec->n_expected++] = db_id;
         }
         if (ec->n_expected > 0)
            n_cases++;
      }

      if (n_cases > 0 && mem_eval_drain_async_queues(&drain_totals) != 0)
      {
         free(cases);
         free(fid_map);
         mem_eval_close_temp_db();
         cJSON_Delete(root);
         if (progress_fp)
            fclose(progress_fp);
         fprintf(stderr, "mem_eval_report_locomo_misses: async drain failed\n");
         return -1;
      }

      if (n_cases > 0)
         mem_eval_print_miss_report(fp, cases, n_cases, limit, max_misses, &reported, &misses,
                                    &temporal_miss, &entity_miss, &lexical_gap, &semantic_gap,
                                    &state_penalty, &granularity_gap, &ranking_gap, &missing,
                                    progress_fp, "locomo", &cases_scanned);

      free(cases);
      free(fid_map);
      mem_eval_close_temp_db();
   }

   cJSON_Delete(root);
   if (progress_fp)
      fclose(progress_fp);
   fprintf(fp,
           "Miss summary: total=%d limit=%d temporal=%d entity=%d lexical=%d semantic=%d state=%d "
           "granularity=%d ranking=%d missing=%d\n",
           misses, limit, temporal_miss, entity_miss, lexical_gap, semantic_gap, state_penalty,
           granularity_gap, ranking_gap, missing);
   mem_eval_emit_async_drain_summary(stderr, &drain_totals);
   return 0;
}

int mem_eval_run_longmemeval(const char *dataset_path, int max_cases, mem_eval_scores_t *out,
                             mem_eval_latency_t *latency_out, int *cases_out,
                             const char *progress_path)
{
   if (!dataset_path || !out)
      return -1;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
   {
      fprintf(stderr, "mem_eval_run_longmemeval: cannot read %s\n", dataset_path);
      return -1;
   }

   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      fprintf(stderr, "mem_eval_run_longmemeval: invalid JSON in %s\n", dataset_path);
      return -1;
   }

   mem_eval_scores_t totals;
   memset(&totals, 0, sizeof(totals));
   double *latencies = NULL;
   int latency_count = 0;
   int latency_cap = 0;
   FILE *progress_fp = mem_eval_open_progress_file(progress_path);
   mem_eval_async_drain_totals_t drain_totals;
   memset(&drain_totals, 0, sizeof(drain_totals));

   cJSON *item = NULL;
   cJSON_ArrayForEach(item, root)
   {
      if (max_cases > 0 && totals.n_cases >= max_cases)
         break;

      cJSON *question_id = cJSON_GetObjectItem(item, "question_id");
      cJSON *question = cJSON_GetObjectItem(item, "question");
      cJSON *session_ids = cJSON_GetObjectItem(item, "haystack_session_ids");
      cJSON *dates = cJSON_GetObjectItem(item, "haystack_dates");
      cJSON *sessions = cJSON_GetObjectItem(item, "haystack_sessions");
      cJSON *answer_ids = cJSON_GetObjectItem(item, "answer_session_ids");

      if (!question || !cJSON_IsString(question) || !session_ids || !cJSON_IsArray(session_ids) ||
          !sessions || !cJSON_IsArray(sessions) || !answer_ids || !cJSON_IsArray(answer_ids))
         continue;
      if (question_id && cJSON_IsString(question_id) &&
          strstr(question_id->valuestring, "_abs") != NULL)
         continue;
      if (cJSON_GetArraySize(answer_ids) <= 0)
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;

      int n_sessions = cJSON_GetArraySize(session_ids);
      corpus_fid_entry_t *fid_map = calloc((size_t)n_sessions, sizeof(corpus_fid_entry_t));
      if (!fid_map)
      {
         mem_eval_close_temp_db();
         continue;
      }
      int fid_count = 0;

      for (int i = 0; i < n_sessions; i++)
      {
         cJSON *sid = cJSON_GetArrayItem(session_ids, i);
         cJSON *sess = cJSON_GetArrayItem(sessions, i);
         cJSON *date = dates ? cJSON_GetArrayItem(dates, i) : NULL;
         if (!sid || !cJSON_IsString(sid) || !sess || !cJSON_IsArray(sess))
            continue;

         char content[2048];
         size_t used = 0;
         content[0] = '\0';
         if (date && cJSON_IsString(date))
            used += (size_t)snprintf(content + used, sizeof(content) - used, "[%s] ",
                                     date->valuestring);

         cJSON *turn = NULL;
         cJSON_ArrayForEach(turn, sess)
         {
            cJSON *role = cJSON_GetObjectItem(turn, "role");
            cJSON *turn_content = cJSON_GetObjectItem(turn, "content");
            if (!turn_content || !cJSON_IsString(turn_content) || used >= sizeof(content) - 1)
               continue;
            used += (size_t)snprintf(content + used, sizeof(content) - used, "%s: %s ",
                                     (role && cJSON_IsString(role)) ? role->valuestring : "turn",
                                     turn_content->valuestring);
            if (used >= sizeof(content) - 1)
               break;
         }

         char key[512];
         snprintf(key, sizeof(key), "longmemeval session %s", sid->valuestring);
         memory_t mem;
         if (memory_insert(TIER_L2, KIND_FACT, key, content, 0.9, "longmemeval", &mem) == 0)
         {
            snprintf(fid_map[fid_count].fid, sizeof(fid_map[fid_count].fid), "%s",
                     sid->valuestring);
            fid_map[fid_count].db_id = mem.id;
            fid_count++;
         }
      }

      if (fid_count > 0 && mem_eval_drain_async_queues(&drain_totals) != 0)
      {
         free(fid_map);
         mem_eval_close_temp_db();
         cJSON_Delete(root);
         free(latencies);
         if (progress_fp)
            fclose(progress_fp);
         fprintf(stderr, "mem_eval_run_longmemeval: async drain failed\n");
         return -1;
      }

      if (fid_count > 0)
      {
         mem_eval_case_t one;
         memset(&one, 0, sizeof(one));
         mem_eval_normalize_question(question->valuestring, one.query, sizeof(one.query));
         cJSON *aid = NULL;
         cJSON_ArrayForEach(aid, answer_ids)
         {
            if (!cJSON_IsString(aid))
               continue;
            int64_t db_id = fid_lookup(fid_map, fid_count, aid->valuestring);
            if (db_id > 0 && one.n_expected < 20)
               one.expected_ids[one.n_expected++] = db_id;
         }

         mem_eval_direct_trace_t trace;
         if (one.n_expected > 0 && mem_eval_score_case(&one, &trace) == 0)
         {
            append_latency_sample(&latencies, &latency_count, &latency_cap, trace.latency_ms);

            totals.mrr += trace.mrr;
            totals.ndcg_5 += trace.ndcg_5;
            totals.ndcg_10 += trace.ndcg_10;
            totals.recall_5 += trace.recall_5;
            totals.recall_10 += trace.recall_10;
            totals.n_cases += 1;
            cJSON *answer = cJSON_GetObjectItem(item, "answer");
            cJSON *subset = cJSON_GetObjectItem(item, "question_type");
            char subset_buf[64];
            if (question_id && cJSON_IsString(question_id) &&
                strstr(question_id->valuestring, "_abs") != NULL)
               snprintf(subset_buf, sizeof(subset_buf), "abstention");
            else if (subset && cJSON_IsString(subset))
               snprintf(subset_buf, sizeof(subset_buf), "%s", subset->valuestring);
            else
               snprintf(subset_buf, sizeof(subset_buf), "default");
            mem_eval_append_direct_progress_row(
                progress_fp, "longmemeval",
                (question_id && cJSON_IsString(question_id)) ? question_id->valuestring : one.query,
                "subset", subset_buf, question->valuestring,
                (answer && cJSON_IsString(answer)) ? answer->valuestring : "", &trace);
         }
      }

      free(fid_map);
      mem_eval_close_temp_db();
   }

   cJSON_Delete(root);

   if (totals.n_cases <= 0)
   {
      free(latencies);
      return -1;
   }

   out->mrr = totals.mrr / totals.n_cases;
   out->ndcg_5 = totals.ndcg_5 / totals.n_cases;
   out->ndcg_10 = totals.ndcg_10 / totals.n_cases;
   out->recall_5 = totals.recall_5 / totals.n_cases;
   out->recall_10 = totals.recall_10 / totals.n_cases;
   out->n_cases = totals.n_cases;
   if (latency_out)
      mem_eval_latency_finalize(latencies, latency_count, latency_out);
   if (cases_out)
      *cases_out = totals.n_cases;
   free(latencies);
   if (progress_fp)
      fclose(progress_fp);
   mem_eval_emit_async_drain_summary(stderr, &drain_totals);
   return 0;
}

int mem_eval_run_longmemeval_qa(const char *dataset_path, int max_cases, int top_k,
                                int token_budget, mem_eval_qa_scores_t *out,
                                mem_eval_latency_t *latency_out, int *cases_out)
{
   if (!dataset_path || !out)
      return -1;

   agent_config_t cfg;
   if (mem_eval_load_benchmark_agent_config(&cfg) != 0 || cfg.agent_count == 0)
      return -1;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
      return -1;

   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   agent_http_init();

   mem_eval_qa_scores_t totals;
   memset(&totals, 0, sizeof(totals));
   double *latencies = NULL;
   int latency_count = 0;
   int latency_cap = 0;

   cJSON *item = NULL;
   cJSON_ArrayForEach(item, root)
   {
      if (max_cases > 0 && totals.n_cases >= max_cases)
         break;

      cJSON *question_id = cJSON_GetObjectItem(item, "question_id");
      cJSON *question = cJSON_GetObjectItem(item, "question");
      cJSON *answer = cJSON_GetObjectItem(item, "answer");
      cJSON *session_ids = cJSON_GetObjectItem(item, "haystack_session_ids");
      cJSON *dates = cJSON_GetObjectItem(item, "haystack_dates");
      cJSON *sessions = cJSON_GetObjectItem(item, "haystack_sessions");
      cJSON *answer_ids = cJSON_GetObjectItem(item, "answer_session_ids");

      if (!question || !cJSON_IsString(question) || !answer || !session_ids ||
          !cJSON_IsArray(session_ids) || !sessions || !cJSON_IsArray(sessions) || !answer_ids ||
          !cJSON_IsArray(answer_ids))
         continue;
      if (question_id && cJSON_IsString(question_id) &&
          strstr(question_id->valuestring, "_abs") != NULL)
         continue;
      if (cJSON_GetArraySize(answer_ids) <= 0)
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;

      int n_sessions = cJSON_GetArraySize(session_ids);
      for (int i = 0; i < n_sessions; i++)
      {
         cJSON *sid = cJSON_GetArrayItem(session_ids, i);
         cJSON *sess = cJSON_GetArrayItem(sessions, i);
         cJSON *date = dates ? cJSON_GetArrayItem(dates, i) : NULL;
         if (!sid || !cJSON_IsString(sid) || !sess || !cJSON_IsArray(sess))
            continue;
         mem_eval_insert_longmemeval_qa_session(
             sid->valuestring, (date && cJSON_IsString(date)) ? date->valuestring : "", sess);
      }

      mem_eval_qa_case_t one;
      memset(&one, 0, sizeof(one));
      snprintf(one.question, sizeof(one.question), "%s", question->valuestring);
      if (cJSON_IsString(answer))
         snprintf(one.answer, sizeof(one.answer), "%s", answer->valuestring);
      else if (cJSON_IsNumber(answer))
         snprintf(one.answer, sizeof(one.answer), "%.0f", answer->valuedouble);
      else
      {
         mem_eval_close_temp_db();
         continue;
      }

      mem_eval_qa_scores_t item_scores;
      mem_eval_latency_t item_latency;
      memory_query_plan_t plan;
      memset(&plan, 0, sizeof(plan));
      (void)memory_query_plan(one.question, top_k, 96, &plan);
      int route_bucket = mem_eval_route_bucket_index(plan.route);
      int shape_bucket = mem_eval_shape_bucket_index(plan.shape);
      if (mem_eval_run_qa_with_latency(&cfg, &one, 1, top_k, token_budget, &item_scores,
                                       &item_latency) == 0)
      {
         totals.accuracy += item_scores.accuracy;
         totals.exact_match += item_scores.exact_match;
         totals.avg_retrieved_tokens += item_scores.avg_retrieved_tokens;
         totals.n_cases++;
         totals.answered_cases += item_scores.answered_cases;
         totals.judged_cases += item_scores.judged_cases;
         totals.cited_answers += item_scores.cited_answers;
         totals.uncited_answers += item_scores.uncited_answers;
         totals.low_confidence_answers += item_scores.low_confidence_answers;
         totals.total_answer_prompt_tokens += item_scores.total_answer_prompt_tokens;
         totals.total_answer_completion_tokens += item_scores.total_answer_completion_tokens;
         totals.total_judge_prompt_tokens += item_scores.total_judge_prompt_tokens;
         totals.total_judge_completion_tokens += item_scores.total_judge_completion_tokens;
         if (route_bucket >= 0 && route_bucket < MEM_EVAL_ROUTE_BUCKET_COUNT)
            mem_eval_bucket_add(&totals.route_buckets[route_bucket], item_scores.accuracy,
                                item_scores.exact_match);
         if (shape_bucket >= 0 && shape_bucket < MEM_EVAL_SHAPE_BUCKET_COUNT)
            mem_eval_bucket_add(&totals.shape_buckets[shape_bucket], item_scores.accuracy,
                                item_scores.exact_match);
         if (item_latency.n_queries > 0)
            append_latency_sample(&latencies, &latency_count, &latency_cap, item_latency.p50_ms);
      }

      mem_eval_close_temp_db();
   }

   agent_http_cleanup();
   cJSON_Delete(root);

   if (totals.n_cases <= 0)
      return -1;

   *out = totals;
   out->accuracy /= totals.n_cases;
   out->exact_match /= totals.n_cases;
   out->avg_retrieved_tokens /= totals.n_cases;
   out->citation_coverage =
       out->answered_cases > 0 ? (double)out->cited_answers / out->answered_cases : 0.0;
   out->citation_miss_rate =
       out->answered_cases > 0 ? (double)out->uncited_answers / out->answered_cases : 0.0;
   out->hallucination_rate = 1.0 - out->accuracy;
   mem_eval_finalize_bucket_array(out->route_buckets, MEM_EVAL_ROUTE_BUCKET_COUNT);
   mem_eval_finalize_bucket_array(out->shape_buckets, MEM_EVAL_SHAPE_BUCKET_COUNT);
   if (latency_out)
      mem_eval_latency_finalize(latencies, latency_count, latency_out);
   if (cases_out)
      *cases_out = totals.n_cases;
   free(latencies);
   return 0;
}

int mem_eval_report_longmemeval_qa_failures(const char *dataset_path, int max_cases, int top_k,
                                            int token_budget, int max_failures, FILE *fp)
{
   if (!dataset_path || !fp)
      return -1;
   if (max_failures <= 0)
      max_failures = 5;

   agent_config_t cfg;
   if (mem_eval_load_benchmark_agent_config(&cfg) != 0 || cfg.agent_count == 0)
      return -1;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
      return -1;
   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   agent_http_init();
   int reported = 0;
   int seen = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, root)
   {
      if (max_cases > 0 && seen >= max_cases)
         break;
      if (reported >= max_failures)
         break;

      cJSON *question_id = cJSON_GetObjectItem(item, "question_id");
      cJSON *question = cJSON_GetObjectItem(item, "question");
      cJSON *answer = cJSON_GetObjectItem(item, "answer");
      cJSON *session_ids = cJSON_GetObjectItem(item, "haystack_session_ids");
      cJSON *dates = cJSON_GetObjectItem(item, "haystack_dates");
      cJSON *sessions = cJSON_GetObjectItem(item, "haystack_sessions");
      cJSON *answer_ids = cJSON_GetObjectItem(item, "answer_session_ids");
      if (!question || !cJSON_IsString(question) || !answer || !session_ids ||
          !cJSON_IsArray(session_ids) || !sessions || !cJSON_IsArray(sessions) || !answer_ids ||
          !cJSON_IsArray(answer_ids))
         continue;
      if (question_id && cJSON_IsString(question_id) && strstr(question_id->valuestring, "_abs"))
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;
      int n_sessions = cJSON_GetArraySize(session_ids);
      for (int i = 0; i < n_sessions; i++)
      {
         cJSON *sid = cJSON_GetArrayItem(session_ids, i);
         cJSON *sess = cJSON_GetArrayItem(sessions, i);
         cJSON *date = dates ? cJSON_GetArrayItem(dates, i) : NULL;
         if (!sid || !cJSON_IsString(sid) || !sess || !cJSON_IsArray(sess))
            continue;
         mem_eval_insert_longmemeval_qa_session(
             sid->valuestring, (date && cJSON_IsString(date)) ? date->valuestring : "", sess);
      }

      mem_eval_qa_case_t qcase;
      memset(&qcase, 0, sizeof(qcase));
      snprintf(qcase.question, sizeof(qcase.question), "%s", question->valuestring);
      if (cJSON_IsString(answer))
         snprintf(qcase.answer, sizeof(qcase.answer), "%s", answer->valuestring);
      else if (cJSON_IsNumber(answer))
         snprintf(qcase.answer, sizeof(qcase.answer), "%.0f", answer->valuedouble);
      else
      {
         mem_eval_close_temp_db();
         continue;
      }

      mem_eval_qa_trace_t trace;
      if (mem_eval_trace_qa_case(&cfg, &qcase, top_k, token_budget, &trace) == 0 &&
          !trace.judged_correct)
      {
         mem_eval_print_qa_failure(fp, &trace);
         reported++;
      }
      seen++;
      mem_eval_close_temp_db();
   }

   agent_http_cleanup();
   cJSON_Delete(root);
   return 0;
}

int mem_eval_report_longmemeval_misses(const char *dataset_path, int max_cases, int limit,
                                       int max_misses, FILE *fp, const char *progress_path)
{
   if (!dataset_path || !fp)
      return -1;
   if (limit <= 0)
      limit = 5;
   if (max_misses <= 0)
      max_misses = 20;

   char *raw = slurp_file_eval(dataset_path);
   if (!raw)
      return -1;
   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   int reported = 0;
   int misses = 0;
   int cases_scanned = 0;
   int setup_completed = 0;
   int temporal_miss = 0, entity_miss = 0, lexical_gap = 0, semantic_gap = 0;
   int state_penalty = 0, granularity_gap = 0, ranking_gap = 0, missing = 0;
   FILE *progress_fp = mem_eval_open_append_progress_file(progress_path);
   int setup_expected = cJSON_GetArraySize(root);
   if (max_cases > 0 && max_cases < setup_expected)
      setup_expected = max_cases;
   mem_eval_async_drain_totals_t drain_totals;
   memset(&drain_totals, 0, sizeof(drain_totals));

   cJSON *item = NULL;
   cJSON_ArrayForEach(item, root)
   {
      if (max_cases > 0 && setup_completed >= max_cases)
         break;
      setup_completed++;
      mem_eval_append_miss_setup_progress_row(progress_fp, "longmemeval", "question",
                                              setup_completed, setup_expected);
      cJSON *question_id = cJSON_GetObjectItem(item, "question_id");
      cJSON *question = cJSON_GetObjectItem(item, "question");
      cJSON *session_ids = cJSON_GetObjectItem(item, "haystack_session_ids");
      cJSON *dates = cJSON_GetObjectItem(item, "haystack_dates");
      cJSON *sessions = cJSON_GetObjectItem(item, "haystack_sessions");
      cJSON *answer_ids = cJSON_GetObjectItem(item, "answer_session_ids");

      if (!question || !cJSON_IsString(question) || !session_ids || !cJSON_IsArray(session_ids) ||
          !sessions || !cJSON_IsArray(sessions) || !answer_ids || !cJSON_IsArray(answer_ids))
         continue;
      if (question_id && cJSON_IsString(question_id) &&
          strstr(question_id->valuestring, "_abs") != NULL)
         continue;
      if (cJSON_GetArraySize(answer_ids) <= 0)
         continue;

      if (mem_eval_open_temp_db() != 0)
         continue;

      int n_sessions = cJSON_GetArraySize(session_ids);
      corpus_fid_entry_t *fid_map = calloc((size_t)n_sessions, sizeof(corpus_fid_entry_t));
      if (!fid_map)
      {
         mem_eval_close_temp_db();
         continue;
      }
      int fid_count = 0;

      for (int i = 0; i < n_sessions; i++)
      {
         cJSON *sid = cJSON_GetArrayItem(session_ids, i);
         cJSON *sess = cJSON_GetArrayItem(sessions, i);
         cJSON *date = dates ? cJSON_GetArrayItem(dates, i) : NULL;
         if (!sid || !cJSON_IsString(sid) || !sess || !cJSON_IsArray(sess))
            continue;

         char content[2048];
         size_t used = 0;
         content[0] = '\0';
         if (date && cJSON_IsString(date))
            used += (size_t)snprintf(content + used, sizeof(content) - used, "[%s] ",
                                     date->valuestring);

         cJSON *turn = NULL;
         cJSON_ArrayForEach(turn, sess)
         {
            cJSON *role = cJSON_GetObjectItem(turn, "role");
            cJSON *turn_content = cJSON_GetObjectItem(turn, "content");
            if (!turn_content || !cJSON_IsString(turn_content) || used >= sizeof(content) - 1)
               continue;
            used += (size_t)snprintf(content + used, sizeof(content) - used, "%s: %s ",
                                     (role && cJSON_IsString(role)) ? role->valuestring : "turn",
                                     turn_content->valuestring);
            if (used >= sizeof(content) - 1)
               break;
         }

         char key[512];
         snprintf(key, sizeof(key), "longmemeval session %s", sid->valuestring);
         memory_t mem;
         if (memory_insert(TIER_L2, KIND_FACT, key, content, 0.9, "longmemeval", &mem) == 0)
         {
            snprintf(fid_map[fid_count].fid, sizeof(fid_map[fid_count].fid), "%s",
                     sid->valuestring);
            fid_map[fid_count].db_id = mem.id;
            fid_count++;
         }
      }

      if (fid_count > 0 && mem_eval_drain_async_queues(&drain_totals) != 0)
      {
         free(fid_map);
         mem_eval_close_temp_db();
         cJSON_Delete(root);
         if (progress_fp)
            fclose(progress_fp);
         fprintf(stderr, "mem_eval_report_longmemeval_misses: async drain failed\n");
         return -1;
      }

      if (fid_count > 0)
      {
         mem_eval_case_t one;
         memset(&one, 0, sizeof(one));
         mem_eval_normalize_question(question->valuestring, one.query, sizeof(one.query));

         cJSON *aid = NULL;
         cJSON_ArrayForEach(aid, answer_ids)
         {
            if (!cJSON_IsString(aid))
               continue;
            int64_t db_id = fid_lookup(fid_map, fid_count, aid->valuestring);
            if (db_id > 0 && one.n_expected < 20)
               one.expected_ids[one.n_expected++] = db_id;
         }

         if (one.n_expected > 0)
            mem_eval_print_miss_report(fp, &one, 1, limit, max_misses, &reported, &misses,
                                       &temporal_miss, &entity_miss, &lexical_gap, &semantic_gap,
                                       &state_penalty, &granularity_gap, &ranking_gap, &missing,
                                       progress_fp, "longmemeval", &cases_scanned);
      }

      free(fid_map);
      mem_eval_close_temp_db();
   }

   cJSON_Delete(root);
   if (progress_fp)
      fclose(progress_fp);
   fprintf(fp,
           "Miss summary: total=%d limit=%d temporal=%d entity=%d lexical=%d semantic=%d state=%d "
           "granularity=%d ranking=%d missing=%d\n",
           misses, limit, temporal_miss, entity_miss, lexical_gap, semantic_gap, state_penalty,
           granularity_gap, ranking_gap, missing);
   mem_eval_emit_async_drain_summary(stderr, &drain_totals);
   return 0;
}
