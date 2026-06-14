/* server_delegate_monitor.c: see server_delegate_monitor.h */

#include "server_delegate_monitor.h"
#include "http_retry.h" /* http_set_progress_cb */
#include "log.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Per-turn heartbeat for the in-flight background delegate on this thread. The
 * http_retry progress callback fires after every model HTTP attempt; bumping the
 * heartbeat there keeps a slow-but-progressing delegate alive (see header). */
static _Thread_local int g_hb_job_id;

static void delegate_heartbeat_cb(void)
{
   if (g_hb_job_id > 0)
      db1_agent_job_heartbeat(g_hb_job_id);
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

int server_delegate_monitor_sweep(int idle_threshold_secs, int in_tool_threshold_secs)
{
   int ids[MAX_RUNNING_JOBS_PER_SWEEP];
   int n = db1_agent_job_list_running_ids(ids, MAX_RUNNING_JOBS_PER_SWEEP);
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

static void *monitor_thread(void *arg)
{
   (void)arg;
   while (!g_monitor_stop)
   {
      (void)server_delegate_monitor_sweep(DEFAULT_IDLE_THRESHOLD_SECS,
                                          DEFAULT_IN_TOOL_THRESHOLD_SECS);

      /* Sleep in 1 s chunks so shutdown is responsive. */
      for (int slept = 0; slept < DEFAULT_POLL_INTERVAL_SECS && !g_monitor_stop; slept++)
         sleep(1);
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
   pthread_join(g_monitor_thread, NULL);
   g_monitor_running = 0;
}
