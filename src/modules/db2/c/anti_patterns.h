/* db2/anti_patterns.h: anti-pattern catalog — DB2 subsystem.
 *
 * Anti-patterns are local heuristics learned from observed agent failures
 * and operator feedback ("never use rm -rf in /etc", etc). User-local data
 * — fits cleanly in DB1.
 *
 * The matcher is word-bounded and case-insensitive: anti_pattern_check
 * scans every row and returns those whose normalized pattern appears as a
 * word-bounded phrase in the normalized (file_path + " " + command) target. */
#ifndef DEC_DB2_ANTI_PATTERNS_H
#define DEC_DB2_ANTI_PATTERNS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      char pattern[512];
      char description[1024];
      char source[32];
      char source_ref[64];
      int hit_count;
      double confidence;
   } anti_pattern_t;

   /* Insert a new anti-pattern. Caller may pass out=NULL to skip the
    * row-back. Returns 0 on success, -1 on failure. */
   int db2_anti_pattern_insert(const char *pattern, const char *description, const char *source,
                               const char *source_ref, double confidence, anti_pattern_t *out);

   /* List all anti-patterns ordered by hit_count DESC, confidence DESC.
    * Returns count written to out (capped at max). */
   int db2_anti_pattern_list(anti_pattern_t *out, int max);

   /* Check whether (file_path + " " + command) hits any anti-pattern via
    * word-bounded phrase matching. Either file_path or command may be NULL.
    * Returns count of matching rows written to out (capped at max). */
   int db2_anti_pattern_check(const char *file_path, const char *command, anti_pattern_t *out,
                              int max);

   /* Returns 1 iff a row exists with pattern equal to the given string
    * (exact match, used by trace_mining to avoid duplicate inserts). */
   int db2_anti_pattern_exists_exact(const char *pattern);

   /* Returns 1 iff a row exists with the given source_ref (used by
    * extract-from-* loops to avoid duplicate inserts). */
   int db2_anti_pattern_exists_by_source_ref(const char *source_ref);

   /* Increment hit_count for the row with the given id. */
   int db2_anti_pattern_bump(int64_t id);

   /* Delete the row with the given id. Returns 0 on success, -1 otherwise. */
   int db2_anti_pattern_delete(int64_t id);

   /* List anti-patterns whose hit_count is at least threshold, ordered by
    * hit_count DESC. Used by the escalation pass that promotes hot anti-
    * patterns to blocking rules. Returns count written to out. */
   int db2_anti_pattern_list_hot(int threshold, anti_pattern_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ANTI_PATTERNS_H */
