/* db2/corpus_jobs.h: staged corpus processing pipeline state. */
#ifndef DEC_DB2_CORPUS_JOBS_H
#define DEC_DB2_CORPUS_JOBS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CORPUS_PIPELINE_STAGE_LEN  32
#define CORPUS_PIPELINE_STATUS_LEN 16
#define CORPUS_PIPELINE_ERROR_LEN  256

   typedef struct
   {
      int64_t doc_id;
      char content_hash[65];
      char stage[CORPUS_PIPELINE_STAGE_LEN];
      char stage_status[CORPUS_PIPELINE_STATUS_LEN];
      int attempts;
      char last_error[CORPUS_PIPELINE_ERROR_LEN];
      char updated_at[32];
   } db2_corpus_job_t;

   typedef struct
   {
      int total;
      int pending;
      int running;
      int failed;
      int complete;
      int processed;
   } db2_corpus_pipeline_stats_t;

   typedef struct
   {
      char stage[CORPUS_PIPELINE_STAGE_LEN];
      char stage_status[CORPUS_PIPELINE_STATUS_LEN];
      int count;
   } db2_corpus_pipeline_stage_count_t;

   const char *db2_corpus_pipeline_next_stage(const char *stage);
   int db2_corpus_job_seed_doc(int64_t doc_id, const char *content_hash);
   int db2_corpus_job_record_version(int64_t doc_id, const char *scope, const char *filename,
                                     const char *content_hash);
   int db2_corpus_job_get(int64_t doc_id, db2_corpus_job_t *out);
   int db2_corpus_job_advance(int64_t doc_id, const char *outcome, const char *detail);
   int db2_corpus_job_mark_restoration_candidate(int64_t doc_id, const char *content_hash,
                                                 const char *signals_json);
   int db2_corpus_job_fail(int64_t doc_id, const char *error);
   int db2_corpus_job_recover_running(int stale_seconds);
   int db2_corpus_pipeline_status(db2_corpus_pipeline_stats_t *out);
   int db2_corpus_pipeline_stage_counts(db2_corpus_pipeline_stage_count_t *out, int max_out);
   int db2_corpus_pipeline_drain(int limit, db2_corpus_pipeline_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CORPUS_JOBS_H */
