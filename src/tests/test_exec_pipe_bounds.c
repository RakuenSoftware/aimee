/* test_exec_pipe_bounds.c -- the bounds every subprocess exchange carries.
 *
 * Three independent live defects existed here, all reachable from request paths.
 * The kb_http listener (src/kb/http/kb_http.c) runs a SERIAL accept loop —
 * accept, handle inline, close — so any handler that blocks indefinitely stops
 * the listener accepting at all. Each of these could do that:
 *
 *   1. Deadlock. The old code wrote ALL input, closed stdin, then read. With both
 *      directions past pipe capacity the child blocks writing to a full stdout
 *      pipe while the parent blocks writing to a full stdin pipe. Reproduced with
 *      1 MiB each way; no external stall required.
 *   2. No bound. waitpid(pid, NULL, 0) — a child that never exits hung the caller
 *      forever.
 *   3. Unbounded output. The reader doubled its buffer with no ceiling.
 *
 * These tests are deliberately behavioural: each drives the real function and
 * fails if the corresponding defect returns.
 */
#include "platform_process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      exit(1);
   }
}

static long long mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* THE deadlock case. Both directions exceed pipe capacity; the child emits before
 * draining. Before the fix this never returned. The assertion is on completion
 * AND on the output being whole — a "fix" that merely timed out would leave the
 * hang in place and silently truncate. */
static void test_large_bidirectional_exchange_does_not_deadlock(void)
{
   size_t n = 1u << 20;
   char *in = malloc(n);
   must(in != NULL, "alloc");
   memset(in, 'x', n);

   char *out = NULL;
   size_t out_len = 0;
   long long t0 = mono_ms();
   int rc = platform_exec_pipe_bounded("head -c 1048576 /dev/zero; cat >/dev/null", in, n, &out,
                                       &out_len, 15000, 64u << 20);
   long long elapsed = mono_ms() - t0;

   must(rc >= 0, "the exchange completes rather than erroring");
   must(elapsed < 10000, "it completes well inside the deadline, i.e. did not deadlock");
   must(out_len == (1u << 20), "the full 1 MiB of output is captured, not truncated");
   free(out);
   free(in);
   printf("  PASS: a large exchange in both directions completes (no deadlock)\n");
}

/* A child that never exits must not hang the caller. */
static void test_a_hung_child_is_bounded(void)
{
   char *out = NULL;
   size_t out_len = 0;
   long long t0 = mono_ms();
   int rc = platform_exec_pipe_bounded("sleep 60", NULL, 0, &out, &out_len, 1500, 1u << 20);
   long long elapsed = mono_ms() - t0;

   must(rc == PLATFORM_EXEC_ERR_TIMEOUT, "a timeout is reported distinctly");
   must(elapsed < 6000, "the caller is released near the deadline, not at the child's pace");
   free(out);
   printf("  PASS: a child that never exits is bounded and reported as a timeout\n");
}

/* A fast-emitting child must not grow the parent's heap without limit. */
static void test_runaway_output_is_capped(void)
{
   char *out = NULL;
   size_t out_len = 0;
   int rc = platform_exec_pipe_bounded("head -c 10485760 /dev/zero", NULL, 0, &out, &out_len, 15000,
                                       1u << 20);
   must(rc == PLATFORM_EXEC_ERR_OUTPUT_LIMIT, "the limit is reported distinctly");
   free(out);
   printf("  PASS: runaway output is capped and reported\n");
}

/* Killing only the child leaves grandchildren alive holding the pipe; the whole
 * process group must go.
 *
 * This must OBSERVE the grandchild's fate, not just that the call returned. An
 * earlier version asserted only prompt return — which is true even when the
 * group is not killed, because the parent stops waiting either way. Mutation
 * testing caught that: replacing kill(-pid) with kill(pid) left the suite green.
 *
 * So the grandchild is given a delayed side effect. It sleeps past the deadline
 * and then creates a file. If the group died, the file never appears. */
static void test_grandchildren_do_not_outlive_the_bound(void)
{
   const char *marker = "/tmp/aimee_exec_pipe_grandchild_marker";
   remove(marker);

   char cmd[512];
   snprintf(cmd, sizeof cmd, "sh -c 'sleep 3; : > %s' & sleep 60", marker);

   char *out = NULL;
   size_t out_len = 0;
   long long t0 = mono_ms();
   int rc = platform_exec_pipe_bounded(cmd, NULL, 0, &out, &out_len, 1000, 1u << 20);
   long long elapsed = mono_ms() - t0;
   must(rc == PLATFORM_EXEC_ERR_TIMEOUT, "still a timeout");
   must(elapsed < 6000, "the call returns despite a backgrounded grandchild");
   free(out);

   /* Outlive the grandchild's sleep: if it survived, it creates the marker. */
   struct timespec nap = {5, 0};
   nanosleep(&nap, NULL);
   FILE *f = fopen(marker, "r");
   if (f)
      fclose(f);
   remove(marker);
   must(f == NULL, "the grandchild was killed with the group, so its later write never happened");
   printf("  PASS: a backgrounded grandchild is killed with the group, not left running\n");
}

/* The ordinary path must be unaffected: bounds are a safety net, not a change in
 * behaviour for commands that behave. */
static void test_the_ordinary_path_is_unchanged(void)
{
   char *out = NULL;
   size_t out_len = 0;
   int rc = platform_exec_pipe_bounded("cat", "hello", 5, &out, &out_len, 5000, 1u << 20);
   must(rc == 0, "exit code is passed through");
   must(out_len == 5 && out && memcmp(out, "hello", 5) == 0, "stdin is echoed to stdout intact");
   free(out);

   out = NULL;
   out_len = 0;
   rc = platform_exec_pipe_bounded("exit 3", NULL, 0, &out, &out_len, 5000, 1u << 20);
   must(rc == 3, "a non-zero exit code is distinguishable from an error constant");
   free(out);
   printf("  PASS: exit codes and payloads pass through unchanged\n");
}

/* The convenience wrapper must be bounded too — the whole point is that no call
 * site can be unbounded. */
static void test_the_default_wrapper_is_also_bounded(void)
{
   must(PLATFORM_EXEC_DEFAULT_TIMEOUT_MS > 0, "the default timeout is a real bound");
   must(PLATFORM_EXEC_DEFAULT_MAX_OUTPUT > 0, "the default output cap is a real bound");
   char *out = NULL;
   size_t out_len = 0;
   int rc = platform_exec_pipe("printf ok", NULL, 0, &out, &out_len);
   must(rc == 0 && out_len == 2, "the wrapper still works normally");
   free(out);
   printf("  PASS: the default wrapper carries real bounds\n");
}

int main(void)
{
   printf("test_exec_pipe_bounds:\n");
   test_large_bidirectional_exchange_does_not_deadlock();
   test_a_hung_child_is_bounded();
   test_runaway_output_is_capped();
   test_grandchildren_do_not_outlive_the_bound();
   test_the_ordinary_path_is_unchanged();
   test_the_default_wrapper_is_also_bounded();
   printf("All tests passed.\n");
   return 0;
}
