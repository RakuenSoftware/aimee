/* agent_eval_internal.h: helpers shared between agent_eval.c and
 * agent_eval_benchmarks.c (LoCoMo + LongMemEval runners). Not a public
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

#define CORPUS_FID_LEN 64

typedef struct
{
   char fid[CORPUS_FID_LEN];
   int64_t db_id;
} corpus_fid_entry_t;

typedef struct
{
   char query[1024];
   int64_t relevant_ids[128];
   int n_relevant;
} mem_eval_support_case_t;

typedef struct
{
   char fid[CORPUS_FID_LEN];
   int64_t db_id;
   char scope[64];
} locomo_fid_entry_t;

typedef struct
{
   char scope[64];
   int64_t ids[128];
   int count;
} mem_eval_scope_bucket_t;

typedef struct
{
   char question[1024];
   char answer[512];
} mem_eval_qa_case_t;

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

typedef struct
{
   char question[1024];
   char gold_answer[512];
   char model_answer[2048];
   char judge_response[2048];
   char retrieved_context[16384];
   int retrieved_tokens;
   int exact;
   int judged_correct;
   int answered;
   int judged;
} mem_eval_qa_trace_t;

/* File / DB helpers */
char *slurp_file_eval(const char *path);
/* Open a per-call isolated scratch DB registered as the DB2 shared handle.
 * Pair with mem_eval_close_temp_db. Returns 0 on success, -1 on failure. */
int mem_eval_open_temp_db(void);
void mem_eval_close_temp_db(void);
FILE *mem_eval_open_progress_file(const char *path);
FILE *mem_eval_open_append_progress_file(const char *path);

/* Latency bookkeeping */
double elapsed_ms(const struct timespec *start, const struct timespec *end);
int append_latency_sample(double **samples, int *count, int *cap, double value);
void mem_eval_latency_finalize(const double *samples, int n_samples, mem_eval_latency_t *out);

/* Case scoring and tracing */
int mem_eval_score_case(const mem_eval_case_t *ecase, mem_eval_direct_trace_t *trace_out);
int mem_eval_run_qa_with_latency(agent_config_t *cfg, mem_eval_qa_case_t *cases, int n_cases,
                                 int top_k, int token_budget, mem_eval_qa_scores_t *out,
                                 mem_eval_latency_t *latency_out);
int mem_eval_trace_qa_case(agent_config_t *cfg, const mem_eval_qa_case_t *qcase, int top_k,
                           int token_budget, mem_eval_qa_trace_t *trace_out);
void mem_eval_print_qa_failure(FILE *fp, const mem_eval_qa_trace_t *trace);
void mem_eval_print_miss_report(FILE *fp, mem_eval_case_t *cases, int n_cases, int limit,
                                int max_misses, int *reported_io, int *misses_io,
                                int *temporal_miss_io, int *entity_miss_io, int *lexical_gap_io,
                                int *semantic_gap_io, int *state_penalty_io,
                                int *granularity_gap_io, int *ranking_gap_io, int *missing_io,
                                FILE *progress_fp, const char *dataset, int *cases_scanned_io);

/* Normalization / lookup */
void mem_eval_normalize_question(const char *question, char *out, size_t out_len);
int64_t fid_lookup(const corpus_fid_entry_t *map, int n_map, const char *fid);
int locomo_fid_lookup(const locomo_fid_entry_t *map, int n_map, const char *fid, int64_t *db_id_out,
                      char *scope_out, size_t scope_len);

/* Support / scope bucket helpers */
mem_eval_scope_bucket_t *mem_eval_get_scope_bucket(mem_eval_scope_bucket_t *buckets, int *n_buckets,
                                                   int max_buckets, const char *scope_key);
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
void mem_eval_insert_longmemeval_qa_session(const char *session_id, const char *date_str,
                                            cJSON *sess);

/* Agent config loader for benchmark runs. */
int mem_eval_load_benchmark_agent_config(agent_config_t *cfg);

#endif /* DEC_AGENT_EVAL_INTERNAL_H */
