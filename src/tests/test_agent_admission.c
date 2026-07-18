/* test_agent_admission.c — the admission controller must be fail-closed and must never
 * let concurrency exceed any of its three caps. These tests are the regression guard. */
#include "agent_admission.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static agent_admit_req_t req(const char *ctx, const char *agent, const char *model, int max)
{
   agent_admit_req_t r;
   memset(&r, 0, sizeof(r));
   r.ctx_handle = ctx;
   r.agent = agent;
   r.model = model;
   r.per_agent_max = max;
   r.flags = AGENT_ADMIT_NONBLOCKING; /* tests drive capacity explicitly; never hang */
   return r;
}

/* Reset to a known configuration between tests. */
static void configure(int global_max, int default_model_limit)
{
   agent_admission_configure(global_max, default_model_limit, NULL, 0);
}

static void test_fail_closed_bad_input(void)
{
   configure(10, 10);
   agent_admit_status_t st;
   agent_admit_req_t r = req("c1", "a", "m", 2);

   assert(agent_admission_acquire(NULL, &st) == NULL && st == AGENT_ADMIT_INVALID);
   r.ctx_handle = "";
   assert(agent_admission_acquire(&r, &st) == NULL && st == AGENT_ADMIT_INVALID);
   r = req("c1", "", "m", 2);
   assert(agent_admission_acquire(&r, &st) == NULL && st == AGENT_ADMIT_INVALID);
   r = req("c1", "a", "", 2);
   assert(agent_admission_acquire(&r, &st) == NULL && st == AGENT_ADMIT_INVALID);
   r = req("c1", "a", "m", 0);
   assert(agent_admission_acquire(&r, &st) == NULL && st == AGENT_ADMIT_INVALID);
   r = req("c1", "a", "m", -1);
   assert(agent_admission_acquire(&r, &st) == NULL && st == AGENT_ADMIT_INVALID);
   printf("  PASS: fail_closed_bad_input\n");
}

static void test_fail_closed_unconfigured(void)
{
   configure(0, 10); /* global_max<=0 -> unconfigured -> every acquire rejects */
   agent_admit_status_t st;
   agent_admit_req_t r = req("c1", "a", "m", 5);
   assert(agent_admission_acquire(&r, &st) == NULL && st == AGENT_ADMIT_INVALID);
   assert(agent_admission_global_active() == -1);
   printf("  PASS: fail_closed_unconfigured\n");
}

static void test_per_agent_cap(void)
{
   configure(100, 100);
   agent_admit_status_t st;
   agent_admit_req_t r1 = req("x1", "codex", "gpt", 2);
   agent_admit_req_t r2 = req("x2", "codex", "gpt", 2);
   agent_admit_req_t r3 = req("x3", "codex", "gpt", 2);
   agent_slot_t *s1 = agent_admission_acquire(&r1, &st);
   assert(s1 && st == AGENT_ADMIT_OK);
   agent_slot_t *s2 = agent_admission_acquire(&r2, &st);
   assert(s2 && st == AGENT_ADMIT_OK);
   assert(agent_admission_agent_active("codex") == 2);
   /* third distinct context on codex exceeds max_parallel=2 -> AT_LIMIT */
   assert(agent_admission_acquire(&r3, &st) == NULL && st == AGENT_ADMIT_AT_LIMIT);
   agent_admission_release(s1);
   agent_slot_t *s3 = agent_admission_acquire(&r3, &st); /* now room */
   assert(s3 && st == AGENT_ADMIT_OK);
   agent_admission_release(s2);
   agent_admission_release(s3);
   assert(agent_admission_agent_active("codex") == 0);
   printf("  PASS: per_agent_cap\n");
}

static void test_global_cap(void)
{
   configure(2, 100); /* global cap 2, big per-agent so only global bites */
   agent_admit_status_t st;
   agent_admit_req_t a = req("g1", "a", "m1", 50);
   agent_admit_req_t b = req("g2", "b", "m2", 50);
   agent_admit_req_t c = req("g3", "c", "m3", 50);
   agent_slot_t *s1 = agent_admission_acquire(&a, &st);
   agent_slot_t *s2 = agent_admission_acquire(&b, &st);
   assert(s1 && s2 && agent_admission_global_active() == 2);
   /* different agent, but global cap is full */
   assert(agent_admission_acquire(&c, &st) == NULL && st == AGENT_ADMIT_AT_LIMIT);
   agent_admission_release(s1);
   agent_admission_release(s2);
   printf("  PASS: global_cap\n");
}

static void test_per_model_cap(void)
{
   agent_admission_model_limit_t lim = {.limit = 1};
   snprintf(lim.model, sizeof(lim.model), "shared-model");
   agent_admission_configure(100, 100, &lim, 1);
   agent_admit_status_t st;
   /* two DIFFERENT agents share one model capped at 1 */
   agent_admit_req_t a = req("m1", "agentA", "shared-model", 50);
   agent_admit_req_t b = req("m2", "agentB", "shared-model", 50);
   agent_slot_t *s1 = agent_admission_acquire(&a, &st);
   assert(s1 && st == AGENT_ADMIT_OK);
   assert(agent_admission_acquire(&b, &st) == NULL && st == AGENT_ADMIT_AT_LIMIT);
   agent_admission_release(s1);
   agent_slot_t *s2 = agent_admission_acquire(&b, &st);
   assert(s2 && st == AGENT_ADMIT_OK);
   agent_admission_release(s2);
   printf("  PASS: per_model_cap\n");
}

static void test_context_reuse(void)
{
   configure(1, 100); /* global cap 1: only reuse of the SAME context can exceed it */
   agent_admit_status_t st;
   agent_admit_req_t r = req("ctxR", "a", "m", 1);
   agent_slot_t *h1 = agent_admission_acquire(&r, &st);
   assert(h1 && agent_admission_global_active() == 1);
   /* same ctx handle -> reuse, refcount, does NOT consume another global slot */
   agent_slot_t *h2 = agent_admission_acquire(&r, &st);
   assert(h2 && st == AGENT_ADMIT_OK && agent_admission_global_active() == 1);
   agent_admission_release(h1);
   assert(agent_admission_global_active() == 1); /* still held by h2 */
   agent_admission_release(h2);
   assert(agent_admission_global_active() == 0); /* last holder freed it */
   printf("  PASS: context_reuse\n");
}

static void test_release_null_and_stale(void)
{
   configure(10, 10);
   agent_admission_release(NULL); /* no crash */
   agent_admit_status_t st;
   agent_admit_req_t r = req("s1", "a", "m", 5);
   agent_slot_t *h = agent_admission_acquire(&r, &st);
   agent_admission_release(h);
   /* releasing again would be a stale/double free — API frees the handle, so we can't
    * legally reuse h; instead assert the counter went to zero exactly once. */
   assert(agent_admission_global_active() == 0);
   printf("  PASS: release_null_and_stale\n");
}

/* ---- concurrency stress: peak in-flight must never exceed the global cap ---- */

#define STRESS_THREADS 40
#define STRESS_CAP     5

static volatile int s_inflight = 0;
static volatile int s_peak = 0;
static pthread_mutex_t s_peak_lock = PTHREAD_MUTEX_INITIALIZER;

static void *stress_worker(void *arg)
{
   long id = (long)arg;
   char ctx[32];
   snprintf(ctx, sizeof(ctx), "sctx-%ld", id);
   agent_admit_req_t r = req(ctx, "codex", "gpt", 1000); /* per-agent huge; global bites */
   r.flags = 0;                                          /* BLOCKING */
   agent_admit_status_t st;
   agent_slot_t *s = agent_admission_acquire(&r, &st);
   assert(s && st == AGENT_ADMIT_OK);

   pthread_mutex_lock(&s_peak_lock);
   int now = ++s_inflight;
   if (now > s_peak)
      s_peak = now;
   assert(now <= STRESS_CAP); /* THE invariant */
   pthread_mutex_unlock(&s_peak_lock);

   usleep(2000);

   pthread_mutex_lock(&s_peak_lock);
   s_inflight--;
   pthread_mutex_unlock(&s_peak_lock);

   agent_admission_release(s);
   return NULL;
}

static void test_stress_peak_never_exceeds_cap(void)
{
   configure(STRESS_CAP, 100000);
   s_inflight = s_peak = 0;
   pthread_t t[STRESS_THREADS];
   for (long i = 0; i < STRESS_THREADS; i++)
      assert(pthread_create(&t[i], NULL, stress_worker, (void *)i) == 0);
   for (int i = 0; i < STRESS_THREADS; i++)
      pthread_join(t[i], NULL);
   assert(s_peak <= STRESS_CAP && s_peak >= 1);
   assert(agent_admission_global_active() == 0);
   printf("  PASS: stress_peak_never_exceeds_cap (peak=%d, cap=%d)\n", s_peak, STRESS_CAP);
}

int main(void)
{
   test_fail_closed_bad_input();
   test_fail_closed_unconfigured();
   test_per_agent_cap();
   test_global_cap();
   test_per_model_cap();
   test_context_reuse();
   test_release_null_and_stale();
   test_stress_peak_never_exceeds_cap();
   printf("agent_admission: all tests passed\n");
   return 0;
}
