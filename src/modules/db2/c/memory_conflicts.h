/* db2/memory_conflicts.h: SQL primitives for the memory_conflicts and
 * contradiction_log tables. DB2-owned.
 *
 * The conflict_t typedef lives in headers/memory.h; callers must
 * include "aimee.h" before this header for the type to resolve
 * (same convention as kind_lifecycle.h). */
#ifndef DEC_DB2_MEMORY_CONFLICTS_H
#define DEC_DB2_MEMORY_CONFLICTS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* INSERT a new (mem_a, mem_b) row into memory_conflicts with
    * `detected_at` stamped to now and resolved=0. Returns 0 on success,
    * -1 on failure. */
   int db2_memory_conflict_record(int64_t mem_a, int64_t mem_b);

   /* INSERT a row into contradiction_log with the given resolution and
    * details (NULL details writes empty). detected_at stamps now. */
   void db2_memory_contradiction_log(int64_t mem_a, int64_t mem_b, const char *resolution,
                                     const char *details);

   /* List up to `max` unresolved conflict_t rows ordered by detected_at
    * DESC. Returns count. */
   int db2_memory_conflict_list(conflict_t *out, int max);

   /* Look up the (memory_a, memory_b) pair for a conflict id. Fills
    * *mem_a / *mem_b with 0 on miss. Returns 0 on hit, -1 on miss. */
   int db2_memory_conflict_get_pair(int64_t conflict_id, int64_t *mem_a, int64_t *mem_b);

   /* Mark a conflict as resolved with the given resolution string.
    * Returns rows changed (1 = success), 0 = no row, -1 on SQL error. */
   int db2_memory_conflict_resolve(int64_t conflict_id, const char *resolution);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_CONFLICTS_H */
