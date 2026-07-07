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

#include "db2/db2.h" /* db2_lease_release_idle */
#include "db2/canonical_index.h"
#include "db2/kb_runtime_state.h"
#include "db2/kb_service_backend.h"
#include "db2/kb_payload.h"
#include "db2/db_postgres.h"
#include "db2/lifecycle.h"
#include "db2/pgvec_kb_service.h"
#include "kb_doc_hash.h"
#include "memory.h"

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
 * default embedder is pplx-embed-v1-4b (2560-dim). Advisory only — the real
 * column dimension comes from the schema (see kbiw_process_job). */
#define KB_DEFAULT_DIM 2560

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
         char pname[256];
         char pws[256];
         workspace_repo_index_keys(projects[i], cfg.workspaces[w], pname, sizeof(pname), pws,
                                   sizeof(pws));
         db2_kb_ingest_queue_enqueue(pname, projects[i], pws, 0);
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
   const char *embed_cmd = config_embedding_command(&cfg, NULL);

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
   /* canonical_index_scan_project returns the number of files scanned (>= 0) on
    * success and a negative value on error — only a negative is a failure. The
    * old `!= 0` check wrongly failed every project that scanned >= 1 file (i.e.
    * any non-empty project), marking the ingest job "failed" even though the
    * embeddings landed. Match the contract the HTTP scan route already uses. */
   if (canonical_index_scan_project(job->project, job->root_path, job->force, &inspected) < 0)
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

      /* Drain the queue; each worker claims independently. Bracket the burst in
       * a DB2 lease so the pooled connection is returned to the pool between
       * bursts (WP-C) instead of held for the worker thread's life. */
      db2_lease_begin();
      while (kbiw_claim_and_process() == 1)
      {
         if (ctx->ingest_stop)
            break;
      }
      db2_lease_end();
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
            char pname[256];
            char pws[256];
            workspace_repo_index_keys(watches[j].root, watches[j].workspace, pname, sizeof(pname),
                                      pws, sizeof(pws));
            db2_kb_ingest_queue_enqueue(pname, watches[j].root, pws, 0);
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

/* ---- KB document ingestion (the in-ingest replacement for `kb build`) ---- */

/* Chunk an in-memory document — used when the content lives in DB2 (pushed by a
 * thin client) or comes from a non-file source (e.g. a PDF converted to text),
 * rather than being read from local disk. Reuses kb.c's heading-aware chunker. */
static int chunk_content(const char *content, size_t len, text_chunk_t *chunks, int max_chunks)
{
   if (!content)
      return 0;
   FILE *f = fmemopen((void *)content, len, "r");
   if (!f)
      return 0;
   int n = chunk_stream(f, chunks, max_chunks);
   fclose(f);
   return n;
}

/* Upper bound on a whole-file body stored verbatim in kb_file_index.content.
 * Larger files are still chunked/embedded for search; only the whole-file copy
 * (served by GET /v1/kb/file) is skipped, to bound DB2 growth. */
#define KB_FILE_INDEX_MAX_BYTES (4 * 1024 * 1024)

/* Read a whole file into a malloc'd, NUL-terminated buffer (NULL on error/oversize). */
static char *kb_read_file_all(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   char *buf = NULL;
   if (fseek(f, 0, SEEK_END) == 0)
   {
      long len = ftell(f);
      if (len >= 0 && len <= KB_FILE_INDEX_MAX_BYTES && fseek(f, 0, SEEK_SET) == 0 &&
          (buf = malloc((size_t)len + 1)) != NULL)
      {
         size_t got = fread(buf, 1, (size_t)len, f);
         buf[got] = '\0';
      }
   }
   fclose(f);
   return buf;
}

void kb_file_index_store_from_path(const char *project, const char *file_path, const char *hash,
                                   const char *src_path)
{
   char *full = kb_read_file_all(src_path);
   db2_kb_file_index_upsert(project, file_path, hash, full);
   free(full);
}

/* General document-ingest entry point. Chunk one document's text `content` into
 * kb_documents and embed each chunk into the KB vector store under `project`,
 * keyed by `source_path`. Source-agnostic by design: the workspace doc-refresh
 * below feeds it from DB2 file_contents today, and future sources (e.g. a PDF
 * converted to text) call this same function. Skips work when the content hash
 * is unchanged. Returns chunks embedded, or -1 on error. */
int kb_ingest_doc_content(const char *project, const char *source_path, const char *content,
                          size_t len, const char *embedding_cmd)
{
   if (!project || !project[0] || !source_path || !source_path[0] || !content ||
       !db2_is_initialized())
      return -1;
   const char *effective_cmd = kb_effective_embedding_cmd(embedding_cmd);

   char hash[KB_DOC_HASH_HEX_LEN + 1];
   kb_doc_content_hash_for_path(source_path, content, (int)len, hash);

   char stored[KB_DOC_HASH_HEX_LEN + 1] = "";
   if (db2_kb_documents_get_stored_hash(project, source_path, stored, sizeof(stored)) == 1 &&
       strcmp(stored, hash) == 0)
      return 0; /* unchanged — already ingested */

   delete_file_chunks(project, source_path);

   text_chunk_t *chunks = malloc(MAX_CHUNKS_PER_FILE * sizeof(text_chunk_t));
   if (!chunks)
      return -1;
   int n_chunks = chunk_content(content, len, chunks, MAX_CHUNKS_PER_FILE);

   int embedded = 0;
   int64_t prev_doc_id = 0;
   for (int ci = 0; ci < n_chunks; ci++)
   {
      int64_t doc_id = db2_kb_documents_insert_chunk(
          project, source_path, hash, ci, chunks[ci].heading_path, chunks[ci].line_start,
          chunks[ci].line_end, chunks[ci].content, chunks[ci].token_count);
      if (doc_id < 0)
         continue;
      db2_kb_documents_link_neighbours(doc_id, prev_doc_id);
      prev_doc_id = doc_id;
      if (!effective_cmd[0])
         continue;

      char embed_text[4096];
      kb_async_make_embed_text(chunks[ci].heading_path, chunks[ci].content, embed_text,
                               sizeof(embed_text));
      /* Drop the pool lease before the (slow) embedder round-trip so a long
       * doc-ingest loop can't pin a connection past the 300s stuck-lease ceiling
       * and wedge the drain. No-op inside an explicit lease scope; DB writes below
       * re-acquire lazily. See kb_curator_extract_code / kb_service_code_embed. */
      db2_lease_release_idle();
      float vec[EMBED_MAX_DIM];
      int dim = memory_embed_text(embed_text, effective_cmd, vec, EMBED_MAX_DIM);
      if (dim > 0)
      {
         accept_generated_embedding(doc_id, vec, dim);
         sync_vector_embedding(doc_id, vec, dim);
         embedded++;
      }
   }
   free(chunks);
   /* Store the whole file body too (served by GET /v1/kb/file), not just chunks. */
   db2_kb_file_index_upsert(project, source_path, hash, content);
   return embedded;
}

/* Background driver (the in-ingest replacement for the old `kb build` command):
 * ingest indexed prose/doc files for `project` that aren't in the KB-docs layer
 * yet, reading content straight from DB2 file_contents (no disk — works for the
 * thin-client push deploy). Bounded by `max_docs` per call so the curator drain
 * makes steady progress without monopolising. Returns chunks embedded. */
int kb_doc_refresh(const char *project, const char *embedding_cmd, int max_docs)
{
   if (!project || !project[0] || !db2_is_initialized())
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (max_docs <= 0)
      max_docs = 200;

   /* Prose/doc files only (code is served by the code-embedding layer). Pull
    * only files with no chunks in kb_documents yet, bounded. */
   static const char *sql =
       "SELECT f.path, fc.content FROM files f"
       " JOIN file_contents fc ON fc.file_id = f.id"
       " JOIN projects p ON f.project_id = p.id"
       " WHERE p.name = ?1"
       "   AND (f.path LIKE '%.md' OR f.path LIKE '%.markdown' OR f.path LIKE '%.rst'"
       "        OR f.path LIKE '%.txt' OR f.path LIKE '%.adoc' OR f.path LIKE '%.org')"
       "   AND NOT EXISTS (SELECT 1 FROM kb_documents kd"
       "                   WHERE kd.project = p.name AND kd.file_path = f.path)"
       " LIMIT ?2";

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_int(st, "?2", max_docs);

   /* Collect rows first: kb_ingest_doc_content issues its own statements, and
    * the pg layer allows only one active statement on a connection. */
   typedef struct
   {
      char path[1024];
      char *content;
   } doc_row_t;
   doc_row_t *rows = calloc((size_t)max_docs, sizeof(doc_row_t));
   int n = 0;
   if (rows)
   {
      while (n < max_docs && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *path = aimee_pg_column_text(st, 0);
         const char *body = aimee_pg_column_text(st, 1);
         if (!path || !body)
            continue;
         snprintf(rows[n].path, sizeof(rows[n].path), "%s", path);
         rows[n].content = strdup(body);
         if (rows[n].content)
            n++;
      }
   }
   aimee_pg_finalize(st);

   int embedded = 0;
   for (int i = 0; i < n; i++)
   {
      int e = kb_ingest_doc_content(project, rows[i].path, rows[i].content, strlen(rows[i].content),
                                    embedding_cmd);
      if (e > 0)
         embedded += e;
      free(rows[i].content);
   }
   free(rows);
   return embedded;
}

/* Self-healing embedding backfill: embed kb_documents chunks that exist but have
 * NO kb_embeddings row. kb_doc_refresh's gate is "file has no chunks yet", so a
 * chunk left unembedded never gets a second chance — this covers the cases that
 * leaves behind: a partial ingest, an embedder that was unavailable at ingest
 * time (embedding_command added later), or a vector-store reset that drops the
 * halfvec columns while the chunk text survives (e.g. an embedding-dim change).
 * Re-embeds in place (no re-chunk), bounded per call. Returns chunks embedded. */
int kb_doc_embed_backfill(const char *project, const char *embedding_cmd, int max_chunks)
{
   if (!project || !project[0] || !db2_is_initialized())
      return -1;
   const char *effective_cmd = kb_effective_embedding_cmd(embedding_cmd);
   if (!effective_cmd[0])
      return 0; /* no embedder configured — nothing to do */
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (max_chunks <= 0)
      max_chunks = 200;

   static const char *sql =
       "SELECT kd.id, kd.heading_path, kd.content FROM kb_documents kd"
       " WHERE kd.project = ?1"
       "   AND NOT EXISTS (SELECT 1 FROM kb_embeddings ke WHERE ke.point_id = kd.id)"
       " LIMIT ?2";

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_int(st, "?2", max_chunks);

   /* Collect rows first: sync_vector_embedding issues its own statements and the
    * pg layer allows only one active statement per connection. */
   typedef struct
   {
      int64_t id;
      char heading[512];
      char *content;
   } chunk_row_t;
   chunk_row_t *rows = calloc((size_t)max_chunks, sizeof(chunk_row_t));
   int n = 0;
   if (rows)
   {
      while (n < max_chunks && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         int64_t id = aimee_pg_column_int64(st, 0);
         const char *heading = aimee_pg_column_text(st, 1);
         const char *body = aimee_pg_column_text(st, 2);
         if (id <= 0 || !body)
            continue;
         rows[n].id = id;
         snprintf(rows[n].heading, sizeof(rows[n].heading), "%s", heading ? heading : "");
         rows[n].content = strdup(body);
         if (rows[n].content)
            n++;
      }
   }
   aimee_pg_finalize(st);

   int embedded = 0;
   for (int i = 0; i < n; i++)
   {
      char embed_text[4096];
      kb_async_make_embed_text(rows[i].heading, rows[i].content, embed_text, sizeof(embed_text));
      /* Drop the pool lease before the embedder round-trip (see above): this
       * backfill loop embeds up to max_chunks per project across every project,
       * so holding the lease across it trips the stuck-lease ceiling. */
      db2_lease_release_idle();
      float vec[EMBED_MAX_DIM];
      int dim = memory_embed_text(embed_text, effective_cmd, vec, EMBED_MAX_DIM);
      if (dim > 0 && sync_vector_embedding(rows[i].id, vec, dim) == 0)
         embedded++;
      free(rows[i].content);
   }
   free(rows);
   return embedded;
}
