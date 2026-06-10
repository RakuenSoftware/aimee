/* kb_ingest_workers.c: aimee-kb's in-process KB ingest driver.
 *
 * aimee-kb owns DB2/DB3, so it claims ingest jobs straight off the DB2 queue
 * (db2_kb_ingest_queue_claim_next, which uses FOR UPDATE SKIP LOCKED and is
 * safe for concurrent claimers) and runs the full build in-process —
 * kb_build() (compute + store) then canonical_index_scan_project(). No RPC
 * round-trip back to a server-side compute pool.
 *
 * This module owns up to KB_WORKER_MAX worker threads plus a periodic
 * enqueue timer and (Linux) an inotify watcher, all hung off kb_service_ctx.
 * It replaces the former server_kb_workers.c dispatcher that existed while
 * ingest compute lived in aimee-server.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "aimee.h"
#include "config.h"
#include "kb.h"
#include "kb_background.h"
#include "kb_ingest_workers.h"
#include "kb_service.h"
#include "log.h"
#include "workspace.h"

#include "db2/canonical_index.h"
#include "db2/kb_runtime_state.h"
#include "db2/kb_service_backend.h"
#include "db2/lifecycle.h"
#include "db2/pgvec_kb_service.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef AIMEE_WINDOWS
#include <unistd.h>
#include <poll.h>
#include <sys/resource.h>
#endif

/* Fallback kb_embeddings dimension when config.embedding_dim is unset; the
 * default embedder is pplx-embed-v1-0.6b (1024-dim). */
#define KB_DEFAULT_DIM 1024

/* ------------------------------------------------------------------ */
/* notify: wake parked workers (called by kb_handle_ingest on enqueue) */
/* ------------------------------------------------------------------ */

void kb_worker_notify(kb_service_ctx_t *ctx)
{
   if (!ctx)
      return;
   pthread_mutex_lock(&ctx->ingest_mu);
   pthread_cond_broadcast(&ctx->ingest_cond);
   pthread_mutex_unlock(&ctx->ingest_mu);
}

/* ------------------------------------------------------------------ */
/* Periodic enqueue-all (DB2-direct)                                   */
/* ------------------------------------------------------------------ */

static void kbiw_enqueue_all(kb_service_ctx_t *ctx)
{
   config_t cfg;
   config_load(&cfg);
   if (!cfg.kb_bg_ingest_enabled || !db2_is_initialized())
      return;

   char(*projects)[MAX_PATH_LEN] = calloc(MAX_DISCOVERED_PROJECTS, MAX_PATH_LEN);
   if (!projects)
      return;

   int total = 0;
   for (int w = 0; w < cfg.workspace_count; w++)
   {
      int n = workspace_discover_projects(cfg.workspaces[w], 3, projects, MAX_DISCOVERED_PROJECTS);
      for (int i = 0; i < n; i++)
      {
         const char *pname = strrchr(projects[i], '/');
         pname = pname ? pname + 1 : projects[i];
         db2_kb_ingest_queue_enqueue(pname, projects[i], cfg.workspaces[w], 0);
         total++;
      }
   }
   free(projects);

   if (total > 0)
   {
      kb_worker_notify(ctx);
      aimee_log(LOG_INFO, "kb.ingest.timer", "enqueued %d project(s) for ingest", total);
   }
}

/* ------------------------------------------------------------------ */
/* Per-job build                                                       */
/* ------------------------------------------------------------------ */

static void kbiw_process_job(const db2_kb_ingest_job_t *job)
{
   aimee_log(LOG_INFO, "kb.ingest.worker", "picked up project='%s' (force=%d)", job->project,
             job->force);
   kb_background_set("ingest", "project=%s phase=build", job->project);

   /* The kb_embeddings vector column dimension comes from the schema (sized to
    * the deployment's configured embedding_dim); pgvec_ensure_index infers the
    * dimension from the data, so the value passed here is advisory only. */
   if (pgvec_kb_service_ensure_kb_collection(KB_DEFAULT_DIM) != 0)
   {
      aimee_log(LOG_WARN, "kb.ingest.worker", "vector store unavailable for project='%s'",
                job->project);
      db2_kb_ingest_queue_fail(job->id, "vector store unavailable");
      kb_background_clear("ingest");
      return;
   }

   config_t cfg;
   config_load(&cfg);
   const char *embed_cmd = cfg.embedding_command[0] ? cfg.embedding_command : "builtin";

   kb_stats_t stats;
   memset(&stats, 0, sizeof(stats));
   int rc = kb_build(job->root_path, job->project, embed_cmd, job->force, &stats);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "kb.ingest.worker", "kb_build failed for project='%s'", job->project);
      db2_kb_ingest_queue_fail(job->id, "kb_build failed");
      kb_background_clear("ingest");
      return;
   }

   kb_background_set("ingest", "project=%s phase=scan", job->project);
   int inspected = 0;
   if (canonical_index_scan_project(job->project, job->root_path, job->force, &inspected) != 0)
   {
      aimee_log(LOG_WARN, "kb.ingest.worker", "canonical index scan failed for project='%s'",
                job->project);
      db2_kb_ingest_queue_fail(job->id, "canonical index scan failed");
      kb_background_clear("ingest");
      return;
   }

   db2_kb_ingest_queue_complete(job->id, stats.files_indexed, stats.chunks_added,
                                stats.embeddings_added);
   db2_kb_runtime_state_set_now("last_ingest_at");
   kb_background_clear("ingest");

   aimee_log(LOG_INFO, "kb.ingest.worker", "done: project='%s' files=%d chunks=%d embeddings=%d",
             job->project, stats.files_indexed, stats.chunks_added, stats.embeddings_added);
}

/* Claim one job and process it. Returns 1 if a job was processed, 0 if the
 * queue was empty or DB2 is unavailable. db2_kb_ingest_queue_claim_next uses
 * FOR UPDATE SKIP LOCKED, so concurrent workers never claim the same row. */
static int kbiw_claim_and_process(void)
{
   if (!db2_is_initialized())
      return 0;
   db2_kb_ingest_job_t job;
   int rc = db2_kb_ingest_queue_claim_next(&job);
   if (rc != 1)
      return 0; /* 0 = empty, -1 = transient error */
   if (job.id <= 0 || !job.project[0] || !job.root_path[0])
      return 0;
   kbiw_process_job(&job);
   return 1;
}

/* ------------------------------------------------------------------ */
/* Worker thread: park on cond, drain the queue when woken             */
/* ------------------------------------------------------------------ */

static void *kbiw_worker_thread(void *arg)
{
   kb_service_ctx_t *ctx = (kb_service_ctx_t *)arg;
#ifndef AIMEE_WINDOWS
   /* Yield CPU priority to user-facing socket threads. */
   setpriority(PRIO_PROCESS, 0, 5);
#endif
   for (;;)
   {
      pthread_mutex_lock(&ctx->ingest_mu);
      if (ctx->ingest_stop)
      {
         pthread_mutex_unlock(&ctx->ingest_mu);
         break;
      }
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;
      pthread_cond_timedwait(&ctx->ingest_cond, &ctx->ingest_mu, &ts);
      int stop = ctx->ingest_stop;
      pthread_mutex_unlock(&ctx->ingest_mu);
      if (stop)
         break;

      /* Drain the queue; each worker claims independently. */
      while (kbiw_claim_and_process() == 1)
      {
         if (ctx->ingest_stop)
            break;
      }
   }
   return NULL;
}

/* ------------------------------------------------------------------ */
/* Periodic enqueue timer                                              */
/* ------------------------------------------------------------------ */

static void *kbiw_timer_thread(void *arg)
{
   kb_service_ctx_t *ctx = (kb_service_ctx_t *)arg;

   /* Fire once on startup. */
   kbiw_enqueue_all(ctx);

   for (;;)
   {
      config_t cfg;
      config_load(&cfg);
      int interval_secs = cfg.kb_bg_ingest_interval_hours * 3600;
      if (interval_secs <= 0)
         interval_secs = 6 * 3600;

      int slept = 0;
      while (slept < interval_secs)
      {
         if (ctx->ingest_stop)
            return NULL;
         int chunk = (interval_secs - slept > 5) ? 5 : (interval_secs - slept);
#ifndef AIMEE_WINDOWS
         sleep((unsigned int)chunk);
#endif
         slept += chunk;
      }
      if (ctx->ingest_stop)
         return NULL;

      config_load(&cfg);
      if (cfg.kb_bg_ingest_enabled)
         kbiw_enqueue_all(ctx);
   }
}

/* ------------------------------------------------------------------ */
/* inotify watch thread (Linux only)                                   */
/* ------------------------------------------------------------------ */

#ifdef __linux__
#include <sys/inotify.h>

static void *kbiw_watch_thread(void *arg)
{
   kb_service_ctx_t *ctx = (kb_service_ctx_t *)arg;

   int ifd = inotify_init1(IN_NONBLOCK);
   if (ifd < 0)
   {
      aimee_log(LOG_WARN, "kb.ingest.watch", "inotify_init1 failed");
      return NULL;
   }

   config_t cfg;
   config_load(&cfg);

   /* Heap-allocate the watch table: at 512 * (2 * MAX_PATH_LEN + ...) bytes it
    * is ~4.2 MB, which overflows the default 8 MB pthread stack and segfaults
    * this thread at startup. Keep it off the stack. */
   struct kbiw_watch_entry
   {
      int wd;
      char root[MAX_PATH_LEN];
      char workspace[MAX_PATH_LEN];
      time_t last_queued;
   };
   struct kbiw_watch_entry *watches = calloc(512, sizeof(struct kbiw_watch_entry));
   if (!watches)
   {
      close(ifd);
      return NULL;
   }
   int nwatches = 0;

   char(*projects)[MAX_PATH_LEN] = calloc(MAX_DISCOVERED_PROJECTS, MAX_PATH_LEN);
   if (!projects)
   {
      free(watches);
      close(ifd);
      return NULL;
   }

   for (int w = 0; w < cfg.workspace_count && nwatches < 512; w++)
   {
      int n = workspace_discover_projects(cfg.workspaces[w], 3, projects, MAX_DISCOVERED_PROJECTS);
      for (int i = 0; i < n && nwatches < 512; i++)
      {
         int wd = inotify_add_watch(ifd, projects[i],
                                    IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
         if (wd < 0)
            continue;
         watches[nwatches].wd = wd;
         snprintf(watches[nwatches].root, MAX_PATH_LEN, "%s", projects[i]);
         snprintf(watches[nwatches].workspace, MAX_PATH_LEN, "%s", cfg.workspaces[w]);
         watches[nwatches].last_queued = 0;
         nwatches++;
      }
   }
   free(projects);
   aimee_log(LOG_INFO, "kb.ingest.watch", "watching %d project root(s)", nwatches);

   char evbuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
   while (!ctx->ingest_stop)
   {
      struct pollfd pfd = {.fd = ifd, .events = POLLIN};
      if (poll(&pfd, 1, 1000) <= 0)
         continue;

      ssize_t n = read(ifd, evbuf, sizeof(evbuf));
      if (n <= 0)
         continue;

      config_load(&cfg);
      int debounce = cfg.kb_bg_watch_debounce_secs;
      time_t now = time(NULL);

      for (char *p = evbuf; p < evbuf + n;)
      {
         struct inotify_event *ev = (struct inotify_event *)p;
         p += sizeof(*ev) + ev->len;
         for (int j = 0; j < nwatches; j++)
         {
            if (watches[j].wd != ev->wd)
               continue;
            if (now - watches[j].last_queued < debounce)
               break;
            const char *pname = strrchr(watches[j].root, '/');
            pname = pname ? pname + 1 : watches[j].root;
            db2_kb_ingest_queue_enqueue(pname, watches[j].root, watches[j].workspace, 0);
            watches[j].last_queued = now;
            kb_worker_notify(ctx);
            break;
         }
      }
   }
   free(watches);
   close(ifd);
   return NULL;
}
#else
static void *kbiw_watch_thread(void *arg)
{
   (void)arg;
   return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/* Start / stop                                                        */
/* ------------------------------------------------------------------ */

void kb_ingest_workers_start(kb_service_ctx_t *ctx)
{
   if (!ctx)
      return;

   pthread_mutex_init(&ctx->ingest_mu, NULL);
   pthread_cond_init(&ctx->ingest_cond, NULL);
   ctx->ingest_stop = 0;
   ctx->ingest_count = 0;
   ctx->ingest_timer_active = 0;
   ctx->bg_watch_active = 0;

   config_t cfg;
   config_load(&cfg);
   int cap = cfg.kb_worker_count;
   if (cap < 0)
      cap = 0;
   if (cap > KB_WORKER_MAX)
      cap = KB_WORKER_MAX;

   if (cap == 0 || !db2_is_initialized())
   {
      aimee_log(LOG_INFO, "kb.ingest", "ingest workers disabled (cap=%d, db2=%d)", cap,
                db2_is_initialized());
      return;
   }

   for (int i = 0; i < cap; i++)
   {
      if (pthread_create(&ctx->ingest_threads[i], NULL, kbiw_worker_thread, ctx) == 0)
         ctx->ingest_count++;
      else
         aimee_log(LOG_ERROR, "kb.ingest", "failed to start ingest worker %d", i);
   }
   aimee_log(LOG_INFO, "kb.ingest", "ingest workers started (%d thread(s))", ctx->ingest_count);

   if (ctx->ingest_count == 0)
      return;

   if (cfg.kb_bg_ingest_enabled)
   {
      if (pthread_create(&ctx->ingest_timer_thread, NULL, kbiw_timer_thread, ctx) == 0)
         ctx->ingest_timer_active = 1;
   }

   if (cfg.kb_bg_watch_enabled)
   {
      if (pthread_create(&ctx->bg_watch_thread, NULL, kbiw_watch_thread, ctx) == 0)
         ctx->bg_watch_active = 1;
   }
}

void kb_ingest_workers_stop(kb_service_ctx_t *ctx)
{
   if (!ctx)
      return;

   pthread_mutex_lock(&ctx->ingest_mu);
   ctx->ingest_stop = 1;
   pthread_cond_broadcast(&ctx->ingest_cond);
   pthread_mutex_unlock(&ctx->ingest_mu);

   for (int i = 0; i < ctx->ingest_count; i++)
      pthread_join(ctx->ingest_threads[i], NULL);
   ctx->ingest_count = 0;

   if (ctx->ingest_timer_active)
   {
      pthread_join(ctx->ingest_timer_thread, NULL);
      ctx->ingest_timer_active = 0;
   }
   if (ctx->bg_watch_active)
   {
      pthread_join(ctx->bg_watch_thread, NULL);
      ctx->bg_watch_active = 0;
   }
}
