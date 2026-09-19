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

int mem_eval_score_retrieval(const char *query, const int64_t *expected_ids, int n_expected,
                             mem_eval_direct_trace_t *trace_out);

/* File / DB helpers */
char *slurp_file_eval(const char *path);
/* Optional progress artifacts for retained native host benchmarks. */
FILE *mem_eval_open_progress_file(const char *path);
FILE *mem_eval_open_append_progress_file(const char *path);

/* Latency bookkeeping */
double elapsed_ms(const struct timespec *start, const struct timespec *end);
int append_latency_sample(double **samples, int *count, int *cap, double value);
void mem_eval_latency_finalize(const double *samples, int n_samples, mem_eval_latency_t *out);

/* Benchmark context is assembled by Go; native callers copy the full reply. */
int mem_eval_build_retrieval_context(const char *query, int top_k, int token_budget,
                                     char *context_out, size_t context_len,
                                     int *retrieved_tokens_out);

/* Case scoring and tracing */
int mem_eval_score_case(const mem_eval_case_t *ecase, mem_eval_direct_trace_t *trace_out);
void mem_eval_print_miss_report(FILE *fp, mem_eval_case_t *cases, int n_cases, int limit,
                                int max_misses, int *reported_io, int *misses_io,
                                int *temporal_miss_io, int *entity_miss_io, int *lexical_gap_io,
                                int *semantic_gap_io, int *state_penalty_io,
                                int *granularity_gap_io, int *ranking_gap_io, int *missing_io,
                                FILE *progress_fp, const char *dataset, int *cases_scanned_io);

/* Normalization / lookup */

/* Support / scope bucket helpers */
void mem_eval_support_case_add(mem_eval_support_case_t *scase, int64_t id);
int mem_eval_run_support_with_latency(mem_eval_support_case_t *cases, int n_cases,
                                      mem_eval_scores_t *out, mem_eval_latency_t *latency_out);

/* Progress writers */
void mem_eval_append_direct_progress_row(FILE *fp, const char *dataset, const char *question_id,
                                         const char *label_key, const char *label_value,
                                         const char *question, const char *gold_answer,
                                         const mem_eval_direct_trace_t *trace);
void mem_eval_append_miss_setup_progress_row(FILE *fp, const char *dataset, const char *question_id,
                                             int n_relevant, int n_retrieved);

/* Agent config loader for benchmark runs. */

#endif /* DEC_AGENT_EVAL_INTERNAL_H */
