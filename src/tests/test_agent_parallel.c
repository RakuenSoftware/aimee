#include "cJSON.h"
/* test_agent_parallel.c: the real agent_run_parallel deadline-preemption path —
 * a slow worker must be cancelled and joined at the deadline, not detached.
 * Links the real server/agent_parallel.o against stubbed agent exec functions. */
#include "aimee.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "request_context.h"
#include <assert.h>
#include <stdatomic.h>
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
static __thread atomic_int *g_request_cancel;
static __thread int g_require_initial_tool_call;
static __thread char g_run_cwd[MAX_PATH_LEN];
static atomic_int g_saw_required_tool_call;
static atomic_int g_saw_expected_cwd;
void run_cmd_set_cwd(const char *cwd)
{
   snprintf(g_run_cwd, sizeof(g_run_cwd), "%s", cwd ? cwd : "");
}
const char *run_cmd_get_cwd(void)
{
   return g_run_cwd[0] ? g_run_cwd : NULL;
}
void agent_run_require_initial_tool_call(int on)
{
   g_require_initial_tool_call = on ? 1 : 0;
}
void agent_set_request_cancel(atomic_int *flag)
{
   g_request_cancel = flag;
}
int agent_request_cancelled(void)
{
   return g_request_cancel && atomic_load(g_request_cancel);
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
static atomic_int g_wave_active;
static atomic_int g_wave_high_water;
int agent_run_named(agent_config_t *cfg, const char *name, const char *role,
                    const aimee_request_t *request, agent_result_t *out)
{
   const char *system_prompt = request->n_system ? request->system[0].text : NULL;
   const char *user_prompt = request->n_messages ? request->messages[0].blocks[0].text : NULL;
   int max_tokens = request->has_max_tokens ? request->max_tokens : 0;
   double temperature = request->has_temperature ? request->temperature : 0.3;

   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   memset(out, 0, sizeof(*out));
   if (name && !strcmp(name, "structured-ir"))
   {
      assert(!aimee_ir_request_admit(request));
      assert(request->n_messages == 1 && request->messages[0].n_blocks == 2);
      assert(!strcmp(request->messages[0].blocks[0].text, "look at this"));
      const aimee_block_t *image = &request->messages[0].blocks[1];
      assert(image->type == AIMEE_BLK_IMAGE && !strcmp(image->media_type, "image/png"));
      assert(!strcmp(image->media_ref, "https://fixture.invalid/input.png"));
      assert(image->raw && cJSON_IsTrue(cJSON_GetObjectItem(image->raw, "opaque")));
      assert(request->has_temperature && request->temperature == 0);
   }
   if (name && !strcmp(name, "context-owner"))
   {
      const request_context_t *owner = request_context_get();
      assert(owner && user_prompt && !strcmp(owner->principal, user_prompt));
      assert(owner->capabilities == 7 && owner->transport == REQ_TRANSPORT_TCP);
      assert(!strcmp(owner->caller_authorization, "test-only-owner-proof"));
      assert(owner->request_budget_present == 1 &&
             !strcmp(owner->request_budget_limits, "{\"max_tokens\":1234}"));
      assert(!owner->context_refused);
      assert(!request_context_refuse_assembly("worker-only-refusal"));
   }
   if (name && strcmp(name, "slow") == 0)
   {
      for (int elapsed = 0; elapsed < 3000 && !agent_request_cancelled(); elapsed += 10)
         sleep_ms(10);
      if (agent_request_cancelled())
      {
         snprintf(out->error, sizeof(out->error), "cancelled");
         return -1;
      }
   }
   else if (name && strcmp(name, "wave") == 0)
   {
      int active = atomic_fetch_add(&g_wave_active, 1) + 1;
      int high = atomic_load(&g_wave_high_water);
      while (active > high && !atomic_compare_exchange_weak(&g_wave_high_water, &high, active))
         ;
      sleep_ms(100);
      atomic_fetch_sub(&g_wave_active, 1);
   }
   out->response = strdup("ok");
   out->success = 1;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", name ? name : "");
   return 0;
}
/* The tools-enabled runners a task with use_tools routes to. They tag the response
 * so a test can assert WHICH runner ran: a review panelist reaching aimee (rather
 * than a plain completion) is the whole point of the flag. */
int agent_run_named_with_tools(agent_config_t *cfg, const char *name, const char *role,
                               const aimee_request_t *request, agent_result_t *out)
{
   const char *system_prompt = request->n_system ? request->system[0].text : NULL;
   const char *user_prompt = request->n_messages ? request->messages[0].blocks[0].text : NULL;
   int max_tokens = request->has_max_tokens ? request->max_tokens : 0;
   double temperature = request->has_temperature ? request->temperature : 0.3;

   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   memset(out, 0, sizeof(*out));
   if (g_require_initial_tool_call)
      atomic_store(&g_saw_required_tool_call, 1);
   if (run_cmd_get_cwd() && strcmp(run_cmd_get_cwd(), "/tmp/review-worktree") == 0)
      atomic_store(&g_saw_expected_cwd, 1);
   out->response = strdup("ok+tools");
   out->success = 1;
   out->tool_calls = 1;
   out->successful_tool_calls = 1;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", name ? name : "");
   return 0;
}
int agent_run_with_tools_write_enforce(agent_config_t *cfg, const char *role,
                                       const aimee_request_t *request, int enforce_writes,
                                       agent_result_t *out)
{
   const char *system_prompt = request->n_system ? request->system[0].text : NULL;
   const char *user_prompt = request->n_messages ? request->messages[0].blocks[0].text : NULL;
   int max_tokens = request->has_max_tokens ? request->max_tokens : 0;

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
int agent_run(agent_config_t *cfg, const char *role, const aimee_request_t *request,
              agent_result_t *out)
{
   const char *system_prompt = request->n_system ? request->system[0].text : NULL;
   const char *user_prompt = request->n_messages ? request->messages[0].blocks[0].text : NULL;
   int max_tokens = request->has_max_tokens ? request->max_tokens : 0;
   double temperature = request->has_temperature ? request->temperature : 0.3;

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

static int fixture_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int count,
                                agent_result_t *out, int deadline)
{
   for (int i = 0; i < count; i++)
      if (!tasks[i].request.n_messages)
         assert(!aimee_ir_request_from_text(&tasks[i].request, NULL, "task", 0, 0.3));
   int rc = agent_run_parallel(cfg, tasks, count, out, deadline);
   for (int i = 0; i < count; i++)
      aimee_request_free(&tasks[i].request);
   return rc;
}

static long now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void test_deadline_cancels_and_joins_slow_worker(void)
{
   agent_task_t tasks[3];
   memset(tasks, 0, sizeof(tasks));
   tasks[0].agent = "fast0";
   tasks[0].role = "review";
   tasks[1].agent = "slow";
   tasks[1].role = "review";
   assert(!aimee_ir_request_from_text(&tasks[1].request, "system-owned", "prompt-owned", 0, 0.3));
   tasks[2].agent = "fast2";
   tasks[2].role = "review";
   agent_result_t out[3];
   memset(out, 0, sizeof(out));
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.default_agent, sizeof(cfg.default_agent), "%s", "config-owned");

   long t0 = now_ms();
   int succ = fixture_run_parallel(&cfg, tasks, 3, out, 200 /* ms deadline */);
   long elapsed = now_ms() - t0;

   /* Cooperative cancellation makes the join transitive without waiting for
    * the slow worker's full 3000ms simulated call. */
   assert(elapsed < 2000);
   assert(succ == 2);
   assert(out[0].success == 1 && out[0].response);
   assert(out[2].success == 1 && out[2].response);
   assert(out[1].success == 0); /* slow worker cancelled -> failed slot */
   assert(out[1].response == NULL);
   assert(strcmp(out[1].error, "cancelled") == 0);
   agent_result_clear_response(&out[0]);
   agent_result_clear_response(&out[2]);
   printf("  test_deadline_cancels_and_joins_slow_worker: ok (%ldms, %d ok)\n", elapsed, succ);
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
   int succ = fixture_run_parallel(&cfg, tasks, 3, out, 0 /* no deadline */);
   assert(succ == 3);
   for (int i = 0; i < 3; i++)
   {
      assert(out[i].success == 1);
      agent_result_clear_response(&out[i]);
   }
   printf("  test_no_deadline_runs_all: ok\n");
}

static void test_parallel_worker_inherits_cwd(void)
{
   agent_task_t task;
   memset(&task, 0, sizeof(task));
   task.agent = "reviewer";
   task.role = "review";
   task.use_tools = 1;
   agent_result_t out = {0};
   memset(&out, 0, sizeof(out));
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   atomic_store(&g_saw_expected_cwd, 0);
   run_cmd_set_cwd("/tmp/review-worktree");
   assert(fixture_run_parallel(&cfg, &task, 1, &out, 1) == 1);
   run_cmd_set_cwd(NULL);
   assert(atomic_load(&g_saw_expected_cwd) == 1);
   agent_result_clear_response(&out);
}

static void test_no_deadline_ignores_generic_compute_width(void)
{
   enum
   {
      task_count = 14
   };
   agent_task_t tasks[task_count];
   agent_result_t out[task_count];
   memset(tasks, 0, sizeof(tasks));
   memset(out, 0, sizeof(out));
   for (int i = 0; i < task_count; i++)
   {
      tasks[i].agent = "wave";
      tasks[i].role = "review";
   }
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   atomic_store(&g_wave_active, 0);
   atomic_store(&g_wave_high_water, 0);
   g_aimee_compute_threads_override = 2;
   assert(fixture_run_parallel(&cfg, tasks, task_count, out, 0) == task_count);
   g_aimee_compute_threads_override = 0;
   assert(atomic_load(&g_wave_high_water) == task_count);
   for (int i = 0; i < task_count; i++)
      agent_result_clear_response(&out[i]);
   printf("  test_no_deadline_ignores_generic_compute_width: ok (%d concurrent)\n",
          atomic_load(&g_wave_high_water));
}

typedef struct
{
   agent_config_t cfg;
   agent_task_t tasks[40];
   agent_result_t out[40];
   int success;
} panel_call_t;

static void *run_test_panel(void *arg)
{
   panel_call_t *call = arg;
   call->success = fixture_run_parallel(&call->cfg, call->tasks, 40, call->out, 0);
   return NULL;
}

static void test_process_limit_is_shared_across_panels(void)
{
   panel_call_t calls[2];
   memset(calls, 0, sizeof(calls));
   for (int p = 0; p < 2; p++)
      for (int i = 0; i < 40; i++)
      {
         calls[p].tasks[i].agent = "wave";
         calls[p].tasks[i].role = "review";
      }
   atomic_store(&g_wave_active, 0);
   atomic_store(&g_wave_high_water, 0);
   pthread_t callers[2];
   assert(pthread_create(&callers[0], NULL, run_test_panel, &calls[0]) == 0);
   assert(pthread_create(&callers[1], NULL, run_test_panel, &calls[1]) == 0);
   pthread_join(callers[0], NULL);
   pthread_join(callers[1], NULL);
   assert(calls[0].success == 40);
   assert(calls[1].success == 40);
   assert(atomic_load(&g_wave_high_water) <= 64);
   for (int p = 0; p < 2; p++)
      for (int i = 0; i < 40; i++)
         agent_result_clear_response(&calls[p].out[i]);
   printf("  test_process_limit_is_shared_across_panels: ok (%d concurrent)\n",
          atomic_load(&g_wave_high_water));
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
   assert(!aimee_ir_request_from_text(&named.request, NULL, "review this", 0, 0.3));
   named.agent = "seat-1";
   named.use_tools = 1;
   named.require_initial_tool_call = 1;
   atomic_store(&g_saw_required_tool_call, 0);
   agent_result_t out1 = {0};
   memset(&out1, 0, sizeof(out1));
   assert(fixture_run_parallel(&cfg, &named, 1, &out1, 0) == 1);
   assert(strcmp(out1.response, "ok+tools") == 0);
   assert(atomic_load(&g_saw_required_tool_call) == 1);
   assert(g_require_initial_tool_call == 0); /* policy cannot bleed into the next task */
   agent_result_clear_response(&out1);

   /* single-task fast path, routed BY ROLE (no agent name) */
   agent_task_t byrole = {0};
   byrole.role = "review";
   assert(!aimee_ir_request_from_text(&byrole.request, NULL, "review this", 0, 0.3));
   byrole.use_tools = 1;
   agent_result_t out2 = {0};
   memset(&out2, 0, sizeof(out2));
   assert(fixture_run_parallel(&cfg, &byrole, 1, &out2, 0) == 1);
   assert(strcmp(out2.response, "ok+tools") == 0);
   agent_result_clear_response(&out2);

   /* the default is unchanged: no use_tools -> the plain completion */
   agent_task_t plain = {0};
   plain.role = "draft";
   assert(!aimee_ir_request_from_text(&plain.request, NULL, "draft this", 0, 0.3));
   plain.agent = "seat-1";
   agent_result_t out3 = {0};
   memset(&out3, 0, sizeof(out3));
   assert(fixture_run_parallel(&cfg, &plain, 1, &out3, 0) == 1);
   assert(strcmp(out3.response, "ok") == 0);
   agent_result_clear_response(&out3);

   /* and through the threaded fan-out, not just the fast path */
   agent_task_t many[2] = {0};
   many[0].role = "review";
   assert(!aimee_ir_request_from_text(&many[0].request, NULL, "a", 0, 0.3));
   many[0].agent = "seat-1";
   many[0].use_tools = 1;
   many[1].role = "review";
   assert(!aimee_ir_request_from_text(&many[1].request, NULL, "b", 0, 0.3));
   many[1].agent = "seat-2";
   many[1].use_tools = 1;
   agent_result_t outn[2];
   memset(outn, 0, sizeof(outn));
   assert(fixture_run_parallel(&cfg, many, 2, outn, 0) == 2);
   for (int i = 0; i < 2; i++)
   {
      assert(strcmp(outn[i].response, "ok+tools") == 0);
      agent_result_clear_response(&outn[i]);
   }
   printf("  test_use_tools_routes_to_tools_runner: ok\n");
}

static void test_parallel_preserves_owner_and_limits(void)
{
   agent_config_t cfg = {0};
   for (int deadline = 0; deadline < 2; deadline++)
   {
      request_context_t owner = {0};
      snprintf(owner.principal, sizeof owner.principal, "owner-%d", deadline);
      snprintf(owner.caller_authorization, sizeof owner.caller_authorization,
               "test-only-owner-proof");
      owner.capabilities = 7;
      owner.transport = REQ_TRANSPORT_TCP;
      owner.request_budget_present = 1;
      snprintf(owner.request_budget_limits, sizeof owner.request_budget_limits,
               "{\"max_tokens\":1234}");
      request_context_set(&owner);
      agent_task_t tasks[2] = {0};
      agent_result_t results[2] = {0};
      for (int i = 0; i < 2; i++)
      {
         tasks[i].agent = "context-owner";
         assert(!aimee_ir_request_from_text(&tasks[i].request, NULL, owner.principal, 0, 0.3));
      }
      assert(fixture_run_parallel(&cfg, tasks, 2, results, deadline ? 1000 : 0) == 2);
      assert(request_context_get() && !memcmp(request_context_get(), &owner, sizeof owner));
      for (int i = 0; i < 2; i++)
         agent_result_clear_response(&results[i]);
      request_context_clear();
   }
   puts("  parallel owner proof, capability scope, limits and local refusal isolation: ok");
}

static void test_parallel_preserves_structured_ir(void)
{
   agent_config_t cfg = {0};
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = "look at this"},
                             {.type = AIMEE_BLK_IMAGE,
                              .media_type = "image/png",
                              .media_ref = "https://fixture.invalid/input.png"}};
   blocks[1].raw = cJSON_Parse("{\"opaque\":true}");
   assert(blocks[1].raw);
   aimee_message_t message = {.role = "user", .blocks = blocks, .n_blocks = 2};
   aimee_request_t request = {
       .messages = &message, .n_messages = 1, .has_temperature = 1, .temperature = 0};
   for (int deadline = 0; deadline < 2; deadline++)
   {
      agent_task_t tasks[2] = {0};
      agent_result_t results[2] = {0};
      for (int i = 0; i < 2; i++)
      {
         tasks[i].agent = "structured-ir";
         assert(!aimee_ir_request_clone(&request, &tasks[i].request));
      }
      assert(fixture_run_parallel(&cfg, tasks, 2, results, deadline ? 1000 : 0) == 2);
      for (int i = 0; i < 2; i++)
         agent_result_clear_response(&results[i]);
   }
   cJSON_Delete(blocks[1].raw);
   puts("  parallel preserves text/image IR, opaque fields and explicit zero sampling: ok");
}

int main(void)
{
   printf("agent_parallel tests\n");
   test_parallel_preserves_owner_and_limits();
   test_parallel_preserves_structured_ir();
   test_deadline_cancels_and_joins_slow_worker();
   test_no_deadline_runs_all();
   test_parallel_worker_inherits_cwd();
   test_no_deadline_ignores_generic_compute_width();
   test_process_limit_is_shared_across_panels();
   test_use_tools_routes_to_tools_runner();
   printf("all tests passed\n");
   return 0;
}
