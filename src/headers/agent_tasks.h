#ifndef DEC_AGENT_TASKS_H
#define DEC_AGENT_TASKS_H 1

#include "agent_types.h"

typedef struct
{
   int id;
   char action[128];
   char precondition[512];
   char success_predicate[512];
   char rollback[512];
   int depends_on[AGENT_MAX_PLAN_DEPS]; /* seq indices of prerequisite steps */
   int dep_count;
   int status; /* 0=pending, 1=running, 2=done, 3=failed, 4=rolled_back */
   char output[4096];
   int wave; /* execution wave index assigned by plan_assign_waves() */
} plan_step_t;

typedef struct
{
   int id;
   char agent_name[MAX_AGENT_NAME];
   char task[1024];
   char status[16];
   plan_step_t steps[AGENT_MAX_PLAN_STEPS];
   int step_count;
} plan_t;

typedef struct
{
   int total_steps;
   int strong_passed;
   int weak_passed;
   int failed;
} plan_verify_summary_t;

/* Assign wave indices to all steps in a plan via topological sort.
 * Steps with no dependencies get wave 0; a step's wave is 1 + the maximum
 * wave of all its dependencies.  Modifies plan->steps[i].wave in place.
 * Returns 0 on success, -1 if a dependency cycle is detected. */
int plan_assign_waves(plan_t *plan);
int agent_plan_execute(plan_t *plan, const agent_t *agent, int timeout_ms);
int agent_plan_rollback_step(plan_t *plan, int step_idx, int timeout_ms);
int agent_plan_verify(plan_t *plan, plan_verify_summary_t *summary);
int agent_execute_with_plan(const agent_t *agent, const agent_network_t *network,
                            const char *system_prompt, const char *user_prompt, int max_tokens,
                            double temperature, agent_result_t *out);

/* Durable jobs.
 *
 * agent_set_durable_job publishes the active job id to the agent runtime.
 * The runtime calls db1_agent_job_update / db1_agent_job_heartbeat
 * directly, so callers no longer hand in a DB connection. */
void agent_set_durable_job(int job_id);
int agent_get_durable_job_id(void);
int agent_job_resume(agent_config_t *cfg, int job_id, agent_result_t *out);

/* Result cache lives in src/db1/caches.h (db1_agent_cache_*). */

/* One-shot hint lookup lives in db2/agent_hints.h
 * (db2_agent_hint_find_and_consume). */

#endif /* DEC_AGENT_TASKS_H */
