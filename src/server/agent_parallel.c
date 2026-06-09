/* agent_parallel.c: bounded parallel fan-out over agent tasks.
 *
 * Extracted from agent_runtime.c (which is pinned at the 2000-line cap). Runs N
 * agent_task_t concurrently — each task either on its named participant agent
 * (agent_run_named) or by role (agent_run_ex) — under a compute-budget thread
 * ceiling, and reports how many succeeded. Used by the MoA ensemble fan-out
 * (delegate_ensemble.c) and the sibling vote (agent_coord.c).
 */
#include "aimee.h"
#include "agent_exec.h"
#include "config.h" /* aimee_resolve_compute_threads */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   agent_config_t *cfg;
   agent_task_t *task;
   agent_result_t *result;
} parallel_ctx_t;

static void *parallel_worker(void *arg)
{
   parallel_ctx_t *ctx = (parallel_ctx_t *)arg;
   /* Per-task temperature, defaulting to the historical 0.3 when unset (0). */
   double temp = ctx->task->temperature > 0.0 ? ctx->task->temperature : 0.3;
   if (ctx->task->agent && ctx->task->agent[0])
      agent_run_named(ctx->cfg, ctx->task->agent, ctx->task->role, ctx->task->system_prompt,
                      ctx->task->user_prompt, ctx->task->max_tokens, temp, ctx->result);
   else
      agent_run_ex(ctx->cfg, ctx->task->role, ctx->task->system_prompt, ctx->task->user_prompt,
                   ctx->task->max_tokens, temp, ctx->result);
   return NULL;
}

/* Cap concurrent parallel_worker threads by the shared compute budget. An
 * explicit AIMEE_PARALLEL_MAX can still tighten the ceiling further. */
static int parallel_worker_ceiling(void)
{
   const char *env = getenv("AIMEE_PARALLEL_MAX");
   if (env && env[0])
   {
      int v = atoi(env);
      if (v >= 1)
         return v;
   }
   return aimee_resolve_compute_threads(0);
}

int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int task_count,
                       agent_result_t *out)
{
   if (task_count <= 0)
      return 0;
   if (task_count == 1)
   {
      double temp = tasks[0].temperature > 0.0 ? tasks[0].temperature : 0.3;
      int rc;
      if (tasks[0].agent && tasks[0].agent[0])
         rc = agent_run_named(cfg, tasks[0].agent, tasks[0].role, tasks[0].system_prompt,
                              tasks[0].user_prompt, tasks[0].max_tokens, temp, &out[0]);
      else
         rc = agent_run_ex(cfg, tasks[0].role, tasks[0].system_prompt, tasks[0].user_prompt,
                           tasks[0].max_tokens, temp, &out[0]);
      return rc == 0 ? 1 : 0;
   }

   parallel_ctx_t *ctxs = calloc((size_t)task_count, sizeof(parallel_ctx_t));
   if (!ctxs)
      return 0;

   pthread_t *threads = calloc((size_t)task_count, sizeof(pthread_t));
   if (!threads)
   {
      free(ctxs);
      return 0;
   }
   int *thread_ok = calloc((size_t)task_count, sizeof(int));
   if (!thread_ok)
   {
      free(threads);
      free(ctxs);
      return 0;
   }

   for (int i = 0; i < task_count; i++)
   {
      memset(&out[i], 0, sizeof(out[i]));
      ctxs[i].cfg = cfg;
      ctxs[i].task = &tasks[i];
      ctxs[i].result = &out[i];
   }

   int ceiling = parallel_worker_ceiling();
   int wave_start = 0;
   while (wave_start < task_count)
   {
      int wave_end = wave_start + ceiling;
      if (wave_end > task_count)
         wave_end = task_count;

      for (int i = wave_start; i < wave_end; i++)
      {
         if (pthread_create(&threads[i], NULL, parallel_worker, &ctxs[i]) == 0)
            thread_ok[i] = 1;
      }
      for (int i = wave_start; i < wave_end; i++)
      {
         if (thread_ok[i])
            pthread_join(threads[i], NULL);
      }
      wave_start = wave_end;
   }

   free(thread_ok);
   free(threads);
   free(ctxs);

   int success_count = 0;
   for (int i = 0; i < task_count; i++)
   {
      if (out[i].success)
         success_count++;
   }
   return success_count;
}
