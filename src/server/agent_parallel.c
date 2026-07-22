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

#define AGENT_PARALLEL_PROCESS_MAX 64

/* Bound fan-out workers across the whole process, not once per panel. Provider
 * admission remains authoritative for actual model concurrency; this limiter
 * only prevents many simultaneous coordinators from materializing an unbounded
 * number of pthreads while they wait for those provider slots. */
static pthread_mutex_t g_parallel_permit_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_parallel_permit_cond;
static pthread_once_t g_parallel_permit_once = PTHREAD_ONCE_INIT;
static int g_parallel_permits_active = 0;

static void parallel_permit_init(void)
{
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&g_parallel_permit_cond, &attr);
   pthread_condattr_destroy(&attr);
}

static int parallel_permit_acquire(const struct timespec *deadline)
{
   pthread_once(&g_parallel_permit_once, parallel_permit_init);
   pthread_mutex_lock(&g_parallel_permit_mutex);
   while (g_parallel_permits_active >= AGENT_PARALLEL_PROCESS_MAX)
   {
      int rc = deadline ? pthread_cond_timedwait(&g_parallel_permit_cond, &g_parallel_permit_mutex,
                                                 deadline)
                        : pthread_cond_wait(&g_parallel_permit_cond, &g_parallel_permit_mutex);
      if (deadline && rc == ETIMEDOUT)
      {
         pthread_mutex_unlock(&g_parallel_permit_mutex);
         return -1;
      }
   }
   g_parallel_permits_active++;
   pthread_mutex_unlock(&g_parallel_permit_mutex);
   return 0;
}

static void parallel_permit_release(void)
{
   pthread_once(&g_parallel_permit_once, parallel_permit_init);
   pthread_mutex_lock(&g_parallel_permit_mutex);
   if (g_parallel_permits_active > 0)
      g_parallel_permits_active--;
   pthread_cond_signal(&g_parallel_permit_cond);
   pthread_mutex_unlock(&g_parallel_permit_mutex);
}

/* Completion barrier for the deadline path: workers flip their `done` flag and
 * bump `done_count` under `mtx`, signalling `cv`, so the caller can cancel
 * stragglers at the deadline and then join every admitted worker. */
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
   atomic_int cancel;
   int owns_process_permit;
} parallel_ctx_t;

static void *parallel_worker(void *arg)
{
   parallel_ctx_t *ctx = (parallel_ctx_t *)arg;
   /* This worker is a fresh thread: re-bind the dispatcher's per-turn credential
    * context (thread-locals are not inherited) so the agent resolves its
    * client-held session key instead of running keyless. */
   agent_request_creds_restore(ctx->creds);
   agent_set_request_cancel(&ctx->cancel);
   /* Per-task temperature, defaulting to the historical 0.3 when unset (0). */
   double temp = ctx->task->temperature > 0.0 ? ctx->task->temperature : 0.3;
   if (ctx->task->agent && ctx->task->agent[0])
      (ctx->task->use_tools ? agent_run_named_with_tools : agent_run_named)(
          ctx->cfg, ctx->task->agent, ctx->task->role, ctx->task->system_prompt,
          ctx->task->user_prompt, ctx->task->max_tokens, temp, ctx->result);
   else if (ctx->task->use_tools)
      agent_run_with_tools_write_enforce(ctx->cfg, ctx->task->role, ctx->task->system_prompt,
                                         ctx->task->user_prompt, ctx->task->max_tokens, 1,
                                         ctx->result);
   else
      agent_run_ex(ctx->cfg, ctx->task->role, ctx->task->system_prompt, ctx->task->user_prompt,
                   ctx->task->max_tokens, temp, ctx->result);
   agent_set_request_cancel(NULL);
   if (ctx->owns_process_permit)
   {
      ctx->owns_process_permit = 0;
      parallel_permit_release();
   }
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

/* Deadline-bounded fan-out: admit workers until the shared absolute deadline,
 * wait for completion until that same deadline, then cooperatively cancel and
 * JOIN every unfinished worker. The caller therefore owns a transitive lifetime:
 * no detached participant can outlive its task/config/result storage or the
 * server shutdown that is waiting on the coordinator. */
static int run_parallel_deadline(agent_config_t *cfg, agent_task_t *tasks, int task_count,
                                 agent_result_t *out, int deadline_ms)
{
   parallel_ctx_t *ctxs = calloc((size_t)task_count, sizeof(*ctxs));
   int *done = calloc((size_t)task_count, sizeof(int));
   int *spawned = calloc((size_t)task_count, sizeof(int));
   pthread_t *threads = calloc((size_t)task_count, sizeof(pthread_t));
   if (!ctxs || !done || !spawned || !threads)
   {
      free(ctxs);
      free(done);
      free(spawned);
      free(threads);
      return 0;
   }

   parallel_sync_t sync;
   memset(&sync, 0, sizeof(sync));
   pthread_mutex_init(&sync.mtx, NULL);
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&sync.cv, &attr);
   pthread_condattr_destroy(&attr);
   agent_request_creds_t creds;
   agent_request_creds_snapshot(&creds);

   struct timespec abs;
   clock_gettime(CLOCK_MONOTONIC, &abs);
   abs.tv_sec += deadline_ms / 1000;
   abs.tv_nsec += (long)(deadline_ms % 1000) * 1000000L;
   if (abs.tv_nsec >= 1000000000L)
   {
      abs.tv_sec++;
      abs.tv_nsec -= 1000000000L;
   }

   for (int i = 0; i < task_count; i++)
      memset(&out[i], 0, sizeof(out[i]));

   int spawned_count = 0;
   for (int i = 0; i < task_count; i++)
   {
      if (parallel_permit_acquire(&abs) != 0)
      {
         for (int j = i; j < task_count; j++)
            snprintf(out[j].error, sizeof(out[j].error),
                     "parallel deadline elapsed before worker admission");
         break;
      }
      ctxs[i].cfg = cfg;
      ctxs[i].task = &tasks[i];
      ctxs[i].result = &out[i];
      ctxs[i].creds = &creds;
      ctxs[i].sync = &sync;
      ctxs[i].done = &done[i];
      ctxs[i].owns_process_permit = 1;
      atomic_init(&ctxs[i].cancel, 0);
      if (pthread_create(&threads[i], NULL, parallel_worker, &ctxs[i]) == 0)
      {
         spawned[i] = 1;
         spawned_count++;
      }
      else
      {
         ctxs[i].owns_process_permit = 0;
         parallel_permit_release();
         snprintf(out[i].error, sizeof(out[i].error), "could not create parallel worker");
      }
   }

   pthread_mutex_lock(&sync.mtx);
   while (sync.done_count < spawned_count)
   {
      if (pthread_cond_timedwait(&sync.cv, &sync.mtx, &abs) == ETIMEDOUT)
         break;
   }
   for (int i = 0; i < task_count; i++)
      if (spawned[i] && !done[i])
         atomic_store(&ctxs[i].cancel, 1);
   pthread_mutex_unlock(&sync.mtx);

   int cancelled = 0;
   for (int i = 0; i < task_count; i++)
   {
      if (!spawned[i])
         continue;
      if (atomic_load(&ctxs[i].cancel))
         cancelled++;
      pthread_join(threads[i], NULL);
   }

   if (cancelled)
      aimee_log(LOG_WARN, "agent.parallel", "%d/%d worker(s) cancelled at the %dms deadline",
                cancelled, spawned_count, deadline_ms);
   pthread_cond_destroy(&sync.cv);
   pthread_mutex_destroy(&sync.mtx);
   free(spawned);
   free(threads);
   free(ctxs);
   free(done);

   int success = 0;
   for (int i = 0; i < task_count; i++)
      if (out[i].success)
         success++;
   return success;
}

int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int task_count,
                       agent_result_t *out, int deadline_ms)
{
   if (task_count <= 0)
      return 0;
   if (task_count == 1 && deadline_ms <= 0)
   {
      double temp = tasks[0].temperature > 0.0 ? tasks[0].temperature : 0.3;
      int rc;
      if (tasks[0].agent && tasks[0].agent[0])
         rc = (tasks[0].use_tools ? agent_run_named_with_tools : agent_run_named)(
             cfg, tasks[0].agent, tasks[0].role, tasks[0].system_prompt, tasks[0].user_prompt,
             tasks[0].max_tokens, temp, &out[0]);
      else if (tasks[0].use_tools)
         rc = agent_run_with_tools_write_enforce(cfg, tasks[0].role, tasks[0].system_prompt,
                                                 tasks[0].user_prompt, tasks[0].max_tokens, 1,
                                                 &out[0]);
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
      atomic_init(&ctxs[i].cancel, 0);
   }

   for (int i = 0; i < task_count; i++)
   {
      if (parallel_permit_acquire(NULL) != 0)
         continue;
      ctxs[i].owns_process_permit = 1;
      if (pthread_create(&threads[i], NULL, parallel_worker, &ctxs[i]) == 0)
         thread_ok[i] = 1;
      else
      {
         ctxs[i].owns_process_permit = 0;
         parallel_permit_release();
      }
   }
   for (int i = 0; i < task_count; i++)
      if (thread_ok[i])
         pthread_join(threads[i], NULL);

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
