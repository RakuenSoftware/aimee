/* kb_ingest_worker_cap: embedding is background work and never takes the whole
 * machine.
 *
 * Ingest work IS embedding, and embedding is CPU-bound: each worker drives the
 * embedder, which itself runs torch with EMBEDDER_THREADS threads. Measured
 * against bekko-a25m on the 4-CPU bench container, one 128-text batch costs
 *
 *   concurrency 1:  6.6s   (51.6 ms/text)
 *   concurrency 4: 13.7s   (26.8 ms/text)
 *   concurrency 8: 25.5s   (24.9 ms/text)
 *
 * Concurrency buys throughput and pays in latency: at 8 workers a single batch
 * takes 25.5s, which is how a build that was progressing normally tripped the
 * client's 30s bound and reported itself as a failure. Meanwhile the interactive
 * path -- search, the /v1 routes, the agent waiting on them -- is competing for
 * the same cores.
 *
 * So the policy: at least one core is ALWAYS left free. Workers additionally run
 * at nice +5, so even the cores they do use yield to foreground work.
 *
 * The cap is pure and takes ncpu as a parameter precisely so this is testable
 * without a host of a particular size. */
#include "kb_ingest_workers.h"
#include "kb_service.h" /* KB_WORKER_MAX */

#include <assert.h>
#include <stdio.h>

/* The headline requirement: never saturate. */
static void test_always_leaves_a_core_free(void)
{
   for (long ncpu = 2; ncpu <= 64; ncpu++)
      assert(kb_ingest_worker_cap(KB_WORKER_MAX, ncpu) <= (int)ncpu - 1);
}

/* The bench container: 4 CPUs, config asking for the max. Before the cap this
 * ran 8 workers on 4 cores. */
static void test_four_cpu_box_runs_three_workers(void)
{
   assert(kb_ingest_worker_cap(KB_WORKER_MAX, 4) == 3);
   assert(kb_ingest_worker_cap(8, 4) == 3);
}

/* A single-core host still makes progress: one worker, nice'd, is the floor.
 * Returning 0 here would disable ingest entirely on small hosts. */
static void test_single_cpu_still_ingests(void)
{
   assert(kb_ingest_worker_cap(4, 1) == 1);
   assert(kb_ingest_worker_cap(1, 1) == 1);
}

/* The operator's number is a ceiling, not a floor: asking for fewer than the
 * machine allows is honoured. */
static void test_configured_value_is_a_ceiling(void)
{
   assert(kb_ingest_worker_cap(2, 32) == 2);
   assert(kb_ingest_worker_cap(1, 32) == 1);
}

/* KB_WORKER_MAX still bounds a generous config on a large host. */
static void test_absolute_max_still_applies(void)
{
   assert(kb_ingest_worker_cap(1000, 64) == KB_WORKER_MAX);
}

/* Explicitly disabled stays disabled -- the cap must not resurrect ingest on a
 * host where an operator turned it off. */
static void test_disabled_stays_disabled(void)
{
   assert(kb_ingest_worker_cap(0, 16) == 0);
   assert(kb_ingest_worker_cap(-1, 16) == 0);
}

/* The cap is only as good as the CPU count it is given, and that is where the
 * first deployment of this policy failed: sysconf(_SC_NPROCESSORS_ONLN) reports
 * the HOST's CPUs and ignores a container's affinity mask. On the bench box it
 * answered 8 while the process was pinned to 4, so the cap reserved headroom on
 * cores that did not exist and started 4 workers on 4 usable CPUs -- exactly the
 * saturation it exists to prevent, and the unit tests were all green while it
 * happened.
 *
 * The lesson is in the shape of the bug, not the arithmetic: a pure policy
 * function cannot protect you from being handed the wrong input. Pin the case
 * that was actually observed. */
static void test_host_cpu_count_must_not_be_used_for_a_confined_process(void)
{
   /* Observed on the bench container: sysconf said 8, affinity said 4. */
   const long host_cpus = 8;
   const long usable_cpus = 4;
   const int configured = 4;
   /* Against the host count the configured value passes straight through and
      saturates the 4 CPUs the process actually has. */
   assert(kb_ingest_worker_cap(configured, host_cpus) == configured);
   /* Against the usable count it leaves one free, which is the requirement. */
   assert(kb_ingest_worker_cap(configured, usable_cpus) == 3);
   assert(kb_ingest_worker_cap(configured, usable_cpus) < usable_cpus);
}

int main(void)
{
   printf("kb_ingest_worker_cap: ");
   test_always_leaves_a_core_free();
   test_four_cpu_box_runs_three_workers();
   test_single_cpu_still_ingests();
   test_configured_value_is_a_ceiling();
   test_absolute_max_still_applies();
   test_disabled_stays_disabled();
   test_host_cpu_count_must_not_be_used_for_a_confined_process();
   printf("ok\n");
   return 0;
}
