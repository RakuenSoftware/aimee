/* db2/prospective_memories.h: prospective-memory storage primitives — DB2.
 *
 * Implements the SQL surface for the prospective_memories table plus
 * DB2-owned lexical trigger lookup. Higher-level matching (stem
 * extraction, dogfood logging, metrics) lives in
 * memory_prospective.c and consumes these helpers.
 *
 * The memory_prospective_t struct lives in headers/memory.h. Callers
 * include "aimee.h" first (which transitively includes memory.h) and
 * then include this header for the typed loaders.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB2_PROSPECTIVE_MEMORIES_H
#define DEC_DB2_PROSPECTIVE_MEMORIES_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Insert a new armed reminder. `recurrence` must be one of the valid
    * MEMORY_PROSPECTIVE_RECUR_* strings; caller validates. Returns the
    * new id on success, or 0 on failure. */
   int64_t db2_prospective_insert(const char *trigger_text, const char *action_text,
                                  const char *anchor_entity, const char *anchor_file,
                                  const char *recurrence, const char *valid_until,
                                  const char *source_session);

   /* Fetch one reminder by id. Returns 0 on success, -1 if not found. */
   int db2_prospective_get(int64_t id, memory_prospective_t *out);

   /* List up to `max` reminders. If `state` is non-NULL/non-empty,
    * filter to that state. Newest-first. Returns count written. */
   int db2_prospective_list(const char *state, memory_prospective_t *out, int max);

   /* Update one reminder's state to a literal string ('completed',
    * 'expired', 'triggered'). Returns 0 on success, -1 on no row updated. */
   int db2_prospective_set_state(int64_t id, const char *new_state);

   /* Sweep armed-with-elapsed-valid_until rows to 'expired'. Returns
    * count updated. */
   int db2_prospective_sweep_expired(void);

   /* Mark a trigger event for `id`. When `terminal` is non-zero, also
    * flips state to 'triggered'. Increments trigger_count and updates
    * last_triggered_at. Returns 0 on success. */
   int db2_prospective_record_trigger(int64_t id, int terminal);

   /* Stage 1 of the matcher: list armed reminders whose anchor_entity
    * matches case-insensitively. */
   int db2_prospective_list_by_entity(const char *entity_lc, memory_prospective_t *out, int max);

   /* Stage 2: anchor_file exact match. */
   int db2_prospective_list_by_file(const char *file, memory_prospective_t *out, int max);

   /* Stage 3: lexical match against trigger_text. The caller passes raw
    * turn text; DB2 owns query normalization and backend syntax. */
   int db2_prospective_list_by_trigger_terms(const char *turn_text, memory_prospective_t *out,
                                             int max);

   /* Stage 4: every armed reminder, newest-first. The caller does
    * substring/stem matching in C. */
   int db2_prospective_list_armed(memory_prospective_t *out, int max);

   /* Counts of prospective_memories rows grouped by `state`. Each
    * pointer may be NULL if the caller is not interested in that
    * bucket. Used by `aimee dashboard reminders` for at-a-glance
    * summaries. */
   void db2_prospective_count_by_state(int *armed_out, int *triggered_out, int *completed_out,
                                       int *expired_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_PROSPECTIVE_MEMORIES_H */
