#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_runtime.h>

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_KIND  5889U
#define EMPTY_KIND 5890U
#define TEST_STAGE 1U
#define MODULE_REF 7U
#define CALLER_REF 90U

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

int main(void)
{
   char directory[] = "/tmp/aimee-module-runtime-XXXXXX";
   assert(mkdtemp(directory) != NULL);
   char socket_path[PATH_MAX], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof socket_path, "%s/module.sock", directory) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);

   uint32_t served[] = {TEST_KIND};
   uint32_t requested[] = {TEST_KIND, EMPTY_KIND};
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

   char body[64];
   uint32_t body_len = 0;
   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1001, 0, "real-result",
                                   11, body, sizeof body, &body_len, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(body_len == 11 && memcmp(body, "real-result", 11) == 0);

   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1002, 1, "late", 4,
                                   body, sizeof body, &body_len, NULL, NULL) ==
          AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(body_len == 0);

   atomic_int cancel;
   atomic_init(&cancel, 0);
   pthread_t cancel_thread;
   assert(pthread_create(&cancel_thread, NULL, cancel_soon, &cancel) == 0);
   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1003, 0, "cancel", 6,
                                   body, sizeof body, &body_len, cancellation_flag, &cancel) ==
          AIMEE_MODULE_CALL_CANCELLED);
   assert(pthread_join(cancel_thread, NULL) == 0 && body_len == 0);

   /* The cancelled handler's terminal reply may arrive after call() returned.
    * A subsequent call must drain that stale correlation and still complete. */
   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1004, 0, "after", 5,
                                   body, sizeof body, &body_len, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(body_len == 5 && memcmp(body, "after", 5) == 0);

   assert(aimee_module_client_call(&module_client, EMPTY_KIND, 2, 1005, 0, NULL, 0, body,
                                   sizeof body, &body_len, NULL, NULL) ==
          AIMEE_MODULE_CALL_CAPABILITY_ABSENT);

   assert(aimee_module_client_call(&module_client, TEST_KIND, TEST_STAGE, 1006, 0, "toolarge", 8,
                                   body, 3, &body_len, NULL, NULL) ==
          AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE);
   assert(body_len == 8);

   aimee_module_client_destroy(&module_client);
   aimee_module_process_stop();
   assert(pthread_join(module_thread, NULL) == 0 && process.result == 0);
   atomic_store_explicit(&pump_state.stop, 1, memory_order_release);
   assert(pthread_join(pump_thread, NULL) == 0);
   bus_client_detach(&caller);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&host_lock);
   assert(rmdir(directory) == 0);
   puts("module runtime: core dispatch, deadline, and cancellation passed");
   return 0;
}
