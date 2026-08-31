/* db1/cognify_jobs.h: per-machine cognification job queue.
 *
 * This queue tracks local background cognify work for shared memories.
 * The queue rows are DB1-owned because claiming, retrying, and failure
 * bookkeeping are machine-local runtime concerns even when the memory
 * content itself lives in DB2.
 */
#ifndef DEC_DB1_COGNIFY_JOBS_H
#define DEC_DB1_COGNIFY_JOBS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int pending;
      int running;
      int done;
      int failed;
      int total;
   } db1_cognify_job_stats_t;

   typedef struct
   {
      int64_t id;
      int64_t memory_id;
      int attempts;
      int max_attempts;
      char kind[32];
      char status[16];
      char claimed_by[64];
      char claimed_at[32];
      char last_error[256];
   } db1_cognify_job_t;

   int db1_cognify_job_enqueue(int64_t memory_id);
   int db1_cognify_job_status(db1_cognify_job_stats_t *out);
   int db1_cognify_job_claim_next(db1_cognify_job_t *out);
   int db1_cognify_job_mark(int64_t job_id, const char *status, const char *error);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_COGNIFY_JOBS_H */
