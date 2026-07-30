/* test_agent_admission.c — the admission controller must be fail-closed and must never
 * let concurrency exceed any of its three caps. These tests are the regression guard. */
#include "agent_admission.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Idle-reaper TTL the tests drive against (seconds); matches the monitor's operating
 * threshold in spirit — the exact value is irrelevant since the clock is injected. */
#define ADMIT_TEST_TTL 240

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

static void test_probe_matches_acquire_without_mutation(void)
{
   agent_admit_capacity_t why;
   agent_admit_status_t st;
   agent_admit_capacity_info_t info;
   configure(2, 10);
   assert(agent_admission_probe("a", "m1", 1, &why) && why == AGENT_ADMIT_CAPACITY_AVAILABLE);
   assert(agent_admission_global_active() == 0);
   assert(agent_admission_probe_info("a", "m1", 1, &info));
   assert(info.available == 1 && info.global_available == 2 && info.agent_available == 1 &&
          info.model_available == 10);
   assert(agent_admission_global_active() == 0); /* detailed probe is also non-mutating */

   agent_admit_req_t a = req("p1", "a", "m1", 1);
   agent_slot_t *sa = agent_admission_acquire(&a, &st);
   assert(sa && st == AGENT_ADMIT_OK);
   assert(!agent_admission_probe("a", "m2", 1, &why) && why == AGENT_ADMIT_CAPACITY_AGENT);
   assert(agent_admission_global_active() == 1);
   assert(!agent_admission_probe_info("a", "m2", 1, &info));
   assert(info.reason == AGENT_ADMIT_CAPACITY_AGENT && info.available == 0);
   assert(agent_admission_global_active() == 1);

   agent_admit_req_t b = req("p2", "b", "m2", 2);
   agent_slot_t *sb = agent_admission_acquire(&b, &st);
   assert(sb && st == AGENT_ADMIT_OK);
   assert(!agent_admission_probe("c", "m3", 2, &why) && why == AGENT_ADMIT_CAPACITY_GLOBAL);
   assert(agent_admission_global_active() == 2);
   assert(!agent_admission_probe_info("c", "m3", 2, &info));
   assert(info.reason == AGENT_ADMIT_CAPACITY_GLOBAL && info.available == 0);
   assert(agent_admission_global_active() == 2);
   agent_admission_release(sa);
   agent_admission_release(sb);

   agent_admission_model_limit_t lim = {.limit = 1};
   snprintf(lim.model, sizeof(lim.model), "shared");
   agent_admission_configure(10, 10, &lim, 1);
   agent_admit_req_t m = req("pm", "a", "shared", 2);
   sa = agent_admission_acquire(&m, &st);
   assert(sa);
   assert(!agent_admission_probe("b", "shared", 2, &why) && why == AGENT_ADMIT_CAPACITY_MODEL);
   assert(agent_admission_global_active() == 1);
   assert(!agent_admission_probe_info("b", "shared", 2, &info));
   assert(info.reason == AGENT_ADMIT_CAPACITY_MODEL && info.available == 0);
   assert(agent_admission_global_active() == 1);
   agent_admission_release(sa);
   printf("  PASS: probe_matches_acquire_without_mutation\n");
}

static void test_model_less_agents_share_default_key(void)
{
   configure(10, 1);
   agent_admit_status_t st;
   agent_admit_capacity_t why;
   agent_admit_req_t a = req("default-a", "claude-a", AGENT_ADMISSION_DEFAULT_MODEL_KEY, 4);
   agent_admit_req_t b = req("default-b", "claude-b", AGENT_ADMISSION_DEFAULT_MODEL_KEY, 4);
   agent_slot_t *slot = agent_admission_acquire(&a, &st);
   assert(slot && st == AGENT_ADMIT_OK);
   assert(!agent_admission_probe(b.agent, b.model, b.per_agent_max, &why));
   assert(why == AGENT_ADMIT_CAPACITY_MODEL);
   assert(agent_admission_acquire(&b, &st) == NULL && st == AGENT_ADMIT_AT_LIMIT);
   agent_admission_release(slot);
   assert(agent_admission_probe(b.agent, b.model, b.per_agent_max, &why));
   printf("  PASS: model_less_agents_share_default_key\n");
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

/* ---- idle reaper: reclaim a slot whose holder wedged/died without releasing ---- */

static time_t s_fake_now;
static time_t fake_now(void)
{
   return s_fake_now;
}

static void test_reap_idle_reclaims_wedged_slot(void)
{
   agent_admission_set_now_hook_for_test(fake_now);
   s_fake_now = 1000;
   configure(10, 10);
   agent_admit_status_t st;

   /* claude at max_parallel=1: one holder saturates it. */
   agent_admit_req_t r = req("deleg-A", "claude", "sonnet", 1);
   agent_slot_t *wedged = agent_admission_acquire(&r, &st);
   assert(wedged && st == AGENT_ADMIT_OK);
   assert(agent_admission_agent_active("claude") == 1);

   /* The leak symptom: a new claude turn is rejected while the holder pins the slot. */
   agent_admit_req_t r2 = req("deleg-B", "claude", "sonnet", 1);
   assert(agent_admission_acquire(&r2, &st) == NULL && st == AGENT_ADMIT_AT_LIMIT);

   /* Holder never releases (its thread wedged). Below the TTL nothing is reclaimed... */
   s_fake_now += ADMIT_TEST_TTL - 1;
   assert(agent_admission_reap_idle(ADMIT_TEST_TTL) == 0);
   assert(agent_admission_agent_active("claude") == 1);

   /* ...at/after the TTL the wedged slot is reclaimed. */
   s_fake_now += 2;
   assert(agent_admission_reap_idle(ADMIT_TEST_TTL) == 1);
   assert(agent_admission_agent_active("claude") == 0);

   /* Capacity is back: a fresh claude turn is admitted. */
   agent_slot_t *fresh = agent_admission_acquire(&r2, &st);
   assert(fresh && st == AGENT_ADMIT_OK);
   assert(agent_admission_agent_active("claude") == 1);

   /* The wedged holder's late real release is a safe no-op (generation guard): it must
    * not double-decrement and drop the fresh holder's count. */
   agent_admission_release(wedged);
   assert(agent_admission_agent_active("claude") == 1);

   agent_admission_release(fresh);
   assert(agent_admission_agent_active("claude") == 0);
   agent_admission_set_now_hook_for_test(NULL);
   printf("  PASS: reap_idle_reclaims_wedged_slot\n");
}

static void test_touch_protects_live_slot(void)
{
   agent_admission_set_now_hook_for_test(fake_now);
   s_fake_now = 5000;
   configure(10, 10);
   agent_admit_status_t st;
   agent_admit_req_t r = req("deleg-live", "agentY", "m", 1);
   agent_slot_t *s = agent_admission_acquire(&r, &st);
   assert(s && st == AGENT_ADMIT_OK);

   /* Wall time marches well past the TTL, but the turn keeps heartbeating within it, so
    * the reaper never touches it. */
   for (int i = 0; i < 6; i++)
   {
      s_fake_now += ADMIT_TEST_TTL - 1;
      agent_admission_touch("deleg-live");
      assert(agent_admission_reap_idle(ADMIT_TEST_TTL) == 0);
      assert(agent_admission_agent_active("agentY") == 1);
   }
   agent_admission_release(s);
   agent_admission_set_now_hook_for_test(NULL);
   printf("  PASS: touch_protects_live_slot\n");
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

#define RACE_THREADS 12

static pthread_mutex_t s_race_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_race_start_cond = PTHREAD_COND_INITIALIZER;
static int s_race_ready;
static int s_race_start;
static int s_race_done;
static pthread_mutex_t s_race_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_race_acquired;
static int s_race_at_limit;
static int s_race_distinct_agents;
static int s_race_distinct_models;

static void *nonblocking_race_worker(void *arg)
{
   long id = (long)arg;
   char ctx[32], agent[32], model[32];
   snprintf(ctx, sizeof(ctx), "race-%ld", id);
   snprintf(agent, sizeof(agent), s_race_distinct_agents ? "agent-%ld" : "agent", id);
   snprintf(model, sizeof(model), s_race_distinct_models ? "model-%ld" : "model", id);
   agent_admit_req_t r = req(ctx, agent, model, 3);
   agent_admit_status_t st;

   pthread_mutex_lock(&s_race_start_lock);
   s_race_ready++;
   pthread_cond_broadcast(&s_race_start_cond);
   while (!s_race_start)
      pthread_cond_wait(&s_race_start_cond, &s_race_start_lock);
   pthread_mutex_unlock(&s_race_start_lock);
   agent_slot_t *slot = agent_admission_acquire(&r, &st);
   pthread_mutex_lock(&s_race_lock);
   if (slot)
      s_race_acquired++;
   else
   {
      assert(st == AGENT_ADMIT_AT_LIMIT);
      s_race_at_limit++;
   }
   pthread_mutex_unlock(&s_race_lock);
   pthread_mutex_lock(&s_race_start_lock);
   s_race_done++;
   pthread_cond_broadcast(&s_race_start_cond);
   while (s_race_done < RACE_THREADS)
      pthread_cond_wait(&s_race_start_cond, &s_race_start_lock);
   pthread_mutex_unlock(&s_race_start_lock);
   agent_admission_release(slot);
   return NULL;
}

static void run_nonblocking_race(int global_max, int model_max, int distinct_agents,
                                 int distinct_models, int expected_acquired)
{
   configure(global_max, model_max);
   s_race_acquired = s_race_at_limit = 0;
   s_race_ready = s_race_start = s_race_done = 0;
   s_race_distinct_agents = distinct_agents;
   s_race_distinct_models = distinct_models;
   pthread_t threads[RACE_THREADS];
   for (long i = 0; i < RACE_THREADS; i++)
      assert(pthread_create(&threads[i], NULL, nonblocking_race_worker, (void *)i) == 0);
   pthread_mutex_lock(&s_race_start_lock);
   while (s_race_ready < RACE_THREADS)
      pthread_cond_wait(&s_race_start_cond, &s_race_start_lock);
   s_race_start = 1;
   pthread_cond_broadcast(&s_race_start_cond);
   pthread_mutex_unlock(&s_race_start_lock);
   for (int i = 0; i < RACE_THREADS; i++)
      pthread_join(threads[i], NULL);
   assert(s_race_acquired == expected_acquired);
   assert(s_race_at_limit == RACE_THREADS - expected_acquired);
   assert(agent_admission_global_active() == 0);
}

static void test_synchronized_nonblocking_races_obey_all_limits(void)
{
   run_nonblocking_race(2, 100, 1, 1, 2);  /* per-agent limit is 3; global bites */
   run_nonblocking_race(100, 100, 0, 1, 3); /* shared agent limit bites */
   run_nonblocking_race(100, 2, 1, 0, 2);   /* shared-model limit bites */
   printf("  PASS: synchronized_nonblocking_races_obey_all_limits\n");
}

int main(void)
{
   test_fail_closed_bad_input();
   test_fail_closed_unconfigured();
   test_per_agent_cap();
   test_model_less_agents_share_default_key();
   test_global_cap();
   test_per_model_cap();
   test_probe_matches_acquire_without_mutation();
   test_context_reuse();
   test_release_null_and_stale();
   test_reap_idle_reclaims_wedged_slot();
   test_touch_protects_live_slot();
   test_stress_peak_never_exceeds_cap();
   test_synchronized_nonblocking_races_obey_all_limits();
   printf("agent_admission: all tests passed\n");
   return 0;
}
