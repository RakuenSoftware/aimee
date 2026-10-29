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

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_notified;

typedef struct
{
   int active;
   char work_item_id[80];
} live_slot_t;

typedef struct
{
   char work_item_id[80];
   time_t until;
} live_cooldown_t;

static live_cooldown_t g_live_cooldowns[64];

/* Work-stealing cursor shared by the pass workers. */
typedef struct
{
   db1_work_item_t *items;
   int n;
   int next; /* next unclaimed index; guarded by lock */
   pthread_mutex_t lock;
} sched_pass_t;

typedef struct
{
   int slot;
   char work_item_id[80];
} live_worker_arg_t;

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
