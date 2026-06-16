/* agent_parallel.c: bounded parallel fan-out over agent tasks.
 *
 * Extracted from agent_runtime.c (which is pinned at the 2000-line cap). Runs N
 * agent_task_t concurrently — each task either on its named participant agent
 * (agent_run_named) or by role (agent_run_ex) — under a compute-budget thread
 * ceiling, and reports how many succeeded. Used by the MoA ensemble fan-out
 * (delegate_ensemble.c) and the sibling vote (agent_coord.c).
 */
#include "aimee.h"
#include "agent_config.h" /* agent_request_creds_t: inherit per-turn creds */
#include "agent_exec.h"
#include "config.h" /* aimee_resolve_compute_threads */
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Completion barrier for the deadline path: workers flip their `done` flag and
 * bump `done_count` under `mtx`, signalling `cv`, so the caller can
 * cond-timedwait and abandon stragglers at the deadline. */
typedef struct
{
   pthread_mutex_t mtx;
   pthread_cond_t cv;
   int done_count;
} parallel_sync_t;

typedef struct
{
   agent_config_t *cfg;
   agent_task_t *task;
   agent_result_t *result;
   const agent_request_creds_t *creds;
   /* Deadline path only (NULL otherwise): the worker signals completion here so a
    * straggler can be abandoned without blocking the join. */
   parallel_sync_t *sync;
   int *done;
} parallel_ctx_t;

static void *parallel_worker(void *arg)
{
   parallel_ctx_t *ctx = (parallel_ctx_t *)arg;
   /* This worker is a fresh thread: re-bind the dispatcher's per-turn credential
    * context (thread-locals are not inherited) so the agent resolves its
    * client-held session key instead of running keyless. */
   agent_request_creds_restore(ctx->creds);
   /* Per-task temperature, defaulting to the historical 0.3 when unset (0). */
   double temp = ctx->task->temperature > 0.0 ? ctx->task->temperature : 0.3;
   if (ctx->task->agent && ctx->task->agent[0])
      agent_run_named(ctx->cfg, ctx->task->agent, ctx->task->role, ctx->task->system_prompt,
                      ctx->task->user_prompt, ctx->task->max_tokens, temp, ctx->result);
   else
      agent_run_ex(ctx->cfg, ctx->task->role, ctx->task->system_prompt, ctx->task->user_prompt,
                   ctx->task->max_tokens, temp, ctx->result);
   if (ctx->sync)
   {
      pthread_mutex_lock(&ctx->sync->mtx);
      *ctx->done = 1;
      ctx->sync->done_count++;
      pthread_cond_broadcast(&ctx->sync->cv);
      pthread_mutex_unlock(&ctx->sync->mtx);
   }
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

/* Deadline-bounded fan-out: spawn all workers, wait on a completion barrier until
 * every spawned worker finishes OR `deadline_ms` elapses, then collect the ones
 * that finished and abandon (detach) the rest. Each worker writes into its OWN
 * heap result cell; finished cells are moved into `out`, and any still-running
 * worker keeps a private cell + shared state alive (intentionally leaked) so its
 * eventual write can never touch freed caller memory. */
static int run_parallel_deadline(agent_config_t *cfg, agent_task_t *tasks, int task_count,
                                 agent_result_t *out, int deadline_ms)
{
   parallel_sync_t *sync = calloc(1, sizeof(*sync));
   agent_request_creds_t *creds = calloc(1, sizeof(*creds));
   parallel_ctx_t *ctxs = calloc((size_t)task_count, sizeof(*ctxs));
   agent_result_t **cells = calloc((size_t)task_count, sizeof(*cells));
   int *done = calloc((size_t)task_count, sizeof(int));
   int *spawned = calloc((size_t)task_count, sizeof(int));
   pthread_t *threads = calloc((size_t)task_count, sizeof(pthread_t));
   int *final_done = calloc((size_t)task_count, sizeof(int));
   if (!sync || !creds || !ctxs || !cells || !done || !spawned || !threads || !final_done)
   {
      free(sync);
      free(creds);
      free(ctxs);
      free(cells);
      free(done);
      free(spawned);
      free(threads);
      free(final_done);
      return 0;
   }
   pthread_mutex_init(&sync->mtx, NULL);
   pthread_cond_init(&sync->cv, NULL);
   agent_request_creds_snapshot(creds);

   int spawned_count = 0;
   for (int i = 0; i < task_count; i++)
   {
      memset(&out[i], 0, sizeof(out[i]));
      cells[i] = calloc(1, sizeof(agent_result_t));
      if (!cells[i])
         continue; /* slot stays a failed result; never spawned */
      ctxs[i].cfg = cfg;
      ctxs[i].task = &tasks[i];
      ctxs[i].result = cells[i];
      ctxs[i].creds = creds;
      ctxs[i].sync = sync;
      ctxs[i].done = &done[i];
      if (pthread_create(&threads[i], NULL, parallel_worker, &ctxs[i]) == 0)
      {
         spawned[i] = 1;
         spawned_count++;
      }
      else
      {
         free(cells[i]);
         cells[i] = NULL;
      }
   }

   struct timespec abs;
   clock_gettime(CLOCK_REALTIME, &abs);
   abs.tv_sec += deadline_ms / 1000;
   abs.tv_nsec += (long)(deadline_ms % 1000) * 1000000L;
   if (abs.tv_nsec >= 1000000000L)
   {
      abs.tv_sec++;
      abs.tv_nsec -= 1000000000L;
   }

   pthread_mutex_lock(&sync->mtx);
   while (sync->done_count < spawned_count)
   {
      if (pthread_cond_timedwait(&sync->cv, &sync->mtx, &abs) == ETIMEDOUT)
         break;
   }
   /* Consistent cut: while holding mtx no worker can flip done, so a done==1 cell
    * has been fully written and is safe to move out. */
   for (int i = 0; i < task_count; i++)
      final_done[i] = done[i];
   pthread_mutex_unlock(&sync->mtx);

   int success = 0, abandoned = 0;
   for (int i = 0; i < task_count; i++)
   {
      if (!spawned[i])
         continue;
      if (final_done[i])
      {
         out[i] = *cells[i]; /* move the result (incl. the response pointer) to the caller */
         if (out[i].success)
            success++;
         free(cells[i]); /* the struct only; the response is now owned by out[i] */
         cells[i] = NULL;
      }
      else
      {
         pthread_detach(threads[i]); /* abandon: it writes its leaked cell, never out[i] */
         abandoned++;
      }
   }

   free(final_done);
   free(spawned);
   free(threads); /* pthread_t ids; detached workers don't reference this array */
   free(cells);   /* the pointer block; abandoned cell STRUCTS stay alive via ctxs[i].result */
   if (!abandoned)
   {
      pthread_mutex_destroy(&sync->mtx);
      pthread_cond_destroy(&sync->cv);
      free(sync);
      free(creds);
      free(ctxs);
      free(done);
   }
   else
   {
      /* Leak sync/creds/ctxs/done + the abandoned cell structs: the still-running
       * detached workers reference them and must not hit freed memory. Bounded
       * and rare (a hung model). */
      aimee_log(LOG_WARN, "agent.parallel",
                "%d/%d worker(s) abandoned at the %dms deadline (resources leaked)", abandoned,
                spawned_count, deadline_ms);
   }
   return success;
}

int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int task_count,
                       agent_result_t *out, int deadline_ms)
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

   if (deadline_ms > 0)
      return run_parallel_deadline(cfg, tasks, task_count, out, deadline_ms);

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

   /* Snapshot the dispatcher's per-turn credential context once; each worker
    * thread restores it (thread-locals don't cross pthread_create). */
   agent_request_creds_t creds;
   agent_request_creds_snapshot(&creds);

   for (int i = 0; i < task_count; i++)
   {
      memset(&out[i], 0, sizeof(out[i]));
      ctxs[i].cfg = cfg;
      ctxs[i].task = &tasks[i];
      ctxs[i].result = &out[i];
      ctxs[i].creds = &creds;
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
