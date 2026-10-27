/* Exercise the production barrier, including a committed cleanup with a lost ACK. */
#include "../server/server_delegate_monitor.c"
#include <assert.h>

/* Unrelated monitor paths must never execute in this admission fixture. */
void db1_agent_job_heartbeat(int id)
{
   (void)id;
   abort();
}
void agent_admission_touch(const char *id)
{
   (void)id;
   abort();
}
void http_set_progress_cb(http_progress_cb_t cb)
{
   (void)cb;
   abort();
}
int db1_agent_job_list_running_ids(int *ids, int max)
{
   (void)ids;
   (void)max;
   abort();
}
int db1_agent_job_classify_stale(int id, int idle, int in_tool, char *state, size_t cap)
{
   (void)id;
   (void)idle;
   (void)in_tool;
   (void)state;
   (void)cap;
   abort();
}
int db1_agent_job_cancel_by_id(int id, const char *reason)
{
   (void)id;
   (void)reason;
   abort();
}
int agent_admission_reap_idle(int seconds)
{
   (void)seconds;
   abort();
}
int delegate_backend_docker_reap_aged(int seconds)
{
   (void)seconds;
   abort();
}

static int ready_markers;
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   if (!strcmp(fmt, "delegate startup reconciliation complete"))
   {
      assert(level == LOG_INFO && !strcmp(module, "server"));
      ready_markers++;
   }
}

static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_changed = PTHREAD_COND_INITIALIZER;
static int unavailable, lost_ack, cleanup_calls, old_jobs, new_jobs;
static int hold_cleanup, entered_cleanup;

int db1_agent_job_cancel_nonterminal_on_restart(const char *reason)
{
   assert(!strcmp(reason, "orphaned by server restart"));
   pthread_mutex_lock(&fixture_lock);
   cleanup_calls++;
   entered_cleanup = 1;
   pthread_cond_broadcast(&fixture_changed);
   while (hold_cleanup)
      pthread_cond_wait(&fixture_changed, &fixture_lock);
   assert(new_jobs == 0); /* A late retry must never cancel this generation. */
   int rc = -1;
   if (!unavailable)
   {
      rc = old_jobs;
      old_jobs = 0;
      if (lost_ack)
      {
         lost_ack = 0;
         rc = -1;
      }
   }
   pthread_mutex_unlock(&fixture_lock);
   return rc;
}

static void *admit(void *arg)
{
   int rc = server_delegate_startup_reconcile();
   if (rc == 0)
   {
      pthread_mutex_lock(&fixture_lock);
      new_jobs++;
      pthread_mutex_unlock(&fixture_lock);
   }
   *(int *)arg = rc;
   return NULL;
}

static void reset(void)
{
   /* Each scenario represents a fresh daemon; no production reset API exists. */
   g_startup_reconciled = 0;
   unavailable = lost_ack = cleanup_calls = new_jobs = ready_markers = 0;
   hold_cleanup = entered_cleanup = 0;
   old_jobs = 3;
}

static void test_unavailable_then_attach(void)
{
   reset();
   unavailable = 1;
   int rc;
   admit(&rc);
   assert(rc == -1 && old_jobs == 3 && new_jobs == 0 && cleanup_calls == 1);
   assert(server_delegate_startup_reconcile() == -1);
   assert(ready_markers == 0);
   unavailable = 0;
   admit(&rc);
   assert(rc == 0 && old_jobs == 0 && new_jobs == 1 && cleanup_calls == 3);
   /* Init, main tick and later admission share the same once-success latch. */
   for (int i = 0; i < 20; i++)
      assert(server_delegate_startup_reconcile() == 0);
   admit(&rc);
   assert(rc == 0 && new_jobs == 2 && cleanup_calls == 3 && ready_markers == 1);
}

static void test_concurrent_lost_ack(void)
{
   reset();
   lost_ack = hold_cleanup = 1;
   enum
   {
      N = 12
   };
   pthread_t threads[N];
   int results[N];
   assert(pthread_create(&threads[0], NULL, admit, &results[0]) == 0);
   pthread_mutex_lock(&fixture_lock);
   while (!entered_cleanup)
      pthread_cond_wait(&fixture_changed, &fixture_lock);
   pthread_mutex_unlock(&fixture_lock);
   for (int i = 1; i < N; i++)
      assert(pthread_create(&threads[i], NULL, admit, &results[i]) == 0);
   pthread_mutex_lock(&fixture_lock);
   assert(new_jobs == 0 && cleanup_calls == 1 && ready_markers == 0);
   hold_cleanup = 0;
   pthread_cond_broadcast(&fixture_changed);
   pthread_mutex_unlock(&fixture_lock);
   for (int i = 0; i < N; i++)
      assert(pthread_join(threads[i], NULL) == 0);
   assert(results[0] == -1);
   for (int i = 1; i < N; i++)
      assert(results[i] == 0);
   assert(old_jobs == 0 && new_jobs == N - 1 && cleanup_calls == 2);
   assert(server_delegate_startup_reconcile() == 0 && cleanup_calls == 2 && ready_markers == 1);
}

int main(void)
{
   test_unavailable_then_attach();
   test_concurrent_lost_ack();
   puts("delegate startup admission barrier: PASS");
   return 0;
}
