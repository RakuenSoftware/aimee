#include <aimee/benchmarks/module_api.h>
#include <aimee/core/event_bus/module_runtime.h>

#include <math.h>
#include <stdlib.h>

static int benchmark_is_relevant(int64_t id, const int64_t *relevant, uint32_t relevant_count)
{
   for (uint32_t i = 0; i < relevant_count; ++i)
      if (relevant[i] == id)
         return 1;
   return 0;
}

static void benchmark_score(const int64_t *retrieved, uint32_t retrieved_count,
                            const int64_t *relevant, uint32_t relevant_count, uint32_t k,
                            aimee_benchmarks_ir_scores_t *scores)
{
   scores->mrr = 0.0;
   scores->ndcg = 0.0;
   scores->recall = 0.0;
   for (uint32_t i = 0; i < retrieved_count; ++i)
      if (benchmark_is_relevant(retrieved[i], relevant, relevant_count))
      {
         scores->mrr = 1.0 / (double)(i + 1u);
         break;
      }
   if (relevant_count == 0)
      return;

   uint32_t limit = retrieved_count < k ? retrieved_count : k;
   double dcg = 0.0;
   uint32_t found = 0;
   for (uint32_t i = 0; i < limit; ++i)
      if (benchmark_is_relevant(retrieved[i], relevant, relevant_count))
      {
         dcg += 1.0 / log2((double)i + 2.0);
         found++;
      }
   uint32_t ideal_limit = relevant_count < k ? relevant_count : k;
   double idcg = 0.0;
   for (uint32_t i = 0; i < ideal_limit; ++i)
      idcg += 1.0 / log2((double)i + 2.0);
   scores->ndcg = idcg > 0.0 ? dcg / idcg : 0.0;
   scores->recall = (double)found / (double)relevant_count;
}

static int benchmark_compare_latency(const void *left, const void *right)
{
   double a = *(const double *)left;
   double b = *(const double *)right;
   return (a > b) - (a < b);
}

static double benchmark_percentile(const double *sorted, uint32_t count, double percentile)
{
   uint32_t index = (uint32_t)(percentile / 100.0 * (double)(count - 1u) + 0.5);
   return sorted[index < count ? index : count - 1u];
}

static aimee_module_status_t benchmark_handle_latency(const aimee_module_invocation_t *invocation,
                                                      const uint8_t *request_body,
                                                      uint32_t request_len, uint8_t *response_body,
                                                      uint32_t response_capacity,
                                                      uint32_t *response_len)
{
   double latencies[AIMEE_BENCHMARKS_MAX_LATENCIES];
   uint32_t count = 0;
   if (response_capacity < AIMEE_BENCHMARKS_LATENCY_RESPONSE_LEN ||
       aimee_benchmarks_latency_request_decode(request_body, request_len, latencies,
                                               AIMEE_BENCHMARKS_MAX_LATENCIES, &count) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   qsort(latencies, count, sizeof(latencies[0]), benchmark_compare_latency);
   aimee_benchmarks_latency_summary_t summary = {
       .p50_ms = benchmark_percentile(latencies, count, 50.0),
       .p95_ms = benchmark_percentile(latencies, count, 95.0),
       .p99_ms = benchmark_percentile(latencies, count, 99.0),
       .min_ms = latencies[0],
       .max_ms = latencies[count - 1u],
       .queries = count,
   };
   if (aimee_benchmarks_latency_response_encode(&summary, response_body, response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_BENCHMARKS_LATENCY_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (invocation->stage_id == AIMEE_BENCHMARKS_STAGE_LATENCY)
      return benchmark_handle_latency(invocation, request_body, request_len, response_body,
                                      response_capacity, response_len);

   int64_t retrieved[AIMEE_BENCHMARKS_MAX_RESULTS];
   int64_t relevant[AIMEE_BENCHMARKS_MAX_RESULTS];
   uint32_t retrieved_count = 0, relevant_count = 0, k = 0;
   if (invocation->stage_id != AIMEE_BENCHMARKS_STAGE_RUN ||
       response_capacity < AIMEE_BENCHMARKS_RESPONSE_LEN ||
       aimee_benchmarks_request_decode(request_body, request_len, retrieved,
                                       AIMEE_BENCHMARKS_MAX_RESULTS, &retrieved_count, relevant,
                                       AIMEE_BENCHMARKS_MAX_RESULTS, &relevant_count, &k) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   aimee_benchmarks_ir_scores_t scores;
   benchmark_score(retrieved, retrieved_count, relevant, relevant_count, k, &scores);
   if (aimee_benchmarks_response_encode(&scores, response_body, response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_BENCHMARKS_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
