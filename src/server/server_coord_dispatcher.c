/* server_coord_dispatcher.c: background thread that drives coord job execution.
 *
 * When delegate.launch creates a coord job the tasks sit in DB1 with status
 * 'pending'.  This dispatcher wakes on notification (new job created or task
 * finished) and on a periodic fallback tick, claims available tasks up to
 * each job's max_concurrent limit, and submits them as delegate workers via
 * server_compute_dispatch_coord_task. */

#include "server_coord_dispatcher.h"
#include "server.h"
#include "server_compute_impl.h"
#include "gw_orch_delegates.h"
#include "db1.h"
#include "log.h"
#include "cJSON.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Coord-task fields the spawn adapter needs beyond (role, brief). Carried as the orchestration
 * capability's opaque ctx; the adapter records the spawn outcome in spawn_rc so the dispatcher
 * can tell an admitted spawn from a refused one (ceiling/pool) and release+retry accordingly. */
typedef struct
{
   server_ctx_t *server;
   int task_id;
   const char *persona;
   const char *files_json;
   const char *cwd;
   int require_handoff;
   int spawn_rc; /* 0 spawned, -1 refused/failed; -1 until the adapter runs */
} coord_spawn_backing_t;

/* The spawn_delegate capability for coord tasks: build the delegate request + compute ctx from
 * (role, brief) and the backing, then submit to an on-demand delegate worker. Returns 0/-1 and
 * also records it in the backing so the dispatcher can release+retry a refused claim. */
static int coord_spawn_delegate(void *ctx, const char *role, const char *brief)
{
   coord_spawn_backing_t *b = (coord_spawn_backing_t *)ctx;
   if (!b)
      return -1;
   b->spawn_rc = -1;
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "role", role && role[0] ? role : "execute");
   cJSON_AddStringToObject(req, "prompt", brief ? brief : "");
   /* Persona (the delegate's identity, required for every delegate) is carried on
    * the coord task now — the orchestrator that enqueued it (coord planner or the
    * workflow engine) named it. Default to engineer for legacy rows. */
   cJSON_AddStringToObject(req, "persona", (b->persona && b->persona[0]) ? b->persona : "engineer");
   /* The structured delegate_result_v1 handoff is the patch-COORDINATOR's contract
    * (plan-based jobs aggregate structured results). WFE's ad-hoc jobs don't use
    * it — the delegate produces free-form output (a plan, a review verdict, code)
    * that WFE reads raw from the task result — so requiring a handoff would fail
    * every WFE task with "invalid delegate handoff". The caller decides. */
   if (b->require_handoff)
      cJSON_AddTrueToObject(req, "handoff_json");
   if (b->files_json && b->files_json[0])
      cJSON_AddStringToObject(req, "files", b->files_json);
   if (b->cwd && b->cwd[0])
      cJSON_AddStringToObject(req, "cwd", b->cwd);
   compute_ctx_t *cctx = calloc(1, sizeof(compute_ctx_t));
   if (!cctx)
   {
      cJSON_Delete(req);
      return -1;
   }
   cctx->server = b->server;
   cctx->conn_fd = -1;
   cctx->async_slot = -1;
   cctx->coord_task_id = b->task_id;
   cctx->req = req;
   /* Coord-task delegates are I/O-bound; run on-demand, not the CPU compute
    * pool (see delegate_spawn_ondemand). */
   if (delegate_spawn_ondemand(cctx) != 0)
   {
      compute_ctx_free(cctx);
      b->spawn_rc = -1;
      return -1;
   }
   b->spawn_rc = 0;
   return 0;
}

/* Build a delegate request from a claimed coord task and submit it to a delegate worker,
 * routed through the DELEGATES orchestration module so the spawn is a togglable, registered
 * hook rather than an inline imperative call (see gw_orch_delegates). Lives here (its only
 * caller) rather than in server_compute.c. */
int server_compute_dispatch_coord_task(server_ctx_t *ctx, int task_id, const char *role,
                                       const char *prompt, const char *files_json, const char *cwd,
                                       const char *persona, int require_handoff)
{
   if (!ctx || task_id <= 0 || !prompt || !prompt[0])
      return -1;
   coord_spawn_backing_t backing = {ctx, task_id, persona, files_json, cwd, require_handoff, -1};
   gw_turn_capabilities_t caps = {&backing, coord_spawn_delegate, NULL};
   char turn_id[48];
   snprintf(turn_id, sizeof(turn_id), "coord-task-%d", task_id);
   if (gw_orch_delegates_run(&caps, turn_id, role, prompt) != 0)
      return -1; /* delegates module disabled -> not dispatched; caller releases + retries */
   return backing.spawn_rc; /* 0 spawned, -1 refused (ceiling/pool full) */
}

#define COORD_POLL_INTERVAL_SECS 10
#define COORD_MAX_ACTIVE_JOBS    32
#define COORD_PROMPT_MAX         16384
#define COORD_FILES_MAX          DB1_COORD_FILES_LEN
#define COORD_CWD_MAX            DB1_COORD_CWD_LEN
#define COORD_ROLE_MAX           DB1_COORD_ROLE_LEN

static pthread_t g_thread;
static volatile int g_running;
static volatile int g_stop;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static server_ctx_t *g_ctx;

/* Sweep all coord jobs that have pending tasks and dispatch up to max_concurrent. */
static int dispatcher_sweep(void)
{
   /* Delegates module disabled -> dispatch nothing: do NOT claim tasks (claiming then failing
    * to dispatch would release them and re-claim every poll tick, churning the queue). Coord
    * tasks ARE delegate work, so they simply wait until the module is re-enabled. Checked here,
    * before any claim, so a disabled module is distinct from a per-task 'refused' (pool full),
    * which alone releases+retries the single claimed task below. */
   if (!gw_orch_delegates_enabled())
      return 0;

   int job_ids[COORD_MAX_ACTIVE_JOBS];
   int njobs = db1_coord_job_list_active_ids(job_ids, COORD_MAX_ACTIVE_JOBS);
   if (njobs <= 0)
      return 0;

   int dispatched = 0;
   for (int i = 0; i < njobs; i++)
   {
      int job_id = job_ids[i];

      db1_coord_job_t job;
      if (db1_coord_job_get(job_id, &job) != 0)
         continue;

      int slots = job.max_concurrent - job.running_tasks;
      if (slots <= 0)
         continue;

      for (int s = 0; s < slots; s++)
      {
         db1_coord_task_t task;
         memset(&task, 0, sizeof(task));
         int task_id = db1_coord_job_claim_next(job_id, "coord-dispatcher", &task);
         if (task_id < 0)
            break; /* no more claimable tasks for this job */

         char role[COORD_ROLE_MAX] = "";
         char prompt[COORD_PROMPT_MAX] = "";
         char files[COORD_FILES_MAX] = "";
         char cwd[COORD_CWD_MAX] = "";
         char persona[COORD_ROLE_MAX] = "";
         if (db1_coord_task_get_dispatch(task_id, role, sizeof(role), prompt, sizeof(prompt), files,
                                         sizeof(files), cwd, sizeof(cwd), persona,
                                         sizeof(persona)) != 0)
         {
            db1_coord_job_fail_task(task_id, "dispatcher could not read task dispatch fields");
            continue;
         }

         if (!prompt[0])
         {
            db1_coord_job_fail_task(task_id, "task has no stored prompt; cannot dispatch");
            LOG_WARN("coord_dispatcher", "coord task #%d (job #%d) has empty prompt — failing task",
                     task_id, job_id);
            continue;
         }

         /* Plan-based coord jobs use the structured handoff contract; an ad-hoc
          * WFE job (WFE_COORD_PLAN_ID) does not — its delegate returns free-form
          * output that the workflow engine reads raw. */
         int require_handoff = (job.plan_id != WFE_COORD_PLAN_ID);
         if (server_compute_dispatch_coord_task(g_ctx, task_id, role, prompt, files, cwd, persona,
                                                require_handoff) != 0)
         {
            /* Pool full — release the claim and retry next sweep. */
            db1_coord_job_release_task(task_id);
            LOG_INFO("coord_dispatcher", "coord task #%d (job #%d): compute pool full, will retry",
                     task_id, job_id);
            break;
         }

         LOG_INFO("coord_dispatcher", "dispatched coord task #%d (job #%d, role=%s)", task_id,
                  job_id, role[0] ? role : "execute");
         dispatched++;
      }
   }
   return dispatched;
}

static void *dispatcher_thread(void *arg)
{
   (void)arg;
   while (!g_stop)
   {
      dispatcher_sweep();

      /* Wait for a notification or the fallback poll interval. */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += COORD_POLL_INTERVAL_SECS;

      pthread_mutex_lock(&g_mutex);
      if (!g_stop)
         pthread_cond_timedwait(&g_cond, &g_mutex, &ts);
      pthread_mutex_unlock(&g_mutex);
   }
   return NULL;
}

void server_coord_dispatcher_init(server_ctx_t *ctx)
{
   if (g_running)
      return;
   g_ctx = ctx;
   g_stop = 0;
   if (pthread_create(&g_thread, NULL, dispatcher_thread, NULL) != 0)
   {
      LOG_ERROR("coord_dispatcher", "failed to start dispatcher thread");
      return;
   }
   g_running = 1;
   LOG_INFO("coord_dispatcher", "started (poll=%ds)", COORD_POLL_INTERVAL_SECS);
}

void server_coord_dispatcher_shutdown(void)
{
   if (!g_running)
      return;
   g_stop = 1;
   server_coord_dispatcher_notify();
   pthread_join(g_thread, NULL);
   g_running = 0;
}

void server_coord_dispatcher_notify(void)
{
   pthread_mutex_lock(&g_mutex);
   pthread_cond_signal(&g_cond);
   pthread_mutex_unlock(&g_mutex);
}
