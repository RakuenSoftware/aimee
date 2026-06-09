/* db1/coord_jobs.h: durable per-machine coordinated job queue.
 *
 * Owns the DB1-local coordination tables used for wave execution and
 * delegate work claiming. Raw SQL for coord_jobs/coord_job_tasks stays
 * here; callers use typed helpers.
 */
#ifndef DEC_DB1_COORD_JOBS_H
#define DEC_DB1_COORD_JOBS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_COORD_STATUS_LEN   16
#define DB1_COORD_DELEGATE_LEN 64
#define DB1_COORD_TS_LEN       32
#define DB1_COORD_FILES_LEN    2048
#define DB1_COORD_ROLE_LEN     32
#define DB1_COORD_CWD_LEN      512
#define DB1_COORD_RESULT_LEN   4096
#define DB1_COORD_ERROR_LEN    1024
#define DB1_COORD_MAX_TASKS    64
#define DB1_COORD_DEFAULT_PAR  3

   typedef struct
   {
      int id;
      int job_id;
      int step_id;
      char status[DB1_COORD_STATUS_LEN];
      char claimed_by[DB1_COORD_DELEGATE_LEN];
      char claimed_at[DB1_COORD_TS_LEN];
      char files[DB1_COORD_FILES_LEN];
      char result[DB1_COORD_RESULT_LEN];
      char error[DB1_COORD_ERROR_LEN];
      int preempt_requeues;
      char created_at[DB1_COORD_TS_LEN];
   } db1_coord_task_t;

   typedef struct
   {
      int id;
      int plan_id;
      char status[DB1_COORD_STATUS_LEN];
      int max_concurrent;
      char created_at[DB1_COORD_TS_LEN];
      char updated_at[DB1_COORD_TS_LEN];
      int total_tasks;
      int done_tasks;
      int failed_tasks;
      int running_tasks;
   } db1_coord_job_t;

   int db1_coord_job_create(int plan_id, int max_concurrent);
   int db1_coord_job_add_task(int job_id, int step_id, const char *files_json, const char *role,
                              const char *prompt, const char *cwd);
   int db1_coord_job_claim_next(int job_id, const char *delegate_name, db1_coord_task_t *out);
   int db1_coord_job_complete_task(int task_id, const char *result);
   int db1_coord_job_fail_task(int task_id, const char *error);
   int db1_coord_job_release_task(int task_id);
   int db1_coord_job_release_task_bounded(int task_id, int max_requeues);
   int db1_coord_job_get(int job_id, db1_coord_job_t *out);
   int db1_coord_job_list_tasks(int job_id, db1_coord_task_t *out, int max);
   int db1_coord_job_cancel(int job_id);
   int db1_coord_job_refresh_status(int job_id);
   int db1_coord_job_has_file_conflict(int job_id, const char *files_json);
   int db1_coord_job_list_recent(db1_coord_job_t *out, int max);
   /* List IDs of coord jobs with pending or running tasks (needs dispatch work). */
   int db1_coord_job_list_active_ids(int *out_ids, int max);
   /* Read the dispatch fields (role, prompt, files, cwd) for a single task. Returns 0 on success.
    */
   int db1_coord_task_get_dispatch(int task_id, char *role_out, size_t role_cap, char *prompt_out,
                                   size_t prompt_cap, char *files_out, size_t files_cap,
                                   char *cwd_out, size_t cwd_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_COORD_JOBS_H */
