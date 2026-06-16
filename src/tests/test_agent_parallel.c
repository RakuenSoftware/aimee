/* test_agent_parallel.c: the real agent_run_parallel deadline-preemption path —
 * a hung worker must be abandoned at the deadline, not wedge the whole fan-out.
 * Links the real server/agent_parallel.o against stubbed agent exec functions. */
#include "aimee.h"
#include "agent_config.h"
#include "agent_exec.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- stubs for agent_parallel.o's dependencies --- */
void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
}
void agent_request_creds_restore(const agent_request_creds_t *creds)
{
   (void)creds;
}
void aimee_log(int level, const char *tag, const char *fmt, ...)
{
   (void)level;
   (void)tag;
   (void)fmt;
}
/* Referenced by the static-inline aimee_resolve_compute_threads in config.h. */
__thread int g_aimee_compute_threads_override = 0;

/* A worker named "slow" blocks well past the test deadline; everyone else
 * returns immediately. */
static void sleep_ms(int ms)
{
   struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
   nanosleep(&ts, NULL);
}
int agent_run_named(agent_config_t *cfg, const char *name, const char *role,
                    const char *system_prompt, const char *user_prompt, int max_tokens,
                    double temperature, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   memset(out, 0, sizeof(*out));
   if (name && strcmp(name, "slow") == 0)
      sleep_ms(3000);
   out->response = strdup("ok");
   out->success = 1;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", name ? name : "");
   return 0;
}
int agent_run_ex(agent_config_t *cfg, const char *role, const char *system_prompt,
                 const char *user_prompt, int max_tokens, double temperature, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   memset(out, 0, sizeof(*out));
   out->response = strdup("ok");
   out->success = 1;
   return 0;
}

static long now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void test_deadline_abandons_hung_worker(void)
{
   agent_task_t tasks[3];
   memset(tasks, 0, sizeof(tasks));
   tasks[0].agent = "fast0";
   tasks[0].role = "review";
   tasks[1].agent = "slow";
   tasks[1].role = "review";
   tasks[2].agent = "fast2";
   tasks[2].role = "review";
   agent_result_t out[3];
   memset(out, 0, sizeof(out));
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   long t0 = now_ms();
   int succ = agent_run_parallel(&cfg, tasks, 3, out, 200 /* ms deadline */);
   long elapsed = now_ms() - t0;

   /* Returned at ~the deadline, not after the 3000ms slow worker. */
   assert(elapsed < 2000);
   assert(succ == 2);
   assert(out[0].success == 1 && out[0].response);
   assert(out[2].success == 1 && out[2].response);
   assert(out[1].success == 0); /* slow worker abandoned -> failed slot */
   assert(out[1].response == NULL);
   free(out[0].response);
   free(out[2].response);
   printf("  test_deadline_abandons_hung_worker: ok (%ldms, %d ok)\n", elapsed, succ);
}

static void test_no_deadline_runs_all(void)
{
   agent_task_t tasks[3];
   memset(tasks, 0, sizeof(tasks));
   for (int i = 0; i < 3; i++)
   {
      tasks[i].agent = "fast";
      tasks[i].role = "review";
   }
   agent_result_t out[3];
   memset(out, 0, sizeof(out));
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   int succ = agent_run_parallel(&cfg, tasks, 3, out, 0 /* no deadline */);
   assert(succ == 3);
   for (int i = 0; i < 3; i++)
   {
      assert(out[i].success == 1);
      free(out[i].response);
   }
   printf("  test_no_deadline_runs_all: ok\n");
}

int main(void)
{
   printf("agent_parallel tests\n");
   test_deadline_abandons_hung_worker();
   test_no_deadline_runs_all();
   printf("all tests passed\n");
   return 0;
}
