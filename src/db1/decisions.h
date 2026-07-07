/* db1/decisions.h: per-window decisions audit — DB1 subsystem.
 *
 * Captures Write/Edit tool calls and other decision-like events detected
 * by memory_scan. Each row references a window_id from the `windows`
 * table. DB1 owns both sides of that relationship.
 *
 * The task-keyed `decision_log` table moved to DB2 per the DB1/DB2
 * storage split. See db2/decision_log.h.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_DECISIONS_H
#define DEC_DB1_DECISIONS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Insert a decision row. `created_at` may be NULL to default to
    * datetime('now'). Returns 0 on success, -1 on error. */
   int db1_decision_record(int64_t window_id, const char *description, const char *created_at);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DECISIONS_H */
