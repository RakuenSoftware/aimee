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

   const uint32_t served[] = {AIMEE_DB2_EVENT_HEALTH, AIMEE_DB2_EVENT_LEVEL3_COUNT};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = MODULE_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = module_executable,
        .serve = served,
        .serve_count = 2},
       {.principal_class = 1,
        .principal_ref = CALLER_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = caller_executable,
        .request = served,
        .request_count = 2},
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

   uint32_t level3_total = 99;
   assert(aimee_db2_level3_count_call(call_client, &client, 9021, 0, &level3_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(level3_total == 0);

   uint32_t level2_total = 99;
   assert(aimee_db2_level2_count_call(call_client, &client, 9022, 0, &level2_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(level2_total == 0);

   uint32_t orphaned_l0_total = 99;
   assert(aimee_db2_orphaned_l0_count_call(call_client, &client, 9023, 0, &orphaned_l0_total, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(orphaned_l0_total == 0);

   uint64_t memory_total = 99;
   assert(aimee_db2_total_count_call(call_client, &client, 9024, 0, &memory_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(memory_total == 0);

   uint32_t session_l2_total = 99;
   assert(aimee_db2_session_l2_count_call(call_client, &client, 9025, 0, "fresh-session-with-no-l2",
                                          &session_l2_total, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(session_l2_total == 0);

   uint32_t key_exists = 99;
   assert(aimee_db2_key_exists_call(call_client, &client, 9026, 0, "fresh-key-with-no-row",
                                    &key_exists, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(key_exists == 0);

   uint32_t found = 99;
   uint64_t memory_id = 99;
   assert(aimee_db2_find_id_by_key_kind_call(call_client, &client, 9027, 0, "fresh-key-with-no-row",
                                             "task", &found, &memory_id, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(found == 0 && memory_id == 0);

   key_exists = 99;
   assert(aimee_db2_key_exists_in_tier_pair_call(call_client, &client, 9028, 0,
                                                 "fresh-key-with-no-row", "L3", "L4", &key_exists,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(key_exists == 0);

   domain_result = 9;
   assert(aimee_db2_effectiveness_update_call(call_client, &client, 9029, 0, 42, 1, 0.75,
                                              &domain_result, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK);
   domain_result = 9;
   assert(aimee_db2_effectiveness_update_call(call_client, &client, 9030, 0, 42, 0, 0.0,
                                              &domain_result, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK);

   uint32_t deleted_count = 99;
   assert(aimee_db2_retention_enforce_call(call_client, &client, 9031, 0, &deleted_count, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(deleted_count == 0);

   uint32_t demoted_count = 99;
   assert(aimee_db2_effectiveness_demote_call(call_client, &client, 9032, 0, &demoted_count, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(demoted_count == 0);

   aimee_db2_effectiveness_stats_t stats = {0};
   assert(aimee_db2_effectiveness_stats_call(call_client, &client, 9033, 0, &stats, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(stats.avg_effectiveness == 0.0 && stats.low_effectiveness_count == 0 &&
          stats.high_impact_count == 0);

   uint64_t l2_ids[AIMEE_DB2_L2_MEMORY_IDS_MAX];
   uint32_t l2_count = 99;
   assert(aimee_db2_l2_memory_ids_call(call_client, &client, 9034, 0, l2_ids,
                                       AIMEE_DB2_L2_MEMORY_IDS_MAX, &l2_count, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(l2_count == 0);

   assert(aimee_db2_health_record_call(call_client, &client, 9035, 0, 4u, 2u, 9u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);

   uint32_t snapshots_deleted = 99, contradictions_deleted = 99;
   assert(aimee_db2_health_retention_call(call_client, &client, 9036, 0, &snapshots_deleted,
                                          &contradictions_deleted, NULL,
                                          NULL) == AIMEE_MODULE_CALL_OK);
   assert(snapshots_deleted == 0 && contradictions_deleted == 0);

   aimee_db2_health_counters_t counters = {.cycles = 99};
   assert(aimee_db2_health_counters_call(call_client, &client, 9037, 0, &counters, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(counters.cycles == 0 && counters.l2_stale_30_days == 0);

   aimee_db2_memory_stats_t corpus = {.total = 99};
   assert(aimee_db2_stats_counts_call(call_client, &client, 9038, 0, &corpus, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(corpus.total == 0 && corpus.conflicts == 0 && corpus.kind_counts[9] == 0);

   uint32_t level0_deleted = 99, stale_deleted = 99;
   assert(aimee_db2_expire_call(call_client, &client, 9039, 0, &level0_deleted, &stale_deleted,
                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(level0_deleted == 0 && stale_deleted == 0);

   aimee_db2_pool_status_t pool = {0};
   domain_result = 9;
   assert(aimee_db2_pool_status_call(call_client, &client, 9011, 0, &domain_result, &pool, NULL,
                                     NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && pool.size == 16 && pool.in_use <= pool.size);

   aimee_db2_embedding_refusals_t refusals = {0};
   domain_result = 9;
   assert(aimee_db2_embedding_refusals_call(call_client, &client, 9012, 0, &domain_result,
                                            &refusals, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && refusals.refused_count == 0 &&
          refusals.last_offered == 0);

   aimee_db2_postgres_status_t postgres = {0};
   domain_result = 9;
   assert(aimee_db2_postgres_status_call(call_client, &client, 9013, 0, &domain_result, &postgres,
                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK);
   assert(postgres.available ==
          (AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE | AIMEE_DB2_POSTGRES_AVAILABLE_MAX |
           AIMEE_DB2_POSTGRES_AVAILABLE_ROLE));
   assert(postgres.active_connections > 0 && postgres.max_connections > 0);
   assert(postgres.is_replica == 0 && postgres.replica_lag_bytes == 0);

   aimee_db2_reembed_status_t reembed = {0};
   domain_result = 9;
   assert(aimee_db2_reembed_status_call(call_client, &client, 9014, 0, &domain_result, &reembed,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_NOT_FOUND && reembed.target_dimension == 0 &&
          reembed.started_epoch == 0);

   domain_result = 9;
   assert(aimee_db2_reembed_clear_call(call_client, &client, 9015, 0, &domain_result, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK);

   aimee_db2_reembed_clear_maintenance_t maintenance = {0};
   domain_result = 9;
   assert(aimee_db2_reembed_clear_maintenance_call(call_client, &client, 9016, 0, 0, &domain_result,
                                                   &maintenance, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && maintenance.was_in_progress == 0 &&
          maintenance.recorded_dimension == 384 && maintenance.running_dimension == 384);

   char serving_id[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1] = "stale";
   domain_result = 9;
   assert(aimee_db2_embedder_serving_id_call(call_client, &client, 9017, 0, &domain_result,
                                             serving_id, sizeof(serving_id), NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && serving_id[0] == '\0');

   /* Exercise the destructive operator action through the packaged process without
    * mutating the shared replay database. A different target forces complete plan
    * discovery; dry-run must leave both the effective width and maintenance marker
    * unchanged. */
   aimee_db2_dimension_reset_t reset = {0};
   domain_result = 9;
   assert(aimee_db2_dimension_reset_call(call_client, &client, 9018, 0, 385, 0, 1, &domain_result,
                                         &reset, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && reset.recorded_dimension == 384 &&
          reset.target_dimension == 385 && reset.tables_discovered > 0 &&
          reset.tables_discovered <= AIMEE_DB2_DIMENSION_RESET_TABLES_MAX &&
          reset.tables_dropped == 0 && reset.curator_requeued == 0 && reset.evidence_requeued == 0);
   dimension = 9;
   assert(aimee_db2_embedding_dimension_call(call_client, &client, 9019, 0, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && dimension == 384);
   reembed = (aimee_db2_reembed_status_t){0};
   assert(aimee_db2_reembed_status_call(call_client, &client, 9020, 0, &domain_result, &reembed,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_NOT_FOUND && reembed.target_dimension == 0 &&
          reembed.started_epoch == 0);

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
