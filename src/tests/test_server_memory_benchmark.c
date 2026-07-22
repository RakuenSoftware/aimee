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
#include "server.h"

static cJSON *g_last_response = NULL;
static char g_last_error[512];
static char g_last_fusion_state[32];

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

double ir_mrr(const int64_t *retrieved, int n_retrieved, const int64_t *relevant, int n_relevant)
{
   for (int i = 0; i < n_retrieved; i++)
      for (int j = 0; j < n_relevant; j++)
         if (retrieved[i] == relevant[j])
            return 1.0 / (double)(i + 1);
   return 0.0;
}

double ir_ndcg_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                    int n_relevant, int k)
{
   return ir_recall_at_k(retrieved, n_retrieved, relevant, n_relevant, k);
}

double ir_recall_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                      int n_relevant, int k)
{
   int hits = 0;
   for (int i = 0; i < n_retrieved && i < k; i++)
      for (int j = 0; j < n_relevant; j++)
         if (retrieved[i] == relevant[j])
            hits++;
   return n_relevant > 0 ? (double)hits / (double)n_relevant : 0.0;
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
   test_async_only_and_unknown();
   printf("all tests passed\n");
   return 0;
}
