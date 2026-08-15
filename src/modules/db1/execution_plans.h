/* db1/execution_plans.h: durable per-machine execution plans, steps, and evidence.
 *
 * Owns the DB1-local plan graph used by agent planning, replay, cancel,
 * dashboard summaries, and wave execution. Raw SQL for execution_plans,
 * plan_steps, and step_evidence stays inside this subsystem.
 */
#ifndef DEC_DB1_EXECUTION_PLANS_H
#define DEC_DB1_EXECUTION_PLANS_H 1

#include "aimee.h"
#include "agent_tasks.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_PLAN_STATUS_LEN        16
#define DB1_PLAN_AGENT_NAME_LEN    MAX_AGENT_NAME
#define DB1_PLAN_TASK_LEN          1024
#define DB1_PLAN_TS_LEN            32
#define DB1_PLAN_STEP_ACTION_LEN   128
#define DB1_PLAN_STEP_TEXT_LEN     512
#define DB1_PLAN_STEP_OUTPUT_LEN   4096
#define DB1_PLAN_DEPS_LEN          256
#define DB1_PLAN_EVIDENCE_KIND_LEN 64
#define DB1_PLAN_EVIDENCE_TEXT_LEN 4096
#define DB1_PLAN_EVIDENCE_STR_LEN  16

   typedef struct
   {
      int id;
      char agent_name[DB1_PLAN_AGENT_NAME_LEN];
      char task[DB1_PLAN_TASK_LEN];
      char status[DB1_PLAN_STATUS_LEN];
      char created_at[DB1_PLAN_TS_LEN];
      int total_steps;
      int done_steps;
   } db1_execution_plan_summary_t;

   typedef struct
   {
      char strength[DB1_PLAN_EVIDENCE_STR_LEN];
      int passed;
      char kind[DB1_PLAN_EVIDENCE_KIND_LEN];
      char created_at[DB1_PLAN_TS_LEN];
   } db1_step_evidence_latest_t;

   int db1_execution_plan_create(const char *agent_name, const char *task, const cJSON *steps_json);
   int db1_execution_plan_get(int plan_id, plan_t *out);
   int db1_execution_plan_list(plan_t *out, int max);
   int db1_execution_plan_exists(int plan_id);
   int db1_execution_plan_count_steps(int plan_id);
   int db1_execution_plan_list_running_ids(int *out_ids, int max);
   int db1_execution_plan_list_recent_summaries(db1_execution_plan_summary_t *out, int max);
   int db1_execution_plan_set_status(int plan_id, const char *status);
   int db1_execution_plan_cancel_by_id(int plan_id, const char *reason);
   int db1_execution_plan_cancel_stale(int threshold_seconds, const char *reason);

   int db1_plan_step_set_status(int step_id, const char *status);
   int db1_plan_step_set_status_output(int step_id, const char *status, const char *output);
   int db1_plan_step_cancel_active_for_plan(int plan_id);
   int db1_plan_step_cancel_orphans(void);

   int db1_step_evidence_insert(int plan_id, int step_id, const char *kind, const char *content,
                                int passed, const char *strength);
   int db1_step_evidence_get_latest(int step_id, db1_step_evidence_latest_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_EXECUTION_PLANS_H */
