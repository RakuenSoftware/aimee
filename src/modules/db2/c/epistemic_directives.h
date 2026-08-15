/* db2/epistemic_directives.h: storage primitives for epistemic_directives
 * — DB2 subsystem.
 *
 * The high-level orchestration (validation, dedup logic, metrics, dogfood
 * logging, FTS-clause building) lives in memory_directives.c and consumes
 * these helpers.
 *
 * The memory_directive_t typedef lives in headers/memory.h. Callers must
 * include "aimee.h" (which transitively pulls memory.h) before this
 * header so the type resolves. */
#ifndef DEC_DB2_EPISTEMIC_DIRECTIVES_H
#define DEC_DB2_EPISTEMIC_DIRECTIVES_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* INSERT … ON CONFLICT DO NOTHING on epistemic_directives. The
    * partial unique indexes dedup by (cause, topic) and by
    * (cause='contradiction', memory pair). On insert success,
    * *out_id is set to the new id and *out_existed to 0. On dedup
    * short-circuit, *out_existed is set to 1 and *out_id is set to
    * the existing row's id (looked up by the natural-key columns
    * cause/topic/question). Returns 0 on success, -1 on error. */
   int db2_directive_insert_ignore(const char *question, const char *topic,
                                   const char *anchor_entity, const char *anchor_file,
                                   const char *cause, int priority, int64_t memory_a_id,
                                   int64_t memory_b_id, const char *evidence,
                                   const char *source_session, const char *valid_until,
                                   int64_t *out_id, int *out_existed);

   /* Look up a directive by id. Returns 0 on hit, -1 on miss. */
   int db2_directive_get(int64_t id, memory_directive_t *out);

   /* Look up the first directive matching (cause, topic) for dedup recovery.
    * Returns 0 on hit, -1 on miss. */
   int db2_directive_find_by_cause_topic(const char *cause, const char *topic,
                                         memory_directive_t *out);

   /* List directives with optional state and cause filters (NULL/empty =
    * "no filter"). Ordered priority DESC, created DESC. Returns count. */
   int db2_directive_list(const char *state, const char *cause, memory_directive_t *out, int max);

   /* Per-state counts of all rows. Returns 0 on success. */
   int db2_directive_counts_by_state(int64_t *open, int64_t *suppressed, int64_t *resolved,
                                     int64_t *expired);

   /* State transition helpers. Each returns 0 if a row was updated, -1
    * otherwise (row missing or already in a non-`open` state for the
    * resolve/suppress paths). */
   int db2_directive_resolve(int64_t id, int64_t resolution_memory_id);
   int db2_directive_suppress(int64_t id);

   /* Sweep open rows whose valid_until has passed into 'expired'.
    * Returns count updated. */
   int db2_directive_sweep_expired(void);

   /* Increment surfaced_count and last_surfaced_at on an open row.
    * Returns 0 if a row was updated. */
   int db2_directive_record_surface(int64_t id);

   /* Stage 1 of the matcher: anchor_entity case-insensitive match. */
   int db2_directive_match_by_entity(const char *entity_lc, memory_directive_t *out, int max);

   /* Stage 2: anchor_file exact match. */
   int db2_directive_match_by_file(const char *file, memory_directive_t *out, int max);

   /* Stage 3: token-OR ILIKE scan over question+topic (caller provides
    * the legacy FTS5-style OR clause; we parse tokens out of it). This
    * is transitional — a pgvector directive index is the proper fix. */
   int db2_directive_match_by_lexical(const char *match_clause, memory_directive_t *out, int max);

   /* Resolve open contradiction directives that link the given memory pair.
    * Idempotent. */
   int db2_directive_resolve_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                           int64_t resolution_memory_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_EPISTEMIC_DIRECTIVES_H */
