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
#include "workspace_provider.h"
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

/* Slice 7: review_indexed carries the read-only worktree tools, but only when the
 * active workspace provider can actually see the review worktree. A DETACHED remote
 * seat's read marshals to the serving client's fs, so the read tools must be denied
 * there — while the index-only tools stay, and write/exec are never granted. */
static void test_review_read_reachability_gate(void)
{
   agent_tools_set_active_toolset("review_indexed");

   /* No provider bound == SHARED default (server worktree): reachable -> read OK. */
   workspace_provider_set_active(NULL);
   assert(agent_tools_tool_allowed_for_role("review", "read_file") == 1);
   assert(agent_tools_tool_allowed_for_role("review", "grep") == 1);
   assert(agent_tools_tool_allowed_for_role("review", "list_files") == 1);
   /* Index-only tools always allowed; a reviewer must never gain write/exec. */
   assert(agent_tools_tool_allowed_for_role("review", "code_search") == 1);
   assert(agent_tools_tool_allowed_for_role("review", "write_file") == 0);
   assert(agent_tools_tool_allowed_for_role("review", "edit_file") == 0);
   assert(agent_tools_tool_allowed_for_role("review", "bash") == 0);

   /* CONTAINER (:ro sandbox mount) sees the review tree -> read OK. */
   workspace_provider_t container = {0};
   container.kind = WS_PROVIDER_CONTAINER;
   workspace_provider_set_active(&container);
   assert(agent_tools_tool_allowed_for_role("review", "read_file") == 1);

   /* DETACHED remote seat: read_all marshals to the client fs, not the review
    * tree -> the read tools are DENIED; index-only tools still work. */
   workspace_provider_t detached = {0};
   detached.kind = WS_PROVIDER_DETACHED;
   workspace_provider_set_active(&detached);
   assert(agent_tools_tool_allowed_for_role("review", "read_file") == 0);
   assert(agent_tools_tool_allowed_for_role("review", "grep") == 0);
   assert(agent_tools_tool_allowed_for_role("review", "list_files") == 0);
   assert(agent_tools_tool_allowed_for_role("review", "code_search") == 1);
   assert(agent_tools_tool_allowed_for_role("review", "write_file") == 0);

   workspace_provider_set_active(NULL);
   agent_tools_set_active_toolset(NULL);
   printf("  PASS: review_read_reachability_gate\n");
}

int main(void)
{
   printf("test_toolset_thread_scope:\n");
   test_active_toolset_is_per_thread();
   test_override_does_not_leak_across_threads();
   test_review_read_reachability_gate();
   printf("All toolset_thread_scope tests passed.\n");
   return 0;
}
