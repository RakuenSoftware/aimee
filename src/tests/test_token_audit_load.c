/* test_token_audit_load.c: DB1 write-concurrency acceptance for the ingress
 * audit write sites (proposal §2 / "DB1 write concurrency").
 *
 * Validates the two acceptance criteria:
 *  1. Drop rate — many concurrent token_audit inserts from multiple threads (the
 *     request-thread + /v1/runs-worker pattern) all land: DB1's single
 *     SQLITE_OPEN_FULLMUTEX handle serializes writers, so internal contention
 *     never triggers SQLITE_BUSY and no row is silently dropped.
 *  2. Async mode does not block the request thread — handing a row to the
 *     background writer (agent_audit_async_enqueue_row) is materially cheaper than
 *     the synchronous DB insert, and after a flush every enqueued row has landed
 *     (drop rate 0).
 * Tail latency for each path is measured and reported. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aimee.h" /* MAX_PATH_LEN, pulled in before agent_types.h */
#include "agent_exec.h"
#include "db1.h"
#include <sqlite3.h>

extern sqlite3 *db1_conn(void);

static long now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static int count_source(const char *src)
{
   sqlite3_stmt *st = NULL;
   assert(sqlite3_prepare_v2(db1_conn(), "SELECT COUNT(*) FROM token_audit WHERE source = ?", -1,
                             &st, NULL) == SQLITE_OK);
   sqlite3_bind_text(st, 1, src, -1, SQLITE_TRANSIENT);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

#define SYNC_THREADS    8
#define SYNC_PER_THREAD 250

typedef struct
{
   int id;
   long max_ns; /* slowest single insert on this thread (tail) */
} sync_worker_t;

static void *sync_worker(void *arg)
{
   sync_worker_t *w = (sync_worker_t *)arg;
   char tool[32];
   snprintf(tool, sizeof(tool), "loadbot-%d", w->id);
   for (int i = 0; i < SYNC_PER_THREAD; i++)
   {
      db1_token_audit_row_t row = {.session_id = "load",
                                   .tool_name = tool,
                                   .role = "execute",
                                   .model = "gpt-4o",
                                   .source = "sync-load",
                                   .prompt_tokens = 10,
                                   .completion_tokens = 5,
                                   .estimated_cost_usd = 0.001};
      long t0 = now_ns();
      assert(db1_token_audit_insert(&row) == 0);
      long dt = now_ns() - t0;
      if (dt > w->max_ns)
         w->max_ns = dt;
   }
   return NULL;
}

static void test_sync_concurrency_no_drops(void)
{
   pthread_t th[SYNC_THREADS];
   sync_worker_t args[SYNC_THREADS];
   long t0 = now_ns();
   for (int i = 0; i < SYNC_THREADS; i++)
   {
      args[i].id = i;
      args[i].max_ns = 0;
      assert(pthread_create(&th[i], NULL, sync_worker, &args[i]) == 0);
   }
   long tail = 0;
   for (int i = 0; i < SYNC_THREADS; i++)
   {
      pthread_join(th[i], NULL);
      if (args[i].max_ns > tail)
         tail = args[i].max_ns;
   }
   long total_us = (now_ns() - t0) / 1000;

   int landed = count_source("sync-load");
   int attempted = SYNC_THREADS * SYNC_PER_THREAD;
   printf("  sync: %d threads x %d = %d inserts in %ld us, tail %ld us/insert, drops=%d\n",
          SYNC_THREADS, SYNC_PER_THREAD, attempted, total_us, tail / 1000, attempted - landed);
   /* Acceptance: zero drops under concurrency (the FULLMUTEX handle serializes
    * writers, so no busy-handler exhaustion). */
   assert(landed == attempted);
   /* Sanity bound on tail latency — catches a pathological stall, not a perf SLA. */
   assert(tail < 5L * 1000000000L);
}

#define ASYNC_N 250 /* <= the async ring so the batch is enqueued without inline fallback */

static void test_async_nonblocking_no_drops(void)
{
   db1_token_audit_row_t row = {.session_id = "load",
                                .tool_name = "asyncbot",
                                .role = "execute",
                                .model = "gpt-4o",
                                .prompt_tokens = 10,
                                .completion_tokens = 5,
                                .estimated_cost_usd = 0.001};

   /* Baseline: synchronous insert cost for the same batch size. */
   long s0 = now_ns();
   for (int i = 0; i < ASYNC_N; i++)
   {
      row.source = "sync-baseline";
      assert(db1_token_audit_insert(&row) == 0);
   }
   long sync_ns = now_ns() - s0;

   /* Async: hand the rows to the background writer. The enqueue is what the
    * request thread pays; it must be cheaper than the synchronous insert. */
   long a0 = now_ns();
   for (int i = 0; i < ASYNC_N; i++)
   {
      row.source = "async-load";
      if (agent_audit_async_enqueue_row(&row) != 0)
         (void)db1_token_audit_insert(&row); /* ring full -> inline, never dropped */
   }
   long enqueue_ns = now_ns() - a0;

   agent_audit_async_flush(); /* wait for the writer to drain */

   int landed = count_source("async-load");
   printf("  async: enqueue %d rows in %ld us vs sync insert %ld us; drops=%d\n", ASYNC_N,
          enqueue_ns / 1000, sync_ns / 1000, ASYNC_N - landed);
   /* Acceptance: every enqueued row lands (drop rate 0)... */
   assert(landed == ASYNC_N);
   /* ...and the request thread is not blocked on the DB — enqueuing is cheaper
    * than synchronously inserting the same batch. */
   assert(enqueue_ns < sync_ns);
}

int main(void)
{
   printf("token_audit load: DB1 write-concurrency acceptance\n");
   assert(db1_init(":memory:") == 0);
   test_sync_concurrency_no_drops();
   test_async_nonblocking_no_drops();
   db1_shutdown();
   printf("All token_audit load tests passed.\n");
   return 0;
}
