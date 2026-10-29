/* agent_eval_internal.h: helpers shared between agent_eval.c and
 * the retained native benchmark host adapters. Not a public
 * API — external callers should use agent_eval.h. */
#ifndef DEC_AGENT_EVAL_INTERNAL_H
#define DEC_AGENT_EVAL_INTERNAL_H 1

#include "aimee.h"
#include "agent_eval.h"
#include "agent_config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

int mem_eval_dispatch_diagnostic(cJSON *args, cJSON **reply);
int mem_eval_write_hard_negative(FILE *fp, const char *suite, const eval_task_t *task,
                                 const agent_result_t *result);

#define CORPUS_FID_LEN 64

typedef struct
{
   char query[1024];
   int64_t relevant_ids[128];
   int n_relevant;
} mem_eval_support_case_t;

typedef struct
{
   double mrr;
   double ndcg_5;
   double ndcg_10;
   double recall_5;
   double recall_10;
   double latency_ms;
   int64_t retrieved_ids[20];
   int n_retrieved;
} mem_eval_direct_trace_t;

/* Latency bookkeeping */
double elapsed_ms(const struct timespec *start, const struct timespec *end);

/* Benchmark context is assembled by Go; native callers copy the full reply. */
int mem_eval_build_retrieval_context(const char *query, int top_k, int token_budget,
                                     char *context_out, size_t context_len,
                                     int *retrieved_tokens_out);

/* Normalization / lookup */

/* Agent config loader for benchmark runs. */

#endif /* DEC_AGENT_EVAL_INTERNAL_H */
