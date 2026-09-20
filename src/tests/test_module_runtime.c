#define _GNU_SOURCE
#include "cJSON.h"
#include <sys/socket.h>

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/benchmarks/module_api.h>
#include <aimee/sandbox/module_api.h>
#include <aimee/control-web/module_api.h>
#include <aimee/delegates/module_api.h>
#include <aimee/egress/module_api.h>
#include <aimee/providers/module_api.h>
#include <aimee/git/module_api.h>
#include <aimee/governance/module_api.h>
#include <aimee/kb-synthesis/module_api.h>
#include <aimee/learning/module_api.h>
#include <aimee/response-composition/module_api.h>
#include <aimee/roundtable/module_api.h>
#include <aimee/routing/module_api.h>
#include <aimee/runtime-web/module_api.h>
#include <aimee/execution-policy/module_api.h>
#include <aimee/postgres/module_api.h>
#include "config_client.h"
#include <aimee/skills/module_api.h>
#include <aimee/tools/module_api.h>
#include <aimee/workspace/module_api.h>
#include "economizer_module_client.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define TEST_KIND  5889U
#define EMPTY_KIND 5890U
#define TEST_STAGE 1U
#define MODULE_REF 7U
#define CALLER_REF 90U
#define LARGE_BODY (128U * 1024U + 37U)
/* The largest stage count any component declares in
 * src/modules/process-contracts.json. `aimee` has twenty-three. */
#define PRODUCTION_STAGE_MAX 32U

typedef struct
{
   aimee_module_process_config_t config;
   int result;
} process_thread_t;

typedef struct
{
   bus_host_t *host;
   pthread_mutex_t *lock;
   atomic_int stop;
} pump_thread_t;

static void pump(bus_host_t *host, pthread_mutex_t *lock);

static aimee_module_status_t handle(const aimee_module_invocation_t *invocation,
                                    const uint8_t *request, uint32_t request_len, uint8_t *response,
                                    uint32_t response_capacity, uint32_t *response_len,
                                    void *user_data)
{
   (void)user_data;
   if (request_len == 6 && memcmp(request, "cancel", 6) == 0)
   {
      const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
      while (!aimee_module_invocation_cancelled(invocation))
         nanosleep(&pause, NULL);
      return AIMEE_MODULE_STATUS_OK; /* core converts this to CANCELLED */
   }
   if (request_len > response_capacity)
      return AIMEE_MODULE_STATUS_INTERNAL;
   memcpy(response, request, request_len);
   *response_len = request_len;
   return AIMEE_MODULE_STATUS_OK;
}

static void *run_process(void *argument)
{
   process_thread_t *thread = argument;
   thread->result = aimee_module_process_run(&thread->config);
   return NULL;
}

static void *run_pump(void *argument)
{
   pump_thread_t *pump_state = argument;
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   while (!atomic_load_explicit(&pump_state->stop, memory_order_acquire))
   {
      pump(pump_state->host, pump_state->lock);
      nanosleep(&pause, NULL);
   }
   return NULL;
}

static int cancellation_flag(void *context)
{
   return atomic_load_explicit((atomic_int *)context, memory_order_acquire);
}

static void *cancel_soon(void *context)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
   nanosleep(&pause, NULL);
   atomic_store_explicit((atomic_int *)context, 1, memory_order_release);
   return NULL;
}

static void pump(bus_host_t *host, pthread_mutex_t *lock)
{
   pthread_mutex_lock(lock);
   (void)bus_host_pump(host);
   pthread_mutex_unlock(lock);
}

static void wait_for_clients(bus_host_t *host, pthread_mutex_t *lock, uint32_t count, pid_t child)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int i = 0; i < 10000; ++i)
   {
      pthread_mutex_lock(lock);
      uint32_t admitted = host->admitted;
      pthread_mutex_unlock(lock);
      if (admitted >= count)
         return;
      int status = 0;
      if (child > 0 && waitpid(child, &status, WNOHANG) == child)
      {
         fprintf(stderr, "module exited before bus admission (status %d)\n", status);
         assert(!"module exited before bus admission");
      }
      nanosleep(&pause, NULL);
   }
   assert(!"timed out waiting for module clients");
}

static pid_t spawn_module_child(const char *executable, const char *socket_path,
                                const char *argument)
{
   pid_t parent = getpid();
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent)
         _exit(0);
      execl(executable, executable, socket_path, argument, (char *)NULL);
      _exit(127);
   }
   return child;
}

static void run_memory_probe(const char *executable, const char *socket_path)
{
   pid_t child = spawn_module_child(executable, socket_path, "--decisions");
   int status = 0;
   while (waitpid(child, &status, 0) < 0)
      assert(errno == EINTR);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* Keep the host and its callers alive while the independent memory process is
 * killed. Its two connections and the finished probe must leave the bus before
 * a replacement can be admitted under the same grants. */
static void wait_for_memory_departure(bus_runtime_t *runtime, bus_host_t *host,
                                      pthread_mutex_t *lock, bus_client_t *caller,
                                      bus_client_t *embedding_host)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int i = 0; i < 10000; ++i)
   {
      struct timespec now;
      assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
      uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
      bus_client_heartbeat(caller, now_ns);
      bus_client_heartbeat(embedding_host, now_ns);
      pthread_mutex_lock(lock);
      /* Production hosts call maintenance separately from dispatch pumping. */
      (void)bus_runtime_maintain(runtime, now_ns);
      uint32_t admitted = host->admitted;
      pthread_mutex_unlock(lock);
      if (admitted == 2)
         return;
      nanosleep(&pause, NULL);
   }
   assert(!"memory process connections survived termination");
}

static void memory_discovery_unavailable(aimee_module_client_t *client)
{
   const uint8_t request[] = {'D', 'C', 'M', 'D', 2, 0, 0, 0};
   uint8_t response[1024];
   memset(response, 0xa5, sizeof(response));
   uint32_t response_len = 99;
   struct timespec now;
   assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
   uint64_t deadline = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec + 2000000000ULL;
   assert(aimee_module_client_call(client, 4096u + 7u * 256u + 255u, 255u, 9901, deadline, request,
                                   sizeof(request), response, sizeof(response), &response_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
   assert(response_len == 0);
}

static int production_contract(const char *name, uint32_t *kind, uint32_t *principal_ref,
                               uint32_t served[PRODUCTION_STAGE_MAX], size_t *serve_count)
{
   if (strcmp(name, "memory") == 0)
   {
      *principal_ref = 7;
      *kind = 4096u + *principal_ref * 256u + 1u;
      for (uint32_t stage = 1; stage <= 8; ++stage)
         served[stage - 1] = 4096u + *principal_ref * 256u + stage;
      served[8] = 4096u + *principal_ref * 256u + 255u;
      *serve_count = 9;
      return 0;
   }
   if (strcmp(name, "learning") == 0)
      *kind = AIMEE_LEARNING_EVENT_OBSERVE, *principal_ref = 8;
   else if (strcmp(name, "routing") == 0)
   {
      *kind = AIMEE_ROUTING_EVENT_KIND, *principal_ref = 9;
      served[0] = AIMEE_ROUTING_EVENT_KIND;
      served[1] = AIMEE_ROUTING_EVENT_PLAN;
      *serve_count = 2;
      return 0;
   }
   else if (strcmp(name, "delegates") == 0)
      *kind = AIMEE_DELEGATES_EVENT_INVOKE, *principal_ref = 10;
   else if (strcmp(name, "tools") == 0)
      *kind = AIMEE_TOOLS_EVENT_DISPATCH, *principal_ref = 11;
   else if (strcmp(name, "workspace") == 0)
      *kind = AIMEE_WORKSPACE_EVENT_ACCESS, *principal_ref = 12;
   else if (strcmp(name, "git") == 0)
   {
      *kind = AIMEE_GIT_EVENT_OPERATION, *principal_ref = 13;
      served[0] = AIMEE_GIT_EVENT_OPERATION;
      served[1] = AIMEE_GIT_EVENT_REF_VALIDATE;
      *serve_count = 2;
      return 0;
   }
   else if (strcmp(name, "skills") == 0)
   {
      *kind = AIMEE_SKILLS_EVENT_CONTEXT, *principal_ref = 14;
      served[0] = AIMEE_SKILLS_EVENT_CONTEXT;
      served[1] = AIMEE_SKILLS_EVENT_TRIGGER;
      *serve_count = 2;
      return 0;
   }
   else if (strcmp(name, "response-composition") == 0)
      *kind = AIMEE_RESPONSE_EVENT_COMPOSE, *principal_ref = 15;
   else if (strcmp(name, "governance") == 0)
      *kind = AIMEE_GOVERNANCE_EVENT_EVALUATE, *principal_ref = 19;
   else if (strcmp(name, "roundtable") == 0)
      *kind = AIMEE_ROUNDTABLE_EVENT_DELIBERATE, *principal_ref = 21;
   else if (strcmp(name, "kb-synthesis") == 0)
      *kind = AIMEE_KB_SYNTHESIS_EVENT_GROUNDING, *principal_ref = 22;
   else if (strcmp(name, "runtime-web") == 0)
      *kind = AIMEE_RUNTIME_WEB_EVENT_CLASSIFY, *principal_ref = 23;
   else if (strcmp(name, "control-web") == 0)
      *kind = AIMEE_CONTROL_WEB_EVENT_AUTHORIZE, *principal_ref = 24;
   else if (strcmp(name, "benchmarks") == 0)
   {
      *kind = AIMEE_BENCHMARKS_EVENT_RUN, *principal_ref = 25;
      served[0] = AIMEE_BENCHMARKS_EVENT_RUN;
      served[1] = AIMEE_BENCHMARKS_EVENT_LATENCY;
      *serve_count = 2;
      return 0;
   }
   else if (strcmp(name, "sandbox") == 0)
   {
      *kind = AIMEE_SANDBOX_EVENT_OBSERVE, *principal_ref = 26;
      served[0] = AIMEE_SANDBOX_EVENT_OBSERVE;
      served[1] = AIMEE_SANDBOX_EVENT_LOAD;
      served[2] = AIMEE_SANDBOX_EVENT_PROXY_REQUEST;
      served[3] = AIMEE_SANDBOX_EVENT_PROXY_ADDRESS;
      *serve_count = 4;
      return 0;
   }
   else if (strcmp(name, "economizer") == 0)
   {
      *kind = AIMEE_ECONOMIZER_EVENT_JSON_COMPACT;
      *principal_ref = 27;
      served[0] = AIMEE_ECONOMIZER_EVENT_REDUCE;
      served[1] = AIMEE_ECONOMIZER_EVENT_JSON_COMPACT;
      served[2] = AIMEE_ECONOMIZER_EVENT_TOOL_RECALL;
      served[3] = AIMEE_ECONOMIZER_EVENT_TOOL_STATS;
      served[4] = AIMEE_ECONOMIZER_EVENT_RECORD_BUILD;
      served[5] = AIMEE_ECONOMIZER_EVENT_POST_STATUS;
      served[6] = AIMEE_ECONOMIZER_EVENT_STATS;
      served[7] = AIMEE_ECONOMIZER_EVENT_REQUEST_BUDGET;
      *serve_count = 8;
      return 0;
   }
   /* These four were missing, and the omission was invisible because the only
    * script that exercises this table -- scripts/test_bus_conformance.sh -- is
    * run by no CI job and no make target. It derives its module list from
    * process-contracts.json, so every component added since this table was
    * written arrived here as "return -1" and aborted the run. config,
    * execution-policy, postgres and aimee had all drifted in that way.
    *
    * The stage lists mirror process-contracts.json, which is the source of
    * truth; a component that grows a stage has to be added here too, and the
    * conformance run is what says so. */
   else if (strcmp(name, "config") == 0)
      *kind = AIMEE_CONFIG_EVENT_KIND, *principal_ref = 2;
   else if (strcmp(name, "execution-policy") == 0)
      *kind = AIMEE_EXECUTION_POLICY_EVENT_TOOL, *principal_ref = 17;
   else if (strcmp(name, "postgres") == 0)
   {
      *kind = AIMEE_POSTGRES_EVENT_HEALTH, *principal_ref = 28;
      served[0] = AIMEE_POSTGRES_EVENT_HEALTH;
      served[1] = AIMEE_POSTGRES_EVENT_SQL;
      *serve_count = 2;
      return 0;
   }
   else if (strcmp(name, "egress") == 0)
   {
      *kind = AIMEE_EGRESS_EVENT_AUTHORIZE, *principal_ref = 32;
      served[0] = AIMEE_EGRESS_EVENT_AUTHORIZE;
      served[1] = AIMEE_EGRESS_EVENT_HTTP;
      served[2] = AIMEE_EGRESS_EVENT_SSE_OPEN;
      served[3] = AIMEE_EGRESS_EVENT_SSE_SEND;
      served[4] = AIMEE_EGRESS_EVENT_SSE_RECV;
      served[5] = AIMEE_EGRESS_EVENT_SSE_CLOSE;
      served[6] = AIMEE_EGRESS_EVENT_CREDENTIAL_KEY;
      *serve_count = 7;
      return 0;
   }
   else if (strcmp(name, "providers") == 0)
   {
      *kind = AIMEE_PROVIDERS_EVENT_RESOLVE, *principal_ref = 33;
      served[0] = AIMEE_PROVIDERS_EVENT_RESOLVE;
      served[1] = AIMEE_PROVIDERS_EVENT_VALIDATE;
      served[2] = AIMEE_PROVIDERS_EVENT_MANAGE;
      *serve_count = 3;
      return 0;
   }
   else if (strcmp(name, "server") == 0 || strcmp(name, "kb") == 0)
   {
      *principal_ref = strcmp(name, "server") == 0 ? BUS_SERVER_ROLE_REF : BUS_KB_ROLE_REF;
      *kind = 4096u + *principal_ref * 256u + 1u;
   }
   else if (strcmp(name, "aimee") == 0)
   {
      /* Twenty-three stages, carved from ref 30 by the canonical rule
       * kind = 4096 + ref*256 + stage. Written as the derivation rather than
       * twenty-three constants, because that IS the contract and a hand-copied
       * list is what drifts. */
      *principal_ref = 30;
      *kind = 4096u + 30u * 256u + 1u;
      for (uint32_t stage = 1; stage <= 23u; stage++)
         served[stage - 1] = 4096u + 30u * 256u + stage;
      *serve_count = 23;
      return 0;
   }
   else
      return -1;
   served[0] = *kind;
   *serve_count = 1;
   return 0;
}

static void smoke_production_module(aimee_module_client_t *client, const char *name, uint32_t kind)
{
   uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN] = {0};
   uint8_t response[1024] = {0};
   uint32_t request_len = 0, response_len = 0;
   if (strcmp(name, "server") == 0 || strcmp(name, "kb") == 0)
   {
      const char identity_request[] = "{\"operation\":\"identity\"}";
      assert(aimee_module_client_call(client, kind, 1, 2110, 0, identity_request,
                                      sizeof(identity_request) - 1, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len < sizeof(response));
      response[response_len] = 0;
      char expected[64];
      snprintf(expected, sizeof(expected), "\"role\":\"%s\"", name);
      assert(strstr((char *)response, expected) != NULL);
      return;
   }
   if (strcmp(name, "learning") == 0)
   {
      uint32_t mask = 0;
      assert(aimee_learning_request_encode("correction", request, sizeof(request)) == 0);
      request_len = AIMEE_LEARNING_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, 1, 2001, 0, request, request_len, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_learning_response_decode(response, response_len, &mask) == 0);
      assert(mask == (AIMEE_LEARNING_SINK_RERANKER | AIMEE_LEARNING_SINK_SUPERSEDE |
                      AIMEE_LEARNING_SINK_RULE));
   }
   else if (strcmp(name, "routing") == 0)
   {
      uint32_t selected = UINT32_MAX;
      assert(aimee_routing_request_encode(AIMEE_ROUTING_SELECT_BALANCED, 3, request,
                                          sizeof(request)) == 0);
      request_len = AIMEE_ROUTING_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, 1, 2002, 0, request, request_len, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_routing_response_decode(response, response_len, 3, &selected) == 0);
      assert(selected == 0);
      static const char plan[] =
          "{\"version\":1,\"input_tokens\":1000,\"output_tokens\":100,\"candidates\":["
          "{\"name\":\"paid\",\"prices\":{\"input\":1,\"output\":2}},"
          "{\"name\":\"free\",\"tier\":9,\"overrides\":{\"input\":0,\"output\":0}}]}";
      assert(aimee_module_client_call(client, AIMEE_ROUTING_EVENT_PLAN, AIMEE_ROUTING_STAGE_PLAN,
                                      2102, 0, (const uint8_t *)plan, sizeof(plan) - 1, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len == strlen("{\"selected\":1}"));
      assert(memcmp(response, "{\"selected\":1}", response_len) == 0);
   }
   else if (strcmp(name, "delegates") == 0)
   {
      char role[AIMEE_DELEGATES_ROLE_MAX + 1u];
      assert(aimee_delegates_message_encode(AIMEE_DELEGATES_REQUEST_MAGIC, "implement", request,
                                            sizeof(request)) == 0);
      request_len = AIMEE_DELEGATES_MESSAGE_LEN;
      assert(aimee_module_client_call(client, kind, 1, 2003, 0, request, request_len, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_delegates_message_decode(response, response_len, AIMEE_DELEGATES_RESPONSE_MAGIC,
                                            role, sizeof(role)) == 0);
      assert(strcmp(role, "code") == 0);
   }
   else if (strcmp(name, "tools") == 0)
   {
      aimee_tool_class_t classification = AIMEE_TOOL_CLASS_UNKNOWN;
      assert(aimee_tools_request_encode("bash", request, sizeof(request)) == 0);
      request_len = AIMEE_TOOLS_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, 1, 2004, 0, request, request_len, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_tools_response_decode(response, response_len, &classification) == 0);
      assert(classification == AIMEE_TOOL_CLASS_EXEC);
   }
   else if (strcmp(name, "workspace") == 0)
   {
      int allowed = 0;
      const char reference[] = "owner/repo";
      assert(aimee_workspace_request_encode(reference, strlen(reference), request,
                                            sizeof(request)) == 0);
      request_len = AIMEE_WORKSPACE_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, 1, 2005, 0, request, request_len, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_workspace_response_decode(response, response_len, &allowed) == 0 && allowed);
   }
   else if (strcmp(name, "git") == 0)
   {
      aimee_git_classification_t classification = {0};
      assert(aimee_git_request_encode("push", request, sizeof(request)) == 0);
      request_len = AIMEE_GIT_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, 1, 2006, 0, request, request_len, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_git_response_decode(response, response_len, &classification) == 0);
      assert(classification.operation == AIMEE_GIT_OP_PUSH && classification.needs_credentials);

      int allowed = 0;
      assert(aimee_git_ref_request_encode("feature/topic-1", request, sizeof(request)) == 0);
      assert(aimee_module_client_call(client, AIMEE_GIT_EVENT_REF_VALIDATE,
                                      AIMEE_GIT_STAGE_REF_VALIDATE, 2016, 0, request,
                                      AIMEE_GIT_REF_REQUEST_LEN, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_git_ref_response_decode(response, response_len, &allowed) == 0 && allowed);
      assert(aimee_git_ref_request_encode(
                 "aimee/wi/wi_57186250728b511961573e5afb37cc93.s4263a4834d.g0.0", request,
                 sizeof(request)) == 0);
      assert(aimee_module_client_call(client, AIMEE_GIT_EVENT_REF_VALIDATE,
                                      AIMEE_GIT_STAGE_REF_VALIDATE, 2017, 0, request,
                                      AIMEE_GIT_REF_REQUEST_LEN, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_git_ref_response_decode(response, response_len, &allowed) == 0 && allowed);
   }
   else if (strcmp(name, "skills") == 0)
   {
      int fire = 0;
      assert(aimee_skills_request_encode(12, 6, request, sizeof(request)) == 0);
      request_len = AIMEE_SKILLS_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, 1, 2007, 0, request, request_len, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_skills_response_decode(response, response_len, &fire) == 0 && fire);

      const char *content = "---\nname: wait\ntriggers:\n  tool: [Bash]\n"
                            "  arg_pattern: [\"sleep \"]\n---\nWait safely.\n";
      size_t encoded_len = aimee_skills_trigger_request_size(content, "Bash", "sleep 5");
      int match = 0;
      assert(encoded_len > 0 && encoded_len <= sizeof(request) && encoded_len <= UINT32_MAX);
      assert(aimee_skills_trigger_request_encode(content, "Bash", "sleep 5", request,
                                                 sizeof(request)) == 0);
      assert(aimee_module_client_call(client, AIMEE_SKILLS_EVENT_TRIGGER,
                                      AIMEE_SKILLS_STAGE_TRIGGER, 2017, 0, request,
                                      (uint32_t)encoded_len, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_skills_trigger_response_decode(response, response_len, &match) == 0 && match);
   }
   else if (strcmp(name, "response-composition") == 0)
   {
      const aimee_response_key_input_t input = {.principal = "uid:1",
                                                .source = "openai-ingress",
                                                .provider = "openai",
                                                .model = "gpt-4o",
                                                .endpoint = "/v1/chat/completions",
                                                .idempotency_key = "idem-a",
                                                .body = "{\"x\":1}",
                                                .context = "ctx",
                                                .behavior_flags = "cs0 rc0",
                                                .stream = 0};
      size_t encoded_len = aimee_response_request_size(&input);
      char key[AIMEE_RESPONSE_KEY_MAX + 1u];
      assert(encoded_len > 0 && encoded_len <= sizeof(request) && encoded_len <= UINT32_MAX);
      assert(aimee_response_request_encode(&input, request, sizeof(request)) == 0);
      request_len = (uint32_t)encoded_len;
      assert(aimee_module_client_call(client, kind, 1, 2008, 0, request, request_len, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_response_response_decode(response, response_len, key, sizeof(key)) == 0);
      assert(strcmp(key, "uid:1|45fd46a03cb4a28da3227155fec20a71") == 0);
   }
   else if (strcmp(name, "governance") == 0)
   {
      static const char *tools[] = {"read_file", "spawn_agent", "Task"};
      aimee_governance_decision_t decision;
      assert(aimee_governance_request_encode(1, tools, 3, "max_tokens", request, sizeof(request)) ==
             0);
      request_len = AIMEE_GOVERNANCE_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, AIMEE_GOVERNANCE_STAGE_EVALUATE, 2009, 0,
                                      request, request_len, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_governance_response_decode(response, response_len, 3, &decision) == 0);
      assert(decision.keep_mask == 1 && decision.drop_count == 2);
      assert(strcmp(decision.stop_reason, "max_tokens") == 0);
   }
   else if (strcmp(name, "roundtable") == 0)
   {
      aimee_roundtable_verify_action_t action = AIMEE_ROUNDTABLE_VERIFY_REJECT;
      char severity[AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u];
      assert(aimee_roundtable_request_encode(AIMEE_ROUNDTABLE_REPLAY_MATCH, 1, "blocking", request,
                                             sizeof(request)) == 0);
      request_len = AIMEE_ROUNDTABLE_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, AIMEE_ROUNDTABLE_STAGE_DELIBERATE, 2011, 0,
                                      request, request_len, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_roundtable_response_decode(response, response_len, &action, severity,
                                              sizeof(severity)) == 0);
      assert(action == AIMEE_ROUNDTABLE_VERIFY_KEEP && strcmp(severity, "blocking") == 0);
   }
   else if (strcmp(name, "kb-synthesis") == 0)
   {
      static const char *callees[] = {"strlen", "PQexec", "write"};
      aimee_kb_synthesis_grounding_decision_t decision;
      assert(aimee_kb_synthesis_request_encode(AIMEE_KB_SYNTHESIS_CLAIM_NONE, NULL, 0, callees, 3,
                                               request, sizeof(request)) == 0);
      request_len = AIMEE_KB_SYNTHESIS_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, AIMEE_KB_SYNTHESIS_STAGE_GROUNDING, 2012, 0,
                                      request, request_len, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_kb_synthesis_response_decode(response, response_len, &decision) == 0);
      assert(decision.contradicts && strcmp(decision.reason, "PQexec") == 0);
   }
   else if (strcmp(name, "runtime-web") == 0)
   {
      const char *kinds[] = {"permission_denied", "conflict", "review_required",
                             "unsupported_mode"};
      const uint32_t statuses[] = {403u, 409u, 409u, 400u};
      for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++)
      {
         uint32_t status = 0;
         assert(aimee_runtime_web_request_encode(kinds[i], request, sizeof(request)) == 0);
         request_len = AIMEE_RUNTIME_WEB_REQUEST_LEN;
         assert(aimee_module_client_call(client, kind, AIMEE_RUNTIME_WEB_STAGE_CLASSIFY, 2013 + i,
                                         0, request, request_len, response, sizeof(response),
                                         &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
         assert(aimee_runtime_web_response_decode(response, response_len, &status) == 0);
         assert(status == statuses[i]);
      }
   }
   else if (strcmp(name, "control-web") == 0)
   {
      int allowed = 0;
      assert(aimee_control_web_request_encode(AIMEE_CONTROL_WEB_TARGET_FLEET, "GET",
                                              "/v1/servers/s1/health", request,
                                              sizeof(request)) == 0);
      request_len = AIMEE_CONTROL_WEB_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, AIMEE_CONTROL_WEB_STAGE_AUTHORIZE, 2014, 0,
                                      request, request_len, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_control_web_response_decode(response, response_len, &allowed) == 0 && allowed);
   }
   else if (strcmp(name, "sandbox") == 0)
   {
      /* The sandbox stages carry JSON, not the fixed framing the other modules
       * use: a shell command and a git root are variable-length. Load a project
       * with nothing learned, which must still answer with a packages field
       * rather than an error. */
      const char *probe = "{\"git_root\":\"/probe\"}";
      request_len = (uint32_t)strlen(probe);
      assert(request_len <= sizeof(request));
      memcpy(request, probe, request_len);
      assert(aimee_module_client_call(client, AIMEE_SANDBOX_EVENT_LOAD, AIMEE_SANDBOX_STAGE_LOAD,
                                      2016, 0, request, request_len, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "packages") != NULL);

      /* The write path, which nothing else exercises across the process
       * boundary. OBSERVE is what sandbox_learned_observe() emits after it has
       * resolved a git root, so this is the same call the delegate shell tool
       * makes -- minus the model. Parsing lives in the module now, so the
       * round-trip is the only thing that proves the C caller's payload is
       * shaped the way the Go side decodes it. */
      const char *learn = "{\"git_root\":\"/probe\",\"command\":\"apt-get install -y tree\"}";
      request_len = (uint32_t)strlen(learn);
      assert(request_len <= sizeof(request));
      memcpy(request, learn, request_len);
      assert(aimee_module_client_call(client, AIMEE_SANDBOX_EVENT_OBSERVE,
                                      AIMEE_SANDBOX_STAGE_OBSERVE, 2017, 0, request, request_len,
                                      response, sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      /* Not just "it answered": it parsed one package and persisted it. */
      assert(strstr((const char *)response, "\"recorded\":1") != NULL);
      assert(strstr((const char *)response, "tree") != NULL);

      /* Read it back through the separate LOAD stage, so the assertion covers
       * the store actually landing on disk rather than the handler echoing its
       * own input. */
      request_len = (uint32_t)strlen(probe);
      memcpy(request, probe, request_len);
      assert(aimee_module_client_call(client, AIMEE_SANDBOX_EVENT_LOAD, AIMEE_SANDBOX_STAGE_LOAD,
                                      2018, 0, request, request_len, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "tree") != NULL);

      /* The package proxy keeps sockets and DNS in C, but all request and
       * address decisions live in this process. Exercise both policy stages
       * over the real C-host/Go-module boundary. */
      const char *proxy = "{\"line\":\"CONNECT registry.npmjs.org:443 HTTP/1.1\"}";
      request_len = (uint32_t)strlen(proxy);
      memcpy(request, proxy, request_len);
      assert(aimee_module_client_call(client, AIMEE_SANDBOX_EVENT_PROXY_REQUEST,
                                      AIMEE_SANDBOX_STAGE_PROXY_REQUEST, 2022, 0, request,
                                      request_len, response, sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "\"kind\":2") != NULL);
      assert(strstr((const char *)response, "\"allowed\":true") != NULL);

      const char *address = "{\"ip\":\"169.254.169.254\"}";
      request_len = (uint32_t)strlen(address);
      memcpy(request, address, request_len);
      assert(aimee_module_client_call(client, AIMEE_SANDBOX_EVENT_PROXY_ADDRESS,
                                      AIMEE_SANDBOX_STAGE_PROXY_ADDRESS, 2023, 0, request,
                                      request_len, response, sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "\"blocked\":true") != NULL);
   }
   else if (strcmp(name, "economizer") == 0)
   {
      static const uint8_t expected[] = {'J', 'C', 'M', 'P', 1, 0, 0, 0, 2, 0, 0, 0, '{', '}'};
      const char input[] = " { } ";
      assert(aimee_module_client_call(client, kind, AIMEE_ECONOMIZER_STAGE_JSON_COMPACT, 2019, 0,
                                      input, sizeof(input) - 1, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len == sizeof(expected) && memcmp(response, expected, sizeof(expected)) == 0);

      assert(aimee_module_client_call(client, AIMEE_ECONOMIZER_EVENT_TOOL_STATS,
                                      AIMEE_ECONOMIZER_STAGE_TOOL_STATS, 2020, 0, NULL, 0, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len == 72 && memcmp(response, "TSTA\1\0\0\0", 8) == 0);

      const char record_request[] =
          "{\"messages\":[{\"role\":\"assistant\",\"content\":\"[done] changed "
          "src/server/session_compact.c\"}],\"start\":0,\"end\":1}";
      assert(aimee_module_client_call(client, AIMEE_ECONOMIZER_EVENT_RECORD_BUILD,
                                      AIMEE_ECONOMIZER_STAGE_RECORD_BUILD, 2021, 0, record_request,
                                      sizeof(record_request) - 1, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "src/server/session_compact.c") != NULL);
      assert(strstr((const char *)response, "decisions_made") != NULL);
      assert(strstr((const char *)response, "[done] changed") != NULL);

      /* Real C host and shipped Go handler: preserve the metadata commitment
       * and distinguish exact-fit admission from one-byte overflow. */
      const char limits[] = "{\"schema_version\":1,\"max_request_bytes\":3}";
      uint8_t budget[256] = {'B', 'D', 'G', 'T', 1, 0, 1, 0};
      budget[48] = sizeof(limits) - 1;
      memcpy(budget + 52, limits, sizeof(limits) - 1);
      assert(SHA256((const unsigned char *)"abc", 3, budget + 16));
      for (unsigned size = 3; size <= 4; size++)
      {
         budget[8] = size;
         uint8_t commitment[SHA256_DIGEST_LENGTH];
         assert(SHA256(budget, 52 + sizeof(limits) - 1, commitment));
         assert(aimee_module_client_call(client, AIMEE_ECONOMIZER_EVENT_REQUEST_BUDGET,
                                         AIMEE_ECONOMIZER_STAGE_REQUEST_BUDGET, 2024 + size, 0,
                                         budget, 52 + sizeof(limits) - 1, response,
                                         sizeof(response), &response_len, NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
         assert(response_len == 44 && memcmp(response, "BDGT\1\0", 6) == 0);
         assert(response[6] == (size == 3 ? 0 : 2) && response[7] == 0);
         assert(response[8] == 32 && response[9] == 0 && response[10] == 0 && response[11] == 0);
         assert(memcmp(response + 12, commitment, sizeof(commitment)) == 0);
      }
      /* Policy metadata v2: absent or more permissive caller limits cannot
       * raise the operator cap. The response binds both independent layers. */
      const char permissive[] = "{\"schema_version\":1,\"max_request_bytes\":999}";
      for (unsigned caller = 0; caller <= 1; caller++)
         for (unsigned size = 3; size <= 4; size++)
         {
            memset(budget, 0, sizeof(budget));
            memcpy(budget, "BDGT\2\0\1\0", 8);
            budget[8] = size;
            assert(SHA256((const unsigned char *)"abc", 3, budget + 16));
            unsigned caller_len = caller ? sizeof(permissive) - 1 : 0;
            budget[48] = caller_len;
            budget[52] = sizeof(limits) - 1;
            memcpy(budget + 56, permissive, caller_len);
            memcpy(budget + 56 + caller_len, limits, sizeof(limits) - 1);
            unsigned length = 56 + caller_len + sizeof(limits) - 1;
            uint8_t commitment[SHA256_DIGEST_LENGTH];
            assert(SHA256(budget, length, commitment));
            assert(aimee_module_client_call(client, AIMEE_ECONOMIZER_EVENT_REQUEST_BUDGET,
                                            AIMEE_ECONOMIZER_STAGE_REQUEST_BUDGET, 2030 + size, 0,
                                            budget, length, response, sizeof(response),
                                            &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
            assert(response_len == 44 && memcmp(response, "BDGT\1\0", 6) == 0);
            assert(response[6] == (size == 3 ? 0 : 2) && response[7] == 0);
            assert(memcmp(response + 12, commitment, sizeof(commitment)) == 0);
         }
   }
   else if (strcmp(name, "postgres") == 0)
   {
      /* The health stage is REACHED, and with no database configured it reports
       * a typed failure rather than inventing an answer.
       *
       * That refusal is the assertion. A probe that fabricated health when it
       * could not reach the store would be wrong exactly when it mattered, and
       * this harness deliberately stands up no database -- so a failure here is
       * the contract being kept. What is proven either way is that the C caller
       * reaches the Go stage and gets its verdict back. */
      assert(aimee_postgres_health_request_encode(request, sizeof(request)) == 0);
      aimee_module_call_result_t health = aimee_module_client_call(
          client, AIMEE_POSTGRES_EVENT_HEALTH, AIMEE_POSTGRES_STAGE_HEALTH, 2101, 0, request,
          AIMEE_POSTGRES_REQUEST_LEN, response, sizeof(response), &response_len, NULL, NULL);
      if (health == AIMEE_MODULE_CALL_OK)
         assert(response_len == AIMEE_POSTGRES_RESPONSE_LEN);
   }
   else if (strcmp(name, "execution-policy") == 0)
   {
      /* A tool decision, JSON both ways. "rm -rf /" is the case where a policy
       * that failed open would be catastrophic, so the assertion is that it is
       * REFUSED rather than merely that something came back. */
      static const char body[] = "{\"tool\":\"bash\",\"side_effect\":\"destructive\","
                                 "\"arguments\":{\"command\":\"rm -rf /\"}}";
      assert(sizeof(body) - 1 <= sizeof(request));
      memcpy(request, body, sizeof(body) - 1);
      assert(aimee_module_client_call(client, AIMEE_EXECUTION_POLICY_EVENT_TOOL,
                                      AIMEE_EXECUTION_POLICY_STAGE_TOOL, 2102, 0, request,
                                      (uint32_t)(sizeof(body) - 1), response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "\"allowed\"") != NULL);
   }
   else if (strcmp(name, "providers") == 0)
   {
      memset(request, 0, AIMEE_PROVIDERS_VALIDATE_REQUEST_LEN);
      aimee_providers_put_u32(request, AIMEE_PROVIDERS_REQUEST_MAGIC);
      aimee_providers_put_u32(request + 4, AIMEE_PROVIDERS_WIRE_VERSION);
      uint8_t *record = request + AIMEE_PROVIDERS_OFF_DECLARED_RECORD;
      assert(aimee_providers_put_str(record, AIMEE_PROVIDERS_NAME_MAX, "fixture") == 0);
      assert(aimee_providers_put_str(record + 32, AIMEE_PROVIDERS_MODEL_MAX, "fixture-model") == 0);
      aimee_providers_put_u32(record + 224, 1024);
      aimee_providers_put_u32(record + 228, 4096);
      aimee_providers_put_u32(record + 236, AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW |
                                                AIMEE_PROVIDERS_DECL_MAX_OUTPUT);
      assert(aimee_module_client_call(
                 client, AIMEE_PROVIDERS_EVENT_VALIDATE, AIMEE_PROVIDERS_STAGE_VALIDATE, 2105, 0,
                 request, AIMEE_PROVIDERS_VALIDATE_REQUEST_LEN, response, sizeof(response),
                 &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_providers_get_u32(response + 8) == AIMEE_PROVIDERS_ERR_INVALID_DECLARATION);

      static const char body[] = "{\"operation\":\"provider.connections\",\"arguments\":{}}";
      assert(aimee_module_client_call(client, AIMEE_PROVIDERS_EVENT_MANAGE,
                                      AIMEE_PROVIDERS_STAGE_MANAGE, 2106, 0, body, sizeof(body) - 1,
                                      response, sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "\"providers\":[]") != NULL);
   }
   else if (strcmp(name, "egress") == 0)
   {
      /* Exercise the authorization stage without touching the network. An
       * incomplete digest must be denied with the current policy revision. */
      static const char body[] =
          "{\"target_url\":\"https://api.github.com/repos/o/r\",\"purpose\":\"forge\","
          "\"method\":\"GET\",\"request_sha256\":\"invalid\"}";
      assert(sizeof(body) - 1 <= sizeof(request));
      memcpy(request, body, sizeof(body) - 1);
      assert(aimee_module_client_call(client, AIMEE_EGRESS_EVENT_AUTHORIZE,
                                      AIMEE_EGRESS_STAGE_AUTHORIZE, 2103, 0, request,
                                      (uint32_t)(sizeof(body) - 1), response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "\"allowed\":false") != NULL);
      assert(strstr((const char *)response, "module-egress-v1") != NULL);

      response_len = 0;
      assert(aimee_module_client_call(client, AIMEE_EGRESS_EVENT_CREDENTIAL_KEY,
                                      AIMEE_EGRESS_STAGE_CREDENTIAL_KEY, 2104, 0, NULL, 0, response,
                                      sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(response_len > 0 && response_len < sizeof(response));
      response[response_len] = '\0';
      assert(strstr((const char *)response, "\"version\":1") != NULL);
      assert(strstr((const char *)response, "\"public_key\":") != NULL);
   }
   else
   {
      assert(strcmp(name, "benchmarks") == 0);
      const int64_t retrieved[] = {5, 9, 7};
      const int64_t relevant[] = {9};
      aimee_benchmarks_ir_scores_t scores;
      assert(aimee_benchmarks_request_encode(retrieved, 3, relevant, 1, 3, request,
                                             sizeof(request)) == 0);
      request_len = AIMEE_BENCHMARKS_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, AIMEE_BENCHMARKS_STAGE_RUN, 2015, 0, request,
                                      request_len, response, sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_benchmarks_response_decode(response, response_len, &scores) == 0);
      assert(scores.mrr == 0.5 && scores.recall == 1.0);
      assert(scores.ndcg > 0.630929753571 && scores.ndcg < 0.630929753572);

      const double latencies[] = {10.0, 1.0, 5.0, 3.0, 8.0};
      aimee_benchmarks_latency_summary_t summary;
      assert(aimee_benchmarks_latency_request_encode(latencies, 5, request, sizeof(request)) == 0);
      request_len = AIMEE_BENCHMARKS_LATENCY_REQUEST_LEN;
      assert(aimee_module_client_call(client, AIMEE_BENCHMARKS_EVENT_LATENCY,
                                      AIMEE_BENCHMARKS_STAGE_LATENCY, 2016, 0, request, request_len,
                                      response, sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_benchmarks_latency_response_decode(response, response_len, &summary) == 0);
      assert(summary.queries == 5 && summary.p50_ms == 5.0 && summary.p95_ms == 10.0 &&
             summary.p99_ms == 10.0 && summary.min_ms == 1.0 && summary.max_ms == 10.0);
   }
}

/* Attach the trusted embedding host before opening the external admission
 * socket, matching the daemon's principal-zero in-process connection. */
typedef struct
{
   bus_host_t *host;
   int socket;
} trusted_attach_t;
static void *trusted_attach(void *arg)
{
   trusted_attach_t *a = arg;
   assert(bus_host_serve_attach(a->host, a->socket) == BUS_HOST_OK);
   return NULL;
}
static void trusted_client(bus_host_t *host, bus_client_t *client)
{
   int sockets[2];
   assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
   trusted_attach_t args = {host, sockets[1]};
   pthread_t thread;
   assert(pthread_create(&thread, NULL, trusted_attach, &args) == 0);
   assert(bus_client_attach(sockets[0], client) == BUS_CLIENT_OK);
   assert(pthread_join(thread, NULL) == 0);
   close(sockets[0]);
   close(sockets[1]);
}
static void command_word(uint8_t *p, uint32_t value)
{
   p[0] = value;
   p[1] = value >> 8;
   p[2] = value >> 16;
   p[3] = value >> 24;
}
static cJSON *host_plan(aimee_module_client_t *client, const char *args)
{
   uint8_t request[2048] = {0}, reply[8192] = {0};
   size_t length = strlen(args);
   assert(length + 23 < sizeof(request));
   command_word(request, 0x51504d43u);
   command_word(request + 4, 1);
   command_word(request + 8, 7);
   command_word(request + 12, (uint32_t)length);
   memcpy(request + 16, "runtime", 7);
   memcpy(request + 23, args, length);
   struct timespec now;
   assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
   uint64_t deadline = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec + 2000000000ULL;
   uint32_t reply_length = 0;
   assert(aimee_module_client_call(client, 4096u + 7u * 256u + 8u, 8u, 8801, deadline, request,
                                   (uint32_t)(23 + length), reply, sizeof(reply), &reply_length,
                                   NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reply_length > 12 && memcmp(reply, "CMPS", 4) == 0 && reply[4] == 1);
   cJSON *json = cJSON_ParseWithLength((const char *)reply + 12, reply_length - 12);
   assert(json && strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "status")),
                         "ok") == 0);
   return json;
}
static const char *json_string(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   assert(value);
   return value;
}
static void smoke_host_gateway_plan(bus_client_t *host)
{
   aimee_module_client_t client;
   assert(aimee_module_client_init(&client, host) == 0);
   cJSON *plan = host_plan(
       &client,
       "{\"operation\":\"gateway-plan\",\"phase\":\"context\",\"roles\":[\"user\"],\"tools\":[],"
       "\"provided_query\":\"deploy matrix\",\"last_user_text\":\"persona text\"}");
   cJSON *steps = cJSON_GetObjectItemCaseSensitive(plan, "steps");
   assert(cJSON_GetArraySize(steps) == 4);
   cJSON *retrieve = cJSON_GetArrayItem(steps, 0);
   assert(strcmp(json_string(retrieve, "binding"), "context") == 0);
   assert(strcmp(json_string(cJSON_GetObjectItemCaseSensitive(retrieve, "args"), "query"),
                 "deploy matrix") == 0);
   cJSON *guidance = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(steps, 1), "context");
   cJSON *evidence = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(steps, 3), "context");
   assert(strcmp(json_string(guidance, "origin"), "platform") == 0 &&
          strcmp(json_string(guidance, "authority"), "task_instruction") == 0);
   assert(strcmp(json_string(evidence, "origin"), "retrieval") == 0 &&
          strcmp(json_string(evidence, "authority"), "evidence") == 0);
   cJSON_Delete(plan);
   plan = host_plan(&client, "{\"operation\":\"gateway-plan\",\"phase\":\"tools\",\"roles\":["
                             "\"user\"],\"tools\":[\"exec_command\",\"apply_patch\",\"shell\"]}");
   steps = cJSON_GetObjectItemCaseSensitive(plan, "steps");
   assert(cJSON_GetArraySize(steps) == 1);
   cJSON *indices = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(steps, 0), "indices");
   assert(cJSON_GetArraySize(indices) == 2 && cJSON_GetArrayItem(indices, 0)->valueint == 2 &&
          cJSON_GetArrayItem(indices, 1)->valueint == 0);
   cJSON_Delete(plan);
   const char *claim = "{\"operation\":\"ingress-task-claim\",\"session\":\"live\",\"project\":"
                       "\"p\",\"query\":\"fix resolver\"}";
   plan = host_plan(&client, claim);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "fetch")));
   cJSON_Delete(plan);
   plan = host_plan(&client, claim);
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(plan, "fetch")));
   cJSON_Delete(plan);
   plan = host_plan(
       &client, "{\"operation\":\"ingress-task-rearm\",\"session\":\"live\",\"project\":\"p\"}");
   cJSON_Delete(plan);
   plan = host_plan(&client, claim);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "fetch")));
   cJSON_Delete(plan);
   plan =
       host_plan(&client, "{\"operation\":\"ingress-task-packet\",\"project\":\"p\",\"packet\":{"
                          "\"status\":\"ok\",\"project\":\"p\",\"generation\":7,\"freshness\":"
                          "\"current\",\"resolved\":true,"
                          "\"max_results\":4,\"max_tokens\":1200,\"item_count\":1,"
                          "\"answerability\":{\"decision\":\"answerable\"},"
                          "\"results\":[{\"project\":\"p\",\"file_path\":\"local.go\","
                          "\"generation\":7,\"freshness\":\"current\","
                          "\"confidence\":0.9,\"accepted\":true,\"provenance\":[\"code\"],\"span\":"
                          "{\"kind\":\"line\",\"line_start\":12,\"line_end\":12}}],\"why\":[]}}");
   assert(strstr(json_string(plan, "block"), "local.go:12 [confidence=0.90; provenance=code]") !=
          NULL);
   assert(cJSON_GetObjectItemCaseSensitive(plan, "item_count")->valueint == 1);
   cJSON_Delete(plan);
   plan =
       host_plan(&client, "{\"operation\":\"ingress-assemble\",\"budget\":1200,\"code\":[{\"file_"
                          "path\":\"local.go\",\"snippet\":\"local resolver\",\"line\":12}],"
                          "\"memories\":[{\"id\":\"9223372036854775807\",\"headline\":\"Use the "
                          "local resolver.\",\"score\":0.95}]}");
   const char *envelope = json_string(plan, "envelope");
   assert(strstr(envelope, "<aimee-context confidence=\"low\">") == envelope);
   assert(strstr(envelope, "memory:9223372036854775807") != NULL);
   assert(strstr(envelope, "local.go\n    > local resolver") != NULL);
   cJSON_Delete(plan);
   plan = host_plan(
       &client, "{\"operation\":\"ingress-begin\",\"session\":\"live-plan\",\"project\":\"p\","
                "\"query\":\"fix resolver\","
                "\"active_scope\":true,\"preview_enabled\":true,\"mode\":\"on\",\"budget\":1200}");
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "active")));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "task")));
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(plan, "legacy_preview")));
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(plan, "facts")));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "temporal")));
   cJSON_Delete(plan);
   plan = host_plan(&client, "{\"operation\":\"ingress-recall-result\",\"project\":\"p\",\"count\":"
                             "0,\"unavailable\":true}");
   assert(strstr(json_string(plan, "warning"), "UNAVAILABLE (not empty)") != NULL);
   cJSON_Delete(plan);
   plan = host_plan(&client, "{\"operation\":\"ingress-metrics\"}");
   assert(cJSON_GetObjectItemCaseSensitive(plan, "recall_unavailable_total")->valueint == 1);
   cJSON_Delete(plan);
   aimee_module_client_destroy(&client);
   puts("memory: authenticated host/Go process gateway plans and ingress policy passed");
}

int main(int argc, char **argv)
{
   assert(argc >= 1 && argc <= 4);
   uint32_t test_kind = TEST_KIND, module_ref = MODULE_REF;
   uint32_t served[PRODUCTION_STAGE_MAX] = {test_kind};
   size_t serve_count = 1;
   if (argc >= 3)
      assert(production_contract(argv[2], &test_kind, &module_ref, served, &serve_count) == 0);
   const int memory_process = argc >= 3 && strcmp(argv[2], "memory") == 0;
   const int provider_process = argc >= 3 && strcmp(argv[2], "providers") == 0;
   char directory[256];
   snprintf(directory, sizeof directory, "%s/aimee-module-runtime-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory) != NULL);
   /* Point the spawned module at a throwaway home BEFORE it is forked: the
    * sandbox module persists what it learns under AIMEE_HOME, and a test that
    * exercises the write path must not touch the developer's real store. */
   assert(setenv("AIMEE_HOME", directory, 1) == 0);
   if (memory_process)
   {
      assert(argc == 4); /* policy parity is tested by the Go caller */
      const char *placement = getenv("AIMEE_TEST_MEMORY_PLACEMENT");
      assert(!placement || strcmp(placement, "server") == 0 || strcmp(placement, "kb") == 0);
      assert(setenv("AIMEE_MODULE_PLACEMENT", placement ? placement : "server", 1) == 0);
   }
   char socket_path[PATH_MAX], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof socket_path, "%s/module.sock", directory) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);

   char module_executable[PATH_MAX];
   if (argc >= 2)
      assert(realpath(argv[1], module_executable) != NULL);
   else
      assert(snprintf(module_executable, sizeof module_executable, "%s", executable) > 0);

   char probe_executable[PATH_MAX] = "";
   if (argc == 4)
      assert(realpath(argv[3], probe_executable) != NULL);

   bus_instance_role_t role = argc >= 3 && strcmp(argv[2], "server") == 0 ? BUS_INSTANCE_SERVER
                              : argc >= 3 && strcmp(argv[2], "kb") == 0   ? BUS_INSTANCE_KB
                                                                          : BUS_INSTANCE_UNSET;
   if (role != BUS_INSTANCE_UNSET)
      assert(bus_instance_ensure_identity(directory, role, module_executable) == 0);

   uint32_t requested[PRODUCTION_STAGE_MAX + 1] = {0};
   memcpy(requested, served, serve_count * sizeof(*requested));
   requested[serve_count] = EMPTY_KIND;
   const uint32_t postgres_request[] = {AIMEE_POSTGRES_EVENT_SQL};
   const uint32_t action_publish[] = {3000u};
   const uint32_t provider_request[] = {12290u, 12295u, 4609u};
   bus_runtime_grant_t grants[] = {{.principal_class = 1,
                                    .principal_ref = module_ref,
                                    .uid = BUS_RUNTIME_SELF_UID,
                                    .executable = module_executable,
                                    .serve = served,
                                    .serve_count = serve_count},
                                   {.principal_class = 1,
                                    .principal_ref = CALLER_REF,
                                    .uid = BUS_RUNTIME_SELF_UID,
                                    .executable = executable,
                                    .request = requested,
                                    .request_count = serve_count + 1},
                                   /* The migrated memory process owns its SQL
                                    * and action observations through a second identity.
                                    * The smoke calls below are deliberately
                                    * store-free, but startup must still prove
                                    * that the shipped process can attach the
                                    * capability it will use in production. */
                                   {.principal_class = 1,
                                    .principal_ref = memory_process ? 73 : 74,
                                    .uid = BUS_RUNTIME_SELF_UID,
                                    .executable = module_executable,
                                    .publish = memory_process ? action_publish : NULL,
                                    .publish_count = memory_process ? 1 : 0,
                                    .request = memory_process ? postgres_request : provider_request,
                                    .request_count = memory_process ? 1 : 3},
                                   {.principal_class = 1,
                                    .principal_ref = 200,
                                    .uid = BUS_RUNTIME_SELF_UID,
                                    .executable = probe_executable,
                                    .request = requested,
                                    .request_count = serve_count}};
   bus_host_config_t host_config = {.max_slots = 8,
                                    .slot_size = 512,
                                    .inline_budget = 400,
                                    .queue_capacity = 16,
                                    .arena_size = 16384};
   bus_host_t host;
   assert(bus_host_create(&host, &host_config, NULL, NULL) == BUS_HOST_OK);
   bus_client_t embedding_host;
   if (memory_process)
      trusted_client(&host, &embedding_host);
   pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t runtime_config = {.instance_role = role,
                                          .socket_path = socket_path,
                                          .socket_mode = 0600,
                                          .backlog = 8,
                                          .stale_after_ns = 5000000000ULL,
                                          .grants = grants,
                                          .grant_count = argc == 4 ? 4
                                                         : (memory_process || provider_process)
                                                             ? 3
                                                             : 2};
   bus_runtime_t *runtime = bus_runtime_start(&host, &host_lock, &runtime_config);
   assert(runtime != NULL);

   static const aimee_module_stage_t stages[] = {{TEST_KIND, TEST_STAGE}};
   process_thread_t process = {.config = {.socket_path = socket_path,
                                          .module_name = "test-module",
                                          .principal_class = 1,
                                          .principal_ref = MODULE_REF,
                                          .stages = stages,
                                          .stage_count = 1,
                                          .handler = handle}};
   pthread_t module_thread;
   pid_t module_pid = -1;
   if (argc >= 2)
   {
      module_pid = spawn_module_child(module_executable, socket_path, NULL);
   }
   else
      assert(pthread_create(&module_thread, NULL, run_process, &process) == 0);

   int caller_fd = -1;
   bus_client_t caller;
   assert(bus_endpoint_connect(socket_path, &caller_fd) == 0);
   assert(bus_client_attach_as(caller_fd, &caller, 1, CALLER_REF) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&caller_fd) == 0);
   wait_for_clients(&host, &host_lock, memory_process ? 4 : provider_process ? 3 : 2, module_pid);

   pump_thread_t pump_state = {.host = &host, .lock = &host_lock};
   atomic_init(&pump_state.stop, 0);
   pthread_t pump_thread;
   assert(pthread_create(&pump_thread, NULL, run_pump, &pump_state) == 0);

   aimee_module_client_t module_client;
   assert(aimee_module_client_init(&module_client, &caller) == 0);
   if (memory_process)
      smoke_host_gateway_plan(&embedding_host);

   if (argc >= 3)
   {
      if (argc == 4)
      {
         run_memory_probe(probe_executable, socket_path);
         if (memory_process)
         {
            assert(kill(module_pid, SIGKILL) == 0);
            int status = 0;
            while (waitpid(module_pid, &status, 0) < 0)
               assert(errno == EINTR);
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            wait_for_memory_departure(runtime, &host, &host_lock, &caller, &embedding_host);
            memory_discovery_unavailable(&module_client);
            module_pid = spawn_module_child(module_executable, socket_path, NULL);
            wait_for_clients(&host, &host_lock, 4, module_pid);
            smoke_host_gateway_plan(&embedding_host);
            run_memory_probe(probe_executable, socket_path);
            puts("memory: terminated provider unavailable; restarted Go owner passed host/client "
                 "parity");
         }
      }
      else
         smoke_production_module(&module_client, argv[2], test_kind);
      goto finish;
   }

   char body[64];
   uint32_t body_len = 0;
   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1001, 0, "real-result",
                                   11, body, sizeof body, &body_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(body_len == 11 && memcmp(body, "real-result", 11) == 0);

   uint8_t *large_request = malloc(LARGE_BODY);
   uint8_t *large_response = malloc(LARGE_BODY);
   assert(large_request != NULL && large_response != NULL);
   for (uint32_t i = 0; i < LARGE_BODY; ++i)
      large_request[i] = (uint8_t)((i * 131U + 17U) & 0xffU);
   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1007, 0, large_request,
                                   LARGE_BODY, large_response, LARGE_BODY, &body_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(body_len == LARGE_BODY && memcmp(large_request, large_response, LARGE_BODY) == 0);

   /* A too-small destination still drains every response fragment and reports
    * the complete response length, leaving the next correlation usable. */
   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1008, 0, large_request,
                                   LARGE_BODY, body, sizeof body, &body_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE);
   assert(body_len == LARGE_BODY);
   free(large_response);
   free(large_request);

   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1002, 1, "late", 4, body,
                                   sizeof body, &body_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(body_len == 0);

   atomic_int cancel;
   atomic_init(&cancel, 0);
   pthread_t cancel_thread;
   assert(pthread_create(&cancel_thread, NULL, cancel_soon, &cancel) == 0);
   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1003, 0, "cancel", 6,
                                   body, sizeof body, &body_len, cancellation_flag,
                                   &cancel) == AIMEE_MODULE_CALL_CANCELLED);
   assert(pthread_join(cancel_thread, NULL) == 0 && body_len == 0);

   /* The cancelled handler's terminal reply may arrive after call() returned.
    * A subsequent call must drain that stale correlation and still complete. */
   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1004, 0, "after", 5, body,
                                   sizeof body, &body_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(body_len == 5 && memcmp(body, "after", 5) == 0);

   assert(aimee_module_client_call(&module_client, EMPTY_KIND, 2, 1005, 0, NULL, 0, body,
                                   sizeof body, &body_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_CAPABILITY_ABSENT);

   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1006, 0, "toolarge", 8,
                                   body, 3, &body_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE);
   assert(body_len == 8);

finish:
   aimee_module_client_destroy(&module_client);
   if (module_pid > 0)
   {
      assert(kill(module_pid, SIGTERM) == 0);
      int status = 0;
      while (waitpid(module_pid, &status, 0) < 0)
         assert(errno == EINTR);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   else
   {
      aimee_module_process_stop();
      assert(pthread_join(module_thread, NULL) == 0 && process.result == 0);
   }
   atomic_store_explicit(&pump_state.stop, 1, memory_order_release);
   assert(pthread_join(pump_thread, NULL) == 0);
   bus_client_detach(&caller);
   if (memory_process)
      bus_client_detach(&embedding_host);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&host_lock);
   /* The sandbox leg drives a stage that persists, so this run leaves a store
    * behind in the throwaway home. Remove exactly that file and keep the rmdir
    * assertion strict, so anything ELSE a module wrote still fails loudly
    * rather than being swept up by a recursive delete. */
   char learned_store[PATH_MAX];
   assert(snprintf(learned_store, sizeof learned_store, "%s/sandbox-learned.json", directory) > 0);
   (void)unlink(learned_store); /* absent for every other module: not an error */
   assert(snprintf(learned_store, sizeof learned_store, "%s/.providers.lock", directory) > 0);
   (void)unlink(learned_store);
   if (role != BUS_INSTANCE_UNSET)
   {
      snprintf(learned_store, sizeof(learned_store), "%s/instance-identity.json", directory);
      assert(unlink(learned_store) == 0);
   }
   assert(rmdir(directory) == 0);
   if (argc >= 3)
      printf("module runtime (%s): %s caller/Go handler wire parity passed\n", argv[2],
             argc == 4 ? "Go" : "C");
   else
      printf(
          "module runtime (%s): dispatch, fragmented payloads, deadline, and cancellation passed\n",
          argc == 2 ? "Go process" : "C process");
   return 0;
}
