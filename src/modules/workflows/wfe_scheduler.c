/* wfe_scheduler.c: server-owned autonomy scheduler (Phase B).
 *
 * Drives active autonomous work items forward via wfe_autonomy_run on a
 * background thread, so a submitted proposal runs to completion (or to a human
 * gate) regardless of who is connected. Wakes on notify (submit / gate satisfied)
 * and on a periodic backstop sweep. The live scheduler keeps a bounded set of
 * in-flight workers (AIMEE_AUTONOMY_CONCURRENCY, default 8) and fills open
 * slots from a fresh LRU snapshot whenever work arrives or a worker finishes;
 * DB transaction safety across workers is the db1 txn gate. */
#include "wfe_scheduler.h"

#include "config.h" /* config_autonomy_lookup: live autonomy.* caps + auto-resume policy */
#include "log.h"
#include "wfe_autonomy.h"
#include "wfe_blocks.h" /* wfe_worktree_cleanup (terminal) + wfe_worktree_orphan_gc (age-based) */
#include "wfe_iface.h"  /* wfe_repo_local — resolve each item's own repo for cleanup */
#include "db1_client/wfe_store.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WFE_SCHED_BACKSTOP_SECS 30

static pthread_t g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_started;
static int g_running;
static int g_notified;

typedef struct
{
   int active;
   char work_item_id[80];
} live_slot_t;

static live_slot_t g_live_slots[64];
static int g_live_workers;

typedef struct
{
   char work_item_id[80];
   time_t until;
} live_cooldown_t;

static live_cooldown_t g_live_cooldowns[64];

/* Drive every active autonomous work item one autonomy pass. wfe_autonomy_run is
 * safe to call on a human-parked item (pending_human / stuck / operator_paused):
 * it re-parks without re-running work, so a sweep never double-executes a gated
 * delegate. A TRANSIENT roundtable park (panel_degraded / panel_unreachable) is
 * the deliberate exception — autonomy re-runs the panel a bounded number of times
 * (one attempt per sweep) so a momentary provider blip self-heals rather than
 * dead-ending the run. Terminal/interactive items are skipped. */
/* Concurrency of the autonomy pass. Delegate work dominates a stage's
 * wall-clock, and stages are independent per work item (a fresh fan-out is a
 * dozen sibling slices), so driving one item at a time serializes hours of
 * delegate runs behind each other. DB safety under parallel advances comes
 * from the db1 transaction gate (db1_internal.h) — the engine's short
 * post-executor transactions are mutually exclusive; delegate execution
 * itself holds no transaction (see wfe_engine_advance). */
static long wfe_sched_concurrency(void)
{
   /* Config-backed + live (env override > autonomy.concurrency snapshot); tunable from
    * the web Settings GUI. Out-of-range / unavailable -> historical default 8. */
   long k;
   if (config_autonomy_lookup("AIMEE_AUTONOMY_CONCURRENCY", &k) && k >= 1 && k <= 64)
      return k;
   return 8;
}

/* Whether the autonomy scheduler should hand this active item a worker slot.
 *
 * A step WAITING on external action holds no agent and does no work — it just
 * waits — so it must NOT be driven. Driving a parked item only burns a scheduler
 * slot to reach a no-op advance (wfe_engine_advance returns PENDING on a still-set
 * pause), which STARVES genuinely-runnable runs when parked items pile up (a fleet
 * of pending_human proposals could hold every worker slot forever). Two kinds are
 * driveable:
 *   - un-parked (pause_reason empty): the next step is ready to run;
 *   - a SELF-RESOLVING park the sweep exists to re-check: ci_pending / merge_pending
 *     re-poll the forge, panel_degraded / panel_unreachable retry a flaky panel,
 *     slices_running re-aggregates a foreach parent's children (advance once all
 *     merged). A foreach parent MUST stay driveable while its slices run: the
 *     fan-in aggregation only happens when the parent is re-driven, so parking it
 *     non-driveably would wedge the whole fan-out.
 * Every other park either waits for a HUMAN (pending_human, operator_paused) or is
 * a runaway/failure backstop only a human (or the stale-run reaper) clears (stuck,
 * turn_cap_exceeded, wall_cap_exceeded, budget_exceeded, max_attempts, failed).
 * Those hold NO agent: the gate/resume API clears the pause and wakes the
 * scheduler, and the next sweep sees it un-parked and drives it. */
static int wfe_sched_driveable(const char *pause_reason)
{
   if (!pause_reason || !pause_reason[0])
      return 1;
   return strcmp(pause_reason, "ci_pending") == 0 || strcmp(pause_reason, "merge_pending") == 0 ||
          strcmp(pause_reason, "panel_degraded") == 0 ||
          strcmp(pause_reason, "panel_unreachable") == 0 ||
          strcmp(pause_reason, "slices_running") == 0;
}

/* Auto-resume policy for wall-cap parks. A wall_cap_exceeded park is a long run
 * hitting its per-resume wall window (a legitimate checkpoint, not a runaway), so
 * when autonomy.auto_resume_cap_parks is on, give it a fresh window instead of
 * leaving it for the reaper — this is what drives an autonomous run to completion
 * across multiple wall windows. Bounded by autonomy.max_resumes, counted from this
 * item's own prior auto-resume events, so a genuinely wedged run still ends up
 * reaped once the budget is spent. A turn_cap park (CUMULATIVE turn count) is
 * deliberately NOT auto-resumed: that cap IS the runaway backstop — raise
 * autonomy.max_turns to give a run more total budget. All knobs are config-backed
 * + live (env override honored), tunable from the web Settings GUI. */
static void wfe_sched_try_auto_resume(const db1_work_item_t *it)
{
   long on = 0;
   if (!(config_autonomy_lookup("AIMEE_AUTONOMY_AUTO_RESUME_CAP_PARKS", &on) && on))
      return; /* policy off (or snapshot unavailable) -> leave it for the reaper */
   long maxr = 50, lv;
   if (config_autonomy_lookup("AIMEE_AUTONOMY_MAX_RESUMES", &lv) && lv >= 0)
      maxr = lv;
   if (maxr <= 0)
      return; /* budget 0 -> effectively off */

   /* Count prior auto-resumes (our own marker) so max_resumes bounds the loop; a
    * manual operator resume uses a different actor and is not counted. */
   db1_lifecycle_event_t *evs = NULL;
   int nev = db1_lifecycle_event_list(it->work_item_id, &evs);
   int resumes = 0;
   for (int i = 0; i < nev; i++)
      if (strcmp(evs[i].kind, "resume") == 0 && strcmp(evs[i].actor, "autonomy-sched") == 0)
         resumes++;
   free(evs);
   if (resumes >= maxr)
      return; /* auto-resume budget spent -> let the stale-park reaper abandon it */

   /* Compare-and-clear: resume only while it still equals (wall_cap_exceeded, stage),
    * so a manual resume or state change racing this sweep is not double-applied. */
   if (db1_work_item_clear_pause_if(it->work_item_id, "wall_cap_exceeded", it->current_stage) != 1)
      return;
   /* Deliberately do NOT reset the per-stage attempt counter: a wall-cap park is a
    * time-box (the run mid-execution ran out of wall window), not a loop-back, so the
    * stage's max_attempts -> 'stuck' backstop must keep counting genuine loop-backs
    * across wall windows rather than being re-armed each one. (A human resume re-arms
    * it deliberately; an automatic wall-window continuation should not.) */
   db1_lifecycle_event_add(it->work_item_id, it->current_stage, "resume", "autonomy-sched",
                           "auto-resume (wall-cap): fresh wall window", "", 0);
   aimee_log(LOG_INFO, "wfe-sched", "auto-resumed %s (wall-cap park; resume %d/%ld)",
             it->work_item_id, resumes + 1, maxr);
}

/* Stage class for sweep priority: LOWER runs first. Downstream-first ("drain
 * the pipe"): an item at a gate/forge stage is a finished diff waiting on
 * review/CI/merge, while every impl delegate launched competes with panel
 * seats for the same small agent roster (observed live: concurrent impl
 * traffic drove review-agent health streaks down and panels degraded with
 * "no viable review agent"). Handing workers to gate stages first lets
 * panels run against a quieter roster and converts WIP into PRs before new
 * WIP starts. Unknown stages rank with implement; LRU breaks ties. */
static int wfe_stage_class(const char *stage)
{
   if (!stage)
      return 5;
   if (!strcmp(stage, "merge"))
      return 0;
   if (!strcmp(stage, "ci"))
      return 1;
   if (!strcmp(stage, "pr"))
      return 2;
   if (!strcmp(stage, "rt_gate") || !strncmp(stage, "gate", 4))
      return 3;
   if (!strcmp(stage, "freeze"))
      return 4;
   if (!strcmp(stage, "scope") || !strcmp(stage, "slices"))
      return 6;
   return 5; /* impl + anything workflow-specific */
}

/* Work-stealing cursor shared by the pass workers. */
typedef struct
{
   db1_work_item_t *items;
   int n;
   int next; /* next unclaimed index; guarded by lock */
   pthread_mutex_t lock;
} sched_pass_t;

static void *wfe_sched_worker(void *arg)
{
   sched_pass_t *p = arg;
   for (;;)
   {
      pthread_mutex_lock(&p->lock);
      int i = (p->next < p->n) ? p->next++ : -1;
      pthread_mutex_unlock(&p->lock);
      if (i < 0)
         return NULL;
      char err[256] = "";
      if (wfe_autonomy_run(p->items[i].work_item_id, err, sizeof err) != 0)
         aimee_log(LOG_WARN, "wfe-sched", "autonomy run %s: %s", p->items[i].work_item_id,
                   err[0] ? err : "error");
   }
}

/* Collect a fresh, priority-sorted set of driveable items. This shared selector
 * keeps deterministic synchronous sweeps and the live capacity-aware dispatcher
 * on exactly the same eligibility, cleanup, auto-resume, and ordering policy. */
static int wfe_sched_collect(db1_work_item_t **run_out)
{
   if (!run_out)
      return 0;
   *run_out = NULL;
   /* LRU order (least-recently-updated first): a fixed newest-first order lets
    * the busiest items eat every sweep and starve the rest (observed live: 2 of
    * 13 slices monopolized 3.5h of sweeps). With the parallel pass below the
    * order decides who gets a worker FIRST when items outnumber workers. */
   /* Backstop reaper (before the drive walk, so a freshly-reaped run's worktree is
    * torn down in this same sweep's terminal-cleanup pass below): abandon dead
    * autonomous runs parked in a runaway/failure backstop past a grace window, so
    * they cannot linger 'active' forever. Legitimate human waits (pending_human /
    * operator_paused) are never reaped. Default 1h; AIMEE_AUTONOMY_STALE_ABANDON_SECS
    * overrides, 0 disables. */
   {
      /* Config-backed + live (env override > autonomy.stale_abandon_secs); 0 disables. */
      long grace = 3600, gv;
      if (config_autonomy_lookup("AIMEE_AUTONOMY_STALE_ABANDON_SECS", &gv) && gv >= 0)
         grace = gv;
      int reaped = db1_work_item_reap_stale_parks(grace);
      if (reaped > 0)
         aimee_log(LOG_INFO, "wfe-sched", "reaped %d stale-parked run(s) -> abandoned", reaped);
   }

   db1_work_item_t *items = NULL;
   int n = db1_work_item_list_lru(&items);
   if (n <= 0 || !items)
      return 0;
   /* Eligible autonomy targets are collected during the (cheap, sequential)
    * cleanup walk, then driven by a bounded worker pool. */
   db1_work_item_t *run = malloc((size_t)n * sizeof *run);
   int run_n = 0;
   for (int i = 0; i < n; i++)
   {
      const char *st = items[i].state;
      /* Orphan-sweep (F2): a TERMINAL item may still hold a worktree that the
       * autonomy terminal path didn't clean — e.g. a reject applied via the API
       * gate, or an override-cap rejection. Tear it down + clear the column. Match
       * the terminal set EXPLICITLY (not just !active): a parked item keeps
       * state=active, and only accepted/rejected/abandoned are immutable-terminal,
       * so acting on this snapshot can't race an item back to life. */
      int terminal = !strcmp(st, "accepted") || !strcmp(st, "rejected") || !strcmp(st, "abandoned");
      if (terminal)
      {
         if (items[i].worktree[0])
         {
            if (wfe_worktree_cleanup(items[i].worktree, wfe_repo_local(items[i].repo)) == 0)
               db1_work_item_set_worktree(items[i].work_item_id, "");
         }
         continue;
      }
      if (strcmp(st, "active") != 0)
         continue; /* any other non-terminal state: leave it (don't reap its worktree) */
      if (strcmp(items[i].mode, "autonomous") != 0)
         continue; /* interactive items are human-driven in the webchat */
      /* A wall-cap park is a long run's checkpoint: auto-resume it (policy-gated,
       * bounded) so it keeps making progress. Cleared here, it is driven next sweep
       * (this snapshot still reads wall_cap_exceeded, so it stays non-driveable now).
       * clear_pause_if bumps updated_at, so the reaper (which ran earlier this sweep,
       * before the list snapshot) will not catch the freshly-resumed run next sweep. */
      if (strcmp(items[i].pause_reason, "wall_cap_exceeded") == 0)
         wfe_sched_try_auto_resume(&items[i]);
      if (!wfe_sched_driveable(items[i].pause_reason))
         continue; /* parked on a human/dead gate: no agent, just waits (a gate/resume
                    * API or the reaper moves it) — driving it only starves runnable runs */
      if (run)
         run[run_n++] = items[i];
   }
   if (run && run_n > 0)
   {
      /* Stable sort by stage class (downstream first), preserving the LRU
       * order within a class: insertion sort on a small array (run_n is the
       * active-item count, tens at most). qsort is not stability-guaranteed,
       * and losing the LRU tie-break would reintroduce intra-class
       * starvation. */
      for (int i = 1; i < run_n; i++)
      {
         db1_work_item_t tmp = run[i];
         int c = wfe_stage_class(tmp.current_stage);
         int j = i - 1;
         while (j >= 0 && wfe_stage_class(run[j].current_stage) > c)
         {
            run[j + 1] = run[j];
            j--;
         }
         run[j + 1] = tmp;
      }
   }
   free(items);

   /* Age-based orphan GC: reap wfe-worktrees dirs no live work item owns (a deleted
    * row, or a crashed run that never reached terminal) once past a grace window,
    * so a full git worktree can't be stranded — and its inodes leaked — forever.
    * Complements the per-item terminal cleanup above, which only fires on a clean
    * state transition. AIMEE_WFE_WORKTREE_GC_GRACE_SECS overrides the default. */
   long grace = 3600;
   const char *gv = getenv("AIMEE_WFE_WORKTREE_GC_GRACE_SECS");
   if (gv && gv[0])
   {
      char *end = NULL;
      long g = strtol(gv, &end, 10);
      if (end && *end == '\0' && g >= 0)
         grace = g;
   }
   const char *rl = getenv("AIMEE_WORKFLOW_REPO");
   int reaped = wfe_worktree_orphan_gc((rl && rl[0]) ? rl : ".", grace);
   if (reaped > 0)
      aimee_log(LOG_INFO, "wfe-sched", "orphan-GC reaped %d stale worktree(s)", reaped);

   *run_out = run;
   return run ? run_n : 0;
}

void wfe_scheduler_run_once(void)
{
   db1_work_item_t *run = NULL;
   int run_n = wfe_sched_collect(&run);
   if (run_n > 0)
   {
      sched_pass_t pass = {run, run_n, 0, PTHREAD_MUTEX_INITIALIZER};
      long want = wfe_sched_concurrency();
      int k = (int)((want < run_n) ? want : run_n);
      pthread_t tids[64];
      int spawned = 0;
      for (int i = 0; i < k; i++)
         if (pthread_create(&tids[spawned], NULL, wfe_sched_worker, &pass) == 0)
            spawned++;
      if (spawned == 0)
         wfe_sched_worker(&pass); /* thread creation failed: degrade to inline */
      for (int i = 0; i < spawned; i++)
         pthread_join(tids[i], NULL);
      pthread_mutex_destroy(&pass.lock);
   }
   free(run);
}

typedef struct
{
   int slot;
   char work_item_id[80];
} live_worker_arg_t;

static void wfe_live_cooldown_set_locked(const char *work_item_id)
{
   int target = -1;
   time_t oldest = 0;
   for (int i = 0; i < 64; i++)
   {
      if (strcmp(g_live_cooldowns[i].work_item_id, work_item_id) == 0)
      {
         target = i;
         break;
      }
      if (target < 0 || !g_live_cooldowns[i].work_item_id[0] || g_live_cooldowns[i].until < oldest)
      {
         target = i;
         oldest = g_live_cooldowns[i].until;
      }
   }
   if (target >= 0)
   {
      snprintf(g_live_cooldowns[target].work_item_id, sizeof(g_live_cooldowns[target].work_item_id),
               "%s", work_item_id);
      g_live_cooldowns[target].until = time(NULL) + WFE_SCHED_BACKSTOP_SECS;
   }
}

static int wfe_live_cooling_locked(const char *work_item_id)
{
   time_t now = time(NULL);
   for (int i = 0; i < 64; i++)
      if (strcmp(g_live_cooldowns[i].work_item_id, work_item_id) == 0)
         return g_live_cooldowns[i].until > now;
   return 0;
}

static void wfe_live_slot_release(int slot, const char *work_item_id, int backoff)
{
   pthread_mutex_lock(&g_lock);
   if (slot >= 0 && slot < 64 && g_live_slots[slot].active &&
       strcmp(g_live_slots[slot].work_item_id, work_item_id) == 0)
   {
      g_live_slots[slot].active = 0;
      g_live_slots[slot].work_item_id[0] = '\0';
      if (g_live_workers > 0)
         g_live_workers--;
   }
   if (backoff)
      wfe_live_cooldown_set_locked(work_item_id);
   g_notified = 1; /* immediately offer the freed slot to another item */
   pthread_cond_signal(&g_cv);
   pthread_mutex_unlock(&g_lock);
}

static void *wfe_live_worker(void *arg)
{
   live_worker_arg_t *worker = arg;
   char err[256] = "";
   if (wfe_autonomy_run(worker->work_item_id, err, sizeof err) != 0)
      aimee_log(LOG_WARN, "wfe-sched", "autonomy run %s: %s", worker->work_item_id,
                err[0] ? err : "error");
   db1_work_item_t item;
   int backoff = db1_work_item_get(worker->work_item_id, &item) == 1 &&
                 strcmp(item.state, "active") == 0 && item.pause_reason[0] &&
                 wfe_sched_driveable(item.pause_reason);
   wfe_live_slot_release(worker->slot, worker->work_item_id, backoff);
   free(worker);
   return NULL;
}

/* Atomically reserve a live slot and fence the same item from being dispatched
 * twice across notifications/backstop scans. Returns slot, -1 at capacity, or
 * -2 when this item is already in flight, or -3 during the normal backstop
 * cooldown for a self-resolving pause (CI/panel/merge/fan-in polling). */
static int wfe_live_slot_claim(const char *work_item_id, int limit)
{
   pthread_mutex_lock(&g_lock);
   if (wfe_live_cooling_locked(work_item_id))
   {
      pthread_mutex_unlock(&g_lock);
      return -3;
   }
   if (!g_running || g_live_workers >= limit)
   {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   int free_slot = -1;
   for (int i = 0; i < 64; i++)
   {
      if (g_live_slots[i].active && strcmp(g_live_slots[i].work_item_id, work_item_id) == 0)
      {
         pthread_mutex_unlock(&g_lock);
         return -2;
      }
      if (!g_live_slots[i].active && free_slot < 0)
         free_slot = i;
   }
   if (free_slot < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   g_live_slots[free_slot].active = 1;
   snprintf(g_live_slots[free_slot].work_item_id, sizeof(g_live_slots[free_slot].work_item_id),
            "%s", work_item_id);
   g_live_workers++;
   pthread_mutex_unlock(&g_lock);
   return free_slot;
}

/* Fill every currently-open slot from a FRESH snapshot. Unlike the synchronous
 * sweep, this does not join long-running autonomy calls: a later submit/worker
 * completion wakes the manager, which can populate spare capacity immediately. */
static void wfe_scheduler_dispatch_available(void)
{
   db1_work_item_t *run = NULL;
   int run_n = wfe_sched_collect(&run);
   int limit = (int)wfe_sched_concurrency();
   for (int i = 0; i < run_n; i++)
   {
      int slot = wfe_live_slot_claim(run[i].work_item_id, limit);
      if (slot == -1)
         break;
      if (slot == -2 || slot == -3)
         continue;

      live_worker_arg_t *worker = calloc(1, sizeof *worker);
      if (!worker)
      {
         wfe_live_slot_release(slot, run[i].work_item_id, 0);
         continue;
      }
      worker->slot = slot;
      snprintf(worker->work_item_id, sizeof(worker->work_item_id), "%s", run[i].work_item_id);

      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      pthread_t tid;
      int rc = pthread_create(&tid, &attr, wfe_live_worker, worker);
      pthread_attr_destroy(&attr);
      if (rc != 0)
      {
         wfe_live_slot_release(slot, run[i].work_item_id, 0);
         free(worker);
         aimee_log(LOG_WARN, "wfe-sched", "failed to start worker for %s", run[i].work_item_id);
      }
   }
   free(run);
}

static void *wfe_scheduler_loop(void *arg)
{
   (void)arg;
   for (;;)
   {
      pthread_mutex_lock(&g_lock);
      if (!g_running)
      {
         pthread_mutex_unlock(&g_lock);
         break;
      }
      pthread_mutex_unlock(&g_lock);

      wfe_scheduler_dispatch_available();

      pthread_mutex_lock(&g_lock);
      if (g_running && !g_notified)
      {
         struct timespec ts;
         clock_gettime(CLOCK_REALTIME, &ts);
         ts.tv_sec += WFE_SCHED_BACKSTOP_SECS;
         pthread_cond_timedwait(&g_cv, &g_lock, &ts);
      }
      g_notified = 0;
      int run = g_running;
      pthread_mutex_unlock(&g_lock);
      if (!run)
         break;
   }
   return NULL;
}

void wfe_scheduler_init(void)
{
   const char *engine = getenv("AIMEE_WFE_ENGINE");
   if (engine && strcmp(engine, "go") == 0)
   {
      aimee_log(LOG_INFO, "wfe-sched", "disabled; Go WFE owns scheduling and execution");
      return;
   }
   pthread_mutex_lock(&g_lock);
   if (g_started)
   {
      pthread_mutex_unlock(&g_lock);
      return;
   }
   g_started = 1;
   g_running = 1;
   pthread_mutex_unlock(&g_lock);
   if (pthread_create(&g_thread, NULL, wfe_scheduler_loop, NULL) != 0)
   {
      pthread_mutex_lock(&g_lock);
      g_started = g_running = 0;
      pthread_mutex_unlock(&g_lock);
      aimee_log(LOG_WARN, "wfe-sched", "failed to start scheduler thread");
   }
}

void wfe_scheduler_notify(void)
{
   pthread_mutex_lock(&g_lock);
   /* A gate/webhook/operator notification may make a self-resolving item ready
    * before its polling backstop expires. Re-scan it immediately. */
   memset(g_live_cooldowns, 0, sizeof(g_live_cooldowns));
   g_notified = 1;
   pthread_cond_signal(&g_cv);
   pthread_mutex_unlock(&g_lock);
}

void wfe_scheduler_shutdown(void)
{
   pthread_mutex_lock(&g_lock);
   if (!g_started || !g_running)
   {
      pthread_mutex_unlock(&g_lock);
      return;
   }
   g_running = 0;
   pthread_cond_signal(&g_cv);
   pthread_mutex_unlock(&g_lock);
   pthread_join(g_thread, NULL);
   pthread_mutex_lock(&g_lock);
   while (g_live_workers > 0)
      pthread_cond_wait(&g_cv, &g_lock);
   g_started = 0;
   g_notified = 0;
   memset(g_live_slots, 0, sizeof(g_live_slots));
   memset(g_live_cooldowns, 0, sizeof(g_live_cooldowns));
   pthread_mutex_unlock(&g_lock);
}
