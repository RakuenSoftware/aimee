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
#include "config.h"
#include "request_context.h"
#include "db1.h"
#include <stdlib.h>
#include <sys/stat.h>
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

/* ── Real ingress-writer concurrency ─────────────────────────────────────────
 * Drives the ACTUAL ingress write site (agent_record_token_audit) — exercising
 * the request-context read, the config read (async decision), the cost authority,
 * and the audit insert — from multiple request threads PLUS a /v1/runs-style
 * worker (the ingress-source thread override + a captured request context), with
 * ingress_audit_async enabled in config. */

#define ING_REQ_THREADS 6
#define ING_PER_THREAD  150
#define RUNS_WORKER_N   300

static int ingress_total(void)
{
   return ING_REQ_THREADS * ING_PER_THREAD + RUNS_WORKER_N;
}

typedef struct
{
   int id;
   long max_ns;
} ingress_arg_t;

/* A request thread: establishes its own per-request context (request id +
 * per-client principal) the way populate_request_context() does, then makes the
 * exact call the OpenAI/Anthropic buffered+streaming handlers make. */
static void *ingress_request_thread(void *arg)
{
   ingress_arg_t *w = (ingress_arg_t *)arg;
   request_context_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.request_id, sizeof(ctx.request_id), "req-%d", w->id);
   snprintf(ctx.principal, sizeof(ctx.principal), "uid:%d", 7000 + w->id);
   request_context_set(&ctx);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   snprintf(result.agent_name, sizeof(result.agent_name), "ingressbot");
   snprintf(result.model, sizeof(result.model), "gpt-4o");
   result.prompt_tokens = 10;
   result.completion_tokens = 5;
   result.success = 1;
   for (int i = 0; i < ING_PER_THREAD; i++)
   {
      long t0 = now_ns();
      agent_record_token_audit(&result, "", "openai-ingress");
      long dt = now_ns() - t0;
      if (dt > w->max_ns)
         w->max_ns = dt;
   }
   request_context_clear();
   return NULL;
}

/* The /v1/runs worker: re-establishes the captured request context on its own
 * thread and tags spend via the ingress-source override, the way run_job_worker
 * does, then logs through the agent path (source "agent" -> retagged ingress). */
static void *runs_worker_thread(void *arg)
{
   ingress_arg_t *w = (ingress_arg_t *)arg;
   request_context_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.request_id, sizeof(ctx.request_id), "run-%d", w->id);
   snprintf(ctx.principal, sizeof(ctx.principal), "uid:runs");
   request_context_set(&ctx);
   agent_set_ingress_source("openai-ingress");

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   snprintf(result.agent_name, sizeof(result.agent_name), "runbot");
   snprintf(result.model, sizeof(result.model), "gpt-4o");
   result.prompt_tokens = 12;
   result.completion_tokens = 6;
   result.success = 1;
   for (int i = 0; i < RUNS_WORKER_N; i++)
   {
      long t0 = now_ns();
      agent_record_token_audit(&result, "execute", "agent");
      long dt = now_ns() - t0;
      if (dt > w->max_ns)
         w->max_ns = dt;
   }
   agent_set_ingress_source("");
   request_context_clear();
   return NULL;
}

static void test_real_ingress_writer_concurrency(void)
{
   pthread_t req[ING_REQ_THREADS], runs;
   ingress_arg_t reqargs[ING_REQ_THREADS], runargs = {.id = 0, .max_ns = 0};
   long t0 = now_ns();
   for (int i = 0; i < ING_REQ_THREADS; i++)
   {
      reqargs[i].id = i;
      reqargs[i].max_ns = 0;
      assert(pthread_create(&req[i], NULL, ingress_request_thread, &reqargs[i]) == 0);
   }
   assert(pthread_create(&runs, NULL, runs_worker_thread, &runargs) == 0);

   long tail = 0;
   for (int i = 0; i < ING_REQ_THREADS; i++)
   {
      pthread_join(req[i], NULL);
      if (reqargs[i].max_ns > tail)
         tail = reqargs[i].max_ns;
   }
   pthread_join(runs, NULL);
   if (runargs.max_ns > tail)
      tail = runargs.max_ns;

   /* Drain the async writer (if config enabled it) before counting. */
   agent_audit_async_flush();
   long total_us = (now_ns() - t0) / 1000;

   int landed = count_source("openai-ingress");
   int attempted = ingress_total();
   printf("  ingress: %d request inserts + %d /v1/runs inserts via "
          "agent_record_token_audit in %ld us, tail %ld us, drops=%d\n",
          ING_REQ_THREADS * ING_PER_THREAD, RUNS_WORKER_N, total_us, tail / 1000,
          attempted - landed);
   /* Acceptance: the real ingress write path, driven concurrently from request
    * threads + the /v1/runs worker, drops nothing. */
   assert(landed == attempted);
   assert(tail < 5L * 1000000000L);
}

/* Best-effort: point config at a temp home and enable ingress_audit_async so the
 * real-writer test exercises the async config path. Falls back to default config
 * (synchronous inline writes) if the temp config cannot be written. */
static void enable_async_config(void)
{
   const char *home = "/tmp/aimee_audit_load_home";
   mkdir(home, 0700);
   setenv("AIMEE_HOME", home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1); /* always re-read config */
   config_t cfg;
   if (config_load(&cfg) != 0)
      return;
   cfg.ingress_audit_async = 1;
   (void)config_save(&cfg);
}

int main(void)
{
   printf("token_audit load: DB1 write-concurrency acceptance\n");
   assert(db1_init(":memory:") == 0);
   test_sync_concurrency_no_drops();
   test_async_nonblocking_no_drops();
   enable_async_config();
   test_real_ingress_writer_concurrency();
   db1_shutdown();
   printf("All token_audit load tests passed.\n");
   return 0;
}
