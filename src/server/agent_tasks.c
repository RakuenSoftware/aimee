/* Durable job context for the active canonical execution owners. */
#include "agent_tasks.h"

static __thread int g_durable_job_id = 0;

void agent_set_durable_job(int job_id)
{
   g_durable_job_id = job_id;
}

int agent_get_durable_job_id(void)
{
   return g_durable_job_id;
}
