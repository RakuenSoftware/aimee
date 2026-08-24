/* db1/pipelines.h: durable per-machine autopilot pipeline state.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_PIPELINES_H
#define DEC_DB1_PIPELINES_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_PIPE_TASK_LEN   1024
#define DB1_PIPE_STATUS_LEN 32
#define DB1_PIPE_PHASE_LEN  32
#define DB1_PIPE_CLASS_LEN  16
#define DB1_PIPE_TS_LEN     32

   typedef struct
   {
      int id;
      char task[DB1_PIPE_TASK_LEN];
      char status[DB1_PIPE_STATUS_LEN];
      char current_phase[DB1_PIPE_PHASE_LEN];
      char request_classification[DB1_PIPE_CLASS_LEN];
      char plan_depth[DB1_PIPE_CLASS_LEN];
      int phase_attempts;
      int plan_id;
      int job_id;
      int clarify_session_id;
      char created_at[DB1_PIPE_TS_LEN];
      char updated_at[DB1_PIPE_TS_LEN];
   } db1_pipeline_t;

   int db1_pipeline_create(const char *task, const char *request_classification,
                           const char *plan_depth, int *out_id);
   int db1_pipeline_get(int pipeline_id, db1_pipeline_t *out);
   int db1_pipeline_update(int pipeline_id, const char *status, const char *current_phase,
                           int phase_attempts, int plan_id, int job_id,
                           const char *request_classification, const char *plan_depth,
                           int clarify_session_id);
   int db1_pipeline_link_plan(int pipeline_id, int plan_id);
   int db1_pipeline_link_job(int pipeline_id, int job_id);
   int db1_pipeline_cancel(int pipeline_id);
   int db1_pipeline_list_active(db1_pipeline_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_PIPELINES_H */
