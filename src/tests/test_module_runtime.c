#define _GNU_SOURCE

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
#include <aimee/git/module_api.h>
#include <aimee/governance/module_api.h>
#include <aimee/kb-synthesis/module_api.h>
#include <aimee/learning/module_api.h>
#include <aimee/memory/module_api.h>
#include "modules/memory/memory_ontology.h" /* NODE_* for the write-gate case */
#include <aimee/response-composition/module_api.h>
#include <aimee/roundtable/module_api.h>
#include <aimee/routing/module_api.h>
#include <aimee/runtime-web/module_api.h>
#include <aimee/skills/module_api.h>
#include <aimee/tools/module_api.h>
#include <aimee/workspace/module_api.h>
#include "economizer_module_client.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
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

#define TEST_KIND            5889U
#define EMPTY_KIND           5890U
#define TEST_STAGE           1U
#define MODULE_REF           7U
#define CALLER_REF           90U
#define LARGE_BODY           (128U * 1024U + 37U)
#define PRODUCTION_STAGE_MAX 5U

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

static void wait_for_clients(bus_host_t *host, pthread_mutex_t *lock, uint32_t count)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int i = 0; i < 2000; ++i)
   {
      pthread_mutex_lock(lock);
      uint32_t admitted = host->admitted;
      pthread_mutex_unlock(lock);
      if (admitted >= count)
         return;
      nanosleep(&pause, NULL);
   }
   assert(!"timed out waiting for module clients");
}

static int production_contract(const char *name, uint32_t *kind, uint32_t *principal_ref,
                               uint32_t served[PRODUCTION_STAGE_MAX], size_t *serve_count)
{
   if (strcmp(name, "memory") == 0)
   {
      *kind = AIMEE_MEMORY_EVENT_RERANK, *principal_ref = 7;
      served[0] = AIMEE_MEMORY_EVENT_EXTRACT_INDEX;
      served[1] = AIMEE_MEMORY_EVENT_WRITE;
      served[2] = AIMEE_MEMORY_EVENT_EMBED;
      served[3] = AIMEE_MEMORY_EVENT_RETRIEVE;
      served[4] = AIMEE_MEMORY_EVENT_RERANK;
      *serve_count = PRODUCTION_STAGE_MAX;
      return 0;
   }
   if (strcmp(name, "learning") == 0)
      *kind = AIMEE_LEARNING_EVENT_OBSERVE, *principal_ref = 8;
   else if (strcmp(name, "routing") == 0)
      *kind = AIMEE_ROUTING_EVENT_KIND, *principal_ref = 9;
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
      *serve_count = 5;
      return 0;
   }
   else
      return -1;
   served[0] = *kind;
   *serve_count = 1;
   return 0;
}

/* The memory module's four decision stages, over the real bus and the real
 * module binary.
 *
 * Everything else that tests these stages does it in process: the Go side
 * against fixtures, the C side against a registered stand-in for the module.
 * Both halves can agree with each other and still not meet in the middle, which
 * is what this crosses. Each call goes through the same encoder the production
 * adapter uses and the same decoder, so an offset either side got wrong shows up
 * as a failed decode rather than as a plausible wrong answer.
 *
 * One case per stage, chosen so the answer could not come from an empty or
 * zeroed response. */
static void smoke_memory_decision_stages(aimee_module_client_t *client)
{
   uint8_t request[1024] = {0};
   uint8_t response[1024] = {0};
   uint32_t response_len = 0;

   /* WRITE: a seeded relation whose ends satisfy it. ACCEPT is 0, so pair it
    * with a rejection below -- a zeroed response would read as ACCEPT. */
   aimee_memory_fact_verdict_t verdict = AIMEE_MEMORY_FACT_BADARG;
   assert(aimee_memory_gate_request_encode(NODE_PERSON, "works_for", NODE_ORG, request,
                                           sizeof(request)) == 0);
   assert(aimee_module_client_call(client, AIMEE_MEMORY_EVENT_WRITE, AIMEE_MEMORY_STAGE_WRITE, 2100,
                                   0, request, AIMEE_MEMORY_GATE_REQUEST_LEN, response,
                                   sizeof(response), &response_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_memory_gate_response_decode(response, response_len, &verdict) == 0);
   assert(verdict == AIMEE_MEMORY_FACT_ACCEPT);

   assert(aimee_memory_gate_request_encode(NODE_DEVICE, "works_for", NODE_ORG, request,
                                           sizeof(request)) == 0);
   assert(aimee_module_client_call(client, AIMEE_MEMORY_EVENT_WRITE, AIMEE_MEMORY_STAGE_WRITE, 2101,
                                   0, request, AIMEE_MEMORY_GATE_REQUEST_LEN, response,
                                   sizeof(response), &response_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_memory_gate_response_decode(response, response_len, &verdict) == 0);
   assert(verdict == AIMEE_MEMORY_FACT_REJECT_KIND); /* a printer does not work for an org */

   /* EXTRACT_INDEX: the canonical template, with the object typed by its shape. */
   aimee_memory_triple_t triples[4];
   uint32_t found = 0;
   assert(aimee_memory_extract_request_encode("my home ip is 192.168.1.254", 4, request,
                                              sizeof(request)) == 0);
   assert(aimee_module_client_call(
              client, AIMEE_MEMORY_EVENT_EXTRACT_INDEX, AIMEE_MEMORY_STAGE_EXTRACT_INDEX, 2102, 0,
              request, (uint32_t)aimee_memory_extract_request_size("my home ip is 192.168.1.254"),
              response, sizeof(response), &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_memory_extract_response_decode(response, response_len, triples, 4, &found) == 0);
   assert(found == 1);
   assert(strcmp(triples[0].subject, "user") == 0);
   assert(strcmp(triples[0].rel_type, "home_ip") == 0);
   assert(strcmp(triples[0].object, "192.168.1.254") == 0);
   assert(triples[0].subject_kind == NODE_PERSON && triples[0].object_kind == NODE_IP);

   /* EXTRACT_INDEX, second shape: the retraction scan. Same stage, different
    * magic, so this also proves the two are told apart on the far side. */
   int is_retraction = -1, has_attr = -1;
   char attr[AIMEE_MEMORY_SCAN_ATTR_MAX] = {0};
   assert(aimee_memory_scan_request_encode("forget my email", request, sizeof(request)) == 0);
   assert(aimee_module_client_call(
              client, AIMEE_MEMORY_EVENT_EXTRACT_INDEX, AIMEE_MEMORY_STAGE_EXTRACT_INDEX, 2103, 0,
              request, (uint32_t)aimee_memory_scan_request_size("forget my email"), response,
              sizeof(response), &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_memory_scan_response_decode(response, response_len, &is_retraction, &has_attr, attr,
                                            sizeof(attr)) == 0);
   assert(is_retraction == 1 && has_attr == 1 && strcmp(attr, "email") == 0);

   /* RETRIEVE: the turn classifier, both answers, so a stage stuck on one of
    * them cannot pass. */
   int requests_sensitive = -1;
   assert(aimee_memory_pii_request_encode("what is my email address", request, sizeof(request)) ==
          0);
   assert(aimee_module_client_call(
              client, AIMEE_MEMORY_EVENT_RETRIEVE, AIMEE_MEMORY_STAGE_RETRIEVE, 2104, 0, request,
              (uint32_t)aimee_memory_pii_request_size("what is my email address"), response,
              sizeof(response), &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_memory_pii_response_decode(response, response_len, &requests_sensitive) == 0);
   assert(requests_sensitive == 1);

   assert(aimee_memory_pii_request_encode("what is the weather", request, sizeof(request)) == 0);
   assert(aimee_module_client_call(
              client, AIMEE_MEMORY_EVENT_RETRIEVE, AIMEE_MEMORY_STAGE_RETRIEVE, 2105, 0, request,
              (uint32_t)aimee_memory_pii_request_size("what is the weather"), response,
              sizeof(response), &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_memory_pii_response_decode(response, response_len, &requests_sensitive) == 0);
   assert(requests_sensitive == 0);

   /* RETRIEVE, second shape: a batch of relations spanning all three tiers, in
    * an order where returning them shuffled or short would be visible. */
   const char *rels[3] = {"works_for", "ssn", "home_password"};
   aimee_memory_sensitivity_t tiers[3] = {0};
   assert(aimee_memory_sens_request_encode(rels, 3, request, sizeof(request)) == 0);
   assert(aimee_module_client_call(
              client, AIMEE_MEMORY_EVENT_RETRIEVE, AIMEE_MEMORY_STAGE_RETRIEVE, 2106, 0, request,
              (uint32_t)aimee_memory_sens_request_size(rels, 3), response, sizeof(response),
              &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_memory_sens_response_decode(response, response_len, tiers, 3) == 0);
   assert(tiers[0] == AIMEE_MEMORY_SENS_NORMAL);
   assert(tiers[1] == AIMEE_MEMORY_SENS_PII);
   assert(tiers[2] == AIMEE_MEMORY_SENS_SECRET);
}

static void smoke_production_module(aimee_module_client_t *client, const char *name, uint32_t kind)
{
   uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN] = {0};
   uint8_t response[1024] = {0};
   uint32_t request_len = 0, response_len = 0;
   if (strcmp(name, "memory") == 0)
   {
      aimee_memory_confidence_t confidence = AIMEE_MEMORY_CONFIDENCE_LOW;
      assert(aimee_memory_request_encode(660000, request, sizeof(request)) == 0);
      request_len = AIMEE_MEMORY_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, AIMEE_MEMORY_STAGE_RERANK, 2000, 0, request,
                                      request_len, response, sizeof(response), &response_len, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_memory_response_decode(response, response_len, &confidence) == 0);
      assert(confidence == AIMEE_MEMORY_CONFIDENCE_HIGH);
      smoke_memory_decision_stages(client);
   }
   else if (strcmp(name, "learning") == 0)
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
      uint32_t status = 0;
      assert(aimee_runtime_web_request_encode("permission_denied", request, sizeof(request)) == 0);
      request_len = AIMEE_RUNTIME_WEB_REQUEST_LEN;
      assert(aimee_module_client_call(client, kind, AIMEE_RUNTIME_WEB_STAGE_CLASSIFY, 2013, 0,
                                      request, request_len, response, sizeof(response),
                                      &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(aimee_runtime_web_response_decode(response, response_len, &status) == 0);
      assert(status == 403u);
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

int main(int argc, char **argv)
{
   assert(argc >= 1 && argc <= 3);
   uint32_t test_kind = TEST_KIND, module_ref = MODULE_REF;
   uint32_t served[PRODUCTION_STAGE_MAX] = {test_kind};
   size_t serve_count = 1;
   if (argc == 3)
      assert(production_contract(argv[2], &test_kind, &module_ref, served, &serve_count) == 0);
   char directory[256];
   snprintf(directory, sizeof directory, "%s/aimee-module-runtime-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory) != NULL);
   /* Point the spawned module at a throwaway home BEFORE it is forked: the
    * sandbox module persists what it learns under AIMEE_HOME, and a test that
    * exercises the write path must not touch the developer's real store. */
   assert(setenv("AIMEE_HOME", directory, 1) == 0);
   char socket_path[PATH_MAX], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof socket_path, "%s/module.sock", directory) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);

   char module_executable[PATH_MAX];
   if (argc >= 2)
      assert(realpath(argv[1], module_executable) != NULL);
   else
      assert(snprintf(module_executable, sizeof module_executable, "%s", executable) > 0);

   uint32_t requested[PRODUCTION_STAGE_MAX + 1] = {0};
   memcpy(requested, served, serve_count * sizeof(*requested));
   requested[serve_count] = EMPTY_KIND;
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
                                    .request_count = serve_count + 1}};
   bus_host_config_t host_config = {.max_slots = 8,
                                    .slot_size = 512,
                                    .inline_budget = 400,
                                    .queue_capacity = 16,
                                    .arena_size = 16384};
   bus_host_t host;
   assert(bus_host_create(&host, &host_config, NULL, NULL) == BUS_HOST_OK);
   pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t runtime_config = {.socket_path = socket_path,
                                          .socket_mode = 0600,
                                          .backlog = 8,
                                          .stale_after_ns = 5000000000ULL,
                                          .grants = grants,
                                          .grant_count = 2};
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
      pid_t parent = getpid();
      module_pid = fork();
      assert(module_pid >= 0);
      if (module_pid == 0)
      {
         /* Outlive the test and this module runs forever: cleanup here is
          * atexit-shaped and does not run when the test dies by a signal. */
         prctl(PR_SET_PDEATHSIG, SIGKILL);
         if (getppid() != parent)
            _exit(0);
         execl(module_executable, module_executable, socket_path, (char *)NULL);
         _exit(127);
      }
   }
   else
      assert(pthread_create(&module_thread, NULL, run_process, &process) == 0);

   int caller_fd = -1;
   bus_client_t caller;
   assert(bus_endpoint_connect(socket_path, &caller_fd) == 0);
   assert(bus_client_attach_as(caller_fd, &caller, 1, CALLER_REF) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&caller_fd) == 0);
   wait_for_clients(&host, &host_lock, 2);

   pump_thread_t pump_state = {.host = &host, .lock = &host_lock};
   atomic_init(&pump_state.stop, 0);
   pthread_t pump_thread;
   assert(pthread_create(&pump_thread, NULL, run_pump, &pump_state) == 0);

   aimee_module_client_t module_client;
   assert(aimee_module_client_init(&module_client, &caller) == 0);

   if (argc == 3)
   {
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
   assert(rmdir(directory) == 0);
   if (argc == 3)
      printf("module runtime (%s): C caller/Go handler wire parity passed\n", argv[2]);
   else
      printf(
          "module runtime (%s): dispatch, fragmented payloads, deadline, and cancellation passed\n",
          argc == 2 ? "Go process" : "C process");
   return 0;
}
