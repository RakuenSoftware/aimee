/* server_session_pools.c: per-aimee-session execution pools */
#include "server.h"
#include "config.h"
#include "compute_pool.h"
#include "log.h"
#include "request_context.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   server_ctx_t *ctx;
   server_session_pool_t *entry;
   void (*fn)(void *);
   void *arg;
   request_context_t request_context;
   int has_request_context;
} session_pool_work_t;

static int session_id_usable(const char *session_id)
{
   return session_id && session_id[0] && strlen(session_id) < SERVER_SESSION_ID_MAX;
}

static void session_pool_shutdown_entry(server_session_pool_t *entry)
{
   if (!entry || !entry->initialized)
      return;
   compute_pool_shutdown(&entry->pool);
   memset(entry, 0, sizeof(*entry));
}

static void *session_pool_shutdown_thread(void *arg)
{
   session_pool_shutdown_entry((server_session_pool_t *)arg);
   return NULL;
}

static void session_pool_shutdown_entry_async(server_session_pool_t *entry)
{
   pthread_t tid;
   if (pthread_create(&tid, NULL, session_pool_shutdown_thread, entry) == 0)
   {
      pthread_detach(tid);
      return;
   }

   LOG_WARN("server.session_pool", "failed to start session-pool shutdown thread for %s",
            entry ? entry->session_id : "");
}

static void session_pool_release_job(server_ctx_t *ctx, server_session_pool_t *entry)
{
   server_session_pool_t *to_shutdown = NULL;

   pthread_mutex_lock(&ctx->session_pools_mutex);
   if (entry->active_jobs > 0)
      entry->active_jobs--;
   if (entry->active_jobs == 0 && entry->close_requested && !entry->shutdown_started)
   {
      entry->shutdown_started = 1;
      to_shutdown = entry;
   }
   pthread_mutex_unlock(&ctx->session_pools_mutex);

   if (to_shutdown)
      session_pool_shutdown_entry_async(to_shutdown);
}

static void session_pool_worker(void *arg)
{
   session_pool_work_t *work = (session_pool_work_t *)arg;
   /* Identity, restrictive budgets and host refusals must survive the thread
    * handoff. Always clear afterward so a reused worker cannot leak a task. */
   request_context_set(work->has_request_context ? &work->request_context : NULL);
   if (work->fn)
      work->fn(work->arg);
   request_context_clear();
   session_pool_release_job(work->ctx, work->entry);
   free(work);
}

static server_session_pool_t *session_pool_find_or_create(server_ctx_t *ctx, const char *session_id)
{
   server_session_pool_t *free_entry = NULL;

   for (int i = 0; i < SERVER_SESSION_POOL_MAX; i++)
   {
      server_session_pool_t *entry = &ctx->session_pools[i];
      if (entry->initialized && strcmp(entry->session_id, session_id) == 0)
         return entry;
      if (!entry->initialized && !free_entry)
         free_entry = entry;
   }

   if (!free_entry)
      return NULL;

   int threads = ctx->session_threads > 0 ? ctx->session_threads : CONFIG_DEFAULT_SESSION_THREADS;
   if (compute_pool_init(&free_entry->pool, threads) != 0)
      return NULL;

   free_entry->initialized = 1;
   free_entry->close_requested = 0;
   free_entry->active_jobs = 0;
   snprintf(free_entry->session_id, sizeof(free_entry->session_id), "%s", session_id);

   return free_entry;
}

int server_session_pool_submit(server_ctx_t *ctx, const char *session_id, void (*fn)(void *),
                               void *arg, int *thread_count_out)
{
   if (thread_count_out)
      *thread_count_out = 0;
   if (!ctx || !ctx->session_pools_initialized || !session_id_usable(session_id) || !fn)
      return -1;

   session_pool_work_t *work = calloc(1, sizeof(*work));
   if (!work)
      return -1;

   pthread_mutex_lock(&ctx->session_pools_mutex);
   server_session_pool_t *entry = session_pool_find_or_create(ctx, session_id);
   if (!entry || entry->close_requested)
   {
      pthread_mutex_unlock(&ctx->session_pools_mutex);
      free(work);
      return -1;
   }
   entry->active_jobs++;
   if (thread_count_out)
      *thread_count_out = compute_pool_thread_count(&entry->pool);
   pthread_mutex_unlock(&ctx->session_pools_mutex);

   work->ctx = ctx;
   work->entry = entry;
   work->fn = fn;
   work->arg = arg;
   const request_context_t *request = request_context_get();
   if (request)
   {
      work->request_context = *request;
      work->has_request_context = 1;
   }

   if (compute_pool_submit(&entry->pool, session_pool_worker, work) != 0)
   {
      session_pool_release_job(ctx, entry);
      free(work);
      return -1;
   }

   return 0;
}

void server_session_pool_close(server_ctx_t *ctx, const char *session_id)
{
   if (!ctx || !ctx->session_pools_initialized || !session_id_usable(session_id))
      return;

   server_session_pool_t *to_shutdown = NULL;
   pthread_mutex_lock(&ctx->session_pools_mutex);
   for (int i = 0; i < SERVER_SESSION_POOL_MAX; i++)
   {
      server_session_pool_t *entry = &ctx->session_pools[i];
      if (entry->initialized && strcmp(entry->session_id, session_id) == 0)
      {
         entry->close_requested = 1;
         if (entry->active_jobs == 0 && !entry->shutdown_started)
         {
            entry->shutdown_started = 1;
            to_shutdown = entry;
         }
         break;
      }
   }
   pthread_mutex_unlock(&ctx->session_pools_mutex);

   if (to_shutdown)
      session_pool_shutdown_entry(to_shutdown);
}

void server_session_pools_shutdown(server_ctx_t *ctx)
{
   if (!ctx || !ctx->session_pools_initialized)
      return;

   for (int i = 0; i < SERVER_SESSION_POOL_MAX; i++)
      session_pool_shutdown_entry(&ctx->session_pools[i]);

   pthread_mutex_destroy(&ctx->session_pools_mutex);
   ctx->session_pools_initialized = 0;
}

char *server_session_pools_json(server_ctx_t *ctx)
{
   if (!ctx || !ctx->session_pools_initialized)
   {
      char *empty = malloc(3);
      if (empty)
         memcpy(empty, "[]", 3);
      return empty;
   }

   size_t cap = 64 + (size_t)SERVER_SESSION_POOL_MAX * 4096;
   char *out = malloc(cap);
   if (!out)
      return NULL;

   size_t pos = 0;
   pos += (size_t)snprintf(out + pos, cap - pos, "[");
   int first = 1;

   pthread_mutex_lock(&ctx->session_pools_mutex);
   for (int i = 0; i < SERVER_SESSION_POOL_MAX; i++)
   {
      server_session_pool_t *entry = &ctx->session_pools[i];
      if (!entry->initialized)
         continue;

      char sid[SERVER_SESSION_ID_MAX];
      int active_jobs = entry->active_jobs;
      int configured = compute_pool_thread_count(&entry->pool);
      snprintf(sid, sizeof(sid), "%s", entry->session_id);

      char *slots = compute_pool_slots_json(&entry->pool);

      int n = snprintf(out + pos, cap - pos,
                       "%s{\"session_id\":\"%s\",\"configured\":%d,\"active_jobs\":%d,"
                       "\"slots\":%s}",
                       first ? "" : ",", sid, configured, active_jobs, slots ? slots : "[]");
      free(slots);
      if (n < 0 || (size_t)n >= cap - pos)
         break;
      pos += (size_t)n;
      first = 0;
   }
   pthread_mutex_unlock(&ctx->session_pools_mutex);

   snprintf(out + pos, cap - pos, "]");
   return out;
}
