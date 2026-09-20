/* db2/memory_lifecycle.h: lifecycle-state SQL primitives for the memories
 * table (and memory_conflicts for the alert bundle). DB2-owned.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB2_MEMORY_LIFECYCLE_H
#define DEC_DB2_MEMORY_LIFECYCLE_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Was this memory true at `as_of`, in EVENT time?
    *
    * lifecycle_state answers "is this true now" and nothing else -- a superseded
    * row looks identically superseded whether it stopped being true yesterday or
    * last year. This reads the valid_from/valid_until interval instead, so
    * "what did we believe on 12 June" is answerable for rows the way it already
    * is for relations (memory_relations.valid_at/invalid_at, --as-of).
    *
    * `as_of` is an ISO-8601 timestamp. Returns 1 when the row was in force,
    * 0 when it was not, -1 on a bad call or no connection.
    *
    * An empty valid_from means "as far back as we know" and an empty valid_until
    * means "still true": absent bounds are open, never closed. A row written
    * before supersession started stamping valid_until therefore still reads as
    * current, which is the truthful answer for it -- we do not know when it
    * stopped being true, and inventing a boundary would be worse than admitting
    * the interval is open. */
   int db2_memory_valid_at(int64_t memory_id, const char *as_of);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_LIFECYCLE_H */
