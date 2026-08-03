#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/delegates/module_api.h>
#include <aimee/git/module_api.h>
#include <aimee/learning/module_api.h>
#include <aimee/memory/module_api.h>
#include <aimee/response-composition/module_api.h>
#include <aimee/routing/module_api.h>
#include <aimee/skills/module_api.h>
#include <aimee/tools/module_api.h>
#include <aimee/workspace/module_api.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_KIND  5889U
#define EMPTY_KIND 5890U
#define TEST_STAGE 1U
#define MODULE_REF 7U
#define CALLER_REF 90U
#define LARGE_BODY (128U * 1024U + 37U)
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
      *kind = AIMEE_GIT_EVENT_OPERATION, *principal_ref = 13;
   else if (strcmp(name, "skills") == 0)
      *kind = AIMEE_SKILLS_EVENT_CONTEXT, *principal_ref = 14;
   else if (strcmp(name, "response-composition") == 0)
      *kind = AIMEE_RESPONSE_EVENT_COMPOSE, *principal_ref = 15;
   else
      return -1;
   served[0] = *kind;
   *serve_count = 1;
   return 0;
}

static void smoke_production_module(aimee_module_client_t *client, const char *name,
                                    uint32_t kind)
{
   uint8_t request[1024] = {0};
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
      assert(aimee_delegates_message_decode(response, response_len,
                                             AIMEE_DELEGATES_RESPONSE_MAGIC, role,
                                             sizeof(role)) == 0);
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
   }
   else
   {
      assert(strcmp(name, "response-composition") == 0);
      const aimee_response_key_input_t input = {
          .principal = "uid:1",
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
}

int main(int argc, char **argv)
{
   assert(argc >= 1 && argc <= 3);
   uint32_t test_kind = TEST_KIND, module_ref = MODULE_REF;
   uint32_t served[PRODUCTION_STAGE_MAX] = {test_kind};
   size_t serve_count = 1;
   if (argc == 3)
      assert(production_contract(argv[2], &test_kind, &module_ref, served, &serve_count) == 0);
   char directory[] = "/tmp/aimee-module-runtime-XXXXXX";
   assert(mkdtemp(directory) != NULL);
   char socket_path[PATH_MAX], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof socket_path, "%s/module.sock", directory) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);

   char module_executable[PATH_MAX];
   if (argc >= 2)
      assert(realpath(argv[1], module_executable) != NULL);
   else
      assert(snprintf(module_executable, sizeof module_executable, "%s", executable) > 0);

   uint32_t requested[] = {test_kind, EMPTY_KIND};
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
                                    .request_count = 2}};
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
      module_pid = fork();
      assert(module_pid >= 0);
      if (module_pid == 0)
      {
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
   assert(rmdir(directory) == 0);
   if (argc == 3)
      printf("module runtime (%s): C caller/Go handler wire parity passed\n", argv[2]);
   else
      printf("module runtime (%s): dispatch, fragmented payloads, deadline, and cancellation passed\n",
             argc == 2 ? "Go process" : "C process");
   return 0;
}
