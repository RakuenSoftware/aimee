/* test_db1_agent_job_cancel_unassigned.c: the unassigned-lease transition.
 *
 * This decides whether a delegate job that nobody picked up gets cancelled. It
 * races a live worker, so every case here is about who wins:
 *   1. A pending job older than the age is cancelled, once.
 *   2. Routing may persist an agent name while the row is still pending, so
 *      pending counts as unassigned regardless of agent_name -- otherwise a
 *      routed-but-unstarted job could never be reclaimed.
 *   3. A running job WITH an agent is assigned and must be protected: cancelling
 *      it would kill work already in flight.
 *   4. A job younger than the supplied age is left alone, so a caller's policy
 *      actually bounds the transition.
 *   5. A job already terminal is not re-cancelled.
 *   6. A worker taking the lease and this cancellation racing cannot both win,
 *      and the stored status agrees with whichever did.
 *   7. Unusable arguments are rejected rather than cancelling something. */

#include <assert.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "agent_jobs.h"

int db1_init(const char *path);
void db1_shutdown(void);
sqlite3 *db1_conn(void);

static char tmp_db_path[256];

static void unlink_siblings(void)
{
   char path[300];
   unlink(tmp_db_path);
   snprintf(path, sizeof(path), "%s-wal", tmp_db_path);
   unlink(path);
   snprintf(path, sizeof(path), "%s-shm", tmp_db_path);
   unlink(path);
}

static void setup_db(void)
{
   snprintf(tmp_db_path, sizeof(tmp_db_path), "/tmp/test_db1_aj_cu_%d.sqlite", (int)getpid());
   unlink_siblings();
   assert(db1_init(tmp_db_path) == 0);
}

/* Age the row so it is older than the caller's threshold without sleeping. */
static void backdate(int job_id, int seconds)
{
   char sql[256];
   snprintf(sql, sizeof(sql),
            "UPDATE agent_jobs SET created_at = datetime('now', '-%d seconds'),"
            " heartbeat_at = '' WHERE id = %d",
            seconds, job_id);
   assert(sqlite3_exec(db1_conn(), sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void status_of(int job_id, char *out, size_t cap)
{
   db1_agent_job_t job;
   assert(db1_agent_job_get(job_id, &job) == 0);
   snprintf(out, cap, "%s", job.status);
   db1_agent_job_free(&job);
}

static int new_backdated_job(void)
{
   int job = db1_agent_job_create("review", "prompt", "", "owner");
   assert(job > 0);
   backdate(job, 600);
   return job;
}

static void test_expired_pending_job_is_cancelled_once(void)
{
   int job = new_backdated_job();
   assert(db1_agent_job_cancel_unassigned(job, "lease expired", 60) == 1);
   char status[DB1_AJ_STATUS_LEN];
   status_of(job, status, sizeof(status));
   assert(strcmp(status, "cancelled") == 0);
   /* A second attempt must not claim it cancelled the job again: the caller
    * treats true as "this call performed the transition". */
   assert(db1_agent_job_cancel_unassigned(job, "lease expired", 60) == 0);
}

/* Routing writes an agent name before the worker starts, so a pending row is
 * still unassigned. Reading agent_name as proof of assignment would strand
 * every routed-but-unstarted job forever. */
static void test_routed_pending_job_is_still_unassigned(void)
{
   int job = new_backdated_job();
   db1_agent_job_set_agent(job, "routed_agent");
   backdate(job, 600);
   assert(db1_agent_job_cancel_unassigned(job, "lease expired", 60) == 1);
}

static void test_assigned_running_job_is_protected(void)
{
   int job = new_backdated_job();
   assert(db1_agent_job_take_lease(job, "worker") == 0);
   db1_agent_job_set_agent(job, "worker_agent");
   backdate(job, 600);
   assert(db1_agent_job_cancel_unassigned(job, "lease expired", 60) == 0);
   char status[DB1_AJ_STATUS_LEN];
   status_of(job, status, sizeof(status));
   assert(strcmp(status, "running") == 0);
}

static void test_young_job_is_left_alone(void)
{
   int job = db1_agent_job_create("review", "prompt", "", "owner");
   assert(job > 0);
   assert(db1_agent_job_cancel_unassigned(job, "lease expired", 3600) == 0);
   char status[DB1_AJ_STATUS_LEN];
   status_of(job, status, sizeof(status));
   assert(strcmp(status, "pending") == 0);
}

static void test_terminal_job_is_not_recancelled(void)
{
   int job = new_backdated_job();
   db1_agent_job_update(job, "done", 1, "finished");
   assert(db1_agent_job_cancel_unassigned(job, "lease expired", 60) == 0);
   char status[DB1_AJ_STATUS_LEN];
   status_of(job, status, sizeof(status));
   assert(strcmp(status, "done") == 0);
}

/* A worker taking the lease and this cancellation can arrive together. Both
 * transitions are reciprocal -- taking the lease requires pending, cancelling
 * requires pending-or-unassigned-running -- so whichever SQLite update wins must
 * prevent the other from reviving the row. Exactly one may succeed, and the
 * stored status must agree with whichever did. */
typedef struct
{
   int job_id;
   int cancelled;
   int leased;
} race_state_t;

static void *race_cancel(void *arg)
{
   race_state_t *state = arg;
   state->cancelled = db1_agent_job_cancel_unassigned(state->job_id, "lease expired", 60);
   return NULL;
}

static void *race_lease(void *arg)
{
   race_state_t *state = arg;
   state->leased = db1_agent_job_take_lease(state->job_id, "worker") == 0;
   return NULL;
}

static void test_cancellation_and_worker_lease_cannot_both_win(void)
{
   for (int attempt = 0; attempt < 50; ++attempt)
   {
      int job = db1_agent_job_create("review", "prompt", "", "owner");
      assert(job > 0);
      /* Routing may have already named an agent while the row is pending. */
      db1_agent_job_set_agent(job, "routed_agent");
      backdate(job, 600);

      race_state_t state = {.job_id = job};
      pthread_t cancel_thread, lease_thread;
      assert(pthread_create(&cancel_thread, NULL, race_cancel, &state) == 0);
      assert(pthread_create(&lease_thread, NULL, race_lease, &state) == 0);
      pthread_join(cancel_thread, NULL);
      pthread_join(lease_thread, NULL);

      char status[DB1_AJ_STATUS_LEN];
      status_of(job, status, sizeof(status));
      /* The invariant is that they cannot both win. Under contention neither
       * may win -- a transient busy is a legitimate outcome and the row simply
       * stays pending for the next attempt -- so requiring a winner would make
       * this test flaky rather than strict. */
      assert(!(state.cancelled == 1 && state.leased));
      if (state.cancelled == 1)
         /* A cancelled job must never be observed running: that would mean a
          * worker revived it and is now doing work nobody will collect. */
         assert(strcmp(status, "cancelled") == 0);
      else if (state.leased)
         assert(strcmp(status, "running") == 0);
      else
         assert(strcmp(status, "pending") == 0);
   }
}

static void test_rejects_unusable_arguments(void)
{
   assert(db1_agent_job_cancel_unassigned(0, "reason", 60) == -1);
   assert(db1_agent_job_cancel_unassigned(1, "reason", 0) == -1);
}

int main(void)
{
   printf("db1_agent_job_cancel_unassigned: ");
   setup_db();

   test_expired_pending_job_is_cancelled_once();
   test_routed_pending_job_is_still_unassigned();
   test_assigned_running_job_is_protected();
   test_young_job_is_left_alone();
   test_terminal_job_is_not_recancelled();
   test_cancellation_and_worker_lease_cannot_both_win();
   test_rejects_unusable_arguments();

   db1_shutdown();
   unlink_siblings();
   printf("ok\n");
   return 0;
}
