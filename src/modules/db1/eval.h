/* db1/eval.h: per-machine evaluation run results.
 *
 * eval_results records each eval task's pass/fail outcome along with
 * basic token and latency metrics. Machine-local because different
 * machines run the suite independently; sharing would require a
 * deliberate working-profile-style promotion step.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_EVAL_H
#define DEC_DB1_EVAL_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_EVAL_SUITE_LEN   64
#define DB1_EVAL_TASK_NAME   128
#define DB1_EVAL_AGENT_NAME  64
#define DB1_EVAL_ERROR_LEN   512
#define DB1_EVAL_CREATED_LEN 32

   typedef struct
   {
      const char *suite;
      const char *task_name;
      const char *agent_name;
      const char *ablation;
      int success;
      int turns;
      int tool_calls;
      int tool_call_failures;
      int rescue_recoveries;
      int prompt_tokens;
      int completion_tokens;
      int latency_ms;
      const char *response; /* may be NULL / empty */
      const char *error;    /* NULL on success */
      const char *dataset_hash;
      const char *target_hash;
      const char *harness_version;
      const char *hardware_profile;
      int seed;
   } db1_eval_result_row_t;

   /* Insert one eval_results row. Returns 0 on success, -1 on error. */
   int db1_eval_result_insert(const db1_eval_result_row_t *row);

   typedef struct
   {
      char task_name[DB1_EVAL_TASK_NAME];
      char error[DB1_EVAL_ERROR_LEN];
   } db1_eval_failed_task_t;

   /* Distinct (task_name, error) pairs from eval_results with success=0
    * in the last 7 days, newest first. Writes up to `max` into `out`.
    * Returns count written (>=0) or -1 on error. */
   int db1_eval_failed_tasks_recent(db1_eval_failed_task_t *out, int max);

   typedef struct
   {
      char task_name[DB1_EVAL_TASK_NAME];
   } db1_eval_passed_task_t;

   /* Distinct task_name from eval_results with success=1 in the last
    * 7 days, newest first. */
   int db1_eval_passed_tasks_recent(db1_eval_passed_task_t *out, int max);

   typedef struct
   {
      char suite[DB1_EVAL_SUITE_LEN];
      char task_name[DB1_EVAL_TASK_NAME];
      char agent_name[DB1_EVAL_AGENT_NAME];
      char ablation[32];
      int success;
      int turns;
      int tool_calls;
      int tool_call_failures;
      int rescue_recoveries;
      int latency_ms;
      char created_at[DB1_EVAL_CREATED_LEN];
   } db1_eval_display_row_t;

   /* List the most recent `max` rows, optionally filtered by suite
    * (pass NULL for all suites). Newest first. */
   int db1_eval_results_list(const char *suite_or_null, db1_eval_display_row_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_EVAL_H */
