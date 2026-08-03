#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/core/event_bus/module_runtime.h>

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_KIND  5889U
#define TEST_STAGE 1U
#define MODULE_REF 7U
#define CALLER_REF 90U

typedef struct
{
   aimee_module_process_config_t config;
   int result;
} process_thread_t;

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

static void send_invoke(bus_client_t *caller, uint64_t correlation, uint64_t deadline,
                        const char *body)
{
   uint8_t payload[256];
   uint32_t body_len = body ? (uint32_t)strlen(body) : 0;
   aimee_module_message_t request = {.operation = AIMEE_MODULE_OP_INVOKE,
                                     .stage_id = TEST_STAGE,
                                     .body_len = body_len,
                                     .deadline_ns = deadline,
                                     .trace_id = correlation + 1000};
   assert(aimee_module_message_encode(&request, payload, sizeof payload) ==
          AIMEE_MODULE_MESSAGE_HEADER_LEN);
   if (body_len)
      memcpy(payload + AIMEE_MODULE_MESSAGE_HEADER_LEN, body, body_len);
   assert(bus_client_request(caller, TEST_KIND, correlation, payload,
                             AIMEE_MODULE_MESSAGE_HEADER_LEN + body_len) == BUS_CLIENT_OK);
}

static aimee_module_message_t wait_for_reply(bus_host_t *host, pthread_mutex_t *lock,
                                             bus_client_t *caller, uint64_t correlation, char *body,
                                             size_t body_capacity)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int i = 0; i < 3000; ++i)
   {
      pump(host, lock);
      bus_event_t event;
      if (bus_client_poll(caller, &event) == BUS_CLIENT_OK &&
          event.frame.correlation_id == correlation && (event.frame.hdr_flags & BUS_F_REPLY))
      {
         aimee_module_message_t reply;
         assert(aimee_module_message_decode(event.payload, event.payload_len, &reply) ==
                AIMEE_MODULE_MESSAGE_OK);
         assert(reply.operation == AIMEE_MODULE_OP_RESULT && reply.stage_id == TEST_STAGE);
         assert(reply.body_len < body_capacity);
         if (reply.body_len)
            memcpy(body, event.payload + AIMEE_MODULE_MESSAGE_HEADER_LEN, reply.body_len);
         body[reply.body_len] = '\0';
         return reply;
      }
      nanosleep(&pause, NULL);
   }
   assert(!"timed out waiting for module reply");
   aimee_module_message_t unreachable = {0};
   return unreachable;
}

int main(void)
{
   char directory[] = "/tmp/aimee-module-runtime-XXXXXX";
   assert(mkdtemp(directory) != NULL);
   char socket_path[PATH_MAX], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof socket_path, "%s/module.sock", directory) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);

   uint32_t served[] = {TEST_KIND};
   uint32_t requested[] = {TEST_KIND};
   bus_runtime_grant_t grants[] = {{.principal_class = 1,
                                    .principal_ref = MODULE_REF,
                                    .uid = BUS_RUNTIME_SELF_UID,
                                    .executable = executable,
                                    .serve = served,
                                    .serve_count = 1},
                                   {.principal_class = 1,
                                    .principal_ref = CALLER_REF,
                                    .uid = BUS_RUNTIME_SELF_UID,
                                    .executable = executable,
                                    .request = requested,
                                    .request_count = 1}};
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
   assert(pthread_create(&module_thread, NULL, run_process, &process) == 0);

   int caller_fd = -1;
   bus_client_t caller;
   assert(bus_endpoint_connect(socket_path, &caller_fd) == 0);
   assert(bus_client_attach_as(caller_fd, &caller, 1, CALLER_REF) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&caller_fd) == 0);
   wait_for_clients(&host, &host_lock, 2);

   char body[64];
   send_invoke(&caller, 1, 0, "real-result");
   aimee_module_message_t reply = wait_for_reply(&host, &host_lock, &caller, 1, body, sizeof body);
   assert(reply.status == AIMEE_MODULE_STATUS_OK && strcmp(body, "real-result") == 0);

   send_invoke(&caller, 2, 1, "late");
   reply = wait_for_reply(&host, &host_lock, &caller, 2, body, sizeof body);
   assert(reply.status == AIMEE_MODULE_STATUS_DEADLINE_EXCEEDED && reply.body_len == 0);

   send_invoke(&caller, 3, 0, "cancel");
   pump(&host, &host_lock); /* establish the pending correlation */
   const struct timespec start_work = {.tv_sec = 0, .tv_nsec = 10000000};
   nanosleep(&start_work, NULL);
   assert(bus_client_cancel(&caller, TEST_KIND, 3) == BUS_CLIENT_OK);
   pump(&host, &host_lock);
   reply = wait_for_reply(&host, &host_lock, &caller, 3, body, sizeof body);
   assert(reply.status == AIMEE_MODULE_STATUS_CANCELLED && reply.body_len == 0);

   aimee_module_process_stop();
   assert(pthread_join(module_thread, NULL) == 0 && process.result == 0);
   bus_client_detach(&caller);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&host_lock);
   assert(rmdir(directory) == 0);
   puts("module runtime: core dispatch, deadline, and cancellation passed");
   return 0;
}
