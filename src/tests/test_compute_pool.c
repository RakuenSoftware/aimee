#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "compute_pool.h"
#include "config.h"

/* The unit-test-compute-pool link line does not include config.o; the
 * extern thread-local from headers/config.h needs a definition here so
 * the inlined aimee_resolve_compute_threads in test_compute_pool.c
 * resolves at link time. */
__thread int g_aimee_compute_threads_override = 0;

/* --- Test helpers --- */

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t counter_cond = PTHREAD_COND_INITIALIZER;
static int counter = 0;

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   int ready;
   int release;
} gate_t;

static void increment_counter(void *arg)
{
   int *val = (int *)arg;
   pthread_mutex_lock(&counter_mutex);
   counter += *val;
   pthread_cond_broadcast(&counter_cond);
   pthread_mutex_unlock(&counter_mutex);
}

static void set_flag(void *arg)
{
   int *flag = (int *)arg;
   *flag = 1;
}

static void gated_task(void *arg)
{
   gate_t *gate = (gate_t *)arg;
   pthread_mutex_lock(&gate->mutex);
   gate->ready++;
   pthread_cond_broadcast(&gate->cond);
   while (!gate->release)
      pthread_cond_wait(&gate->cond, &gate->mutex);
   pthread_mutex_unlock(&gate->mutex);
}

typedef struct
{
   gate_t gate;
   pool_job_kind_t kind;
   const char *descriptor;
} slot_gate_t;

static void gated_task_with_slot(void *arg)
{
   slot_gate_t *sg = (slot_gate_t *)arg;
   compute_pool_set_job(sg->kind, "%s", sg->descriptor);
   pthread_mutex_lock(&sg->gate.mutex);
   sg->gate.ready++;
   pthread_cond_broadcast(&sg->gate.cond);
   while (!sg->gate.release)
      pthread_cond_wait(&sg->gate.cond, &sg->gate.mutex);
   pthread_mutex_unlock(&sg->gate.mutex);
   compute_pool_clear_job();
}

int main(void)
{
   printf("compute_pool: ");

   /* --- Init and shutdown --- */
   {
      compute_pool_t pool;
      int rc = compute_pool_init(&pool, 2);
      assert(rc == 0);
      assert(pool.thread_count == 2);
      compute_pool_shutdown(&pool);
   }

   /* --- Submit and execute single task --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 2);

      int flag = 0;
      int rc = compute_pool_submit(&pool, set_flag, &flag);
      assert(rc == 0);

      pthread_mutex_lock(&counter_mutex);
      for (int i = 0; i < 100 && !flag; i++)
      {
         struct timespec ts;
         clock_gettime(CLOCK_REALTIME, &ts);
         ts.tv_nsec += 10000000;
         if (ts.tv_nsec >= 1000000000)
         {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
         }
         pthread_cond_timedwait(&counter_cond, &counter_mutex, &ts);
      }
      pthread_mutex_unlock(&counter_mutex);
      assert(flag == 1);

      compute_pool_shutdown(&pool);
   }

   /* --- Submit multiple tasks, all execute --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 4);

      counter = 0;
      int vals[8] = {1, 2, 3, 4, 5, 6, 7, 8};
      for (int i = 0; i < 8; i++)
         compute_pool_submit(&pool, increment_counter, &vals[i]);

      pthread_mutex_lock(&counter_mutex);
      while (counter != 36)
         pthread_cond_wait(&counter_cond, &counter_mutex);
      pthread_mutex_unlock(&counter_mutex);
      assert(counter == 36); /* 1+2+3+4+5+6+7+8 */

      compute_pool_shutdown(&pool);
   }

   /* --- Concurrent execution (tasks run in parallel, not serial) --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 4);

      gate_t gate = {
          .mutex = PTHREAD_MUTEX_INITIALIZER,
          .cond = PTHREAD_COND_INITIALIZER,
      };

      for (int i = 0; i < 4; i++)
         compute_pool_submit(&pool, gated_task, &gate);

      pthread_mutex_lock(&gate.mutex);
      while (gate.ready < 4)
         pthread_cond_wait(&gate.cond, &gate.mutex);
      gate.release = 1;
      pthread_cond_broadcast(&gate.cond);
      pthread_mutex_unlock(&gate.mutex);

      compute_pool_shutdown(&pool);
      assert(gate.ready == 4);
   }

   /* --- Queue overflow returns -1 --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 1);

      gate_t gate = {
          .mutex = PTHREAD_MUTEX_INITIALIZER,
          .cond = PTHREAD_COND_INITIALIZER,
      };
      assert(compute_pool_submit(&pool, gated_task, &gate) == 0);
      pthread_mutex_lock(&gate.mutex);
      while (gate.ready < 1)
         pthread_cond_wait(&gate.cond, &gate.mutex);
      pthread_mutex_unlock(&gate.mutex);

      int submitted = 1;
      int failed = 0;
      for (int i = 0; i < COMPUTE_QUEUE_SIZE + 8; i++)
      {
         int rc = compute_pool_submit(&pool, gated_task, &gate);
         if (rc == 0)
            submitted++;
         else
         {
            failed = 1;
            break;
         }
      }
      assert(failed == 1);
      assert(submitted >= COMPUTE_QUEUE_SIZE);

      pthread_mutex_lock(&gate.mutex);
      gate.release = 1;
      pthread_cond_broadcast(&gate.cond);
      pthread_mutex_unlock(&gate.mutex);
      compute_pool_shutdown(&pool);
   }

   /* --- Closing admission rejects new work while shutdown still drains --- */
   {
      compute_pool_t pool;
      assert(compute_pool_init(&pool, 1) == 0);
      gate_t gate = {
          .mutex = PTHREAD_MUTEX_INITIALIZER,
          .cond = PTHREAD_COND_INITIALIZER,
      };
      assert(compute_pool_submit(&pool, gated_task, &gate) == 0);
      pthread_mutex_lock(&gate.mutex);
      while (gate.ready < 1)
         pthread_cond_wait(&gate.cond, &gate.mutex);
      pthread_mutex_unlock(&gate.mutex);
      compute_pool_close(&pool);
      int flag = 0;
      assert(compute_pool_submit(&pool, set_flag, &flag) == -1);
      pthread_mutex_lock(&gate.mutex);
      gate.release = 1; /* represents dependency cancellation before the join */
      pthread_cond_broadcast(&gate.cond);
      pthread_mutex_unlock(&gate.mutex);
      compute_pool_shutdown(&pool);
      assert(flag == 0);
   }

   /* --- Thread count clamping --- */
   {
      compute_pool_t pool;
      /* Request an arbitrary nontrivial size */
      int rc = compute_pool_init(&pool, 16);
      assert(rc == 0);
      assert(pool.thread_count == 16);
      compute_pool_shutdown(&pool);

      /* Request zero -- should clamp to 1 */
      rc = compute_pool_init(&pool, 0);
      assert(rc == 0);
      assert(pool.thread_count >= 1);
      compute_pool_shutdown(&pool);
   }

   /* --- Default compute-thread resolver --- */
   {
      /* The harness may set compute thread env vars for parallel test
       * execution; clear them so the configured-arg branch is testable. */
      unsetenv("AIMEE_COMPUTE_THREADS");
      g_aimee_compute_threads_override = 0;
      int def = aimee_default_compute_threads();
      assert(def >= 1);
      assert(aimee_resolve_compute_threads(0) == def);
      assert(aimee_resolve_compute_threads(7) == 7);
      setenv("AIMEE_COMPUTE_THREADS", "3", 1);
      assert(aimee_resolve_compute_threads(0) == 3);
      unsetenv("AIMEE_COMPUTE_THREADS");
      assert(aimee_resolve_compute_threads(0) == def);
   }

   /* --- Graceful shutdown drains queue --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 2);

      counter = 0;
      int vals[4] = {10, 20, 30, 40};
      for (int i = 0; i < 4; i++)
         compute_pool_submit(&pool, increment_counter, &vals[i]);

      /* Shutdown should wait for all tasks to complete */
      compute_pool_shutdown(&pool);
      assert(counter == 100); /* 10+20+30+40 */
   }

   /* --- pool_job_kind_name coverage --- */
   {
      assert(strcmp(pool_job_kind_name(POOL_JOB_DELEGATE), "delegate") == 0);
      assert(strcmp(pool_job_kind_name(POOL_JOB_VERIFY), "verify") == 0);
      assert(strcmp(pool_job_kind_name(POOL_JOB_INGEST), "ingest") == 0);
      assert(strcmp(pool_job_kind_name(POOL_JOB_TOOL), "tool") == 0);
      assert(strcmp(pool_job_kind_name(POOL_JOB_CHAT), "chat") == 0);
      assert(pool_job_kind_name(POOL_JOB_KB_CURATOR) != NULL);
      assert(pool_job_kind_name(POOL_JOB_OTHER) != NULL);
      assert(pool_job_kind_name(POOL_JOB_NONE) != NULL);
   }

   /* --- slots_json idle baseline --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 1);
      char *json = compute_pool_slots_json(&pool);
      assert(json != NULL);
      /* Must be a JSON array with one idle slot */
      assert(strstr(json, "\"active\":false") != NULL || strstr(json, "\"active\": false") != NULL);
      assert(strstr(json, "\"index\"") != NULL);
      free(json);
      compute_pool_shutdown(&pool);
   }

   /* --- Slot lifecycle: active/idle while job in flight --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 2);

      slot_gate_t sg = {
          .gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER},
          .kind = POOL_JOB_DELEGATE,
          .descriptor = "role=code sess=test",
      };

      compute_pool_submit(&pool, gated_task_with_slot, &sg);

      /* Wait for worker to park (slot should now be active) */
      pthread_mutex_lock(&sg.gate.mutex);
      while (!sg.gate.ready)
         pthread_cond_wait(&sg.gate.cond, &sg.gate.mutex);
      pthread_mutex_unlock(&sg.gate.mutex);

      char *json = compute_pool_slots_json(&pool);
      assert(json != NULL);
      assert(strstr(json, "\"active\":true") != NULL || strstr(json, "\"active\": true") != NULL);
      assert(strstr(json, "delegate") != NULL);
      assert(strstr(json, "role=code") != NULL);
      free(json);

      /* Release the worker; slot should clear */
      pthread_mutex_lock(&sg.gate.mutex);
      sg.gate.release = 1;
      pthread_cond_broadcast(&sg.gate.cond);
      pthread_mutex_unlock(&sg.gate.mutex);

      compute_pool_shutdown(&pool);

      /* After shutdown all workers have returned; slots are torn down */
   }

   /* --- Secondary pool registry --- */
   {
      compute_pool_t pa, pb;
      compute_pool_init(&pa, 1);
      compute_pool_init(&pb, 1);

      compute_pool_register_secondary(&pa, "verify");
      compute_pool_register_secondary(&pb, "scratch");

      char *json = compute_pool_secondary_pools_json();
      assert(json != NULL);
      assert(strstr(json, "verify") != NULL);
      assert(strstr(json, "scratch") != NULL);
      free(json);

      compute_pool_unregister_secondary(&pa);
      json = compute_pool_secondary_pools_json();
      assert(json != NULL);
      assert(strstr(json, "verify") == NULL);
      assert(strstr(json, "scratch") != NULL);
      free(json);

      compute_pool_unregister_secondary(&pb);
      json = compute_pool_secondary_pools_json();
      assert(json != NULL);
      /* Both gone — either empty array or no pool names present */
      assert(strstr(json, "scratch") == NULL);
      free(json);

      compute_pool_shutdown(&pa);
      compute_pool_shutdown(&pb);
   }

   printf("all tests passed\n");
   return 0;
}
