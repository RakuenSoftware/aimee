/* wfe_scheduler.c: server-owned autonomy scheduler (Phase B).
 *
 * Drives active autonomous work items forward via wfe_autonomy_run on a
 * background thread, so a submitted proposal runs to completion (or to a human
 * gate) regardless of who is connected. Wakes on notify (submit / gate satisfied)
 * and on a periodic backstop sweep. Each sweep drives eligible items via a
 * bounded worker pool (AIMEE_AUTONOMY_CONCURRENCY, default 8) in LRU order;
 * DB transaction safety across workers is the db1 txn gate. */
#include "wfe_scheduler.h"

#include "log.h"
#include "wfe_autonomy.h"
#include "wfe_blocks.h" /* wfe_worktree_cleanup (terminal) + wfe_worktree_orphan_gc (age-based) */
#include "wfe_iface.h"  /* wfe_repo_local — resolve each item's own repo for cleanup */
#include "wfe_store.h"

#include <pthread.h>
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
   const char *v = getenv("AIMEE_AUTONOMY_CONCURRENCY");
   if (v && v[0])
   {
      char *end = NULL;
      long k = strtol(v, &end, 10);
      if (end && *end == '\0' && k >= 1 && k <= 64)
         return k;
   }
   return 8;
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

void wfe_scheduler_run_once(void)
{
   /* LRU order (least-recently-updated first): a fixed newest-first order lets
    * the busiest items eat every sweep and starve the rest (observed live: 2 of
    * 13 slices monopolized 3.5h of sweeps). With the parallel pass below the
    * order decides who gets a worker FIRST when items outnumber workers. */
   db1_work_item_t *items = NULL;
   int n = db1_work_item_list_lru(&items);
   if (n <= 0 || !items)
      return;
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

      wfe_scheduler_run_once();

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
   g_started = 0;
   pthread_mutex_unlock(&g_lock);
}
