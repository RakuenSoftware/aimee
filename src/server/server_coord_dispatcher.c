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
#include <aimee/delegates/gw_orch_delegates.h>
#include <aimee/delegates/delegate_role.h> /* delegate_role_is_write — force tools for coord write tasks */
#include "config.h"
#include "db1_client/db1.h"
#include "log.h"
#include "platform_random.h"
#include "cJSON.h"

#include <pthread.h>
#include <stdatomic.h>
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
   const char *claim_owner;
   const char *persona;
   const char *files_json;
   const char *cwd;
   int require_handoff;
   int spawn_rc; /* 0 spawned, -1 refused/failed; -1 until the adapter runs */
} coord_spawn_backing_t;

/* Coord write tasks are unattended and may otherwise inherit an unlimited code
 * role. Forty-eight turns leaves headroom above successful live runs while
 * preventing a confused model from continuing indefinitely after useful work. */
#define COORD_WRITE_MAX_TURNS 48

/* The spawn_delegate capability for coord tasks: build the delegate request + compute ctx from
 * (role, brief) and the backing, then submit to an on-demand delegate worker. Returns 0/-1 and
 * also records it in the backing so the dispatcher can release+retry a refused claim. */
static int coord_spawn_delegate(void *ctx, const char *role, const aimee_message_t *message)
{
   coord_spawn_backing_t *b = (coord_spawn_backing_t *)ctx;
   if (!b)
      return -1;
   b->spawn_rc = -1;
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   const char *req_role = role && role[0] ? role : "execute";
   cJSON_AddStringToObject(req, "role", req_role);
   /* A coord WRITE task (code/refactor/...) is agentic worktree work: it must edit
    * files IN PLACE via the Write/Edit tools. Force tools on. Unlike the CLI
    * single-shot `aimee delegate code` (which returns a diff as text for the caller
    * to apply), a coord write delegate has no one to apply a text diff — so without
    * tools it mutates nothing, the run is flagged "produced no diff", and a WFE
    * implement slice loops to max_iters and never lands a change. Forcing tools
    * also makes the router require a tool-capable model (MODEL_CAP_TOOLS). Read
    * roles already opt in through their `tools` permission; a caller can
    * still pass tools:false explicitly, which server_compute honors. */
   if (delegate_role_is_write(req_role))
   {
      cJSON_AddTrueToObject(req, "tools");
      cJSON_AddNumberToObject(req, "max_turns_cap", COORD_WRITE_MAX_TURNS);
   }
   cJSON *input = aimee_ir_message_json(message);
   if (!input || !cJSON_AddItemToObject(req, "message", input))
   {
      cJSON_Delete(input);
      cJSON_Delete(req);
      return -1;
   }
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
   snprintf(cctx->coord_claim_owner, sizeof(cctx->coord_claim_owner), "%s",
            b->claim_owner ? b->claim_owner : "");
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

/* Resolve whether the delegates module is enabled: the config-store `modules.delegates` toggle
 * (canonical), falling back to the deprecated env default. Loaded per call (mtime-cached), so
 * an operator toggle takes effect on the next sweep without a restart. Keeps the pure
 * gw_orch_delegates module config-free. */
static int coord_delegates_enabled(void)
{
   int tri = config_present() ? config_module_delegates() : -1;
   return config_module_enabled(tri, gw_orch_delegates_enabled());
}

/* Build a delegate request from a claimed coord task and submit it to a delegate worker,
 * routed through the DELEGATES orchestration module so the spawn is a togglable, registered
 * hook rather than an inline imperative call (see gw_orch_delegates). Lives here (its only
 * caller) rather than in server_compute.c. */
int server_compute_dispatch_coord_task(server_ctx_t *ctx, int task_id, const char *role,
                                       const aimee_message_t *message, const char *files_json,
                                       const char *cwd, const char *persona,
                                       const char *claim_owner, int require_handoff)
{
   if (!ctx || task_id <= 0 || !message || !message->role || strcmp(message->role, "user") ||
       message->n_blocks < 1)
      return -1;
   coord_spawn_backing_t backing = {ctx,        task_id, claim_owner,     persona,
                                    files_json, cwd,     require_handoff, -1};
   gw_turn_capabilities_t caps = {&backing, coord_spawn_delegate, NULL};
   char turn_id[48];
   snprintf(turn_id, sizeof(turn_id), "coord-task-%d", task_id);
   if (gw_orch_delegates_run(&caps, turn_id, role, message, coord_delegates_enabled()) != 0)
      return -1; /* delegates module disabled -> not dispatched; caller releases + retries */
   return backing.spawn_rc; /* 0 spawned, -1 refused (ceiling/pool full) */
}

/* While coord jobs are active the dispatcher re-checks on this interval, for
 * capacity freed somewhere nothing announces. With none active it sleeps until
 * a delegation, agent-work or config change is announced, or work is queued
 * here. */
#define COORD_POLL_INTERVAL_SECS 10
#define COORD_MAX_ACTIVE_JOBS    32
#define COORD_FILES_MAX          DB1_COORD_FILES_LEN
#define COORD_CWD_MAX            DB1_COORD_CWD_LEN
#define COORD_ROLE_MAX           DB1_COORD_ROLE_LEN

static pthread_t g_thread;
static atomic_int g_running;
static atomic_int g_stop;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_notified; /* under g_mutex: a notify not yet consumed by a wait */
static server_ctx_t *g_ctx;
static char g_claim_owner[DB1_COORD_DELEGATE_LEN];

#define COORD_BOOT_OWNER_KEY    "coord_dispatcher_boot_owner"
#define COORD_CRASH_REQUEUE_MAX 3
#ifndef COORD_STARTUP_RETRY_MSECS
#define COORD_STARTUP_RETRY_MSECS 250
#endif

typedef struct
{
   int read, recovered;
   char previous[DB1_COORD_DELEGATE_LEN];
   char owner[DB1_COORD_DELEGATE_LEN];
} coord_startup_t;

/* Claims belong to a server incarnation, not the generic dispatcher name. The
 * previous incarnation's worker threads cannot survive process exit, so its
 * remaining claims are provably orphaned and safe to retry before this boot
 * starts dispatching. Persist the new owner only after recovery: if recovery is
 * interrupted, the next boot retries the same previous owner. */
static int dispatcher_recover_previous_boot(coord_startup_t *startup)
{
   if (g_stop)
      return -1;
   if (!startup->read)
   {
      int found = db1_runtime_state_lookup(COORD_BOOT_OWNER_KEY, startup->previous,
                                           sizeof startup->previous);
      if (found < 0)
         return -1;
      startup->read = 1;
   }
   if (g_stop)
      return -1;
   if (!startup->recovered && startup->previous[0])
   {
      int requeued = 0, failed = 0;
      if (db1_coord_job_recover_owner(startup->previous, COORD_CRASH_REQUEUE_MAX, &requeued,
                                      &failed) != 0)
         return -1;
      if (requeued || failed)
         LOG_WARN("coord_dispatcher",
                  "recovered previous-boot claims owner=%s: requeued=%d failed_at_cap=%d",
                  startup->previous, requeued, failed);
   }
   startup->recovered = 1;
   if (g_stop)
      return -1;
   if (!startup->owner[0])
   {
      unsigned char rnd[8];
      if (platform_random_bytes(rnd, sizeof rnd) != 0)
         return -1;
      snprintf(startup->owner, sizeof startup->owner, "coord-%ld-", (long)getpid());
      size_t used = strlen(startup->owner);
      for (size_t i = 0; i < sizeof rnd && used + 2 < sizeof startup->owner; i++)
         used +=
             (size_t)snprintf(startup->owner + used, sizeof startup->owner - used, "%02x", rnd[i]);
   }
   if (db1_runtime_state_set(COORD_BOOT_OWNER_KEY, startup->owner) != 0)
      return -1;
   snprintf(g_claim_owner, sizeof g_claim_owner, "%s", startup->owner);
   return 0;
}

/* Wait for a notify, or for milliseconds when it is not negative. A notify that
 * arrived while the dispatcher was sweeping ends the wait at once. */
static void dispatcher_wait(int milliseconds)
{
   if (milliseconds < 0)
   {
      pthread_mutex_lock(&g_mutex);
      while (!g_stop && !g_notified)
         pthread_cond_wait(&g_cond, &g_mutex);
      g_notified = 0;
      pthread_mutex_unlock(&g_mutex);
      return;
   }
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += milliseconds / 1000;
   ts.tv_nsec += (milliseconds % 1000) * 1000000L;
   if (ts.tv_nsec >= 1000000000L)
   {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
   }
   pthread_mutex_lock(&g_mutex);
   if (!g_stop && !g_notified)
      pthread_cond_timedwait(&g_cond, &g_mutex, &ts);
   g_notified = 0;
   pthread_mutex_unlock(&g_mutex);
}

/* Sweep all coord jobs that have pending tasks and dispatch up to max_concurrent.
 * *active says whether any coord job is still active, so worth re-checking. */
static int dispatcher_sweep(int *active)
{
   *active = 0;
   /* Delegates module disabled -> dispatch nothing: do NOT claim tasks (claiming then failing
    * to dispatch would release them and re-claim every poll tick, churning the queue). Coord
    * tasks ARE delegate work, so they simply wait until the module is re-enabled. Checked here,
    * before any claim, so a disabled module is distinct from a per-task 'refused' (pool full),
    * which alone releases+retries the single claimed task below. */
   if (!coord_delegates_enabled())
      return 0;

   int job_ids[COORD_MAX_ACTIVE_JOBS];
   int njobs = db1_coord_job_list_active_ids(job_ids, COORD_MAX_ACTIVE_JOBS);
   if (njobs < 0)
   {
      *active = 1; /* the store did not answer: look again */
      return 0;
   }
   if (njobs == 0)
      return 0;
   *active = 1;

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
         int task_id = db1_coord_job_claim_next(job_id, g_claim_owner, &task);
         if (task_id < 0)
            break; /* no more claimable tasks for this job */

         char role[COORD_ROLE_MAX] = "";
         aimee_message_t message = {0};
         char files[COORD_FILES_MAX] = "";
         char cwd[COORD_CWD_MAX] = "";
         char persona[COORD_ROLE_MAX] = "";
         if (db1_coord_task_get_dispatch(task_id, role, sizeof(role), &message, files,
                                         sizeof(files), cwd, sizeof(cwd), persona,
                                         sizeof(persona)) != 0)
         {
            aimee_message_free(&message);
            db1_coord_job_fail_task_owned(task_id, g_claim_owner,
                                          "dispatcher could not read task dispatch fields");
            continue;
         }

         /* Plan-based coord jobs use the structured handoff contract; an ad-hoc
          * WFE job (WFE_COORD_PLAN_ID) does not — its delegate returns free-form
          * output that the workflow engine reads raw. */
         int require_handoff = (job.plan_id != WFE_COORD_PLAN_ID);
         if (server_compute_dispatch_coord_task(g_ctx, task_id, role, &message, files, cwd, persona,
                                                g_claim_owner, require_handoff) != 0)
         {
            /* Pool full — release the claim and retry next sweep. */
            db1_coord_job_release_task(task_id);
            LOG_INFO("coord_dispatcher", "coord task #%d (job #%d): compute pool full, will retry",
                     task_id, job_id);
            aimee_message_free(&message);
            break;
         }

         LOG_INFO("coord_dispatcher", "dispatched coord task #%d (job #%d, role=%s)", task_id,
                  job_id, role[0] ? role : "execute");
         dispatched++;
         aimee_message_free(&message);
      }
   }
   return dispatched;
}

static void *dispatcher_thread(void *arg)
{
   (void)arg;
   coord_startup_t startup = {0};
   int ready = 0, warned = 0;
   while (!g_stop)
   {
      if (!ready)
      {
         if (dispatcher_recover_previous_boot(&startup))
         {
            if (!warned && !g_stop)
               LOG_WARN("coord_dispatcher",
                        "waiting for durable boot ownership; no tasks dispatched");
            warned = 1;
            dispatcher_wait(COORD_STARTUP_RETRY_MSECS);
            continue;
         }
         ready = 1;
         if (g_stop)
            break;
         LOG_INFO("coord_dispatcher", "started (re-check=%ds while jobs are active)",
                  COORD_POLL_INTERVAL_SECS);
      }
      int active = 0;
      dispatcher_sweep(&active);
      dispatcher_wait(active ? COORD_POLL_INTERVAL_SECS * 1000 : -1);
   }
   return NULL;
}

void server_coord_dispatcher_init(server_ctx_t *ctx)
{
   if (g_running)
      return;
   g_ctx = ctx;
   g_stop = 0;
   g_claim_owner[0] = 0;
   if (pthread_create(&g_thread, NULL, dispatcher_thread, NULL) != 0)
   {
      LOG_ERROR("coord_dispatcher", "failed to start dispatcher thread");
      return;
   }
   g_running = 1;
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
   g_notified = 1;
   pthread_cond_signal(&g_cond);
   pthread_mutex_unlock(&g_mutex);
}
