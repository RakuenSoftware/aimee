/* db2/kind_lifecycle.h: per-kind lifecycle thresholds — DB2 subsystem.
 *
 * Reads the kind_lifecycle row that controls promotion / demotion /
 * expiry math for a given memory kind.
 *
 * The kind_lifecycle_t typedef lives in headers/memory.h. Callers must
 * include "aimee.h" (which transitively pulls memory.h) before this
 * header so the type resolves. */
#ifndef DEC_DB2_KIND_LIFECYCLE_H
#define DEC_DB2_KIND_LIFECYCLE_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Load the lifecycle thresholds for `kind` into `out`. On any miss
    * (kind not configured, db unavailable) `out` is filled with the
    * default fact-tier lifecycle and the function returns -1. Returns 0
    * when a kind_lifecycle row was found. */
   int db2_kind_lifecycle_load(const char *kind, kind_lifecycle_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_KIND_LIFECYCLE_H */
