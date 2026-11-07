/* Real dispatcher thread: durable ownership must precede every sweep. */
#define COORD_STARTUP_RETRY_MSECS 20
#include "../server/server_coord_dispatcher.c"
#include <assert.h>
#include <errno.h>
#include <time.h>

static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_changed = PTHREAD_COND_INITIALIZER;
static int lookup_status, lookup_calls, recovery_calls, recovery_successes;
static int recover_failures, write_failures, write_calls, sweep_calls, committed;
static int lost_write_ack;
static char durable_owner[DB1_COORD_DELEGATE_LEN], first_new_owner[DB1_COORD_DELEGATE_LEN];

static void changed(void)
{
   pthread_cond_broadcast(&fixture_changed);
}
int db1_runtime_state_lookup(const char *key, char *out, size_t cap)
{
   assert(!strcmp(key, COORD_BOOT_OWNER_KEY));
   pthread_mutex_lock(&fixture_lock);
   lookup_calls++;
   out[0] = 0;
   int status = lookup_status;
   if (!status)
      snprintf(out, cap, "%s", durable_owner);
   changed();
   pthread_mutex_unlock(&fixture_lock);
   return status;
}
/* Needed to run this same regression against the pre-fix dispatcher. */
int db1_runtime_state_get(const char *key, char *out, size_t cap)
{
   return db1_runtime_state_lookup(key, out, cap) == 0 ? 0 : -1;
}
int db1_coord_job_recover_owner(const char *owner, int max, int *requeued, int *failed)
{
   assert(!strcmp(owner, "previous-owner") && max == 3);
   pthread_mutex_lock(&fixture_lock);
   recovery_calls++;
   int rc = recover_failures ? (--recover_failures, -1) : 0;
   if (!rc)
      recovery_successes++;
   *requeued = *failed = 0;
   changed();
   pthread_mutex_unlock(&fixture_lock);
   return rc;
}
int db1_runtime_state_set(const char *key, const char *value)
{
   assert(!strcmp(key, COORD_BOOT_OWNER_KEY) && !strncmp(value, "coord-", 6));
   pthread_mutex_lock(&fixture_lock);
   assert(lookup_status >= 0); /* A read failure is never permission to replace an owner. */
   assert(lookup_status == 1 || recovery_successes == 1);
   if (!first_new_owner[0])
      snprintf(first_new_owner, sizeof first_new_owner, "%s", value);
   assert(!strcmp(first_new_owner, value)); /* Retry a lost acknowledgement with the SAME owner. */
   write_calls++;
   int rc = write_failures ? (--write_failures, -1) : 0;
   if (!rc || lost_write_ack)
      snprintf(durable_owner, sizeof durable_owner, "%s", value);
   if (!rc)
      committed = 1;
   changed();
   pthread_mutex_unlock(&fixture_lock);
   return rc;
}
int platform_random_bytes(void *out, size_t size)
{
   memset(out, 0x35, size);
   return 0;
}
int config_present(void)
{
   return 0;
}
int config_module_delegates(void)
{
   return 1;
}
int config_module_enabled(int config, int fallback)
{
   (void)config;
   return fallback;
}
int gw_orch_delegates_enabled(void)
{
   return 1;
}
int db1_coord_job_list_active_ids(int *ids, int max)
{
   (void)ids;
   (void)max;
   pthread_mutex_lock(&fixture_lock);
   assert(committed && durable_owner[0]);
   sweep_calls++;
   changed();
   pthread_mutex_unlock(&fixture_lock);
   return 0;
}
/* Unused work-execution branches stay linked without substituting the startup owner. */
int db1_coord_job_get(int id, db1_coord_job_t *job)
{
   (void)id;
   (void)job;
   return -1;
}
int db1_coord_job_claim_next(int id, const char *owner, db1_coord_task_t *task)
{
   (void)id;
   (void)owner;
   (void)task;
   assert(0);
   return -1;
}
int db1_coord_task_get_dispatch(int id, char *role, size_t role_n, aimee_message_t *message,
                                char *files, size_t files_n, char *cwd, size_t cwd_n, char *persona,
                                size_t persona_n)
{
   (void)id;
   (void)role;
   (void)role_n;
   (void)message;
   (void)files;
   (void)files_n;
   (void)cwd;
   (void)cwd_n;
   (void)persona;
   (void)persona_n;
   assert(0);
   return -1;
}
int db1_coord_job_fail_task_owned(int id, const char *owner, const char *error)
{
   (void)id;
   (void)owner;
   (void)error;
   assert(0);
   return -1;
}
int db1_coord_job_release_task(int id)
{
   (void)id;
   assert(0);
   return -1;
}
int delegate_role_is_write(const char *role)
{
   (void)role;
   assert(0);
   return 0;
}
int delegate_spawn_ondemand(compute_ctx_t *ctx)
{
   (void)ctx;
   assert(0);
   return -1;
}
void compute_ctx_free(compute_ctx_t *ctx)
{
   free(ctx);
}
int gw_orch_delegates_run(const gw_turn_capabilities_t *caps, const char *turn, const char *role,
                          const aimee_message_t *message, int enabled)
{
   (void)caps;
   (void)turn;
   (void)role;
   (void)message;
   (void)enabled;
   assert(0);
   return -1;
}

static void reset(int status)
{
   assert(!g_running);
   lookup_status = status;
   lookup_calls = recovery_calls = recovery_successes = write_calls = sweep_calls = committed = 0;
   recover_failures = write_failures = lost_write_ack = 0;
   first_new_owner[0] = 0;
   snprintf(durable_owner, sizeof durable_owner, "%s", status == 1 ? "" : "previous-owner");
}
static void wait_for(int *counter, int count)
{
   struct timespec deadline;
   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += 3;
   pthread_mutex_lock(&fixture_lock);
   while (*counter < count)
   {
      struct timespec tick;
      clock_gettime(CLOCK_REALTIME, &tick);
      assert(tick.tv_sec < deadline.tv_sec ||
             (tick.tv_sec == deadline.tv_sec && tick.tv_nsec < deadline.tv_nsec));
      tick.tv_nsec += 20000000L;
      if (tick.tv_nsec >= 1000000000L)
      {
         tick.tv_sec++;
         tick.tv_nsec -= 1000000000L;
      }
      int rc = pthread_cond_timedwait(&fixture_changed, &fixture_lock, &tick);
      assert(!rc || rc == ETIMEDOUT);
      /* Counter notification can precede the production wait. Re-signal on a
       * bounded tick rather than depending on catching that narrow window. */
      pthread_mutex_unlock(&fixture_lock);
      server_coord_dispatcher_notify();
      pthread_mutex_lock(&fixture_lock);
   }
   pthread_mutex_unlock(&fixture_lock);
}
static void test_attach_and_retry(void)
{
   reset(-1);
   server_coord_dispatcher_init(NULL);
   wait_for(&lookup_calls, 3);
   pthread_mutex_lock(&fixture_lock);
   assert(!write_calls && !recovery_calls && !sweep_calls);
   lookup_status = 0;
   recover_failures = 1;
   write_failures = 1;
   lost_write_ack = 1;
   pthread_mutex_unlock(&fixture_lock);
   server_coord_dispatcher_notify();
   wait_for(&sweep_calls, 1);
   pthread_mutex_lock(&fixture_lock);
   assert(recovery_calls == 2 && recovery_successes == 1 && write_calls == 2);
   int reads = lookup_calls;
   pthread_mutex_unlock(&fixture_lock);
   server_coord_dispatcher_notify();
   wait_for(&sweep_calls, 2);
   server_coord_dispatcher_shutdown();
   assert(lookup_calls == reads && recovery_calls == 2 && write_calls == 2);
}
static void test_explicit_missing(void)
{
   reset(1);
   server_coord_dispatcher_init(NULL);
   wait_for(&sweep_calls, 1);
   server_coord_dispatcher_shutdown();
   assert(lookup_calls == 1 && !recovery_calls && write_calls == 1);
}
static void test_stop_waiting(int during_write)
{
   reset(during_write ? 0 : -1);
   write_failures = 100;
   server_coord_dispatcher_init(NULL);
   wait_for(during_write ? &write_calls : &lookup_calls, 2);
   struct timespec start, stop;
   clock_gettime(CLOCK_MONOTONIC, &start);
   server_coord_dispatcher_shutdown();
   clock_gettime(CLOCK_MONOTONIC, &stop);
   double seconds = stop.tv_sec - start.tv_sec + (stop.tv_nsec - start.tv_nsec) / 1e9;
   assert(seconds < 1.0 && !sweep_calls && !committed);
   assert(!strcmp(durable_owner, "previous-owner"));
   assert(during_write ? recovery_successes == 1 : !recovery_calls && !write_calls);
}
int main(void)
{
   test_attach_and_retry();
   test_explicit_missing();
   test_stop_waiting(0);
   test_stop_waiting(1);
   puts("coord dispatcher: attach/recovery/write retries, no early sweep and responsive shutdown "
        "passed");
   return 0;
}
