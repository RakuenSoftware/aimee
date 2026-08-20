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

   /* One entry per family: every operation in a family shares its event kind,
    * so listing operations here would repeat each kind and say nothing extra
    * about what the process serves. */
   const uint32_t served[] = {AIMEE_DB2_EVENT_LIFECYCLE,    AIMEE_DB2_EVENT_MEMORY,
                              AIMEE_DB2_EVENT_INDEX,        AIMEE_DB2_EVENT_LEARNING,
                              AIMEE_DB2_EVENT_ORGANIZATION, AIMEE_DB2_EVENT_CUSTODY,
                              AIMEE_DB2_EVENT_MAINTENANCE};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = MODULE_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = module_executable,
        .serve = served,
        .serve_count = (uint32_t)(sizeof(served) / sizeof(served[0]))},
       {.principal_class = 1,
        .principal_ref = CALLER_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = caller_executable,
        .request = served,
        .request_count = (uint32_t)(sizeof(served) / sizeof(served[0]))},
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

   uint64_t scoped_ids[AIMEE_DB2_TOP_L2_FACTS_MAX];
   uint32_t scoped_count = 99;
   assert(aimee_db2_top_l2_facts_call(call_client, &client, 9038, 0, 8u, 3u, "replay-workspace",
                                      "replay-project", scoped_ids, AIMEE_DB2_TOP_L2_FACTS_MAX,
                                      &scoped_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(scoped_count == 0);

   scoped_count = 99;
   assert(aimee_db2_list_session_scope_priority_call(
              call_client, &client, 9039, 0, 8u, 0u, "", "", scoped_ids,
              AIMEE_DB2_LIST_SESSION_SCOPE_PRIORITY_MAX, &scoped_count, NULL,
              NULL) == AIMEE_MODULE_CALL_OK);
   assert(scoped_count == 0);

   uint64_t probe_ids[AIMEE_DB2_COLLECT_ALIAS_MATCHES_MAX];
   uint32_t probe_count = 99;
   probe_count = 99;
   assert(aimee_db2_collect_alias_matches_call(call_client, &client, 9040, 0, "replay-term", 4u, 1u,
                                               "replay-workspace", "replay-project", probe_ids,
                                               AIMEE_DB2_COLLECT_ALIAS_MATCHES_MAX, &probe_count,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   probe_count = 99;
   assert(aimee_db2_collect_entity_matches_call(call_client, &client, 9041, 0, "replay-term", 4u,
                                                1u, "replay-workspace", "replay-project", probe_ids,
                                                AIMEE_DB2_COLLECT_ENTITY_MATCHES_MAX, &probe_count,
                                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   probe_count = 99;
   assert(aimee_db2_collect_event_frame_matches_call(
              call_client, &client, 9042, 0, "replay-term", 4u, 1u, "replay-workspace",
              "replay-project", probe_ids, AIMEE_DB2_COLLECT_EVENT_FRAME_MATCHES_MAX, &probe_count,
              NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   probe_count = 99;
   assert(aimee_db2_collect_relation_token_matches_call(
              call_client, &client, 9043, 0, "replay-term", 4u, 1u, "replay-workspace",
              "replay-project", probe_ids, AIMEE_DB2_COLLECT_RELATION_TOKEN_MATCHES_MAX,
              &probe_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   probe_count = 99;
   assert(aimee_db2_collect_summary_matches_call(call_client, &client, 9044, 0, "replay-term", 4u,
                                                 1u, "replay-workspace", "replay-project",
                                                 probe_ids, AIMEE_DB2_COLLECT_SUMMARY_MATCHES_MAX,
                                                 &probe_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   probe_count = 99;
   assert(aimee_db2_collect_temporal_matches_call(
              call_client, &client, 9045, 0, "replay-term", 4u, 1u, "replay-workspace",
              "replay-project", probe_ids, AIMEE_DB2_COLLECT_TEMPORAL_MATCHES_MAX, &probe_count,
              NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   probe_count = 99;
   assert(aimee_db2_find_facts_like_call(call_client, &client, 9050, 0, "replay-term", 4u, 1u,
                                         "replay-workspace", "replay-project", probe_ids,
                                         AIMEE_DB2_FIND_FACTS_LIKE_MAX, &probe_count, NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   probe_count = 99;
   assert(aimee_db2_list_session_scope_priority_like_call(
              call_client, &client, 9051, 0, "replay-term", 4u, 1u, "replay-workspace",
              "replay-project", probe_ids, AIMEE_DB2_LIST_SESSION_SCOPE_PRIORITY_LIKE_MAX,
              &probe_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   probe_count = 99;
   assert(aimee_db2_negation_fts_search_call(call_client, &client, 9052, 0, "replay-term", 4u, 1u,
                                             "replay-workspace", "replay-project", probe_ids,
                                             AIMEE_DB2_NEGATION_FTS_SEARCH_MAX, &probe_count, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   uint64_t walk_ids[AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_MAX];
   uint32_t walk_count = 99;
   walk_count = 99;
   assert(aimee_db2_session_neighbors_before_call(call_client, &client, 9060, 0, "replay-session",
                                                  4096u, 4u, walk_ids,
                                                  AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_MAX,
                                                  &walk_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(walk_count == 0);

   walk_count = 99;
   assert(aimee_db2_session_neighbors_after_call(call_client, &client, 9061, 0, "replay-session",
                                                 4096u, 4u, walk_ids,
                                                 AIMEE_DB2_SESSION_NEIGHBORS_AFTER_MAX, &walk_count,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(walk_count == 0);

   /* The corpus is empty, so both getters answer not_found -- which is also what
    * they would answer if the statement had not run. Only a real database can
    * say the statement did run. */
   aimee_db2_memory_row_t fetched;
   uint32_t row_result = 99;
   row_result = 99;
   assert(aimee_db2_row_get_call(call_client, &client, 9070, 0, 4096u, &row_result, &fetched, NULL,
                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(row_result == AIMEE_DB2_RESULT_NOT_FOUND && fetched.id == 0);

   row_result = 99;
   assert(aimee_db2_row_get_by_unit_id_call(call_client, &client, 9071, 0, 4096u, &row_result,
                                            &fetched, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(row_result == AIMEE_DB2_RESULT_NOT_FOUND && fetched.id == 0);

   probe_count = 99;
   assert(aimee_db2_search_facts_patterns_by_keyword_call(
              call_client, &client, 9080, 0, "replay-term", 4u, 1u, "replay-workspace",
              "replay-project", probe_ids, AIMEE_DB2_SEARCH_FACTS_PATTERNS_BY_KEYWORD_MAX,
              &probe_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 0);

   uint64_t history_ids[AIMEE_DB2_FACT_HISTORY_MAX];
   uint32_t history_count = 99;
   assert(aimee_db2_fact_history_call(call_client, &client, 9081, 0, "fact:replay", 4u, history_ids,
                                      AIMEE_DB2_FACT_HISTORY_MAX, &history_count, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
   assert(history_count == 0);

   /* Both filter shapes are replayed, because they are different statements:
    * the tier and kind clauses are only in the query when their value is set. */
   uint64_t list_ids[AIMEE_DB2_LIST_ROWS_MAX];
   uint32_t list_count = 99;
   assert(aimee_db2_list_rows_call(call_client, &client, 9090, 0, 4u, 1u, 1u, "L2", "fact",
                                   "replay-workspace", "replay-project", list_ids,
                                   AIMEE_DB2_LIST_ROWS_MAX, &list_count, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(list_count == 0);

   list_count = 99;
   assert(aimee_db2_list_rows_call(call_client, &client, 9091, 0, 4u, 0u, 0u, "", "", "", "",
                                   list_ids, AIMEE_DB2_LIST_ROWS_MAX, &list_count, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(list_count == 0);

   /* All three aggregate statements are replayed, because which one runs depends
    * on which selector is set and only a real database proves each parses. */
   uint64_t aggregate_ids[AIMEE_DB2_AGGREGATE_MAX];
   uint32_t aggregate_count = 99, aggregate_truncated = 99;
   assert(aimee_db2_aggregate_call(call_client, &client, 9100, 0, "replay-entity", "", 4u,
                                   &aggregate_truncated, aggregate_ids, AIMEE_DB2_AGGREGATE_MAX,
                                   &aggregate_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aggregate_count == 0 && aggregate_truncated == 0);

   aggregate_count = 99;
   assert(aimee_db2_aggregate_call(call_client, &client, 9101, 0, "", "replay-keyword", 4u,
                                   &aggregate_truncated, aggregate_ids, AIMEE_DB2_AGGREGATE_MAX,
                                   &aggregate_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aggregate_count == 0);

   aggregate_count = 99;
   assert(aimee_db2_aggregate_call(call_client, &client, 9102, 0, "", "", 4u, &aggregate_truncated,
                                   aggregate_ids, AIMEE_DB2_AGGREGATE_MAX, &aggregate_count, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(aggregate_count == 0);

   /* An empty corpus matches no plan, so the label comes back empty too. */
   uint64_t eval_corpus_ids[AIMEE_DB2_LOAD_EVAL_CORPUS_MAX];
   uint32_t eval_corpus_count = 99;
   char eval_corpus_label[AIMEE_DB2_LOAD_EVAL_CORPUS_LABEL_MAX + 1] = "unset";
   assert(aimee_db2_load_eval_corpus_call(call_client, &client, 9103, 0, 4u, eval_corpus_label,
                                          sizeof(eval_corpus_label), eval_corpus_ids,
                                          AIMEE_DB2_LOAD_EVAL_CORPUS_MAX, &eval_corpus_count, NULL,
                                          NULL) == AIMEE_MODULE_CALL_OK);
   assert(eval_corpus_count == 0 && eval_corpus_label[0] == '\0');

   /* An empty corpus has no such rows, so both probes answer false against a
    * real database rather than against a stub that decided to. */
   uint32_t probe_exists = 99;
   assert(aimee_db2_record_exists_call(call_client, &client, 9110, 0, 4096u, &probe_exists, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_exists == 0);
   probe_exists = 99;
   assert(aimee_db2_document_exists_call(call_client, &client, 9111, 0, 4096u, &probe_exists, NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_exists == 0);

   /* The watermark write is an insert, so it proves the table and the clock
    * function both exist. */
   assert(aimee_db2_trace_mining_record_call(call_client, &client, 9112, 0, 90210u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);

   /* Four different tables on an empty schema, so each answers zero -- which is
    * only meaningful because a statement that failed to prepare would abort the
    * call rather than answer. */
   uint32_t string_answer = 99;
   string_answer = 99;
   assert(aimee_db2_anti_pattern_exists_exact_call(call_client, &client, 9120, 0, "replay-argument",
                                                   &string_answer, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_anti_pattern_exists_by_source_ref_call(call_client, &client, 9121, 0,
                                                           "replay-argument", &string_answer, NULL,
                                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_artifact_citation_count_call(call_client, &client, 9122, 0, "replay-argument",
                                                 &string_answer, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_commits_in_last_7_days_call(call_client, &client, 9123, 0, "replay-argument",
                                                &string_answer, NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_entity_observation_count_call(call_client, &client, 9130, 0, "replay-argument",
                                                  &string_answer, NULL,
                                                  NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_fidelity_attribution_count_call(call_client, &client, 9131, 0,
                                                    "replay-argument", &string_answer, NULL,
                                                    NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_blob_referenced_call(call_client, &client, 9132, 0, "replay-argument",
                                         &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_async_pending_count_call(call_client, &client, 9133, 0, "replay-argument",
                                             &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   /* Eight statements against real tables. The values they answer with depend on
    * an empty schema and are not asserted here; what matters is that each
    * statement prepared and ran, which only a real database can say. */
   assert(aimee_db2_artifact_stamp_reflected_call(call_client, &client, 9140, 0, "replay-argument",
                                                  NULL, NULL) == AIMEE_MODULE_CALL_OK);

   string_answer = 99;
   assert(aimee_db2_failed_query_bump_call(call_client, &client, 9141, 0, "replay-argument",
                                           &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);

   string_answer = 99;
   assert(aimee_db2_fence_active_call(call_client, &client, 9142, 0, "replay-argument",
                                      &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);

   assert(aimee_db2_runtime_state_touch_call(call_client, &client, 9143, 0, "replay-argument", NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);

   /* The queue's column references artifacts, so this names one that does not
    * exist and the database refuses it. The operation has one result, so the
    * refusal arrives as an internal failure -- which is the point: a caller
    * cannot tell it from the statement having gone wrong. */
   assert(aimee_db2_synth_enqueue_call(call_client, &client, 9144, 0, "replay-argument", NULL,
                                       NULL) == AIMEE_MODULE_CALL_INTERNAL);

   assert(aimee_db2_synth_mark_done_call(call_client, &client, 9145, 0, "replay-argument", NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);

   assert(aimee_db2_reembed_mark_finished_call(call_client, &client, 9146, 0,
                                               "2026-01-01T00:00:00Z", NULL,
                                               NULL) == AIMEE_MODULE_CALL_OK);

   string_answer = 99;
   assert(aimee_db2_mining_job_try_lock_call(call_client, &client, 9147, 0, "replay-argument",
                                             &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);

   /* Seven pair statements against real tables. Two of them insert into tables
    * whose artifact column references artifacts(id), so on an empty schema the
    * database refuses them -- which the expected result states rather than
    * hides. */
   assert(aimee_db2_artifact_set_state_call(call_client, &client, 9160, 0, "proposed",
                                            "replay-artifact", NULL, NULL) == AIMEE_MODULE_CALL_OK);

   assert(aimee_db2_artifact_register_exemplar_call(call_client, &client, 9161, 0,
                                                    "replay-artifact", "case_exemplars", NULL,
                                                    NULL) == AIMEE_MODULE_CALL_INTERNAL);

   assert(aimee_db2_evidence_enqueue_call(call_client, &client, 9162, 0, "replay-artifact",
                                          "evidence", NULL, NULL) == AIMEE_MODULE_CALL_INTERNAL);

   assert(aimee_db2_evidence_mark_failed_call(call_client, &client, 9163, 0, "replay-artifact",
                                              "replay error", NULL, NULL) == AIMEE_MODULE_CALL_OK);

   assert(aimee_db2_synth_mark_failed_call(call_client, &client, 9164, 0, "replay-artifact",
                                           "replay error", NULL, NULL) == AIMEE_MODULE_CALL_OK);

   assert(aimee_db2_runtime_state_set_call(call_client, &client, 9165, 0, "replay-key",
                                           "replay-value", NULL, NULL) == AIMEE_MODULE_CALL_OK);

   assert(aimee_db2_set_active_embedder_version_call(call_client, &client, 9166, 0,
                                                     "replay-version", "2026-01-01T00:00:00Z", NULL,
                                                     NULL) == AIMEE_MODULE_CALL_OK);

   /* Five pair statements against real tables. entity_profile_fresh passes a
    * real Postgres interval, because an unparseable one fails at the database
    * rather than answering false -- which is the property its policy states. */
   string_answer = 99;
   assert(aimee_db2_entity_profile_fresh_call(call_client, &client, 9180, 0, "replay-first",
                                              "-1 hour", &string_answer, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_doc_exists_by_hash_call(call_client, &client, 9181, 0, "replay-first",
                                            "replay-scope", &string_answer, NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_pdf_quarantine_confirm_call(call_client, &client, 9182, 0, "replay-first",
                                                "replay/file.pdf", &string_answer, NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_pdf_quarantine_reject_call(call_client, &client, 9183, 0, "replay-first",
                                               "replay/file.pdf", &string_answer, NULL,
                                               NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   string_answer = 99;
   assert(aimee_db2_enrollment_active_call(call_client, &client, 9184, 0, "replay-first",
                                           "replay-serial", &string_answer, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0);

   /* runtime_state_set wrote this key earlier in the run, so reading it back is
    * a round trip through two operations on two different formats. */
   {
      char state_value[AIMEE_DB2_RUNTIME_STATE_GET_STATE_VALUE_MAX + 1] = "";
      assert(aimee_db2_runtime_state_get_call(call_client, &client, 9190, 0, "replay-key",
                                              state_value, sizeof(state_value), NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
      assert(strcmp(state_value, "replay-value") == 0);
   }

   assert(aimee_db2_health_record_call(call_client, &client, 9035, 0, 4u, 2u, 9u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);

   uint32_t snapshots_deleted = 99, contradictions_deleted = 99;
   assert(aimee_db2_health_retention_call(call_client, &client, 9036, 0, &snapshots_deleted,
                                          &contradictions_deleted, NULL,
                                          NULL) == AIMEE_MODULE_CALL_OK);
   assert(snapshots_deleted == 0 && contradictions_deleted == 0);

   /* health_record above wrote one cycle row through the packaged process, and
    * retention did not prune it (it is not 90 days old), so these counters read
    * back exactly what that write recorded. This is the round trip the packaged
    * replay exists to prove: a write and a later read, both across the bus,
    * against a real database. */
   aimee_db2_health_counters_t counters = {.cycles = 99};
   assert(aimee_db2_health_counters_call(call_client, &client, 9037, 0, &counters, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(counters.cycles == 1 && counters.total_promotions == 4 && counters.total_demotions == 2 &&
          counters.total_expirations == 9);
   /* The corpus itself is untouched, so the derived counters stay empty. */
   assert(counters.total_contradictions == 0 && counters.new_memories == 0 &&
          counters.l2_total == 0 && counters.l2_stale_30_days == 0);

   aimee_db2_memory_stats_t corpus = {.total = 99};
   assert(aimee_db2_stats_counts_call(call_client, &client, 9038, 0, &corpus, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(corpus.total == 0 && corpus.conflicts == 0 && corpus.kind_counts[9] == 0);

   uint32_t level0_deleted = 99, stale_deleted = 99;
   assert(aimee_db2_expire_call(call_client, &client, 9039, 0, &level0_deleted, &stale_deleted,
                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(level0_deleted == 0 && stale_deleted == 0);

   uint32_t tier_demoted = 99, tier_cascaded = 99;
   assert(aimee_db2_demote_call(call_client, &client, 9040, 0, &tier_demoted, &tier_cascaded, NULL,
                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(tier_demoted == 0 && tier_cascaded == 0);

   uint32_t tier_promoted = 99;
   assert(aimee_db2_promote_stable_call(call_client, &client, 9041, 0, &tier_promoted, NULL,
                                        NULL) == AIMEE_MODULE_CALL_OK);
   assert(tier_promoted == 0);

   uint32_t reclassified = 99;
   assert(aimee_db2_reclassify_directives_call(call_client, &client, 9042, 0, 1u, &reclassified,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reclassified == 0);

   /* Memory 42 does not exist, so the approval row's foreign key onto memories
    * refuses the insert. The point is that a backend failure crosses the
    * packaged boundary as INTERNAL rather than being reported as success. */
   assert(aimee_db2_record_l4_approval_call(call_client, &client, 9043, 0, 42u, "operator",
                                            "reviewed", NULL, NULL) == AIMEE_MODULE_CALL_INTERNAL);

   /* The schema is fresh, so no L0 row is old enough to fall outside the fixed
    * seven-day window and the sweep deletes nothing. Replaying it must return
    * the same zero: that is what the catalog's `safe` idempotency claims, and a
    * second call is the only thing that actually demonstrates it. */
   uint32_t pruned = 99;
   assert(aimee_db2_prune_orphaned_l0_call(call_client, &client, 9044, 0, &pruned, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(pruned == 0);
   pruned = 99;
   assert(aimee_db2_prune_orphaned_l0_call(call_client, &client, 9045, 0, &pruned, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(pruned == 0);

   /* No pending row exists on a fresh schema, so nothing has a time-to-live to
    * outlive and the sweep archives nothing. Replayed for the same reason as
    * the prune above: the comparison is against now, so a second call is what
    * actually demonstrates the catalog's `safe` claim. */
   uint32_t archived = 99;
   assert(aimee_db2_lifecycle_sweep_expired_call(call_client, &client, 9046, 0, &archived, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(archived == 0);
   archived = 99;
   assert(aimee_db2_lifecycle_sweep_expired_call(call_client, &client, 9047, 0, &archived, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(archived == 0);

   /* Memory 42 does not exist on a fresh schema, so the primary-key predicate
    * matches nothing and no row decays. Not replayed the way the sweeps above
    * are: this operation is declared unsafe precisely because a second call
    * would decay a real row again, so proving neutrality here would prove
    * nothing and asserting it would contradict the catalog. */
   uint32_t decayed = 99;
   assert(aimee_db2_demote_id_call(call_client, &client, 9048, 0, 42u, &decayed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(decayed == 0);

   /* Zero is refused before anything is published, so the process never sees
    * a request that names no row. */
   assert(aimee_db2_demote_id_call(call_client, &client, 9049, 0, 0u, &decayed, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 carries no attribution row on a fresh schema, so the probe
    * misses. Replayed because it is a pure read: two identical answers are
    * exactly what a safe operation must give. */
   uint32_t tagged = 99;
   assert(aimee_db2_has_workspace_tag_call(call_client, &client, 9050, 0, 42u, &tagged, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(tagged == 0);
   tagged = 99;
   assert(aimee_db2_has_workspace_tag_call(call_client, &client, 9051, 0, 42u, &tagged, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(tagged == 0);
   assert(aimee_db2_has_workspace_tag_call(call_client, &client, 9052, 0, 0u, &tagged, NULL,
                                           NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist, so the delete removes nothing and reports zero.
    * Replayed because deleting an absent row is genuinely neutral: the second
    * call must give the same answer, which is what `safe` claims here. */
   uint32_t removed = 99;
   assert(aimee_db2_delete_row_call(call_client, &client, 9053, 0, 42u, &removed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(removed == 0);
   removed = 99;
   assert(aimee_db2_delete_row_call(call_client, &client, 9054, 0, 42u, &removed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(removed == 0);
   assert(aimee_db2_delete_row_call(call_client, &client, 9055, 0, 0u, &removed, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist, so the bump matches no row and the backend
    * reports failure, which crosses the packaged boundary as INTERNAL rather
    * than a quiet acknowledgement. Called once: the operation is declared
    * unsafe because a real row's use count would move on every call. */
   assert(aimee_db2_touch_call(call_client, &client, 9056, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);
   assert(aimee_db2_touch_call(call_client, &client, 9057, 0, 0u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Link 7 does not exist, but a delete that matches no row is still a
    * completed statement, so this acknowledges rather than failing. Replayed
    * because removing an absent relation is genuinely neutral. */
   assert(aimee_db2_link_delete_call(call_client, &client, 9058, 0, 7u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_link_delete_call(call_client, &client, 9059, 0, 7u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_link_delete_call(call_client, &client, 9060, 0, 0u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist, so the statement completes with no row: the
    * memory is not in force, and that is a real answer rather than a failure
    * to evaluate. Pinning ok/zero here is what keeps the invalid_state path
    * meaning "could not tell" instead of collecting every empty result. */
   uint32_t valid_result = 99, in_force = 99;
   assert(aimee_db2_valid_at_call(call_client, &client, 9061, 0, 42u, "2026-08-18 12:00:00",
                                  &valid_result, &in_force, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(valid_result == AIMEE_DB2_RESULT_OK && in_force == 0);
   valid_result = 99;
   in_force = 99;
   assert(aimee_db2_valid_at_call(call_client, &client, 9062, 0, 42u, "2026-08-18 12:00:00",
                                  &valid_result, &in_force, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(valid_result == AIMEE_DB2_RESULT_OK && in_force == 0);
   assert(aimee_db2_valid_at_call(call_client, &client, 9063, 0, 42u, "", &valid_result, &in_force,
                                  NULL, NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* No scope rows exist on a fresh schema, so the probe misses. Replayed
    * because a pure read must give the same answer twice. */
   uint32_t scoped = 99;
   assert(aimee_db2_has_scope_type_call(call_client, &client, 9064, 0, 42u, "workspace", &scoped,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(scoped == 0);
   scoped = 99;
   assert(aimee_db2_has_scope_type_call(call_client, &client, 9065, 0, 42u, "workspace", &scoped,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(scoped == 0);
   assert(aimee_db2_has_scope_type_call(call_client, &client, 9066, 0, 42u, "", &scoped, NULL,
                                        NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist, so nothing is penalised and the backend reports
    * that as failure. Called once: the operation is declared unsafe because a
    * real row's confidence would fall further on every call. */
   assert(aimee_db2_reject_call(call_client, &client, 9067, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);
   assert(aimee_db2_reject_call(call_client, &client, 9068, 0, 0u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist, so nothing is rewritten and the count is zero.
    * Replayed because writing the same text to the same absent row is
    * genuinely neutral -- and would be neutral for a present row too, which is
    * why this one is safe while memory.reject beside it is not. */
   uint32_t rewritten = 99;
   assert(aimee_db2_update_content_call(call_client, &client, 9069, 0, 42u, "revised text",
                                        &rewritten, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(rewritten == 0);
   rewritten = 99;
   assert(aimee_db2_update_content_call(call_client, &client, 9070, 0, 42u, "revised text",
                                        &rewritten, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(rewritten == 0);
   assert(aimee_db2_update_content_call(call_client, &client, 9071, 0, 42u, "", &rewritten, NULL,
                                        NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist. The backend returns void, so the packaged
    * process acknowledges regardless -- there is no answer to assert beyond
    * that the call completed. Called once: the operation is declared unsafe
    * because a real row's confidence would fall on every call. */
   assert(aimee_db2_decay_confidence_call(call_client, &client, 9072, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_decay_confidence_call(call_client, &client, 9073, 0, 0u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist, so the foreign key refuses the attribution row
    * -- but the backend returns void and swallows that, so the packaged
    * process still acknowledges. Replayed because ON CONFLICT DO NOTHING makes
    * a repeat a genuine no-op, which is what `safe` claims here. */
   assert(aimee_db2_workspace_tag_insert_call(call_client, &client, 9074, 0, 42u, "aimee", NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_workspace_tag_insert_call(call_client, &client, 9075, 0, 42u, "aimee", NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_workspace_tag_insert_call(call_client, &client, 9076, 0, 42u, "", NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist, so the update matches no row; the void backend
    * swallows that and the process acknowledges. Replayed because writing the
    * same kind twice leaves the column where the first call left it. */
   assert(aimee_db2_set_cognified_kind_call(call_client, &client, 9077, 0, 42u, "preference", NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_set_cognified_kind_call(call_client, &client, 9078, 0, 42u, "preference", NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_set_cognified_kind_call(call_client, &client, 9079, 0, 42u, "", NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 does not exist, so the update matches no row and the void
    * backend swallows it. The clear is exercised against the real process too,
    * because an empty session is a request this operation must accept rather
    * than refuse -- the boundary between the two setters sharing this format. */
   assert(aimee_db2_set_source_session_call(call_client, &client, 9080, 0, 42u, "sess-1", NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_set_source_session_call(call_client, &client, 9081, 0, 42u, "", NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);

   /* Same shape as the session setter above: the update matches no row on a
    * fresh schema and the void backend swallows it. Both a populated and an
    * empty extraction are exercised, because an empty one is what a memory
    * with no negations produces and it still has to clear the column. */
   assert(aimee_db2_negation_tokens_update_call(call_client, &client, 9082, 0, 42u, "not never",
                                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_negation_tokens_update_call(call_client, &client, 9083, 0, 42u, "", NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);

   /* Memory 42 does not exist, so the read is a genuine miss and must come
    * back as not_found rather than as empty content -- the distinction this
    * operation carries, exercised against the real process. */
   uint32_t content_result = 99;
   static char read_content[AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1];
   assert(aimee_db2_get_content_call(call_client, &client, 9084, 0, 42u, &content_result,
                                     read_content, sizeof(read_content), NULL,
                                     NULL) == AIMEE_MODULE_CALL_OK);
   assert(content_result == AIMEE_DB2_RESULT_NOT_FOUND && read_content[0] == '\0');
   assert(aimee_db2_get_content_call(call_client, &client, 9085, 0, 0u, &content_result,
                                     read_content, sizeof(read_content), NULL,
                                     NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);

   /* Memory 42 has no row and therefore no session. This backend cannot tell
    * that apart from a blank column, so both arrive as not_found -- the
    * narrower answer than get_content gives above, and deliberately so. */
   uint32_t session_result = 99;
   char read_session[AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX + 1];
   assert(aimee_db2_get_source_session_call(call_client, &client, 9086, 0, 42u, &session_result,
                                            read_session, sizeof(read_session), NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(session_result == AIMEE_DB2_RESULT_NOT_FOUND && read_session[0] == '\0');

   /* No temporal reference rows exist on a fresh schema, so the ranked pick
    * has nothing to return and reports not_found. */
   uint32_t ref_result = 99;
   char read_ref[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX + 1];
   assert(aimee_db2_pick_first_temporal_ref_call(call_client, &client, 9087, 0, 42u, &ref_result,
                                                 read_ref, sizeof(read_ref), NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(ref_result == AIMEE_DB2_RESULT_NOT_FOUND && read_ref[0] == '\0');

   /* The aggregate runs against an empty corpus: zero rows, no latest update.
    * That is ok with an empty stamp, NOT invalid_state -- the boundary this
    * operation exists to keep, exercised against the real process. */
   uint32_t corpus_result = 99, corpus_count = 99;
   char corpus_stamp[AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX + 1];
   assert(aimee_db2_count_and_max_updated_call(call_client, &client, 9088, 0, &corpus_result,
                                               &corpus_count, corpus_stamp, sizeof(corpus_stamp),
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(corpus_result == AIMEE_DB2_RESULT_OK && corpus_count == 0 && corpus_stamp[0] == '\0');

   /* The index family reaching the real process for the first time. An empty
    * schema has no orphaned edges to prune, no edges to renormalise and no
    * current projects, so all three answer zero -- and zero is the success
    * these operations report, not an absent result. */
   uint32_t index_pruned = 99, index_normalized = 99, projects = 99;
   assert(aimee_db2_entity_edge_prune_orphans_call(call_client, &client, 9089, 0, &index_pruned,
                                                   NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(index_pruned == 0);
   assert(aimee_db2_entity_edge_normalize_weights_call(call_client, &client, 9090, 0,
                                                       &index_normalized, NULL,
                                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(index_normalized == 0);
   assert(aimee_db2_project_count_call(call_client, &client, 9091, 0, &projects, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(projects == 0);

   /* A fresh schema has no inadmissible file to remove, and replaying the sweep
    * must return the same zero: that is what the catalog's `safe` idempotency
    * claims, and a second call is the only thing that demonstrates it. */
   uint32_t purged = 99;
   assert(aimee_db2_purge_hidden_pollution_call(call_client, &client, 9092, 0, &purged, NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(purged == 0);
   purged = 99;
   assert(aimee_db2_purge_hidden_pollution_call(call_client, &client, 9093, 0, &purged, NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(purged == 0);

   /* No project has drifted on a fresh schema, so nothing is enqueued. Zero is
    * also what the dedup produces once every drifted project already holds a
    * pending row, which is why replaying the sweep is harmless. */
   uint32_t requeued = 99;
   assert(aimee_db2_requeue_drifted_call(call_client, &client, 9094, 0, &requeued, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(requeued == 0);
   requeued = 99;
   assert(aimee_db2_requeue_drifted_call(call_client, &client, 9095, 0, &requeued, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(requeued == 0);

   /* A rebuild inside one transaction. On an empty index the table is emptied
    * and refilled with nothing, and the size it reports is a genuine zero
    * rather than the -1 a rolled-back rebuild would have produced. */
   uint32_t route_count = 99;
   assert(aimee_db2_cross_repo_rebuild_routes_call(call_client, &client, 9107, 0, &route_count,
                                                   NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(route_count == 0);

   /* No manifest is indexed on a fresh schema, so the identity rebuild writes
    * nothing. Zero here is an empty result, not the -1 a rollback produces. */
   uint32_t identities_written = 99;
   assert(aimee_db2_cross_repo_rebuild_identities_call(call_client, &client, 9108, 0,
                                                       &identities_written, NULL,
                                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(identities_written == 0);

   /* The third cross-repo rebuild. No project exists, so the project list is
    * empty rather than truncated, and the rebuild commits an empty table. */
   uint32_t build_deps_written = 99;
   assert(aimee_db2_cross_repo_rebuild_build_deps_call(call_client, &client, 9109, 0,
                                                       &build_deps_written, NULL,
                                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(build_deps_written == 0);

   /* The drift count against real Postgres. Nothing is indexed, so nothing has
    * drifted, and the requeue above found the same nothing through the same
    * predicate. */
   uint64_t drift = 99;
   assert(aimee_db2_drift_candidates_call(call_client, &client, 9122, 0, &drift, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(drift == 0);

   /* The learning family reaching the real process for the first time. No rule
    * has ever been reinforced on a fresh schema, so nothing is due for decay
    * and nothing falls through the archive threshold. */
   uint32_t rules_touched = 99;
   assert(aimee_db2_rules_decay_call(call_client, &client, 9110, 0, &rules_touched, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(rules_touched == 0);

   /* No curiosity item exists on a fresh schema, so the rescore has nothing to
    * weigh. Zero here is an empty working set, not a failure. */
   uint32_t items_rescored = 99;
   assert(aimee_db2_curiosity_rescore_all_call(call_client, &client, 9111, 0, &items_rescored, NULL,
                                               NULL) == AIMEE_MODULE_CALL_OK);
   assert(items_rescored == 0);

   /* Seeding the mining jobs, then seeding them again. The second pass finds
    * every job already present, every insert conflicts and does nothing, and
    * the acknowledgement is identical. That is what makes the seed safe to
    * replay against a database an operator has since tuned. */
   assert(aimee_db2_mining_seed_job_defaults_call(call_client, &client, 9112, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_mining_seed_job_defaults_call(call_client, &client, 9113, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);

   /* The organization family reaching the real process for the first time, and
    * the first seed here that writes rows the module does not own: the
    * relation-type ontology is declared elsewhere and only persisted here.
    * Seeding twice is the same acknowledgement both times. */
   assert(aimee_db2_rel_types_ensure_seed_call(call_client, &client, 9114, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_rel_types_ensure_seed_call(call_client, &client, 9115, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);

   /* No learning proposal exists on a fresh schema, so the archive sweep has
    * nothing to retire. It would answer ok either way: this is the one
    * operation on the bus whose backend cannot report a failure. */
   assert(aimee_db2_proposals_archive_expired_call(call_client, &client, 9116, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);

   /* trace_mining_record wrote this watermark earlier in the run, so reading it
    * back is a round trip rather than a fresh-schema zero: one operation wrote
    * the row and another read it, both across the bus and against a real
    * database. A failed read would answer zero, which is why the value matters
    * more than the call succeeding. */
   uint64_t watermark = 99;
   assert(aimee_db2_trace_mining_last_id_call(call_client, &client, 9124, 0, &watermark, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(watermark == 90210);

   /* The custody family against real Postgres. Sequentially the lock behaves:
    * the first acquire claims it, the second is refused because the row the
    * first wrote is inside its lease window, and after a release the next
    * acquire succeeds again. That is what this replay can show. The race the
    * catalog records is concurrent, not sequential -- two callers interleaving
    * between the read and the write -- and a single-threaded replay cannot
    * produce it. The gap is recorded in the catalog rather than demonstrated
    * here, and the release below removes the row without asking who owns it. */
   uint32_t acquired = 99;
   assert(aimee_db2_vector_rebuild_lock_try_acquire_call(call_client, &client, 9117, 0, &acquired,
                                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acquired == 1);
   acquired = 99;
   assert(aimee_db2_vector_rebuild_lock_try_acquire_call(call_client, &client, 9118, 0, &acquired,
                                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acquired == 0);
   assert(aimee_db2_vector_rebuild_lock_release_call(call_client, &client, 9119, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   acquired = 99;
   assert(aimee_db2_vector_rebuild_lock_try_acquire_call(call_client, &client, 9120, 0, &acquired,
                                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acquired == 1);
   assert(aimee_db2_vector_rebuild_lock_release_call(call_client, &client, 9121, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);

   /* No active release is set on a fresh schema. The zero that comes back is
    * the missing-key case; the other two the catalog records -- a value that
    * will not parse, and one that is not positive -- need a corrupted row to
    * produce and are indistinguishable from this one when they do. */
   uint64_t release_id = 99;
   assert(aimee_db2_release_get_active_call(call_client, &client, 9123, 0, &release_id, NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(release_id == 0);

   /* The maintenance family reaching the real process for the first time. No
    * prospective memory is armed on a fresh schema, so nothing expires, and
    * the second call returning the same zero is what the catalog's safe
    * idempotency claims. */
   uint32_t expired = 99;
   assert(aimee_db2_prospective_sweep_expired_call(call_client, &client, 9096, 0, &expired, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(expired == 0);
   expired = 99;
   assert(aimee_db2_prospective_sweep_expired_call(call_client, &client, 9097, 0, &expired, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(expired == 0);

   /* No epistemic directive is open on a fresh schema, so nothing expires. */
   uint32_t directives = 99;
   assert(aimee_db2_directive_sweep_expired_call(call_client, &client, 9098, 0, &directives, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(directives == 0);

   /* No directive exists, so neither the suppression nor the surfacing matches
    * a row. Both report that the same way they report a failed statement,
    * which is the collapse the catalog records. */
   assert(aimee_db2_directive_suppress_call(call_client, &client, 9125, 0, 1, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);
   assert(aimee_db2_directive_record_surface_call(call_client, &client, 9126, 0, 1, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);

   /* Four by-id operations across two families against real Postgres, none of
    * which matches a row on a fresh schema. The bump is the one that reports
    * success anyway: its statement ran, and it does not ask whether it counted
    * anything. The three deletes all report the miss as a failure. */
   assert(aimee_db2_anti_pattern_bump_call(call_client, &client, 9127, 0, 41, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(aimee_db2_anti_pattern_delete_call(call_client, &client, 9128, 0, 41, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);
   assert(aimee_db2_doc_delete_call(call_client, &client, 9129, 0, 43, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);
   assert(aimee_db2_task_delete_call(call_client, &client, 9130, 0, 44, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);

   /* The three project clears against real Postgres. No project exists, so all
    * three delete nothing and report zero -- which for the file index is also
    * what a failed statement would report, since it does not check. */
   uint32_t cleared = 99;
   assert(aimee_db2_file_index_delete_project_call(call_client, &client, 9131, 0, "demo", &cleared,
                                                   NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(cleared == 0);
   cleared = 99;
   assert(aimee_db2_clear_project_call(call_client, &client, 9132, 0, "demo", &cleared, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(cleared == 0);
   cleared = 99;
   assert(aimee_db2_clear_current_project_call(call_client, &client, 9133, 0, "demo", &cleared,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(cleared == 0);

   /* No decision is logged on a fresh schema, so none is due for review. This
    * backend reports a failed statement as -1 rather than as zero, so the zero
    * arriving here is the sweep having genuinely run and found nothing. */
   uint32_t marked = 99;
   assert(aimee_db2_mark_revisit_due_call(call_client, &client, 9099, 0, &marked, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(marked == 0);

   /* Nothing has ever claimed an ingest queue row on a fresh schema, so the
    * recovery has nothing to hand back. Replaying it finds the same nothing. */
   uint32_t reset_rows = 99;
   assert(aimee_db2_ingest_queue_reset_running_call(call_client, &client, 9100, 0, &reset_rows,
                                                    NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reset_rows == 0);
   reset_rows = 99;
   assert(aimee_db2_ingest_queue_reset_running_call(call_client, &client, 9101, 0, &reset_rows,
                                                    NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reset_rows == 0);

   /* The evidence index is empty on a fresh schema, so the total-reach reset
    * touches nothing. That is the one case where its reach does not matter. */
   uint32_t evidence_rows = 99;
   assert(aimee_db2_evidence_reembed_all_call(call_client, &client, 9102, 0, &evidence_rows, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(evidence_rows == 0);

   /* No artifact has ever been committed on a fresh schema, so the six
    * re-derivable kinds have nothing to demote. */
   uint32_t demoted_artifacts = 99;
   assert(aimee_db2_curator_reembed_all_call(call_client, &client, 9103, 0, &demoted_artifacts,
                                             NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(demoted_artifacts == 0);

   /* No learning synthesis operation exists on a fresh schema, so the mirror
    * of the evidence reset above touches nothing either. */
   uint32_t reenqueued_ops = 99;
   assert(aimee_db2_synth_reenqueue_all_call(call_client, &client, 9104, 0, &reenqueued_ops, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(reenqueued_ops == 0);

   /* No document exists on a fresh schema, so the pass enqueues nothing and
    * the queue it then counts is empty. Replaying it returns the same size,
    * because the number is a queue size rather than a change count. */
   uint32_t extract_jobs = 99;
   assert(aimee_db2_curator_reenqueue_extract_all_call(call_client, &client, 9105, 0, &extract_jobs,
                                                       NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(extract_jobs == 0);
   extract_jobs = 99;
   assert(aimee_db2_curator_reenqueue_extract_all_call(call_client, &client, 9106, 0, &extract_jobs,
                                                       NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(extract_jobs == 0);

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

   /* The seven operations added with this batch, against real Postgres.
    *
    * flag_review is the one that matters. Its merge used to be a
    * json_build_object expression that Postgres refused to plan, so it failed
    * on every call; a fresh schema hid that, because with no artifact to flag
    * the refusal was the expected answer. The replay environment seeds one
    * committed artifact carrying a payload of its own, so the acknowledgement
    * here means a row was read, merged and written back. Whether the merge
    * kept the artifact's own keys is asserted by the replay script afterwards,
    * which reads the row: this call can only see the reply. */
   uint32_t flagged = 9;
   assert(aimee_db2_artifact_flag_review_call(call_client, &client, 9191, 0, "replay-flag-probe",
                                              "replayed", &flagged, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(flagged == 1);
   flagged = 9;
   assert(aimee_db2_artifact_flag_review_call(call_client, &client, 9192, 0, "replay-flag-missing",
                                              "replayed", &flagged, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(flagged == 0);

   /* Nothing has been thumbed down on this schema, so the answer is no. The
    * catalog records that the same no comes back when the read fails, which is
    * why this assertion proves less than it looks. */
   uint32_t suppressed = 9;
   assert(aimee_db2_verdict_suppressed_call(call_client, &client, 9193, 0, "replay-tag",
                                            "replay-scope", &suppressed, NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(suppressed == 0);

   /* No project is indexed, so there is no document to invalidate and no asset
    * to delete; both answer zero, which is also what a project whose work was
    * already done answers. */
   uint32_t invalidated = 9;
   assert(aimee_db2_curator_invalidate_doc_call(call_client, &client, 9194, 0, "demo",
                                                "src/replay.c", &invalidated, NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(invalidated == 0);
   uint32_t assets_deleted = 9;
   assert(aimee_db2_doc_assets_delete_for_doc_call(call_client, &client, 9195, 0, "demo",
                                                   "docs/replay.pdf", &assets_deleted, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(assets_deleted == 0);
   uint32_t minhash_gone = 9;
   assert(aimee_db2_minhash_delete_file_call(call_client, &client, 9196, 0, "demo", "src/replay.c",
                                             &minhash_gone, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(minhash_gone == 1);

   /* The three ontology decisions all require an evaluation row to exist, and
    * none does here, so each is refused. That the refusal reaches the caller
    * as a zero rather than a transport error is the part worth replaying: each
    * one rolls its transaction back inside the module. */
   uint32_t decided = 9;
   assert(aimee_db2_ontology_approve_call(call_client, &client, 9197, 0, "replay_rel", &decided,
                                          NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided == 0);
   decided = 9;
   assert(aimee_db2_ontology_reject_call(call_client, &client, 9198, 0, "replay_rel", &decided,
                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided == 0);
   decided = 9;
   assert(aimee_db2_ontology_map_call(call_client, &client, 9199, 0, "replay_rel", "replay_target",
                                      &decided, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided == 0);

   /* Nothing is indexed for this project, so nothing is enumerated and no
    * convention can be derived. assert_conventions also answers zero when the
    * style-graph or typed-fact configuration is off, and the module process
    * decides that, not this caller. */
   uint32_t enumerated = 9;
   assert(aimee_db2_css_migration_enumerate_call(call_client, &client, 9200, 0, "demo", &enumerated,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(enumerated == 0);
   uint32_t asserted = 9;
   assert(aimee_db2_css_migration_assert_conventions_call(call_client, &client, 9201, 0, "demo",
                                                          "2026-01-01T00:00:00Z", &asserted, NULL,
                                                          NULL) == AIMEE_MODULE_CALL_OK);
   assert(asserted == 0);

   /* Nothing carries this directive type and no project of this name exists,
    * so both deletes report zero rows. */
   uint32_t rules_deleted = 9;
   assert(aimee_db2_rules_delete_by_directive_type_call(call_client, &client, 9202, 0, "replay",
                                                        &rules_deleted, NULL,
                                                        NULL) == AIMEE_MODULE_CALL_OK);
   assert(rules_deleted == 0);
   uint32_t project_deleted = 9;
   assert(aimee_db2_project_delete_call(call_client, &client, 9203, 0, "replay-absent-project",
                                        &project_deleted, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(project_deleted == 0);

   /* Everything catalogued that this replay was not calling.
    *
    * Until the module binary started being rebuilt, an operation nothing
    * replayed was an operation nothing had ever run: artifact_flag_review was
    * broken from the day it was written and every gate stayed green. These
    * cases exist so that stops being possible, and where a round trip can be
    * arranged they do more than watch a fresh-schema zero come back.
    *
    * The collaboration rules are a real round trip. propose returns the
    * identifier the other three take, so each decision is applied to a rule
    * this run created, and a decision on an identifier no rule has is refused.
    * Rule identifiers start at one, so zero back from propose would mean the
    * insert did not happen. */
   uint32_t proposed_rule = 0, decided_rule = 9;
   assert(aimee_db2_collab_rule_propose_call(call_client, &client, 9204, 0, "replay rule one",
                                             "replay reason", "replay", &proposed_rule, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(proposed_rule > 0);
   assert(aimee_db2_collab_rule_approve_call(call_client, &client, 9205, 0, proposed_rule,
                                             &decided_rule, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided_rule == 1);
   uint32_t second_rule = 0;
   assert(aimee_db2_collab_rule_propose_call(call_client, &client, 9206, 0, "replay rule two",
                                             "replay reason", "replay", &second_rule, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(second_rule > proposed_rule);
   decided_rule = 9;
   assert(aimee_db2_collab_rule_reject_call(call_client, &client, 9207, 0, second_rule,
                                            &decided_rule, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided_rule == 1);
   /* Retire only touches an active rule, and approve is what makes one active,
    * so the third rule has to go through both. Retiring it straight from
    * proposed is refused, which is the assertion before the approve. */
   uint32_t third_rule = 0;
   assert(aimee_db2_collab_rule_propose_call(call_client, &client, 9208, 0, "replay rule three",
                                             "replay reason", "replay", &third_rule, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   decided_rule = 9;
   assert(aimee_db2_collab_rule_retire_call(call_client, &client, 9209, 0, third_rule,
                                            &decided_rule, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided_rule == 0);
   decided_rule = 9;
   assert(aimee_db2_collab_rule_approve_call(call_client, &client, 9245, 0, third_rule,
                                             &decided_rule, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided_rule == 1);
   decided_rule = 9;
   assert(aimee_db2_collab_rule_retire_call(call_client, &client, 9246, 0, third_rule,
                                            &decided_rule, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided_rule == 1);
   decided_rule = 9;
   assert(aimee_db2_collab_rule_approve_call(call_client, &client, 9210, 0, 2000000000u,
                                             &decided_rule, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(decided_rule == 0);
   uint32_t rule_removed = 9;
   assert(aimee_db2_rules_delete_by_id_call(call_client, &client, 9211, 0, 2000000000u,
                                            &rule_removed, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(rule_removed == 0);

   /* A promotion round trip: one promotion per decision point, so the second
    * set replaces the first and the get proves which one survived. */
   uint32_t promoted = 9;
   char promoted_arm[AIMEE_DB2_BANDIT_PROMOTION_GET_ARM_ID_MAX + 1] = "unset";
   assert(aimee_db2_bandit_promotion_set_call(call_client, &client, 9212, 0, "replay-dp", "arm-one",
                                              "rollback-one", &promoted, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(promoted == 1);
   assert(aimee_db2_bandit_promotion_get_call(call_client, &client, 9213, 0, "replay-dp",
                                              promoted_arm, sizeof(promoted_arm), NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(strcmp(promoted_arm, "arm-one") == 0);
   promoted = 9;
   assert(aimee_db2_bandit_promotion_set_call(call_client, &client, 9214, 0, "replay-dp", "arm-two",
                                              "", &promoted, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(promoted == 1);
   assert(aimee_db2_bandit_promotion_get_call(call_client, &client, 9215, 0, "replay-dp",
                                              promoted_arm, sizeof(promoted_arm), NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(strcmp(promoted_arm, "arm-two") == 0);

   /* No arm has been pulled at this decision point. The reply is an empty JSON
    * array rather than an empty string, which is also what a decision point
    * nobody has heard of returns, and what a failed read returns: the buffer
    * is filled in before the statement is prepared. */
   char arms[AIMEE_DB2_BANDIT_ARMS_LIST_ARMS_MAX + 1] = "unset";
   assert(aimee_db2_bandit_arms_list_call(call_client, &client, 9216, 0, "replay-dp", arms,
                                          sizeof(arms), NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(strcmp(arms, "[]") == 0);

   /* Citing and linking both need the artifact to exist, which the replay
    * environment seeds. A citation's source is not checked, so citing a source
    * that was never created is accepted; a link's far end is checked, so
    * linking to one that was not is refused. */
   uint32_t cited = 9;
   assert(aimee_db2_artifact_cite_call(call_client, &client, 9217, 0, "replay-flag-probe", "memory",
                                       "replay-source-that-does-not-exist", &cited, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(cited == 1);
   uint32_t linked = 9;
   assert(aimee_db2_artifact_link_call(call_client, &client, 9218, 0, "replay-flag-probe",
                                       "replay-link-target", "derives-from", &linked, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(linked == 1);
   linked = 9;
   assert(aimee_db2_artifact_link_call(call_client, &client, 9219, 0, "replay-flag-probe",
                                       "replay-artifact-that-does-not-exist", "derives-from",
                                       &linked, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(linked == 0);

   /* The rest have nothing to act on against a fresh schema, so each answers
    * its own kind of nothing. Replayed because an operation that is never
    * called is an operation whose handler, decoder and backend have never run
    * together against a real database -- which is how a statement that could
    * not be planned survived being written. */
   uint32_t surfaces = 9;
   assert(aimee_db2_calibration_surfaces_with_data_call(call_client, &client, 9220, 0, 1, &surfaces,
                                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(surfaces == 0);
   uint32_t merged_memories = 9;
   assert(aimee_db2_dedupe_by_key_call(call_client, &client, 9221, 0, 1, &merged_memories, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(merged_memories == 0);
   uint32_t stuck_reset = 9;
   assert(aimee_db2_reset_stuck_vector_ops_call(call_client, &client, 9222, 0, 3, &stuck_reset,
                                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(stuck_reset == 0);

   uint32_t acknowledged = 9;
   assert(aimee_db2_decision_log_set_outcome_call(call_client, &client, 9223, 0, 4242, "replayed",
                                                  &acknowledged, NULL,
                                                  NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 0);
   acknowledged = 9;
   assert(aimee_db2_decision_log_set_revisit_call(call_client, &client, 9224, 0, 4242,
                                                  "2026-01-01T00:00:00Z", &acknowledged, NULL,
                                                  NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 0);
   acknowledged = 9;
   assert(aimee_db2_decision_log_set_status_call(call_client, &client, 9225, 0, 4242, "active",
                                                 &acknowledged, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 0);

   acknowledged = 9;
   assert(aimee_db2_directive_resolve_call(call_client, &client, 9226, 0, 4242, 4243, &acknowledged,
                                           NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 0);
   acknowledged = 9;
   assert(aimee_db2_release_add_doc_call(call_client, &client, 9227, 0, 4242, 4243, &acknowledged,
                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 0);
   uint32_t member = 9;
   assert(aimee_db2_scene_member_exists_call(call_client, &client, 9228, 0, 4242, 4243, &member,
                                             NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(member == 0);
   uint32_t connected = 9;
   assert(aimee_db2_unit_edge_exists_call(call_client, &client, 9229, 0, 4242, 4243, &connected,
                                          NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(connected == 0);

   /* No proposal has this identifier, and both of these acknowledge anyway:
    * each reports that its statement ran, not that a row matched. The one is
    * the assertion -- a zero here would mean the statement failed. */
   acknowledged = 9;
   assert(aimee_db2_proposal_bump_corroboration_call(call_client, &client, 9230, 0, 4242,
                                                     &acknowledged, NULL,
                                                     NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 1);
   acknowledged = 9;
   assert(aimee_db2_proposal_mark_committed_call(call_client, &client, 9231, 0, 4242, &acknowledged,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 1);
   acknowledged = 9;
   assert(aimee_db2_prospective_set_state_call(call_client, &client, 9232, 0, 4242, "fired",
                                               &acknowledged, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 0);
   uint32_t task_changed = 9;
   assert(aimee_db2_task_update_state_call(call_client, &client, 9233, 0, 4242, "done",
                                           &task_changed, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(task_changed == 0);
   /* No such job, and the acknowledgement comes back anyway: the backend
    * discards the step result and returns success as long as the statement
    * could be prepared. */
   acknowledged = 9;
   assert(aimee_db2_ingest_queue_fail_call(call_client, &client, 9234, 0, 4242, "replayed",
                                           &acknowledged, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 1);

   /* The three generation writes, against a generation that does not exist.
    * Two of them acknowledge it: abort and set_source_hash report that their
    * statement completed and never that it matched. publish is the one that
    * checks -- it requires exactly one pending generation to become visible
    * and rolls back otherwise -- so it is also the only one whose reply here
    * distinguishes a missing generation from a present one. */
   acknowledged = 9;
   assert(aimee_db2_generation_abort_call(call_client, &client, 9235, 0, 4242, "replayed",
                                          &acknowledged, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 1);
   acknowledged = 9;
   assert(aimee_db2_generation_set_source_hash_call(call_client, &client, 9236, 0, 4242,
                                                    "replayhash", &acknowledged, NULL,
                                                    NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 1);
   acknowledged = 9;
   assert(aimee_db2_generation_publish_call(call_client, &client, 9237, 0, 4242, "demo",
                                            &acknowledged, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 0);

   /* The project-scoped index operations, against a project nothing indexed. */
   uint32_t index_deleted = 9;
   assert(aimee_db2_file_index_delete_current_generation_call(call_client, &client, 9238, 0, "demo",
                                                              &index_deleted, NULL,
                                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(index_deleted == 0);
   uint32_t minhash_cleared = 9;
   assert(aimee_db2_minhash_delete_current_generation_call(call_client, &client, 9239, 0, "demo",
                                                           &minhash_cleared, NULL,
                                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(minhash_cleared == 1);
   uint32_t purged_files = 9;
   assert(aimee_db2_purge_files_matching_call(call_client, &client, 9240, 0, 4242, "src/%",
                                              &purged_files, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(purged_files == 0);

   /* The four string-returning reads, each against something that is not
    * there. All four answer with an empty string, which is also what a failed
    * read answers -- the reason for replaying them is the handler and the
    * decoder, not the value. */
   char fingerprint[AIMEE_DB2_PROJECT_FINGERPRINT_FINGERPRINT_MAX + 1] = "unset";
   assert(aimee_db2_project_fingerprint_call(call_client, &client, 9241, 0, "demo", fingerprint,
                                             sizeof(fingerprint), NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   char source_hash[AIMEE_DB2_VISIBLE_SOURCE_HASH_SOURCE_HASH_MAX + 1] = "unset";
   assert(aimee_db2_visible_source_hash_call(call_client, &client, 9242, 0, "demo", source_hash,
                                             sizeof(source_hash), NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   static char profile_card[AIMEE_DB2_ENTITY_PROFILE_CARD_CARD_JSON_MAX + 1];
   profile_card[0] = 'x';
   assert(aimee_db2_entity_profile_card_call(call_client, &client, 9243, 0, "replay-entity",
                                             profile_card, sizeof(profile_card), NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   char eval_status[AIMEE_DB2_ONTOLOGY_EVAL_STATUS_STATUS_MAX + 1] = "unset";
   assert(aimee_db2_ontology_eval_status_call(call_client, &client, 9244, 0, "replay_rel",
                                              eval_status, sizeof(eval_status), NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(eval_status[0] == '\0');

   /* A release round trip: create returns the identifier that add_doc takes,
    * and adding a document to it is still refused because the document is a
    * foreign key too. The name is unique, so the second create with the same
    * name is refused and answers zero -- which is what this pair is here to
    * pin, the first draft of the review having claimed the opposite. */
   uint64_t first_release = 0, second_release = 99;
   assert(aimee_db2_release_create_call(call_client, &client, 9247, 0, "replay-release",
                                        &first_release, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(first_release > 0);
   assert(aimee_db2_release_create_call(call_client, &client, 9248, 0, "replay-release",
                                        &second_release, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(second_release == 0);
   acknowledged = 9;
   assert(aimee_db2_release_add_doc_call(call_client, &client, 9249, 0, first_release, 4242,
                                         &acknowledged, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acknowledged == 0);

   /* No project of this name is indexed, so its generation reads as none, no
    * projection generation can be opened for it, and none is visible. All
    * three answer zero, which each of them also answers for a project that
    * exists but is not current. */
   uint64_t generation = 99;
   assert(aimee_db2_project_current_generation_call(call_client, &client, 9250, 0, "demo",
                                                    &generation, NULL,
                                                    NULL) == AIMEE_MODULE_CALL_OK);
   assert(generation == 0);
   generation = 99;
   assert(aimee_db2_projection_generation_create_call(call_client, &client, 9251, 0, "demo",
                                                      &generation, NULL,
                                                      NULL) == AIMEE_MODULE_CALL_OK);
   assert(generation == 0);
   generation = 99;
   assert(aimee_db2_projection_visible_id_call(call_client, &client, 9252, 0, "demo", &generation,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(generation == 0);

   /* The conventions document is written whether or not anything is indexed,
    * which is the point of replaying it: a project with no CSS still gets the
    * whole template back, with zeros in it. */
   static char rules_doc[AIMEE_DB2_CSS_MIGRATION_RULES_DOC_RULES_DOC_MAX + 1];
   rules_doc[0] = '\0';
   assert(aimee_db2_css_migration_rules_doc_call(call_client, &client, 9253, 0, "demo", rules_doc,
                                                 sizeof(rules_doc), NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(strstr(rules_doc, "Indexed rules in exemplar: **0**") != NULL);

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
