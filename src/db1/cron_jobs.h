/* db1/cron_jobs.h: cron_jobs and cron_job_runs DB1 access layer. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* ------------------------------------------------------------------ */
   /* cron_jobs table                                                    */
   /* ------------------------------------------------------------------ */

   /* Cron job execution mode. */
   typedef enum
   {
      CRON_MODE_LLM = 0,
      CRON_MODE_SCRIPT = 1,
      CRON_MODE_HYBRID = 2,
   } cron_job_mode_t;

   /* Per-job delivery target, stored as JSON. */
   typedef struct
   {
      char target[256];
      int only_if_changed;
      int first_run_silent;
   } cron_job_deliver_t;

   typedef struct
   {
      char id[64];
      char schedule[256];
      cron_job_mode_t mode;
      char script[8192];
      char prompt[8192];
      char skills_csv[512];
      char workdir[512];
      char deliver_target[256];
      int deliver_only_if_changed;
      int deliver_first_run_silent;
      char context_from[64];
      char when_context_contains[512];
      int pre_wake_gate;
      int enabled;
      int64_t next_run_at;
      int64_t last_run_at;
      char last_run_status[32];
      char last_run_output_hash[64];
      int64_t created_at;
   } db1_cron_job_t;

   /* Upsert a cron job. Returns 0 on success. id is required; created_at is
    * set if this is a new insert. */
   int db1_cron_job_upsert(const db1_cron_job_t *job);

   /* Load a cron job by id. Returns 0 on success, -1 if not found. */
   int db1_cron_job_get(const char *id, db1_cron_job_t *out);

   /* List all cron jobs. out_count receives the number of entries written.
    * Pass NULL for out to count only. Returns 0 on success. */
   int db1_cron_job_list(db1_cron_job_t *out, int max, int *out_count);

   /* Delete a cron job by id. Returns 0 on success. */
   int db1_cron_job_delete(const char *id);

   /* Update next_run_at and/or last_run_at for a job. Pass -1 to skip a field. */
   int db1_cron_job_touch(const char *id, int64_t next_run_at, int64_t last_run_at);

   /* Update last_run fields after a run completes. */
   int db1_cron_job_record_run(const char *id, const char *status, const char *output_hash);

   /* ------------------------------------------------------------------ */
   /* cron_job_runs table                                                */
   /* ------------------------------------------------------------------ */

   typedef struct
   {
      int64_t id;
      char job_id[64];
      int64_t started_at;
      int64_t completed_at;
      char status[32];
      int silent;
      int delivered;
      char output[16384];
      char error[1024];
   } db1_cron_job_run_t;

   /* Insert a new run row and return its integer id in *out_id (or -1 if not
    * applicable). Returns 0 on success. */
   int db1_cron_job_run_insert(const char *job_id, int64_t started_at, int64_t *out_id);

   /* Update a run row on completion. */
   int db1_cron_job_run_complete(int64_t run_id, int64_t completed_at, const char *status,
                                 int silent, const char *output, const char *error);

   /* Mark a run as delivered. */
   int db1_cron_job_run_mark_delivered(int64_t run_id);

   /* Get the most recent run for a job. Returns 0 on success, -1 if none. */
   int db1_cron_job_run_last(const char *job_id, db1_cron_job_run_t *out);

   /* List recent runs for a job (most recent first). Pass 0 for limit to get
    * all. Returns 0 on success, -1 on error. */
   int db1_cron_job_run_history(const char *job_id, int limit, db1_cron_job_run_t *out, int max_out,
                                int *out_count);

#ifdef __cplusplus
}
#endif
