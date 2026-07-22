/* test_platform_process.c: tests for platform_exec_capture timeout behavior */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_process.h"

static int always_cancel(void *ctx)
{
   (void)ctx;
   return 1;
}

static void test_exec_capture_basic(void)
{
   char *out = NULL;
   size_t len = 0;
   int rc = platform_exec_capture("echo hello", &out, &len, 0);
   assert(rc == 0);
   assert(out != NULL);
   assert(strncmp(out, "hello", 5) == 0);
   free(out);
}

static void test_exec_capture_exit_code(void)
{
   char *out = NULL;
   size_t len = 0;
   int rc = platform_exec_capture("exit 42", &out, &len, 0);
   assert(rc == 42);
   free(out);
}

static void test_exec_capture_timeout(void)
{
   char *out = NULL;
   size_t len = 0;
   /* sleep 10 should be killed well before completion */
   int rc = platform_exec_capture("sleep 10", &out, &len, 200);
   assert(rc == -1); /* timeout returns -1 */
   free(out);
}

static void test_exec_capture_no_timeout_fast_cmd(void)
{
   char *out = NULL;
   size_t len = 0;
   /* Fast command with generous timeout should succeed normally */
   int rc = platform_exec_capture("echo quick", &out, &len, 5000);
   assert(rc == 0);
   assert(out != NULL);
   assert(strncmp(out, "quick", 5) == 0);
   free(out);
}

static void test_exec_capture_cancellable(void)
{
   char *out = NULL;
   size_t len = 0;
   int rc = platform_exec_capture_cancellable("sleep 10", &out, &len, 0, always_cancel, NULL);
   assert(rc == -1);
   free(out);
}

/* Regression: a child that exits before draining its stdin must not crash the
 * caller. platform_exec_pipe write()s `input` to the child's stdin; if the child
 * has already closed the read end, that write raises SIGPIPE, which by default
 * terminates THIS process. That turned a handled subprocess failure (a rerank
 * command exiting non-zero) into a hard SIGPIPE kill of the caller -- flaky under
 * load in unit-test-memory, and a latent server crash wherever exec_pipe feeds a
 * command that can fail fast. The fix ignores SIGPIPE across the write.
 *
 * Made deterministic by two things: `true` exits immediately without reading, and
 * the input is far larger than any pipe buffer (64 KiB pipe on Linux), so the
 * write is guaranteed to reach the closed pipe rather than fitting entirely in the
 * kernel buffer before the child is gone. Before the fix this test dies with
 * signal 13; after it, exec_pipe returns the child's status and we survive to
 * assert. */
static void test_exec_pipe_child_exits_without_reading(void)
{
   size_t big = 1024 * 1024; /* 1 MiB >> pipe capacity */
   char *input = malloc(big);
   assert(input);
   memset(input, 'x', big);

   char *out = NULL;
   size_t len = 0;
   /* `true` ignores and closes stdin, then exits 0. The point is not the status
    * but that we are still alive to read it. */
   int rc = platform_exec_pipe("true", input, big, &out, &len);
   assert(rc == 0); /* reached at all == SIGPIPE did not kill us */
   free(out);

   /* And a fast NON-zero exit (the real rerank-fails case) is likewise survivable
    * and reported, not swallowed. */
   out = NULL;
   len = 0;
   rc = platform_exec_pipe("exit 2", input, big, &out, &len);
   assert(rc == 2);
   free(out);
   free(input);
}

int main(void)
{
   test_exec_capture_basic();
   test_exec_capture_exit_code();
   test_exec_capture_timeout();
   test_exec_capture_no_timeout_fast_cmd();
   test_exec_capture_cancellable();
   test_exec_pipe_child_exits_without_reading();
   printf("platform_process: all tests passed\n");
   return 0;
}
