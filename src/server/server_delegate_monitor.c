/* server_delegate_monitor.c: see server_delegate_monitor.h */

#include "server_delegate_monitor.h"
#include <aimee/delegates/delegate_backend_docker.h> /* delegate_backend_docker_reap_aged (leaked-container reap) */
#include "agent_admission.h" /* agent_admission_touch / _reap_idle (wedged-slot reclaim) */
#include "http_retry.h"      /* http_set_progress_cb */
#include "log.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Pull domain helpers via forward decls — server_delegate_monitor.c is
 * server-side, db1/agent_jobs.h is reachable through db1.h's umbrella but
 * the umbrella drags in unrelated symbols. Forward declarations keep the
 * coupling explicit. */
int db1_agent_job_list_running_ids(int *out_ids, int max);
int db1_agent_job_classify_stale(int job_id, int idle_threshold_secs, int in_tool_threshold_secs,
                                 char *out_state, size_t out_state_cap);
int db1_agent_job_cancel_by_id(int job_id, const char *reason);
void db1_agent_job_heartbeat(int job_id);

/* Bulk restart reconciliation must finish before admitting any new durable job.
 * Keep the lock across the typed owner call: an unavailable store or lost ACK
 * leaves admission closed, and only a confirmed success latches it open. */
int db1_agent_job_cancel_nonterminal_on_restart(const char *reason);
static pthread_mutex_t g_startup_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_startup_reconciled;

int server_delegate_startup_reconcile(void)
{
   pthread_mutex_lock(&g_startup_lock);
   int rc = 0;
   if (!g_startup_reconciled)
   {
      int cancelled = db1_agent_job_cancel_nonterminal_on_restart("orphaned by server restart");
      if (cancelled < 0)
         rc = -1;
      else
      {
         g_startup_reconciled = 1;
         LOG_INFO("server", "delegate startup reconciliation complete");
         if (cancelled > 0)
            LOG_INFO("server", "cancelled %d delegate jobs orphaned by the prior process",
                     cancelled);
      }
   }
   pthread_mutex_unlock(&g_startup_lock);
   return rc;
}

/* The id of the delegation running on this thread (== the admission context handle for a
 * delegate turn). Weak so unit binaries that don't link the runtime resolve it to NULL and
 * simply skip the admission touch; the server always provides the strong definition. */
const char *delegation_active_id(void) __attribute__((weak));

/* Per-turn heartbeat for the in-flight background delegate on this thread. The
 * http_retry progress callback fires after every model HTTP attempt; bumping the
 * heartbeat there keeps a slow-but-progressing delegate alive (see header). */
static _Thread_local int g_hb_job_id;

static void delegate_heartbeat_cb(void)
{
   if (g_hb_job_id > 0)
      db1_agent_job_heartbeat(g_hb_job_id);
   /* Same liveness signal for the admission controller: a tmux-CLI delegate holds its
    * slot for the whole turn, so touch it here to keep the wedged-slot reaper off a
    * healthy long turn. delegation_active_id() is the slot's context handle. */
   if (delegation_active_id)
   {
      const char *deleg = delegation_active_id();
      if (deleg && deleg[0])
         agent_admission_touch(deleg);
   }
}

void server_delegate_heartbeat_begin(int job_id)
{
   g_hb_job_id = job_id > 0 ? job_id : 0;
   http_set_progress_cb(job_id > 0 ? delegate_heartbeat_cb : NULL);
}

void server_delegate_heartbeat_end(void)
{
   http_set_progress_cb(NULL);
   g_hb_job_id = 0;
}

/* Defaults from delegate-reliability-heartbeat-and-cost-rollup.md §1. */
#define DEFAULT_IDLE_THRESHOLD_SECS    450
#define DEFAULT_IN_TOOL_THRESHOLD_SECS 1200
#define DEFAULT_POLL_INTERVAL_SECS     30
#define MAX_RUNNING_JOBS_PER_SWEEP     128

static pthread_t g_monitor_thread;
static volatile int g_monitor_running;
static volatile int g_monitor_stop;
/* The monitor sleeps on this until a delegation changes (announced by the store,
 * ouroboros ring D14), its staleness check falls due, or shutdown. */
static pthread_mutex_t g_monitor_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_monitor_wake = PTHREAD_COND_INITIALIZER;
static int g_monitor_notified = 1; /* look once at start */
/* Running jobs the last sweep saw: while there are any, staleness is a deadline
 * that has to be checked; while there are none, nothing can go stale. */
static int g_monitor_running_jobs;

void server_delegate_monitor_wake(void)
{
   pthread_mutex_lock(&g_monitor_lock);
   g_monitor_notified = 1;
   pthread_cond_signal(&g_monitor_wake);
   pthread_mutex_unlock(&g_monitor_lock);
}

int server_delegate_monitor_sweep(int idle_threshold_secs, int in_tool_threshold_secs)
{
   int ids[MAX_RUNNING_JOBS_PER_SWEEP];
   int n = db1_agent_job_list_running_ids(ids, MAX_RUNNING_JOBS_PER_SWEEP);
   g_monitor_running_jobs = n > 0 ? n : 0;
   if (n <= 0)
      return 0;

   int cancelled = 0;
   for (int i = 0; i < n; i++)
   {
      char state[16] = {0};
      if (db1_agent_job_classify_stale(ids[i], idle_threshold_secs, in_tool_threshold_secs, state,
                                       sizeof(state)) != 1)
         continue;
      char reason[96];
      snprintf(reason, sizeof(reason), "stale: %s (no heartbeat progress)", state);
      if (db1_agent_job_cancel_by_id(ids[i], reason) > 0)
      {
         LOG_WARN("server.delegate_monitor", "auto-cancelled job #%d (state=%s)", ids[i], state);
         cancelled++;
      }
   }
   return cancelled;
}

/* Reap aged delegate containers this often. The sweep above stale-cancels the
 * JOB but never removes its container; only the normal completion path does.
 * Without this, a stale/crashed delegate leaks its container until the next
 * restart, accumulating until the image volume fills. It asks the container
 * runtime, not the store, so an idle server still makes no store call for it. */
#define CONTAINER_REAP_INTERVAL_SECS 300
/* Age threshold for the container reap. Above DEFAULT_IN_TOOL_THRESHOLD_SECS so a
 * live long-running turn is never removed. */
#define CONTAINER_REAP_AGE_SECS 1800

/* Idle threshold for reclaiming a wedged admission slot: a holder that stops touching
 * its context (heartbeat every ~15 s) for this long has died or wedged without releasing,
 * so its concurrency capacity is pinned and blocks new turns for that agent. Sits far
 * above the heartbeat interval (a healthy long turn is never reaped) yet below the
 * stale-job cancel thresholds so freed capacity returns promptly, each sweep. */
#define ADMISSION_IDLE_REAP_SECS 240

static void *monitor_thread(void *arg)
{
   (void)arg;
   time_t last_container_reap = time(NULL);
   while (!g_monitor_stop)
   {
      pthread_mutex_lock(&g_monitor_lock);
      int notified = g_monitor_notified;
      g_monitor_notified = 0;
      pthread_mutex_unlock(&g_monitor_lock);
      int busy = g_monitor_running_jobs > 0 || agent_admission_global_active() > 0;

      /* The store is asked only when a delegation changed or one is running:
       * this used to sweep every 30s with nothing to find. */
      if (notified || busy)
         (void)server_delegate_monitor_sweep(DEFAULT_IDLE_THRESHOLD_SECS,
                                             DEFAULT_IN_TOOL_THRESHOLD_SECS);

      int freed = agent_admission_reap_idle(ADMISSION_IDLE_REAP_SECS);
      if (freed > 0)
         LOG_WARN("server.delegate_monitor", "reclaimed %d wedged admission slot(s)", freed);

      time_t now = time(NULL);
      if (now - last_container_reap >= CONTAINER_REAP_INTERVAL_SECS)
      {
         last_container_reap = now;
         int reaped = delegate_backend_docker_reap_aged(CONTAINER_REAP_AGE_SECS);
         if (reaped > 0)
            LOG_INFO("server.delegate_monitor", "reaped %d aged delegate container(s)", reaped);
      }

      /* Busy: staleness is a deadline, so check it every poll interval. Idle:
       * sleep until a delegation changes or the container reap is due. */
      busy = g_monitor_running_jobs > 0 || agent_admission_global_active() > 0;
      int wait = busy ? DEFAULT_POLL_INTERVAL_SECS : CONTAINER_REAP_INTERVAL_SECS;
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += wait;
      pthread_mutex_lock(&g_monitor_lock);
      while (!g_monitor_notified && !g_monitor_stop &&
             pthread_cond_timedwait(&g_monitor_wake, &g_monitor_lock, &until) == 0)
         ;
      pthread_mutex_unlock(&g_monitor_lock);
   }
   return NULL;
}

static int env_gate_enabled(void)
{
   const char *v = getenv("AIMEE_DELEGATE_HEARTBEAT_MONITOR");
   if (!v || !v[0])
      return 1;
   if (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F')
      return 0;
   return 1;
}

void server_delegate_monitor_init(void)
{
   if (g_monitor_running)
      return;
   if (!env_gate_enabled())
      return;
   g_monitor_stop = 0;
   if (pthread_create(&g_monitor_thread, NULL, monitor_thread, NULL) != 0)
   {
      LOG_ERROR("server.delegate_monitor", "failed to start monitor thread");
      return;
   }
   g_monitor_running = 1;
   LOG_INFO("server.delegate_monitor", "started (idle=%ds, in_tool=%ds, poll=%ds)",
            DEFAULT_IDLE_THRESHOLD_SECS, DEFAULT_IN_TOOL_THRESHOLD_SECS,
            DEFAULT_POLL_INTERVAL_SECS);
}

void server_delegate_monitor_shutdown(void)
{
   if (!g_monitor_running)
      return;
   g_monitor_stop = 1;
   server_delegate_monitor_wake();
   pthread_join(g_monitor_thread, NULL);
   g_monitor_running = 0;
}
