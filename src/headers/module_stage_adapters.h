#ifndef AIMEE_SERVER_MODULE_STAGE_ADAPTERS_H
#define AIMEE_SERVER_MODULE_STAGE_ADAPTERS_H 1

#include <aimee/benchmarks/module_api.h>

#include <stdint.h>

/* Register every server-owned production seam with its separately supervised
 * process module. Calls fail closed; readiness keeps the listener out of
 * rotation until all required modules have attached to the local bus. */
void server_module_stage_adapters_configure(void);

/* The skills nudge has no reusable monolith service object, so its call site
 * invokes this adapter directly. Returns 0 on a valid module reply. */
int server_module_skill_should_fire(int hook_count, int interval, int *fire);

/* Score one bounded benchmark result set through the separately supervised
 * benchmarks process. There is no in-process scoring fallback. */
int server_module_benchmark_score(const int64_t *retrieved, uint32_t retrieved_count,
                                  const int64_t *relevant, uint32_t relevant_count, uint32_t k,
                                  aimee_benchmarks_ir_scores_t *scores);

/* Summarize measured query latency through the benchmarks process. The server
 * only captures timings and does not retain a local percentile fallback. */
int server_module_benchmark_latency(const double *latencies, uint32_t count,
                                    aimee_benchmarks_latency_summary_t *summary);

#endif
