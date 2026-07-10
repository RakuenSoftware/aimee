/* wfe_scheduler.c: server-owned autonomy scheduler (Phase B).
 *
 * Drives active autonomous work items forward via wfe_autonomy_run on a
 * background thread, so a submitted proposal runs to completion (or to a human
 * gate) regardless of who is connected. Wakes on notify (submit / gate satisfied)
 * and on a periodic backstop sweep. Bounded concurrency = 1 (sequential) for now;
 * the roundtable's Q1 home (coord-dispatcher) pattern is mirrored here. */
#include "wfe_scheduler.h"

#include "log.h"
#include "wfe_autonomy.h"
#include "wfe_blocks.h" /* wfe_worktree_cleanup (terminal) + wfe_worktree_orphan_gc (age-based) */
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
 * safe to call on a human-parked item (it re-parks without re-running work), so a
 * sweep never double-executes a delegate. Terminal/interactive items are skipped. */
void wfe_scheduler_run_once(void)
{
   db1_work_item_t *items = NULL;
   int n = db1_work_item_list(&items);
   if (n <= 0 || !items)
      return;
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
            const char *rl = getenv("AIMEE_WORKFLOW_REPO");
            if (wfe_worktree_cleanup(items[i].worktree, (rl && rl[0]) ? rl : ".") == 0)
               db1_work_item_set_worktree(items[i].work_item_id, "");
         }
         continue;
      }
      if (strcmp(st, "active") != 0)
         continue; /* any other non-terminal state: leave it (don't reap its worktree) */
      if (strcmp(items[i].mode, "autonomous") != 0)
         continue; /* interactive items are human-driven in the webchat */
      char err[256] = "";
      if (wfe_autonomy_run(items[i].work_item_id, err, sizeof err) != 0)
         aimee_log(LOG_WARN, "wfe-sched", "autonomy run %s: %s", items[i].work_item_id,
                   err[0] ? err : "error");
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
