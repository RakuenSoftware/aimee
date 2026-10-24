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
   int wave; /* retained execution wave index */
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

/* Durable jobs.
 *
 * agent_set_durable_job publishes the active job id to the agent runtime.
 * The runtime calls db1_agent_job_update / db1_agent_job_heartbeat
 * directly, so callers no longer hand in a DB connection. */
void agent_set_durable_job(int job_id);
int agent_get_durable_job_id(void);

/* Result cache lives in src/db1/caches.h (db1_agent_cache_*). */

/* One-shot hint lookup lives in kb_store/agent_hints.h
 * (kb_store_agent_hint_find_and_consume). */

#endif /* DEC_AGENT_TASKS_H */
