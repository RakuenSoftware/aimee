#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/db2/client.h>
#include <aimee/db2/module_api.h>

#include "platform_test_util.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MODULE_REF 29u
#define CALLER_REF 90u

typedef struct
{
   bus_host_t *host;
   pthread_mutex_t *lock;
   atomic_int stop;
} pump_thread_t;

static void pump(bus_host_t *host, pthread_mutex_t *lock)
{
   pthread_mutex_lock(lock);
   (void)bus_host_pump(host);
   pthread_mutex_unlock(lock);
}

static void *run_pump(void *argument)
{
   pump_thread_t *state = argument;
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   while (!atomic_load_explicit(&state->stop, memory_order_acquire))
   {
      pump(state->host, state->lock);
      nanosleep(&pause, NULL);
   }
   return NULL;
}

static void wait_for_clients(bus_host_t *host, pthread_mutex_t *lock, pid_t child)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int attempt = 0; attempt < 60000; ++attempt)
   {
      pthread_mutex_lock(lock);
      uint32_t admitted = host->admitted;
      pthread_mutex_unlock(lock);
      if (admitted >= 2)
         return;
      int status = 0;
      pid_t exited = waitpid(child, &status, WNOHANG);
      if (exited == child)
      {
         fprintf(stderr, "DB2 process exited before bus admission (status=%d)\n", status);
         assert(!"DB2 process failed before bus admission");
      }
      assert(exited == 0);
      nanosleep(&pause, NULL);
   }
   assert(!"timed out waiting for the DB2 process");
}

static aimee_module_call_result_t
call_client(void *context, uint32_t event_kind, uint32_t stage_id, uint64_t trace_id,
            uint64_t deadline_ns, const void *request_body, uint32_t request_len,
            void *response_body, uint32_t response_capacity, uint32_t *response_len,
            aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   return aimee_module_client_call(context, event_kind, stage_id, trace_id, deadline_ns,
                                   request_body, request_len, response_body, response_capacity,
                                   response_len, cancelled, cancel_context);
}

static int cancel_after_request(void *context)
{
   int *checks = context;
   *checks += 1;
   /* module_client checks once before locking and once before publishing.
    * The third check happens only after the request entered the bus. */
   return *checks >= 3;
}

int main(int argc, char **argv)
{
   assert(argc == 2);

   char module_executable[PATH_MAX], caller_executable[PATH_MAX];
   assert(realpath(argv[1], module_executable) != NULL);
   assert(realpath("/proc/self/exe", caller_executable) != NULL);

   char directory[256];
   snprintf(directory, sizeof(directory), "%s/aimee-db2-process-bus-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory) != NULL);
   char socket_path[512];
   assert(snprintf(socket_path, sizeof(socket_path), "%s/module.sock", directory) > 0);

   const uint32_t served[] = {AIMEE_DB2_EVENT_HEALTH};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = MODULE_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = module_executable,
        .serve = served,
        .serve_count = 1},
       {.principal_class = 1,
        .principal_ref = CALLER_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = caller_executable,
        .request = served,
        .request_count = 1},
   };
   bus_host_config_t host_config = {.max_slots = 4,
                                    .slot_size = 256,
                                    .inline_budget = 128,
                                    .queue_capacity = 8,
                                    .arena_size = 4096};
   bus_host_t host;
   assert(bus_host_create(&host, &host_config, NULL, NULL) == BUS_HOST_OK);
   pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t runtime_config = {.socket_path = socket_path,
                                          .socket_mode = 0600,
                                          .backlog = 4,
                                          .stale_after_ns = 5000000000ULL,
                                          .grants = grants,
                                          .grant_count = 2};
   bus_runtime_t *runtime = bus_runtime_start(&host, &host_lock, &runtime_config);
   assert(runtime != NULL);

   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      execl(module_executable, module_executable, socket_path, (char *)NULL);
      _exit(127);
   }

   int caller_fd = -1;
   bus_client_t caller;
   assert(bus_endpoint_connect(socket_path, &caller_fd) == 0);
   assert(bus_client_attach_as(caller_fd, &caller, 1, CALLER_REF) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&caller_fd) == 0);
   wait_for_clients(&host, &host_lock, child);

   pump_thread_t pump_state = {.host = &host, .lock = &host_lock};
   atomic_init(&pump_state.stop, 0);
   pthread_t pump_thread;
   assert(pthread_create(&pump_thread, NULL, run_pump, &pump_state) == 0);

   aimee_module_client_t client;
   assert(aimee_module_client_init(&client, &caller) == 0);

   uint8_t request[AIMEE_DB2_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RESPONSE_LEN];
   uint8_t expected[AIMEE_DB2_RESPONSE_LEN];
   uint32_t response_len = 0;
   assert(aimee_db2_health_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_health_response_encode(AIMEE_DB2_FLAG_ALL, expected, sizeof(expected)) == 0);
   assert(aimee_module_client_call(&client, AIMEE_DB2_EVENT_HEALTH, AIMEE_DB2_STAGE_HEALTH, 9001, 0,
                                   request, sizeof(request), response, sizeof(response),
                                   &response_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(response_len == sizeof(expected));
   assert(memcmp(response, expected, sizeof(expected)) == 0);

   int schema_ok = 0, have_pg_trgm = 0, kb_tables_ok = 0;
   assert(aimee_db2_health_call(call_client, &client, 9002, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(schema_ok == 1 && have_pg_trgm == 1 && kb_tables_ok == 1);

   uint32_t domain_result = 9, dimension = 9;
   assert(aimee_db2_embedding_dimension_call(call_client, &client, 9010, 0, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && dimension == 384);

   schema_ok = have_pg_trgm = kb_tables_ok = 9;
   assert(aimee_db2_health_call(call_client, &client, 9003, 1, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(schema_ok == 0 && have_pg_trgm == 0 && kb_tables_ok == 0);

   int cancel_checks = 0;
   schema_ok = have_pg_trgm = kb_tables_ok = 9;
   assert(aimee_db2_health_call(call_client, &client, 9004, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, cancel_after_request,
                                &cancel_checks) == AIMEE_MODULE_CALL_CANCELLED);
   assert(cancel_checks >= 3);
   assert(schema_ok == 0 && have_pg_trgm == 0 && kb_tables_ok == 0);

   /* A cancellation can race the packaged handler's terminal reply. Prove the
    * next live PostgreSQL request drains it and remains byte-canonical. */
   schema_ok = have_pg_trgm = kb_tables_ok = 0;
   assert(aimee_db2_health_call(call_client, &client, 9005, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(schema_ok == 1 && have_pg_trgm == 1 && kb_tables_ok == 1);

   aimee_module_client_destroy(&client);
   assert(kill(child, SIGTERM) == 0);
   int child_status = 0;
   assert(waitpid(child, &child_status, 0) == child);
   assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
   atomic_store_explicit(&pump_state.stop, 1, memory_order_release);
   assert(pthread_join(pump_thread, NULL) == 0);
   bus_client_detach(&caller);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&host_lock);
   assert(rmdir(directory) == 0);
   puts("test_bus_db2_process: Postgres lifecycle facts, deadline, and cancellation replayed over "
        "the bus");
   return 0;
}
