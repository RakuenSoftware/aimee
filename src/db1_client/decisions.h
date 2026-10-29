/* db1/decisions.h: per-window decisions audit — DB1 subsystem.
 *
 * Captures Write/Edit tool calls and other decision-like events detected
 * by memory_scan. Each row references a window_id from the `windows`
 * table. DB1 owns both sides of that relationship.
 *
 * The task-keyed `decision_log` table moved to KB_STORE per the DB1/KB_STORE
 * storage split. See kb_store/decision_log.h.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_DECISIONS_H
#define DEC_DB1_DECISIONS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DECISIONS_H */
