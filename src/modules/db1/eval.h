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
#include <stdint.h>

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

   /* --- Synthesised regression candidates (recursive-self-improvement S1) ---
    *
    * A confirmed live failure is fingerprinted and parked here in state
    * 'candidate'. Nothing runs a candidate as a gate. When the same signature
    * reproduces from distinct sessions it is admitted: materialised as an
    * ordinary suite task file and marked 'admitted'. Machine-local for the
    * same reason eval_results is — each machine observes its own failures.
    *
    * States: candidate | admitted | rejected | archived.
    * 'rejected' is terminal and permanent: it is the escape hatch for a
    * poisoned yardstick, so a rejected signature is never re-admitted even if
    * it keeps reproducing.
    *
    * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#define DB1_EVAL_CAND_SIGNATURE_LEN 33 /* 32 hex chars + NUL */
#define DB1_EVAL_CAND_STATE_LEN     16
#define DB1_EVAL_CAND_ORIGIN_LEN    32
#define DB1_EVAL_CAND_REF_LEN       128
#define DB1_EVAL_CAND_TASK_JSON_LEN 4096
#define DB1_EVAL_CAND_SESSIONS_LEN  512
#define DB1_EVAL_CAND_PATH_LEN      512
#define DB1_EVAL_CAND_REASON_LEN    256

   /* Bound on session ids retained per candidate. Admission needs only a
    * small distinct count, so the set is capped rather than unbounded. */
#define DB1_EVAL_CAND_MAX_SESSIONS 8

   typedef struct
   {
      int64_t id;
      char signature[DB1_EVAL_CAND_SIGNATURE_LEN];
      char state[DB1_EVAL_CAND_STATE_LEN];
      char suite[DB1_EVAL_SUITE_LEN];
      char task_name[DB1_EVAL_TASK_NAME];
      char task_json[DB1_EVAL_CAND_TASK_JSON_LEN];
      char origin[DB1_EVAL_CAND_ORIGIN_LEN];
      char origin_ref[DB1_EVAL_CAND_REF_LEN];
      int occurrences;
      int distinct_sessions; /* derived from the retained session set */
      char admitted_by[DB1_EVAL_CAND_ORIGIN_LEN];
      char admitted_path[DB1_EVAL_CAND_PATH_LEN];
      char reject_reason[DB1_EVAL_CAND_REASON_LEN];
      int passing_windows;
      char created_at[DB1_EVAL_CREATED_LEN];
      char updated_at[DB1_EVAL_CREATED_LEN];
   } db1_eval_candidate_t;

   /* Record one observation of `signature`. Inserts the candidate on first
    * sight; otherwise merges `session_id` into the retained set (capped at
    * DB1_EVAL_CAND_MAX_SESSIONS) and bumps occurrences.
    *
    * Observation is IDEMPOTENT PER SESSION: a session already on the row is
    * the same observation reported again — a re-run scan over the same
    * failure ledger — and bumps nothing. So `occurrences` counts distinct
    * reporters, which is what the reproduction bar is asking about. A caller
    * that passes no session id has given no identity to deduplicate on, and
    * always counts. An existing row's
    * state is never changed here — observing a rejected signature bumps its
    * counter and leaves it rejected. Read the resulting row back with
    * db1_eval_candidate_get_by_signature when it is needed. Returns 0 on
    * success, -1 on bad args / SQL error. */
   int db1_eval_candidate_observe(const char *signature, const char *suite, const char *task_name,
                                  const char *task_json, const char *origin, const char *origin_ref,
                                  const char *session_id);

   /* Fetch one candidate. Returns 1 when found, 0 when absent, -1 on error. */
   int db1_eval_candidate_get_by_signature(const char *signature, db1_eval_candidate_t *out);

   /* List candidates, optionally filtered by state (NULL / "" = all), newest
    * update first. Returns rows written (capped at max) or -1 on error. */
   int db1_eval_candidate_list(const char *state_or_null, db1_eval_candidate_t *out, int max);

   /* candidate -> admitted, recording who admitted it and where it was
    * materialised. Refuses a row that is not in state 'candidate'. */
   int db1_eval_candidate_mark_admitted(int64_t id, const char *admitted_by, const char *path);

   /* -> rejected (terminal). Permitted from any non-rejected state. */
   int db1_eval_candidate_mark_rejected(int64_t id, const char *reason);

   /* admitted -> archived, for a task that has passed long enough to retire. */
   int db1_eval_candidate_mark_archived(int64_t id);

   /* Set the consecutive-passing-window counter on an admitted row. */
   int db1_eval_candidate_set_passing_windows(int64_t id, int windows);

   /* --- Ablation grid (recursive-self-improvement S2) ---
    *
    * `aimee eval run --ablation all` already runs every task once per preset,
    * each preset removing one capability, and records the outcome. That grid is
    * a counterfactual the harness produces and nobody reads: same task, one
    * thing changed, same success check. These cells are that grid. */

#define DB1_EVAL_ABLATION_LEN 32

   typedef struct
   {
      char task_name[DB1_EVAL_TASK_NAME];
      char ablation[DB1_EVAL_ABLATION_LEN];
      int passed;
      int total;
   } db1_eval_ablation_cell_t;

   /* (task_name, ablation) cells with pass/total counts, optionally filtered by
    * suite, ordered by task then ablation so a caller can walk them grouped.
    * Returns rows written (capped at max) or -1 on error. */
   int db1_eval_ablation_grid(const char *suite_or_null, db1_eval_ablation_cell_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_EVAL_H */
