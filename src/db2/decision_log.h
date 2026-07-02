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
       * Populated by the decision write path (S5). On legacy/existing rows the
       * ALTER DEFAULTs apply: status='active', the rest empty/0. */
      char status[24];          /* active | superseded | revisit_due (headroom) */
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

   /* Record a governance decision (P1). Writes an `active` decision for `subject`
    * (scope key) with the given rationale/author/policy/revisit, and — atomically
    * in one transaction — flips the decision `supersedes_id` (if >0) to
    * `superseded`. The one-active-per-scope invariant is enforced by the DB
    * (idx_dl_active_scope): a second active decision for the same
    * (subject, linked_policy_id) is rejected. Returns 0 on success (row in `out`
    * if non-NULL), -1 on any failure (including the invariant rejection), with
    * the transaction rolled back so no partial write survives. */
   int db2_decision_log_record(const char *subject, const char *options, const char *chosen,
                               const char *rationale, const char *author, int64_t linked_policy_id,
                               const char *revisit_when, int64_t supersedes_id,
                               db2_decision_log_row_t *out);

   /* Load a task decision_log row by id. Returns 0 on success, -1 if the
    * row does not exist or DB2 is unavailable. */
   int db2_decision_log_get(int64_t id, db2_decision_log_row_t *out);

   /* Flip active decisions whose revisit_when has elapsed to 'revisit_due' so
    * they resurface for review (P1). Idempotent: only active rows with a due,
    * non-empty revisit_when are touched, compared lexicographically against the
    * current time (ISO-8601). Returns the number flipped this call, or -1 on
    * error. Reuses the existing curator drain poll — no new scheduler. */
   int db2_decision_log_mark_revisit_due(void);

   /* Update the outcome for a task decision_log row. */
   int db2_decision_log_set_outcome(int64_t id, const char *outcome);

   /* List task decision_log rows, newest first. If `outcome` is NULL/empty,
    * no outcome filter is applied. `limit <= 0` means no SQL LIMIT. */
   int db2_decision_log_list(const char *outcome, int limit, db2_decision_log_row_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_DECISION_LOG_H */
