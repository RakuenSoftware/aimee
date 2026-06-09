/* server_memory_benchmark.c: the memory.benchmark RPC handler.
 *
 * The code-vector-graph fusion rollout eval ships its harness in mem_benchmark
 * / agent_eval, but the `aimee` CLI is a thin RPC client and never links it, so
 * `aimee memory benchmark code-graph-fusion` had no runnable entrypoint.
 *
 * The shared harness (mem_eval_run_with_latency) retrieves via the in-process
 * memory_find_facts, which is a no-op stub in aimee-server (this target is built
 * without DB2 — see the $(SERVER) Makefile rule). So this handler runs the
 * corpus against the LIVE store the way the server reaches it: per query through
 * kb_client_memory_find_facts_ex(), which forwards the arm's
 * graph_code_fusion_state to aimee-kb, where the graph-code fusion rerank runs.
 * Recall/MRR/nDCG reuse the shared ir_* scorers; latency is measured around the
 * kb RPC (so it includes the kb hop, as the latency-budget AC requires).
 *
 * Split into its own file so server_state.c stays under the 2000-line cap. */
#include "aimee.h"
#include "server.h"
#include "json_fluent.h"
#include "cJSON.h"
#include "kb_client.h"  /* kb_client_memory_find_facts_ex, memory_t */
#include "agent_eval.h" /* mem_eval_* corpus loader + ir_* scorers */
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int send_and_free(server_conn_t *conn, cJSON *resp)
{
   return server_send_ok(conn, resp);
}

static double bench_elapsed_ms(const struct timespec *a, const struct timespec *b)
{
   return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1.0e6;
}

static int bench_cmp_double(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return (x > y) - (x < y);
}

/* Nearest-rank percentile over a sorted ascending array (n > 0). */
static double bench_percentile(const double *sorted, int n, double pct)
{
   if (n <= 0)
      return 0.0;
   int idx = (int)(pct / 100.0 * (double)(n - 1) + 0.5);
   if (idx < 0)
      idx = 0;
   if (idx >= n)
      idx = n - 1;
   return sorted[idx];
}

static void bench_add_metrics(cJSON *resp, double mrr, double ndcg5, double ndcg10, double recall5,
                              double recall10, int n_cases)
{
   cJSON *metrics = cJSON_CreateObject();
   jo_add_num(metrics, "mrr", mrr);
   jo_add_num(metrics, "ndcg_5", ndcg5);
   jo_add_num(metrics, "ndcg_10", ndcg10);
   jo_add_num(metrics, "recall_5", recall5);
   jo_add_num(metrics, "recall_10", recall10);
   jo_add_i64(metrics, "cases", n_cases);
   cJSON_AddItemToObject(resp, "metrics", metrics);
}

static void bench_add_latency(cJSON *resp, const double *latencies, int n_lat)
{
   if (n_lat <= 0)
      return;
   double *sorted = calloc((size_t)n_lat, sizeof(*sorted));
   if (!sorted)
      return;
   memcpy(sorted, latencies, (size_t)n_lat * sizeof(*sorted));
   qsort(sorted, (size_t)n_lat, sizeof(double), bench_cmp_double);

   cJSON *lat = cJSON_CreateObject();
   jo_add_num(lat, "p50_ms", bench_percentile(sorted, n_lat, 50.0));
   jo_add_num(lat, "p95_ms", bench_percentile(sorted, n_lat, 95.0));
   jo_add_num(lat, "p99_ms", bench_percentile(sorted, n_lat, 99.0));
   jo_add_num(lat, "min_ms", sorted[0]);
   jo_add_num(lat, "max_ms", sorted[n_lat - 1]);
   jo_add_i64(lat, "queries", n_lat);
   cJSON_AddItemToObject(resp, "latency", lat);
   free(sorted);
}

static int bench_run_live_cases(server_conn_t *conn, const char *suite, mem_eval_case_t *cases,
                                int n_cases, const char *fstate, cJSON *resp)
{
   double *latencies = calloc((size_t)n_cases, sizeof(double));
   if (!latencies)
      return server_send_error(conn, "out of memory", NULL);

   double total_mrr = 0, total_ndcg5 = 0, total_ndcg10 = 0, total_recall5 = 0, total_recall10 = 0;
   int labelled = 0, errors = 0, n_lat = 0;
   for (int c = 0; c < n_cases; c++)
   {
      if (cases[c].n_expected > 0)
         labelled++;

      struct timespec t0, t1;
      memory_t results[20];
      clock_gettime(CLOCK_MONOTONIC, &t0);
      int n_results = kb_client_memory_find_facts_ex(cases[c].query, 20, results, 20, fstate);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (n_results < 0)
      {
         errors++;
         continue;
      }
      latencies[n_lat++] = bench_elapsed_ms(&t0, &t1);

      int64_t retrieved[20];
      memset(retrieved, 0, sizeof(retrieved));
      for (int i = 0; i < n_results && i < 20; i++)
         retrieved[i] = results[i].id;

      total_mrr += ir_mrr(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected);
      total_ndcg5 +=
          ir_ndcg_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 5);
      total_ndcg10 +=
          ir_ndcg_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 10);
      total_recall5 +=
          ir_recall_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 5);
      total_recall10 +=
          ir_recall_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 10);
   }

   if (n_lat == 0)
   {
      free(latencies);
      return server_send_error(
          conn, "all benchmark queries failed (is aimee-kb reachable for memory.find_facts?)",
          NULL);
   }

   jo_add_str(resp, "suite", suite);
   jo_add_i64(resp, "queries", n_cases);
   jo_add_i64(resp, "labelled", labelled);
   jo_add_i64(resp, "errors", errors);
   bench_add_metrics(resp, total_mrr / n_cases, total_ndcg5 / n_cases, total_ndcg10 / n_cases,
                     total_recall5 / n_cases, total_recall10 / n_cases, n_cases);
   bench_add_latency(resp, latencies, n_lat);
   free(latencies);
   return 0;
}

static int bench_live_eval_corpus(server_conn_t *conn, const char *suite, const char *fstate,
                                  int max_cases)
{
   if (max_cases <= 0 || max_cases > 512)
      max_cases = 100;

   memory_t *rows = calloc((size_t)max_cases, sizeof(*rows));
   mem_eval_case_t *cases = calloc((size_t)max_cases, sizeof(*cases));
   if (!rows || !cases)
   {
      free(rows);
      free(cases);
      return server_send_error(conn, "out of memory loading benchmark corpus", NULL);
   }

   char basis[128] = "";
   int n_rows = kb_client_memory_load_eval_corpus(rows, max_cases, basis, sizeof(basis));
   if (n_rows <= 0)
   {
      free(rows);
      free(cases);
      return server_send_error(conn, "failed to load live memory eval corpus from aimee-kb", NULL);
   }

   int n_cases = 0;
   for (int i = 0; i < n_rows && n_cases < max_cases; i++)
   {
      const char *key = rows[i].key;
      const char *content = rows[i].content;
      snprintf(cases[n_cases].query, sizeof(cases[n_cases].query), "%s",
               key && key[0] ? key : (content ? content : ""));
      if (!cases[n_cases].query[0])
         continue;
      cases[n_cases].expected_ids[0] = rows[i].id;
      cases[n_cases].n_expected = 1;
      n_cases++;
   }
   free(rows);
   if (n_cases <= 0)
   {
      free(cases);
      return server_send_error(conn, "live memory eval corpus had no queryable rows", NULL);
   }

   cJSON *resp = jo_ok();
   jo_add_str(resp, "corpus", basis[0] ? basis : "live-memory-eval-corpus");
   int rc = bench_run_live_cases(conn, suite, cases, n_cases, fstate, resp);
   free(cases);
   if (rc != 0)
   {
      cJSON_Delete(resp);
      return rc;
   }
   return send_and_free(conn, resp);
}

static int bench_async_only(server_conn_t *conn, const char *suite, const char *run_via,
                            const char *reason)
{
   cJSON *resp = jo_ok();
   cJSON_ReplaceItemInObject(resp, "status", cJSON_CreateString("async-only"));
   jo_add_str(resp, "suite", suite);
   jo_add_str(resp, "run_via", run_via);
   jo_add_str(resp, "reason", reason);
   return send_and_free(conn, resp);
}

static int bench_code_graph_fusion(server_conn_t *conn, cJSON *req)
{
   const char *suite = "code-graph-fusion";

   const char *corpus =
       jo_str(req, "corpus", "benchmarks/code-vector-graph/production-corpus.json");
   const char *matrix = jo_str(req, "matrix", "benchmarks/code-vector-graph/ablation-matrix.json");
   const char *arm = jo_str(req, "arm", NULL);
   const char *fstate_override = jo_str(req, "fusion_state", NULL);

   /* Resolve the arm's wired knobs from the ablation matrix. The fusion state is
    * forwarded to aimee-kb; utility_scoring / code_projection are reported for
    * traceability but are not yet separately plumbed through the kb RPC, so arms
    * that differ only on those sub-gates currently score identically. */
   char arm_state[16] = "";
   int utility = 1, projection = 1;
   int have_arm = (arm && mem_eval_fusion_arm_resolve(matrix, arm, arm_state, sizeof(arm_state),
                                                      &utility, &projection) == 0);
   const char *fstate = (fstate_override && fstate_override[0]) ? fstate_override : NULL;
   if (!fstate)
   {
      if (have_arm && arm_state[0])
         fstate = arm_state;
      else
         fstate = (arm && strcmp(arm, "baseline") == 0) ? "off" : "on";
   }

   enum
   {
      MAX_BENCH_CASES = 256
   };
   mem_eval_case_t *cases = calloc(MAX_BENCH_CASES, sizeof(*cases));
   if (!cases)
      return server_send_error(conn, "out of memory loading benchmark corpus", NULL);
   int n_cases = mem_eval_load_production_corpus(corpus, cases, MAX_BENCH_CASES);
   if (n_cases <= 0)
   {
      free(cases);
      return server_send_error(conn, "failed to load benchmark corpus", NULL);
   }
   cJSON *resp = jo_ok();
   if (arm)
      jo_add_str(resp, "arm", arm);
   jo_add_str(resp, "fusion_state", fstate);
   jo_add_i64(resp, "utility_scoring", utility);
   jo_add_i64(resp, "code_projection", projection);
   int rc = bench_run_live_cases(conn, suite, cases, n_cases, fstate, resp);
   free(cases);
   if (rc != 0)
   {
      cJSON_Delete(resp);
      return rc;
   }

   return send_and_free(conn, resp);
}

/* Run a memory benchmark suite.
 *
 * Synchronous retrieval suites:
 * - code-graph-fusion: committed production corpus + live KB retrieval.
 * - memory/corpus/memory-retrieval/live: KB-provided eval corpus + live KB retrieval.
 *
 * Dataset and judge-style suites stay out of the synchronous RPC and return an
 * async-only envelope with the CLI/delegate path that owns scratch DB setup and
 * model/provider work. */
int handle_memory_benchmark(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *suite = jo_str(req, "suite", "code-graph-fusion");
   if (strcmp(suite, "code-graph-fusion") == 0)
      return bench_code_graph_fusion(conn, req);

   if (strcmp(suite, "memory") == 0 || strcmp(suite, "corpus") == 0 ||
       strcmp(suite, "memory-retrieval") == 0 || strcmp(suite, "live") == 0)
   {
      const char *fstate = jo_str(req, "fusion_state", NULL);
      int max_cases = jo_int(req, "max_cases", jo_int(req, "limit", 100));
      return bench_live_eval_corpus(conn, suite, fstate, max_cases);
   }

   if (strcmp(suite, "locomo") == 0 || strcmp(suite, "longmemeval") == 0 ||
       strcmp(suite, "locomo-qa") == 0 || strcmp(suite, "longmemeval-qa") == 0)
      return bench_async_only(
          conn, suite, "aimee memory benchmark <suite>",
          "dataset or judge-style memory suites run through the CLI/delegate benchmark path");

   return server_send_error(
       conn,
       "unsupported benchmark suite (known: code-graph-fusion, memory, corpus, "
       "memory-retrieval, live; async-only: locomo, longmemeval, locomo-qa, longmemeval-qa)",
       NULL);
}
