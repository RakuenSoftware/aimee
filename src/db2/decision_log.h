/* db2/decision_log.h: task-keyed decision log — DB2 subsystem.
 *
 * Per the accepted three-db split proposal, decision_log lives in DB2
 * alongside notes and shareable task state. The per-window `decisions`
 * audit table remains in DB1 (db1/decisions.h).
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB2_DECISION_LOG_H
#define DEC_DB2_DECISION_LOG_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      int64_t task_id;
      char options[1024];
      char chosen[256];
      char rationale[1024];
      char assumptions[512];
      char outcome[32];
      char created_at[32];
      /* Governance decision-record fields (per-action governance audit, P1).
       * Populated by the decision write path (S5); default/empty on legacy rows. */
      char status[16];          /* active | superseded | revisit_due */
      char revisit_when[32];    /* ISO-8601 date/condition to re-review, or empty */
      int64_t supersedes_id;    /* decision_log.id this replaces, or 0 */
      char subject[256];        /* scope key: what this decision is about */
      char author[128];         /* who decided */
      int64_t linked_policy_id; /* bound policy id, or 0 */
   } db2_decision_log_row_t;

   /* Insert a task decision_log row. `created_at` may be NULL to default
    * to datetime('now'). Optionally returns the inserted row in `out`. */
   int db2_decision_log_insert(int64_t task_id, const char *options, const char *chosen,
                               const char *rationale, const char *assumptions,
                               const char *created_at, db2_decision_log_row_t *out);

   /* Load a task decision_log row by id. Returns 0 on success, -1 if the
    * row does not exist or DB2 is unavailable. */
   int db2_decision_log_get(int64_t id, db2_decision_log_row_t *out);

   /* Update the outcome for a task decision_log row. */
   int db2_decision_log_set_outcome(int64_t id, const char *outcome);

   /* List task decision_log rows, newest first. If `outcome` is NULL/empty,
    * no outcome filter is applied. `limit <= 0` means no SQL LIMIT. */
   int db2_decision_log_list(const char *outcome, int limit, db2_decision_log_row_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_DECISION_LOG_H */
