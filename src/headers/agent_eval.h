#ifndef DEC_AGENT_EVAL_H
#define DEC_AGENT_EVAL_H 1

#include "agent_types.h"
#include <stdio.h>

typedef struct
{
   char name[128];
   char prompt[2048];
   char role[32];
   char success_check_type[32];
   char success_check_value[512];
   int max_turns;
   int max_latency_ms;
} eval_task_t;

typedef struct
{
   char task_name[128];
   char agent_name[MAX_AGENT_NAME];
   char ablation[32];
   int success;
   int turns;
   int tool_calls;
   int tool_call_failures;
   int rescue_recoveries;
   double tool_call_success_rate;
   int prompt_tokens;
   int completion_tokens;
   int latency_ms;
   char error[512];
} eval_result_t;

typedef struct
{
   const char *ablation; /* preset name, or "all"; NULL/empty = full */
   int runs;             /* repetitions per task/preset; <=0 = 1 */
   unsigned int seed;
} agent_eval_run_options_t;

int agent_eval_run_with_options(agent_config_t *cfg, const char *suite_dir,
                                const agent_eval_run_options_t *options, eval_result_t *results,
                                int max_results);
int agent_eval_load_tasks(const char *suite_dir, eval_task_t *tasks, int max_tasks);
int agent_eval_ablation_preset(const char *preset, agent_ablation_flags_t *out);

/* Eval-to-behavior feedback loop: adjust rule weights based on recent
 * eval results. Reads/writes go through DB1 directly; callers do not
 * pass a handle. */
int eval_feedback_loop(void);

/* --- IR Metrics --- */

/* Compute Mean Reciprocal Rank. Returns 0.0 if no relevant results found. */
double ir_mrr(const int64_t *retrieved, int n_retrieved, const int64_t *relevant, int n_relevant);

/* Compute NDCG@k (Normalized Discounted Cumulative Gain). */
double ir_ndcg_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                    int n_relevant, int k);

/* Compute Recall@k. */
double ir_recall_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                      int n_relevant, int k);

/* --- Memory retrieval eval --- */

typedef struct
{
   char query[1024];
   int64_t expected_ids[20];
   int n_expected;
} mem_eval_case_t;

#define MEM_EVAL_ROUTE_BUCKET_COUNT 4
#define MEM_EVAL_SHAPE_BUCKET_COUNT 9

typedef struct
{
   int cases;
   double metric_a;
   double metric_b;
} mem_eval_bucket_scores_t;

typedef struct
{
   double mrr;
   double ndcg_5;
   double ndcg_10;
   double recall_5;
   double recall_10;
   int n_cases;
   mem_eval_bucket_scores_t route_buckets[MEM_EVAL_ROUTE_BUCKET_COUNT];
   mem_eval_bucket_scores_t shape_buckets[MEM_EVAL_SHAPE_BUCKET_COUNT];
} mem_eval_scores_t;

typedef struct
{
   double p50_ms;
   double p95_ms;
   double p99_ms;
   double min_ms;
   double max_ms;
   int n_queries;
} mem_eval_latency_t;

typedef struct
{
   double accuracy;
   double exact_match;
   double citation_coverage;
   double citation_miss_rate;
   double hallucination_rate;
   double avg_retrieved_tokens;
   int n_cases;
   int answered_cases;
   int judged_cases;
   int cited_answers;
   int uncited_answers;
   int low_confidence_answers;
   int total_answer_prompt_tokens;
   int total_answer_completion_tokens;
   int total_judge_prompt_tokens;
   int total_judge_completion_tokens;
   mem_eval_bucket_scores_t route_buckets[MEM_EVAL_ROUTE_BUCKET_COUNT];
   mem_eval_bucket_scores_t shape_buckets[MEM_EVAL_SHAPE_BUCKET_COUNT];
} mem_eval_qa_scores_t;

int mem_eval_run(mem_eval_case_t *cases, int n_cases, mem_eval_scores_t *out);
int mem_eval_run_with_latency(mem_eval_case_t *cases, int n_cases, mem_eval_scores_t *out,
                              mem_eval_latency_t *latency_out);

/* --- Corpus-based memory retrieval eval --- */

/* Maximum fixtures and cases supported by the corpus loader. */
#define MEM_CORPUS_MAX_FIXTURES 256
#define MEM_CORPUS_MAX_CASES    512

/* Load golden corpus from JSON file, open a fresh in-memory scratch DB,
 * insert fixtures, and populate cases[] with correctly mapped expected IDs.
 * Returns the number of cases loaded (>= 0), or -1 on error.
 * On success the caller must call mem_eval_close_temp_db() once done. */
int mem_eval_load_corpus(const char *corpus_path, mem_eval_case_t *cases, int max_cases);

/* Load the code-vector-graph production corpus (queries with already-resolved
 * live DB2 memory ids in `expected_ids`). Opens NO scratch DB — cases run
 * against the live store (for the graph-code fusion ablation). Returns the
 * number of cases loaded (>= 0), or -1 on read/parse error. No teardown needed. */
int mem_eval_load_production_corpus(const char *corpus_path, mem_eval_case_t *cases, int max_cases);

/* Resolve a code-graph-fusion ablation arm from the matrix JSON
 * (benchmarks/code-vector-graph/ablation-matrix.json) into the wired knobs:
 * *state_out ("off"/"shadow"/"on"), *utility_out, *projection_out. Leaves any
 * output the arm's config does not set untouched (caller seeds defaults).
 * Returns 0 if the named arm was found, -1 otherwise. */
int mem_eval_fusion_arm_resolve(const char *matrix_path, const char *arm, char *state_out,
                                size_t state_len, int *utility_out, int *projection_out);

/* Close the per-call scratch DB opened by mem_eval_load_corpus or
 * mem_eval_open_temp_db. Idempotent. */
void mem_eval_close_temp_db(void);

/* Run retrieval evaluation over LoCoMo. Each QA item is evaluated only against
 * turns from its own conversation. samples_out receives the number of
 * conversations evaluated when non-NULL. */
int mem_eval_run_locomo(const char *dataset_path, int max_samples, mem_eval_scores_t *out,
                        mem_eval_latency_t *latency_out, int *samples_out,
                        const char *progress_path);
int mem_eval_run_locomo_session_support(const char *dataset_path, int max_samples,
                                        mem_eval_scores_t *out, mem_eval_latency_t *latency_out,
                                        int *samples_out);
int mem_eval_run_locomo_qa(const char *dataset_path, int max_samples, int top_k, int token_budget,
                           mem_eval_qa_scores_t *out, mem_eval_latency_t *latency_out,
                           int *samples_out);
int mem_eval_report_locomo_qa_failures(const char *dataset_path, int max_samples, int top_k,
                                       int token_budget, int max_failures, FILE *out);
int mem_eval_report_locomo_misses(const char *dataset_path, int max_samples, int limit,
                                  int max_misses, FILE *out, const char *progress_path);

/* Run retrieval evaluation over LongMemEval. Each question is evaluated only
 * against sessions from its own haystack history. cases_out receives the
 * number of benchmark instances evaluated when non-NULL. */
int mem_eval_run_longmemeval(const char *dataset_path, int max_cases, mem_eval_scores_t *out,
                             mem_eval_latency_t *latency_out, int *cases_out,
                             const char *progress_path);
int mem_eval_run_longmemeval_qa(const char *dataset_path, int max_cases, int top_k,
                                int token_budget, mem_eval_qa_scores_t *out,
                                mem_eval_latency_t *latency_out, int *cases_out);
int mem_eval_report_longmemeval_qa_failures(const char *dataset_path, int max_cases, int top_k,
                                            int token_budget, int max_failures, FILE *out);
int mem_eval_report_longmemeval_misses(const char *dataset_path, int max_cases, int limit,
                                       int max_misses, FILE *out, const char *progress_path);

/* Load baseline metrics from a JSON file. Returns 0 on success, -1 on error. */
int mem_eval_load_baseline(const char *baseline_path, mem_eval_scores_t *out,
                           double *threshold_pct_out);

/* Compare scores against baseline. Returns 0 if no regression, 1 if any metric dropped
 * by more than threshold_pct percent. Prints details to stderr on regression. */
int mem_eval_check_regression(const mem_eval_scores_t *scores, const mem_eval_scores_t *baseline,
                              double threshold_pct);

/* Save scores as the new baseline JSON file. Returns 0 on success, -1 on error. */
int mem_eval_save_baseline(const char *baseline_path, const mem_eval_scores_t *scores,
                           double threshold_pct);

#endif /* DEC_AGENT_EVAL_H */
