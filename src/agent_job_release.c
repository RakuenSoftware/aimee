/* agent_job_release.c: releasing a job row the caller was handed.
 *
 * db1_agent_job_t carries two members the store allocates -- the prompt and
 * the result -- and this frees them. It is not storage: it reads no database
 * and answers no query, it simply gives back memory. That is why it lives here
 * rather than in the module. Since the row now crosses the module boundary,
 * the memory a caller frees was allocated by the client on this side, and a
 * copy of this inside the module would be freeing an allocation it never made.
 *
 * The module has no need of it: its stage releases what the domain allocated,
 * having written the values out first.
 */
#include <stdlib.h>

#include "agent_jobs.h"

void db1_agent_job_free(db1_agent_job_t *job)
{
   if (!job)
      return;
   free(job->prompt);
   free(job->result);
   job->prompt = NULL;
   job->result = NULL;
}
