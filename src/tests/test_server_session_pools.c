#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "server.h"
#include "request_context.h"

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   int running;
   int release;
} gate_t;

static void gate_init(gate_t *gate)
{
   memset(gate, 0, sizeof(*gate));
   pthread_mutex_init(&gate->mutex, NULL);
   pthread_cond_init(&gate->cond, NULL);
}

static void gate_destroy(gate_t *gate)
{
   pthread_mutex_destroy(&gate->mutex);
   pthread_cond_destroy(&gate->cond);
}

static void gated_job(void *arg)
{
   gate_t *gate = (gate_t *)arg;
   pthread_mutex_lock(&gate->mutex);
   gate->running = 1;
   pthread_cond_broadcast(&gate->cond);
   while (!gate->release)
      pthread_cond_wait(&gate->cond, &gate->mutex);
   pthread_mutex_unlock(&gate->mutex);
}

static void wait_for_gate_running(gate_t *gate)
{
   pthread_mutex_lock(&gate->mutex);
   while (!gate->running)
      pthread_cond_wait(&gate->cond, &gate->mutex);
   pthread_mutex_unlock(&gate->mutex);
}

static void release_gate(gate_t *gate)
{
   pthread_mutex_lock(&gate->mutex);
   gate->release = 1;
   pthread_cond_broadcast(&gate->cond);
   pthread_mutex_unlock(&gate->mutex);
}

static int session_pool_initialized(server_ctx_t *ctx, const char *sid)
{
   int initialized = 0;
   pthread_mutex_lock(&ctx->session_pools_mutex);
   for (int i = 0; i < SERVER_SESSION_POOL_MAX; i++)
   {
      server_session_pool_t *entry = &ctx->session_pools[i];
      if (entry->initialized && strcmp(entry->session_id, sid) == 0)
      {
         initialized = 1;
         break;
      }
   }
   pthread_mutex_unlock(&ctx->session_pools_mutex);
   return initialized;
}

static void wait_for_session_pool_removed(server_ctx_t *ctx, const char *sid)
{
   for (int i = 0; i < 200; i++)
   {
      if (!session_pool_initialized(ctx, sid))
         return;
      usleep(10000);
   }
   assert(!session_pool_initialized(ctx, sid));
}

static server_ctx_t *ctx_new(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   assert(ctx != NULL);
   memset(ctx, 0, sizeof(*ctx));
   pthread_mutex_init(&ctx->session_pools_mutex, NULL);
   ctx->session_pools_initialized = 1;
   ctx->session_threads = 1;
   return ctx;
}

static void ctx_free(server_ctx_t *ctx)
{
   pthread_mutex_destroy(&ctx->session_pools_mutex);
   free(ctx);
}

static void test_close_reaps_after_active_job_finishes(void)
{
   server_ctx_t *ctx = ctx_new();

   gate_t gate;
   gate_init(&gate);

   int threads = 0;
   assert(server_session_pool_submit(ctx, "sid-active", gated_job, &gate, &threads) == 0);
   assert(threads == 1);
   wait_for_gate_running(&gate);
   assert(session_pool_initialized(ctx, "sid-active"));

   server_session_pool_close(ctx, "sid-active");
   assert(session_pool_initialized(ctx, "sid-active"));

   release_gate(&gate);
   wait_for_session_pool_removed(ctx, "sid-active");

   gate_destroy(&gate);
   ctx_free(ctx);
}

static void noop_job(void *arg)
{
   int *done = (int *)arg;
   *done = 1;
}

static void test_close_idle_pool_reaps_immediately(void)
{
   server_ctx_t *ctx = ctx_new();

   int done = 0;
   assert(server_session_pool_submit(ctx, "sid-idle", noop_job, &done, NULL) == 0);
   for (int i = 0; i < 200 && !done; i++)
      usleep(10000);
   assert(done == 1);
   assert(session_pool_initialized(ctx, "sid-idle"));

   server_session_pool_close(ctx, "sid-idle");
   wait_for_session_pool_removed(ctx, "sid-idle");

   ctx_free(ctx);
}

static void context_job(void *arg)
{
   const request_context_t *request = request_context_get();
   assert(request && strcmp(request->principal, "uid:1000") == 0);
   assert(strcmp(request->request_id, "captured-request") == 0);
   assert(request->request_budget_present == 1);
   assert(strcmp(request->request_budget_limits, "{\"max_request_bytes\":0}") == 0);
   assert(request->context_refused == 1);
   assert(strcmp(request->memory_source_release, "host-source-handle") == 0);
   gated_job(arg);
}

static void anonymous_context_job(void *arg)
{
   assert(request_context_get() == NULL);
   gated_job(arg);
}

static void test_request_context_survives_queue_without_leaking(void)
{
   server_ctx_t *ctx = ctx_new();
   gate_t first, second;
   gate_init(&first);
   gate_init(&second);
   request_context_t request = {0};
   snprintf(request.principal, sizeof(request.principal), "uid:1000");
   snprintf(request.request_id, sizeof(request.request_id), "captured-request");
   request.request_budget_present = 1;
   snprintf(request.request_budget_limits, sizeof(request.request_budget_limits),
            "{\"max_request_bytes\":0}");
   request.context_refused = 1;
   snprintf(request.memory_source_release, sizeof(request.memory_source_release),
            "host-source-handle");
   request_context_set(&request);
   assert(server_session_pool_submit(ctx, "sid-context", context_job, &first, NULL) == 0);
   request_context_clear();
   wait_for_gate_running(&first);
   assert(server_session_pool_submit(ctx, "sid-context", anonymous_context_job, &second, NULL) ==
          0);
   release_gate(&first);
   wait_for_gate_running(&second);
   release_gate(&second);
   server_session_pool_close(ctx, "sid-context");
   wait_for_session_pool_removed(ctx, "sid-context");
   gate_destroy(&first);
   gate_destroy(&second);
   ctx_free(ctx);
}

int main(void)
{
   printf("server_session_pools: ");
   test_request_context_survives_queue_without_leaking();
   test_close_reaps_after_active_job_finishes();
   test_close_idle_pool_reaps_immediately();
   printf("ok\n");
   return 0;
}
