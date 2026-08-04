/* test_server_memory_benchmark.c: memory.benchmark suite dispatch regressions. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "agent_eval.h"
#include "cJSON.h"
#include "kb_client.h"
#include "module_stage_adapters.h"
#include "server.h"

#include <aimee/core/event_bus/module_runtime.h>

extern aimee_module_status_t aimee_benchmarks_module_handler(const aimee_module_invocation_t *,
                                                             const uint8_t *, uint32_t, uint8_t *,
                                                             uint32_t, uint32_t *, void *);

static cJSON *g_last_response = NULL;
static char g_last_error[512];
static char g_last_fusion_state[32];
static int g_retrieval_failure = 0;
static int g_scoring_failure = 0;
static int g_scoring_calls = 0;
static int g_latency_failure = 0;
static int g_latency_calls = 0;

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

int server_module_benchmark_score(const int64_t *retrieved, uint32_t retrieved_count,
                                  const int64_t *relevant, uint32_t relevant_count, uint32_t k,
                                  aimee_benchmarks_ir_scores_t *scores)
{
   g_scoring_calls++;
   if (g_scoring_failure)
      return -1;
   uint8_t request[AIMEE_BENCHMARKS_REQUEST_LEN];
   uint8_t response[AIMEE_BENCHMARKS_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_BENCHMARKS_STAGE_RUN};
   if (aimee_benchmarks_request_encode(retrieved, retrieved_count, relevant, relevant_count, k,
                                       request, sizeof(request)) != 0 ||
       aimee_benchmarks_module_handler(&invocation, request, sizeof(request), response,
                                       sizeof(response), &response_len,
                                       NULL) != AIMEE_MODULE_STATUS_OK)
      return -1;
   return aimee_benchmarks_response_decode(response, response_len, scores);
}

int server_module_benchmark_latency(const double *latencies, uint32_t count,
                                    aimee_benchmarks_latency_summary_t *summary)
{
   g_latency_calls++;
   if (g_latency_failure)
      return -1;
   uint8_t request[AIMEE_BENCHMARKS_LATENCY_REQUEST_LEN];
   uint8_t response[AIMEE_BENCHMARKS_LATENCY_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_BENCHMARKS_STAGE_LATENCY};
   if (aimee_benchmarks_latency_request_encode(latencies, count, request, sizeof(request)) != 0 ||
       aimee_benchmarks_module_handler(&invocation, request, sizeof(request), response,
                                       sizeof(response), &response_len,
                                       NULL) != AIMEE_MODULE_STATUS_OK)
      return -1;
   return aimee_benchmarks_latency_response_decode(response, response_len, summary);
}

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   cJSON_Delete(g_last_response);
   g_last_response = cJSON_Duplicate(resp, 1);
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   snprintf(g_last_error, sizeof(g_last_error), "%s", message ? message : "");
   return 0;
}

static void reset_capture(void)
{
   cJSON_Delete(g_last_response);
   g_last_response = NULL;
   g_last_error[0] = '\0';
   g_last_fusion_state[0] = '\0';
   g_retrieval_failure = 0;
   g_scoring_failure = 0;
   g_scoring_calls = 0;
   g_latency_failure = 0;
   g_latency_calls = 0;
}

int kb_client_memory_load_eval_corpus(memory_t *out, int max, char *label_out, size_t label_len)
{
   assert(max >= 2);
   if (label_out && label_len > 0)
      snprintf(label_out, label_len, "unit-live-corpus");
   memset(out, 0, (size_t)max * sizeof(*out));
   out[0].id = 101;
   snprintf(out[0].key, sizeof(out[0].key), "alpha");
   out[1].id = 102;
   snprintf(out[1].key, sizeof(out[1].key), "beta");
   return 2;
}

int kb_client_memory_find_facts_ex(const char *query, int limit, memory_t *out, int max,
                                   const char *graph_code_fusion_state)
{
   (void)limit;
   assert(max >= 1);
   if (g_retrieval_failure)
      return -1;
   snprintf(g_last_fusion_state, sizeof(g_last_fusion_state), "%s",
            graph_code_fusion_state ? graph_code_fusion_state : "");
   memset(out, 0, (size_t)max * sizeof(*out));
   if (strcmp(query, "alpha") == 0 || strcmp(query, "prod-alpha") == 0)
      out[0].id = 101;
   else if (strcmp(query, "beta") == 0 || strcmp(query, "prod-beta") == 0)
      out[0].id = 102;
   else
      out[0].id = 999;
   return 1;
}

int mem_eval_load_production_corpus(const char *corpus_path, mem_eval_case_t *cases, int max_cases)
{
   (void)corpus_path;
   assert(max_cases >= 2);
   memset(cases, 0, (size_t)max_cases * sizeof(*cases));
   snprintf(cases[0].query, sizeof(cases[0].query), "prod-alpha");
   cases[0].expected_ids[0] = 101;
   cases[0].n_expected = 1;
   snprintf(cases[1].query, sizeof(cases[1].query), "prod-beta");
   cases[1].expected_ids[0] = 102;
   cases[1].n_expected = 1;
   return 2;
}

int mem_eval_fusion_arm_resolve(const char *matrix_path, const char *arm, char *state_out,
                                size_t state_len, int *utility_out, int *projection_out)
{
   (void)matrix_path;
   if (!arm || strcmp(arm, "baseline") != 0)
      return -1;
   snprintf(state_out, state_len, "off");
   *utility_out = 0;
   *projection_out = 0;
   return 0;
}

static void test_live_corpus_suite(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "corpus");
   cJSON_AddStringToObject(req, "fusion_state", "shadow");
   cJSON_AddNumberToObject(req, "max_cases", 2);
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(g_last_error[0] == '\0');
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "status")->valuestring, "ok") == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "suite")->valuestring, "corpus") == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "corpus")->valuestring, "unit-live-corpus") ==
          0);
   assert(strcmp(g_last_fusion_state, "shadow") == 0);
   assert(cJSON_GetObjectItem(g_last_response, "queries")->valueint == 2);
   cJSON *metrics = cJSON_GetObjectItem(g_last_response, "metrics");
   assert(cJSON_GetObjectItem(metrics, "cases")->valueint == 2);
   assert(cJSON_GetObjectItem(metrics, "mrr")->valuedouble == 1.0);
   assert(g_scoring_calls == 4);
   cJSON *latency = cJSON_GetObjectItem(g_last_response, "latency");
   assert(cJSON_GetObjectItem(latency, "queries")->valueint == 2);
   assert(cJSON_GetObjectItem(latency, "min_ms")->valuedouble >= 0.0);
   assert(cJSON_GetObjectItem(latency, "p50_ms")->valuedouble >=
          cJSON_GetObjectItem(latency, "min_ms")->valuedouble);
   assert(cJSON_GetObjectItem(latency, "max_ms")->valuedouble >=
          cJSON_GetObjectItem(latency, "p99_ms")->valuedouble);
   assert(g_latency_calls == 1);
   cJSON_Delete(req);
   reset_capture();
}

/* A --corpus FILE passed to a live-memory suite must be REJECTED, not silently
 * dropped (which used to make the caller think they benchmarked their file when
 * they benchmarked live memory). */
static void test_live_suite_rejects_corpus_file(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "memory-retrieval");
   cJSON_AddStringToObject(req, "corpus", "/tmp/some_corpus.json");
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(g_last_error[0] != '\0');                /* errored loudly ... */
   assert(g_last_response == NULL);                /* ... instead of emitting an ok result */
   assert(strstr(g_last_error, "corpus") != NULL); /* actionable message */
   cJSON_Delete(req);
   reset_capture();
}

static void test_code_graph_suite(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "code-graph-fusion");
   cJSON_AddStringToObject(req, "arm", "baseline");
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "suite")->valuestring, "code-graph-fusion") ==
          0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "fusion_state")->valuestring, "off") == 0);
   assert(cJSON_GetObjectItem(g_last_response, "utility_scoring")->valueint == 0);
   assert(cJSON_GetObjectItem(g_last_response, "code_projection")->valueint == 0);
   assert(g_scoring_calls == 4);
   assert(g_latency_calls == 1);
   cJSON_Delete(req);
   reset_capture();
}

static void test_scoring_module_failure_fails_once(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "corpus");
   cJSON_AddNumberToObject(req, "max_cases", 2);
   g_scoring_failure = 1;
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(g_scoring_calls == 1);
   assert(g_latency_calls == 0);
   assert(g_last_response == NULL);
   assert(strstr(g_last_error, "benchmarks scoring module unavailable") != NULL);
   cJSON_Delete(req);
   reset_capture();
}

static void test_latency_module_failure_fails_once(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "corpus");
   cJSON_AddNumberToObject(req, "max_cases", 2);
   g_latency_failure = 1;
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(g_scoring_calls == 4);
   assert(g_latency_calls == 1);
   assert(g_last_response == NULL);
   assert(strstr(g_last_error, "benchmarks latency module unavailable") != NULL);
   cJSON_Delete(req);
   reset_capture();
}

static void test_all_retrieval_failures_do_not_send_success(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "corpus");
   cJSON_AddNumberToObject(req, "max_cases", 2);
   g_retrieval_failure = 1;
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(g_scoring_calls == 0);
   assert(g_latency_calls == 0);
   assert(g_last_response == NULL);
   assert(strstr(g_last_error, "all benchmark queries failed") != NULL);
   cJSON_Delete(req);
   reset_capture();
}

static void test_async_only_and_unknown(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "longmemeval");
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "status")->valuestring, "async-only") == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "suite")->valuestring, "longmemeval") == 0);
   cJSON_Delete(req);
   reset_capture();

   req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "locomo");
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "status")->valuestring, "async-only") == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "suite")->valuestring, "locomo") == 0);
   cJSON_Delete(req);
   reset_capture();

   req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "suite", "nope");
   assert(handle_memory_benchmark(&ctx, &conn, req) == 0);
   assert(strstr(g_last_error, "unsupported benchmark suite (known:") != NULL);
   assert(g_last_response == NULL);
   cJSON_Delete(req);
   reset_capture();
}

int main(void)
{
   printf("server_memory_benchmark: ");
   test_live_corpus_suite();
   test_live_suite_rejects_corpus_file();
   test_code_graph_suite();
   test_scoring_module_failure_fails_once();
   test_latency_module_failure_fails_once();
   test_all_retrieval_failures_do_not_send_success();
   test_async_only_and_unknown();
   printf("all tests passed\n");
   return 0;
}
