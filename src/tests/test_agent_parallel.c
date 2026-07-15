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
/* The tools-enabled runners a task with use_tools routes to. They tag the response
 * so a test can assert WHICH runner ran: a review panelist reaching aimee (rather
 * than a plain completion) is the whole point of the flag. */
int agent_run_named_with_tools(agent_config_t *cfg, const char *name, const char *role,
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
   out->response = strdup("ok+tools");
   out->success = 1;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", name ? name : "");
   return 0;
}
int agent_run_with_tools_write_enforce(agent_config_t *cfg, const char *role,
                                       const char *system_prompt, const char *user_prompt,
                                       int max_tokens, int enforce_writes, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)enforce_writes;
   memset(out, 0, sizeof(*out));
   out->response = strdup("ok+tools");
   out->success = 1;
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

/* use_tools picks the tools-enabled runner — the difference between a review
 * panelist that can look up whether a change is reachable and one that can only
 * squint at the diff. Both routes (by name, and by role) must honour it, and a task
 * that does not ask for tools must still get the historical plain completion. */
static void test_use_tools_routes_to_tools_runner(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   /* single-task fast path, routed BY NAME */
   agent_task_t named = {0};
   named.role = "review";
   named.user_prompt = "review this";
   named.agent = "seat-1";
   named.use_tools = 1;
   agent_result_t out1;
   memset(&out1, 0, sizeof(out1));
   assert(agent_run_parallel(&cfg, &named, 1, &out1, 0) == 1);
   assert(strcmp(out1.response, "ok+tools") == 0);
   free(out1.response);

   /* single-task fast path, routed BY ROLE (no agent name) */
   agent_task_t byrole = {0};
   byrole.role = "review";
   byrole.user_prompt = "review this";
   byrole.use_tools = 1;
   agent_result_t out2;
   memset(&out2, 0, sizeof(out2));
   assert(agent_run_parallel(&cfg, &byrole, 1, &out2, 0) == 1);
   assert(strcmp(out2.response, "ok+tools") == 0);
   free(out2.response);

   /* the default is unchanged: no use_tools -> the plain completion */
   agent_task_t plain = {0};
   plain.role = "draft";
   plain.user_prompt = "draft this";
   plain.agent = "seat-1";
   agent_result_t out3;
   memset(&out3, 0, sizeof(out3));
   assert(agent_run_parallel(&cfg, &plain, 1, &out3, 0) == 1);
   assert(strcmp(out3.response, "ok") == 0);
   free(out3.response);

   /* and through the threaded fan-out, not just the fast path */
   agent_task_t many[2] = {0};
   many[0].role = "review";
   many[0].user_prompt = "a";
   many[0].agent = "seat-1";
   many[0].use_tools = 1;
   many[1].role = "review";
   many[1].user_prompt = "b";
   many[1].agent = "seat-2";
   many[1].use_tools = 1;
   agent_result_t outn[2];
   memset(outn, 0, sizeof(outn));
   assert(agent_run_parallel(&cfg, many, 2, outn, 0) == 2);
   for (int i = 0; i < 2; i++)
   {
      assert(strcmp(outn[i].response, "ok+tools") == 0);
      free(outn[i].response);
   }
   printf("  test_use_tools_routes_to_tools_runner: ok\n");
}

int main(void)
{
   printf("agent_parallel tests\n");
   test_deadline_abandons_hung_worker();
   test_no_deadline_runs_all();
   test_use_tools_routes_to_tools_runner();
   printf("all tests passed\n");
   return 0;
}
