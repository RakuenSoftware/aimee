/* db2/failed_queries.h: failed-query counter table — DB2 subsystem.
 *
 * Tracks normalized query strings that produced no confident answer
 * during retrieval. Memory-directive code uses the threshold count to
 * decide when to surface a clarifying directive. Curiosity-pivot code
 * separately reads recent rows to seed exploration.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB2_FAILED_QUERIES_H
#define DEC_DB2_FAILED_QUERIES_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Atomically increment the failure_count for `query_norm` (inserting
    * a fresh row at count=1 if absent), update last_failed_at to now,
    * and return the resulting count. Returns 0 on any error. */
   int db2_failed_query_bump(const char *query_norm);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FAILED_QUERIES_H */
