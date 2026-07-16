/* test_toolset_thread_scope.c: the active toolset must be scoped to ONE turn.
 *
 * Delegate turns run on POOLED worker threads and overlap by design
 * (session_threads defaults above 1, and review panels fan out through
 * agent_run_parallel). The toolset override used to be the process-wide
 * AIMEE_ACTIVE_TOOLSET env var, set per turn with a save/restore bracket — which
 * made it LOOK scoped. It was not: a concurrent delegate's setenv decided what
 * another delegate resolved.
 *
 * That is not a cosmetic race. agent_tools_filter_for_role is the thing that stops
 * a reviewer holding a coder's tools, and it resolves through this value. Get it
 * wrong under concurrency and the boundary silently is not one.
 *
 * These tests fail against the env-var implementation and pass against the
 * thread-local, which is the whole point of writing them. */
#include "aimee.h"
#include "agent_tools.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Each worker sets its OWN toolset, waits for the others to set theirs, and only
 * then reads back. The barrier is what makes the race deterministic instead of
 * lucky: with a process-global, the last writer wins and the others see it. */
static pthread_barrier_t g_barrier;

typedef struct
{
   const char *mine;
   int ok;
} worker_ctx_t;

static void *worker(void *arg)
{
   worker_ctx_t *c = arg;
   agent_tools_set_active_toolset(c->mine);
   pthread_barrier_wait(&g_barrier); /* every thread has now written */
   const char *seen = agent_tools_active_toolset();
   c->ok = (seen && strcmp(seen, c->mine) == 0);
   pthread_barrier_wait(&g_barrier); /* hold the value while others read */
   agent_tools_set_active_toolset(NULL);
   return NULL;
}

/* Concurrent turns must each see their own toolset. */
static void test_active_toolset_is_per_thread(void)
{
   enum
   {
      N = 4
   };
   const char *names[N] = {"review_indexed", "code", "full_stack", "readonly"};
   pthread_t th[N];
   worker_ctx_t ctx[N];
   assert(pthread_barrier_init(&g_barrier, NULL, N) == 0);
   for (int i = 0; i < N; i++)
   {
      ctx[i].mine = names[i];
      ctx[i].ok = 0;
      assert(pthread_create(&th[i], NULL, worker, &ctx[i]) == 0);
   }
   for (int i = 0; i < N; i++)
      pthread_join(th[i], NULL);
   pthread_barrier_destroy(&g_barrier);

   for (int i = 0; i < N; i++)
      assert(ctx[i].ok); /* each thread saw ITS OWN, not the last writer's */
   printf("  PASS: active_toolset_is_per_thread\n");
}

/* A worker thread's override must not leak into the thread that spawned it, nor
 * survive into the next turn on a pooled thread. */
static void *setter(void *arg)
{
   (void)arg;
   agent_tools_set_active_toolset("code");
   return NULL;
}

static void test_override_does_not_leak_across_threads(void)
{
   agent_tools_set_active_toolset("review_indexed");
   pthread_t t;
   assert(pthread_create(&t, NULL, setter, NULL) == 0);
   pthread_join(t, NULL);
   /* The worker set "code"; this thread must still be its own. With the process env
    * this assertion fails — which is exactly the delegate-to-delegate leak. */
   const char *seen = agent_tools_active_toolset();
   assert(seen && strcmp(seen, "review_indexed") == 0);

   /* Cleared means cleared: a leftover override would silently re-scope the next
    * delegate to land on this pooled thread. */
   agent_tools_set_active_toolset(NULL);
   assert(agent_tools_active_toolset() == NULL);
   agent_tools_set_active_toolset("");
   assert(agent_tools_active_toolset() == NULL); /* "" is not a toolset */
   printf("  PASS: override_does_not_leak_across_threads\n");
}

int main(void)
{
   printf("test_toolset_thread_scope:\n");
   test_active_toolset_is_per_thread();
   test_override_does_not_leak_across_threads();
   printf("All toolset_thread_scope tests passed.\n");
   return 0;
}
