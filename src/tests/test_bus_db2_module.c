#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/db2/client.h>
#include <aimee/db2/module_api.h>

#include "platform_test_util.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MODULE_REF 29u
#define CALLER_REF 90u

typedef struct
{
   int (*health_probe)(int *schema_ok, int *have_pg_trgm);
   int (*kb_health_probe)(int *kb_tables_ok);
   int (*embedding_dimension)(void);
   int (*pool_status)(aimee_db2_pool_status_t *status);
   int (*embedding_refusals)(aimee_db2_embedding_refusals_t *status);
   int (*postgres_status)(aimee_db2_postgres_status_t *status);
} aimee_db2_module_backend_t;

extern aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                                  const uint8_t *request_body, uint32_t request_len,
                                                  uint8_t *response_body,
                                                  uint32_t response_capacity,
                                                  uint32_t *response_len, void *user_data);

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

static int health_calls;
static int kb_health_calls;
static int embedding_dimension_calls;
static atomic_int block_health;
static atomic_int health_entered;
static atomic_int health_release;

static int health_probe(int *schema_ok, int *have_pg_trgm)
{
   health_calls++;
   if (atomic_load_explicit(&block_health, memory_order_acquire))
   {
      const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
      atomic_store_explicit(&health_entered, 1, memory_order_release);
      while (!atomic_load_explicit(&health_release, memory_order_acquire))
         nanosleep(&pause, NULL);
   }
   *schema_ok = 1;
   *have_pg_trgm = 0;
   return 0;
}

static int kb_health_probe(int *kb_tables_ok)
{
   kb_health_calls++;
   *kb_tables_ok = 1;
   return 0;
}

int db2_health_probe(int *schema_ok, int *have_pg_trgm)
{
   return health_probe(schema_ok, have_pg_trgm);
}

int db2_kb_health_probe(int *kb_tables_ok)
{
   return kb_health_probe(kb_tables_ok);
}

int db2_embedding_dim(void)
{
   embedding_dimension_calls++;
   return 384;
}

static int embedding_dimension(void)
{
   embedding_dimension_calls++;
   return 384;
}

void db2_pool_stats(int *size, int *in_use, int *waiters, long *lease_grants, long *lease_timeouts,
                    long *stuck, long *poisoned)
{
   (void)size;
   (void)in_use;
   (void)waiters;
   (void)lease_grants;
   (void)lease_timeouts;
   (void)stuck;
   (void)poisoned;
}

static int pool_status(aimee_db2_pool_status_t *status)
{
   *status = (aimee_db2_pool_status_t){16, 2, 1, 10, 3, 4, 5};
   return 0;
}

long long db2_embedding_dim_refused_count(void)
{
   return 7;
}

int db2_embedding_dim_last_offered(void)
{
   return 768;
}

static int embedding_refusals(aimee_db2_embedding_refusals_t *status)
{
   *status = (aimee_db2_embedding_refusals_t){7, 768};
   return 0;
}

int db2_pg_stat_summary(int *active, int *maximum, int *replica, int64_t *lag)
{
   if (active)
      *active = 12;
   if (maximum)
      *maximum = 100;
   if (replica)
      *replica = 1;
   if (lag)
      *lag = 1048576;
   return 0;
}

static int postgres_status(aimee_db2_postgres_status_t *status)
{
   *status = (aimee_db2_postgres_status_t){15, 12, 100, 1, 1048576};
   return 0;
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

typedef struct
{
   atomic_int *cancel;
   int entered;
} cancel_inflight_t;

static int cancellation_flag(void *context)
{
   return atomic_load_explicit((atomic_int *)context, memory_order_acquire);
}

static void *cancel_inflight(void *argument)
{
   cancel_inflight_t *state = argument;
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int attempt = 0; attempt < 5000; ++attempt)
   {
      if (atomic_load_explicit(&health_entered, memory_order_acquire))
      {
         state->entered = 1;
         break;
      }
      nanosleep(&pause, NULL);
   }
   atomic_store_explicit(state->cancel, 1, memory_order_release);
   for (int attempt = 0; attempt < 10; ++attempt)
      nanosleep(&pause, NULL);
   atomic_store_explicit(&health_release, 1, memory_order_release);
   return NULL;
}

static void wait_for_clients(bus_host_t *host, pthread_mutex_t *lock)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int attempt = 0; attempt < 2000; ++attempt)
   {
      pthread_mutex_lock(lock);
      uint32_t admitted = host->admitted;
      pthread_mutex_unlock(lock);
      if (admitted >= 2)
         return;
      nanosleep(&pause, NULL);
   }
   assert(!"timed out waiting for DB2 module clients");
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

int main(void)
{
   char directory[256];
   snprintf(directory, sizeof(directory), "%s/aimee-db2-module-bus-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory) != NULL);
   char socket_path[512], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof(socket_path), "%s/module.sock", directory) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);

   const uint32_t served[] = {AIMEE_DB2_EVENT_HEALTH};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = MODULE_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
        .serve = served,
        .serve_count = 1},
       {.principal_class = 1,
        .principal_ref = CALLER_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
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

   static const aimee_module_stage_t stages[] = {
       {AIMEE_DB2_EVENT_HEALTH, AIMEE_DB2_STAGE_HEALTH},
   };
   static const aimee_db2_module_backend_t backend = {
       .health_probe = health_probe,
       .kb_health_probe = kb_health_probe,
       .embedding_dimension = embedding_dimension,
       .pool_status = pool_status,
       .embedding_refusals = embedding_refusals,
       .postgres_status = postgres_status,
   };
   process_thread_t process = {
       .config = {.socket_path = socket_path,
                  .module_name = "db2",
                  .principal_class = 1,
                  .principal_ref = MODULE_REF,
                  .stages = stages,
                  .stage_count = 1,
                  .handler = aimee_module_handler,
                  .user_data = (void *)&backend},
   };
   pthread_t module_thread;
   assert(pthread_create(&module_thread, NULL, run_process, &process) == 0);

   int caller_fd = -1;
   bus_client_t caller;
   assert(bus_endpoint_connect(socket_path, &caller_fd) == 0);
   assert(bus_client_attach_as(caller_fd, &caller, 1, CALLER_REF) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&caller_fd) == 0);
   wait_for_clients(&host, &host_lock);

   pump_thread_t pump_state = {.host = &host, .lock = &host_lock};
   atomic_init(&pump_state.stop, 0);
   pthread_t pump_thread;
   assert(pthread_create(&pump_thread, NULL, run_pump, &pump_state) == 0);

   aimee_module_client_t client;
   assert(aimee_module_client_init(&client, &caller) == 0);
   int schema_ok = 0, have_pg_trgm = 1, kb_tables_ok = 0;
   assert(aimee_db2_health_call(call_client, &client, 7001, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(schema_ok == 1 && have_pg_trgm == 0 && kb_tables_ok == 1);
   assert(health_calls == 1 && kb_health_calls == 1);

   uint32_t domain_result = 9, dimension = 9;
   assert(aimee_db2_embedding_dimension_call(call_client, &client, 7010, 0, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && dimension == 384);
   assert(embedding_dimension_calls == 1);

   aimee_db2_pool_status_t pool = {0};
   domain_result = 9;
   assert(aimee_db2_pool_status_call(call_client, &client, 7011, 0, &domain_result, &pool, NULL,
                                     NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && pool.size == 16 && pool.in_use == 2 &&
          pool.waiters == 1 && pool.lease_grants == 10 && pool.lease_timeouts == 3 &&
          pool.stuck == 4 && pool.poisoned == 5);

   aimee_db2_embedding_refusals_t refusals = {0};
   domain_result = 9;
   assert(aimee_db2_embedding_refusals_call(call_client, &client, 7012, 0, &domain_result,
                                            &refusals, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && refusals.refused_count == 7 &&
          refusals.last_offered == 768);

   aimee_db2_postgres_status_t postgres = {0};
   domain_result = 9;
   assert(aimee_db2_postgres_status_call(call_client, &client, 7013, 0, &domain_result, &postgres,
                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && postgres.available == 15 &&
          postgres.active_connections == 12 && postgres.max_connections == 100 &&
          postgres.is_replica == 1 && postgres.replica_lag_bytes == 1048576);

   schema_ok = have_pg_trgm = kb_tables_ok = 9;
   assert(aimee_db2_health_call(call_client, &client, 7002, 1, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(schema_ok == 0 && have_pg_trgm == 0 && kb_tables_ok == 0);

   atomic_store_explicit(&block_health, 1, memory_order_release);
   atomic_store_explicit(&health_entered, 0, memory_order_release);
   atomic_store_explicit(&health_release, 0, memory_order_release);
   atomic_int cancel;
   atomic_init(&cancel, 0);
   cancel_inflight_t cancel_state = {.cancel = &cancel};
   pthread_t cancel_thread;
   assert(pthread_create(&cancel_thread, NULL, cancel_inflight, &cancel_state) == 0);
   schema_ok = have_pg_trgm = kb_tables_ok = 9;
   assert(aimee_db2_health_call(call_client, &client, 7003, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, cancellation_flag,
                                &cancel) == AIMEE_MODULE_CALL_CANCELLED);
   assert(schema_ok == 0 && have_pg_trgm == 0 && kb_tables_ok == 0);
   assert(pthread_join(cancel_thread, NULL) == 0 && cancel_state.entered == 1);
   atomic_store_explicit(&block_health, 0, memory_order_release);

   /* The cancelled handler finishes after its caller. The typed client must
    * drain that stale terminal reply and keep the next correlation healthy. */
   schema_ok = have_pg_trgm = kb_tables_ok = 0;
   assert(aimee_db2_health_call(call_client, &client, 7004, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(schema_ok == 1 && have_pg_trgm == 0 && kb_tables_ok == 1);
   assert(health_calls == 3 && kb_health_calls == 3);

   aimee_module_client_destroy(&client);
   aimee_module_process_stop();
   assert(pthread_join(module_thread, NULL) == 0 && process.result == 0);
   atomic_store_explicit(&pump_state.stop, 1, memory_order_release);
   assert(pthread_join(pump_thread, NULL) == 0);
   bus_client_detach(&caller);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&host_lock);
   assert(rmdir(directory) == 0);
   puts("test_bus_db2_module: typed client, deadline, and cancellation crossed the real event bus");
   return 0;
}
