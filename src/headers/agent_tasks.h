#ifndef DEC_AGENT_TASKS_H
#define DEC_AGENT_TASKS_H 1

#include "agent_types.h"
#include "wire_fence.h"

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
 * Its retained progress owner serializes transport counts and heartbeat writes. */
void agent_set_durable_job(int job_id);
int agent_get_durable_job_id(void);

/* One actual job owns progress across dispatch retries and joined callbacks.
 * begin requires the persisted counter and a held owner before provider work.
 * The legacy setter preserves its prior binding if a new owner cannot be held;
 * shipping positive job starts use checked begin. */
typedef struct agent_job_progress agent_job_progress_t;
int agent_durable_job_begin(int job_id);
agent_job_progress_t *agent_job_progress_capture(void); /* retained, may be NULL */
void agent_job_progress_release(agent_job_progress_t *progress);
/* bind retains the supplied owner and transfers the old TLS reference out.
 * restore consumes that reference after releasing the temporary binding. */
typedef struct
{
   agent_job_progress_t *previous;
   wire_fence_transport_observer_t observer, previous_transport;
   int transport_bound;
} agent_job_progress_binding_t;
agent_job_progress_binding_t agent_job_progress_bind(agent_job_progress_t *progress);
void agent_job_progress_restore(agent_job_progress_binding_t previous);
void agent_job_progress_heartbeat(const char *current_tool);
void agent_job_progress_observe_transport(int attempts);
int agent_job_progress_transport_attempts(void);

/* Result cache lives in src/db1/caches.h (db1_agent_cache_*). */

/* One-shot hint lookup lives in kb_store/agent_hints.h
 * (kb_store_agent_hint_find_and_consume). */

#endif /* DEC_AGENT_TASKS_H */
