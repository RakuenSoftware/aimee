#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <aimee/db2/module_api.h>
#include <aimee/db2/client.h>

#include "module_adapter.h"

static int cancelled;
static int cancel_after;
static int cancel_checks;
static int health_result;
static int kb_health_result;
static int initialized_value;
static int initialized_calls;
static int health_calls;
static int kb_health_calls;
static int embedding_dimension_value;
static int embedding_dimension_calls;
static int level3_count_value;
static int level3_count_calls;
static int level2_count_value;
static int level2_count_calls;
static int orphaned_l0_count_value;
static int orphaned_l0_count_calls;
static int64_t total_count_value;
static int total_count_calls;
static int session_l2_count_value;
static int session_l2_count_calls;
static int key_exists_value;
static int key_exists_calls;
static int64_t find_id_by_key_kind_value;
static int find_id_by_key_kind_calls;
static int key_exists_in_tier_pair_value;
static int key_exists_in_tier_pair_calls;
static int effectiveness_result;
static int clear_effectiveness_calls;
static int set_effectiveness_calls;
static int64_t effectiveness_memory_id;
static double effectiveness_value;
static int retention_restricted_value;
static int retention_sensitive_value;
static int retention_delete_calls;
static int demote_effectiveness_value;
static int demote_effectiveness_calls;
static double demote_effectiveness_threshold;
static int effectiveness_stats_result;
static int effectiveness_stats_calls;
static double effectiveness_stats_low_threshold;
static double effectiveness_stats_average_value;
static int effectiveness_stats_low_value;
static int effectiveness_stats_high_value;
static int list_l2_memory_ids_result;
static int list_l2_memory_ids_calls;
static int64_t list_l2_memory_ids_first;
static int count_memories_value;
static int count_recent_conflicts_value;
static int count_recent_conflicts_days;
static int health_record_calls;
static int health_record_total;
static int health_record_contradictions;
static int health_record_promotions;
static int health_record_demotions;
static int health_record_expirations;
static int pool_status_result;
static long long refused_count_value;
static int last_offered_value;
static int embedding_refusals_result;
static int postgres_status_result;
static int reembed_status_result;
static int reembed_clear_result;
static int reembed_maintenance_result;
static int reembed_maintenance_force;
static int reembed_maintenance_was;
static int reembed_maintenance_recorded;
static int reembed_maintenance_running;
static int reembed_maintenance_calls;
static const char *serving_id_value;
static int dimension_reset_result;
static int dimension_reset_calls;
static uint32_t dimension_reset_target;
static uint32_t dimension_reset_force;
static uint32_t dimension_reset_dry_run;
static aimee_db2_dimension_reset_t dimension_reset_status;
static aimee_module_call_result_t transport_result;
/* Sized for the largest reply any operation can produce, so one shared buffer
 * serves every typed-client test. */
static uint8_t transport_response[AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MAX_LEN];
static uint32_t transport_response_len;
static int transport_calls;
static int transport_expect_dimension;
static int transport_expect_level3_count;
static int transport_expect_level2_count;
static int transport_expect_orphaned_l0_count;
static int transport_expect_total_count;
static int transport_expect_session_l2_count;
static int transport_expect_key_exists;
static int transport_expect_find_id_by_key_kind;
static int transport_expect_key_exists_in_tier_pair;
static int transport_expect_effectiveness_update;
static int transport_expect_retention_enforce;
static int transport_expect_effectiveness_demote;
static int transport_expect_effectiveness_stats;
static int transport_expect_l2_memory_ids;
static int transport_expect_health_record;
static int transport_expect_pool;
static int transport_expect_refusals;
static int transport_expect_postgres;
static int transport_expect_reembed;
static int transport_expect_reembed_clear;
static int transport_expect_reembed_maintenance;
static int transport_expect_serving_id;
static int transport_expect_dimension_reset;

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   assert(invocation);
   cancel_checks++;
   return cancelled || (cancel_after > 0 && cancel_checks >= cancel_after);
}

static int health_probe(int *schema_ok, int *have_pg_trgm)
{
   health_calls++;
   if (schema_ok)
      *schema_ok = 1;
   if (have_pg_trgm)
      *have_pg_trgm = 1;
   return health_result;
}

static int kb_health_probe(int *kb_tables_ok)
{
   kb_health_calls++;
   if (kb_tables_ok)
      *kb_tables_ok = 1;
   return kb_health_result;
}

static int is_initialized(void)
{
   initialized_calls++;
   return initialized_value;
}

int db2_is_initialized(void)
{
   return is_initialized();
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
   return embedding_dimension_value;
}

static int embedding_dimension(void)
{
   embedding_dimension_calls++;
   return embedding_dimension_value;
}

int db2_memory_count_l3(void)
{
   level3_count_calls++;
   return level3_count_value;
}

static int level3_count(void)
{
   level3_count_calls++;
   return level3_count_value;
}

int db2_memory_count_l2(void)
{
   level2_count_calls++;
   return level2_count_value;
}

static int level2_count(void)
{
   level2_count_calls++;
   return level2_count_value;
}

int db2_memory_count_orphaned_l0(void)
{
   orphaned_l0_count_calls++;
   return orphaned_l0_count_value;
}

static int orphaned_l0_count(void)
{
   orphaned_l0_count_calls++;
   return orphaned_l0_count_value;
}

int64_t db2_memory_count(void)
{
   total_count_calls++;
   return total_count_value;
}

static int64_t total_count(void)
{
   total_count_calls++;
   return total_count_value;
}

int db2_memory_count_l2_for_session(const char *source_session)
{
   assert(strcmp(source_session, "session-123") == 0);
   session_l2_count_calls++;
   return session_l2_count_value;
}

static int session_l2_count(const char *source_session)
{
   assert(strcmp(source_session, "session-123") == 0);
   session_l2_count_calls++;
   return session_l2_count_value;
}

int db2_memory_key_exists(const char *key)
{
   assert(strcmp(key, "recovery:tool-a->tool-b") == 0);
   key_exists_calls++;
   return key_exists_value;
}

static int key_exists(const char *key)
{
   assert(strcmp(key, "recovery:tool-a->tool-b") == 0);
   key_exists_calls++;
   return key_exists_value;
}

int64_t db2_memory_find_id_by_key_kind(const char *key, const char *kind)
{
   assert(strcmp(key, "task:deploy-fix") == 0);
   assert(strcmp(kind, "task") == 0);
   find_id_by_key_kind_calls++;
   return find_id_by_key_kind_value;
}

static int64_t find_id_by_key_kind(const char *key, const char *kind)
{
   assert(strcmp(key, "task:deploy-fix") == 0);
   assert(strcmp(kind, "task") == 0);
   find_id_by_key_kind_calls++;
   return find_id_by_key_kind_value;
}

int db2_memory_key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b)
{
   assert(strcmp(key, "recovery:tool-a->tool-b") == 0);
   assert(strcmp(tier_a, "L3") == 0);
   assert(strcmp(tier_b, "L4") == 0);
   key_exists_in_tier_pair_calls++;
   return key_exists_in_tier_pair_value;
}

static int key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b)
{
   assert(strcmp(key, "recovery:tool-a->tool-b") == 0);
   assert(strcmp(tier_a, "L3") == 0);
   assert(strcmp(tier_b, "L4") == 0);
   key_exists_in_tier_pair_calls++;
   return key_exists_in_tier_pair_value;
}

int db2_memory_health_clear_effectiveness(int64_t memory_id)
{
   clear_effectiveness_calls++;
   effectiveness_memory_id = memory_id;
   return effectiveness_result;
}

int db2_memory_health_set_effectiveness(int64_t memory_id, double value)
{
   set_effectiveness_calls++;
   effectiveness_memory_id = memory_id;
   effectiveness_value = value;
   return effectiveness_result;
}

static int clear_effectiveness(int64_t memory_id)
{
   clear_effectiveness_calls++;
   effectiveness_memory_id = memory_id;
   return effectiveness_result;
}

static int set_effectiveness(int64_t memory_id, double value)
{
   set_effectiveness_calls++;
   effectiveness_memory_id = memory_id;
   effectiveness_value = value;
   return effectiveness_result;
}

static int retention_delete_impl(const char *sensitivity, int days)
{
   retention_delete_calls++;
   if (strcmp(sensitivity, AIMEE_DB2_RETENTION_RESTRICTED) == 0)
   {
      assert(days == (int)AIMEE_DB2_RETENTION_RESTRICTED_DAYS);
      return retention_restricted_value;
   }
   assert(strcmp(sensitivity, AIMEE_DB2_RETENTION_SENSITIVE) == 0);
   assert(days == (int)AIMEE_DB2_RETENTION_SENSITIVE_DAYS);
   return retention_sensitive_value;
}

int db2_memory_health_delete_by_sensitivity(const char *sensitivity, int days)
{
   return retention_delete_impl(sensitivity, days);
}

static int retention_delete(const char *sensitivity, int days)
{
   return retention_delete_impl(sensitivity, days);
}

int db2_memory_health_demote_low_effectiveness(double threshold)
{
   demote_effectiveness_calls++;
   demote_effectiveness_threshold = threshold;
   return demote_effectiveness_value;
}

static int demote_effectiveness(double threshold)
{
   demote_effectiveness_calls++;
   demote_effectiveness_threshold = threshold;
   return demote_effectiveness_value;
}

static int effectiveness_stats_impl(double low_threshold, double *avg_effectiveness,
                                    int *low_effectiveness, int *high_impact)
{
   effectiveness_stats_calls++;
   effectiveness_stats_low_threshold = low_threshold;
   if (avg_effectiveness)
      *avg_effectiveness = effectiveness_stats_average_value;
   if (low_effectiveness)
      *low_effectiveness = effectiveness_stats_low_value;
   if (high_impact)
      *high_impact = effectiveness_stats_high_value;
   return effectiveness_stats_result;
}

int db2_memory_health_effectiveness_stats(double low_threshold, double *avg_effectiveness,
                                          int *low_effectiveness, int *high_impact)
{
   return effectiveness_stats_impl(low_threshold, avg_effectiveness, low_effectiveness,
                                   high_impact);
}

static int effectiveness_stats(double low_threshold, double *avg_effectiveness,
                               int *low_effectiveness, int *high_impact)
{
   return effectiveness_stats_impl(low_threshold, avg_effectiveness, low_effectiveness,
                                   high_impact);
}

static int list_l2_memory_ids_impl(int64_t *out, int max)
{
   list_l2_memory_ids_calls++;
   if (list_l2_memory_ids_result < 0)
      return list_l2_memory_ids_result;
   int listed = 0;
   for (; listed < list_l2_memory_ids_result && listed < max; listed++)
      out[listed] = listed == 0 ? list_l2_memory_ids_first : (int64_t)(listed + 1) * 11;
   return listed;
}

int db2_memory_health_list_l2_memory_ids(int64_t *out, int max)
{
   return list_l2_memory_ids_impl(out, max);
}

static int list_l2_memory_ids(int64_t *out, int max)
{
   return list_l2_memory_ids_impl(out, max);
}

int db2_memory_health_count_memories(void)
{
   return count_memories_value;
}

static int count_memories(void)
{
   return count_memories_value;
}

static int count_recent_conflicts_impl(int days)
{
   count_recent_conflicts_days = days;
   return count_recent_conflicts_value;
}

int db2_memory_health_count_recent_conflicts(int days)
{
   return count_recent_conflicts_impl(days);
}

static int count_recent_conflicts(int days)
{
   return count_recent_conflicts_impl(days);
}

static void health_record_impl(int total_memories, int contradictions_detected, int promotions,
                               int demotions, int expirations)
{
   health_record_calls++;
   health_record_total = total_memories;
   health_record_contradictions = contradictions_detected;
   health_record_promotions = promotions;
   health_record_demotions = demotions;
   health_record_expirations = expirations;
}

void db2_memory_health_record(int total_memories, int contradictions_detected, int promotions,
                              int demotions, int expirations)
{
   health_record_impl(total_memories, contradictions_detected, promotions, demotions, expirations);
}

static void health_record(int total_memories, int contradictions_detected, int promotions,
                          int demotions, int expirations)
{
   health_record_impl(total_memories, contradictions_detected, promotions, demotions, expirations);
}

void db2_pool_stats(int *size, int *in_use, int *waiters, long *lease_grants, long *lease_timeouts,
                    long *stuck, long *poisoned)
{
   if (size)
      *size = 16;
   if (in_use)
      *in_use = 2;
   if (waiters)
      *waiters = 1;
   if (lease_grants)
      *lease_grants = 10;
   if (lease_timeouts)
      *lease_timeouts = 3;
   if (stuck)
      *stuck = 4;
   if (poisoned)
      *poisoned = 5;
}

static int pool_status(aimee_db2_pool_status_t *status)
{
   *status = (aimee_db2_pool_status_t){16, 2, 1, 10, 3, 4, 5};
   return pool_status_result;
}

long long db2_embedding_dim_refused_count(void)
{
   return refused_count_value;
}

int db2_embedding_dim_last_offered(void)
{
   return last_offered_value;
}

static int embedding_refusals(aimee_db2_embedding_refusals_t *status)
{
   *status = (aimee_db2_embedding_refusals_t){7, 768};
   return embedding_refusals_result;
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
   return postgres_status_result;
}

int db2_reembed_in_progress_get(int *target, long *started)
{
   if (target)
      *target = 384;
   if (started)
      *started = 1700000000;
   return 1;
}

static int reembed_status(aimee_db2_reembed_status_t *status)
{
   *status = (aimee_db2_reembed_status_t){384, 1700000000};
   return reembed_status_result;
}

int db2_reembed_in_progress_clear(void)
{
   return reembed_clear_result;
}

int db2_reembed_clear_maintenance(int force, int *was_in_progress, int *recorded, int *running)
{
   reembed_maintenance_calls++;
   reembed_maintenance_force = force;
   if (was_in_progress)
      *was_in_progress = reembed_maintenance_was;
   if (recorded)
      *recorded = reembed_maintenance_recorded;
   if (running)
      *running = reembed_maintenance_running;
   return reembed_maintenance_result;
}

const char *db2_embedder_serving_id(void)
{
   return serving_id_value;
}

int db2_probe_embedder_dim(int budget_ms, int *out)
{
   (void)budget_ms;
   if (out)
      *out = 384;
   return 0;
}

int db2_dim_change_reset(int target_dim, int force, int dry_run, void *out)
{
   (void)target_dim;
   (void)force;
   (void)dry_run;
   (void)out;
   return -1;
}

static int dimension_reset(uint32_t target_dimension, uint32_t force, uint32_t dry_run,
                           aimee_db2_dimension_reset_t *status)
{
   dimension_reset_calls++;
   dimension_reset_target = target_dimension;
   dimension_reset_force = force;
   dimension_reset_dry_run = dry_run;
   *status = dimension_reset_status;
   return dimension_reset_result;
}

static void reset(void)
{
   cancelled = 0;
   cancel_after = 0;
   cancel_checks = 0;
   health_result = 0;
   kb_health_result = 0;
   initialized_value = 1;
   initialized_calls = 0;
   health_calls = 0;
   kb_health_calls = 0;
   embedding_dimension_value = 384;
   embedding_dimension_calls = 0;
   level3_count_value = 42;
   level3_count_calls = 0;
   level2_count_value = 17;
   level2_count_calls = 0;
   orphaned_l0_count_value = 5;
   orphaned_l0_count_calls = 0;
   total_count_value = 1234567890123LL;
   total_count_calls = 0;
   session_l2_count_value = 3;
   session_l2_count_calls = 0;
   key_exists_value = 1;
   key_exists_calls = 0;
   find_id_by_key_kind_value = 42;
   find_id_by_key_kind_calls = 0;
   key_exists_in_tier_pair_value = 1;
   key_exists_in_tier_pair_calls = 0;
   effectiveness_result = 0;
   clear_effectiveness_calls = 0;
   set_effectiveness_calls = 0;
   effectiveness_memory_id = 0;
   effectiveness_value = 0.0;
   retention_restricted_value = 2;
   retention_sensitive_value = 3;
   retention_delete_calls = 0;
   demote_effectiveness_value = 2;
   demote_effectiveness_calls = 0;
   demote_effectiveness_threshold = 0.0;
   effectiveness_stats_result = 0;
   effectiveness_stats_calls = 0;
   effectiveness_stats_low_threshold = 0.0;
   effectiveness_stats_average_value = 0.5;
   effectiveness_stats_low_value = 3;
   effectiveness_stats_high_value = 1;
   list_l2_memory_ids_result = 3;
   list_l2_memory_ids_calls = 0;
   list_l2_memory_ids_first = 7;
   count_memories_value = 512;
   count_recent_conflicts_value = 6;
   count_recent_conflicts_days = 0;
   health_record_calls = 0;
   health_record_total = 0;
   health_record_contradictions = 0;
   health_record_promotions = 0;
   health_record_demotions = 0;
   health_record_expirations = 0;
   pool_status_result = 0;
   refused_count_value = 7;
   last_offered_value = 768;
   embedding_refusals_result = 0;
   postgres_status_result = 0;
   reembed_status_result = 1;
   reembed_clear_result = 0;
   reembed_maintenance_result = 0;
   reembed_maintenance_force = -1;
   reembed_maintenance_was = 1;
   reembed_maintenance_recorded = 384;
   reembed_maintenance_running = 384;
   reembed_maintenance_calls = 0;
   serving_id_value = "bekko-a25m/8721341054416418";
   dimension_reset_result = 0;
   dimension_reset_calls = 0;
   dimension_reset_target = dimension_reset_force = dimension_reset_dry_run = 99u;
   dimension_reset_status = (aimee_db2_dimension_reset_t){768, 384, 6, 0, 1234, 0, 0};
   transport_result = AIMEE_MODULE_CALL_OK;
   transport_response_len = AIMEE_DB2_RESPONSE_LEN;
   transport_calls = 0;
   transport_expect_dimension = 0;
   transport_expect_level3_count = 0;
   transport_expect_level2_count = 0;
   transport_expect_orphaned_l0_count = 0;
   transport_expect_total_count = 0;
   transport_expect_session_l2_count = 0;
   transport_expect_key_exists = 0;
   transport_expect_find_id_by_key_kind = 0;
   transport_expect_key_exists_in_tier_pair = 0;
   transport_expect_effectiveness_update = 0;
   transport_expect_retention_enforce = 0;
   transport_expect_effectiveness_demote = 0;
   transport_expect_effectiveness_stats = 0;
   transport_expect_l2_memory_ids = 0;
   transport_expect_health_record = 0;
   transport_expect_pool = 0;
   transport_expect_refusals = 0;
   transport_expect_postgres = 0;
   transport_expect_reembed = 0;
   transport_expect_reembed_clear = 0;
   transport_expect_reembed_maintenance = 0;
   transport_expect_serving_id = 0;
   transport_expect_dimension_reset = 0;
   assert(aimee_db2_health_response_encode(AIMEE_DB2_FLAG_SCHEMA | AIMEE_DB2_FLAG_KB_TABLES,
                                           transport_response, sizeof(transport_response)) == 0);
}

static aimee_module_call_result_t
transport(void *context, uint32_t event_kind, uint32_t stage_id, uint64_t trace_id,
          uint64_t deadline_ns, const void *request_body, uint32_t request_len, void *response_body,
          uint32_t response_capacity, uint32_t *response_len,
          aimee_module_cancelled_fn cancelled_fn, void *cancel_context)
{
   assert(context == (void *)0x1234);
   uint32_t expected_event =
       transport_expect_health_record             ? AIMEE_DB2_EVENT_HEALTH_RECORD
       : transport_expect_l2_memory_ids           ? AIMEE_DB2_EVENT_L2_MEMORY_IDS
       : transport_expect_effectiveness_stats     ? AIMEE_DB2_EVENT_EFFECTIVENESS_STATS
       : transport_expect_effectiveness_demote    ? AIMEE_DB2_EVENT_EFFECTIVENESS_DEMOTE
       : transport_expect_retention_enforce       ? AIMEE_DB2_EVENT_RETENTION_ENFORCE
       : transport_expect_effectiveness_update    ? AIMEE_DB2_EVENT_EFFECTIVENESS_UPDATE
       : transport_expect_key_exists_in_tier_pair ? AIMEE_DB2_EVENT_KEY_EXISTS_IN_TIER_PAIR
       : transport_expect_find_id_by_key_kind     ? AIMEE_DB2_EVENT_FIND_ID_BY_KEY_KIND
       : transport_expect_key_exists              ? AIMEE_DB2_EVENT_KEY_EXISTS
       : transport_expect_session_l2_count        ? AIMEE_DB2_EVENT_SESSION_L2_COUNT
       : transport_expect_total_count             ? AIMEE_DB2_EVENT_TOTAL_COUNT
       : transport_expect_orphaned_l0_count       ? AIMEE_DB2_EVENT_ORPHANED_L0_COUNT
       : transport_expect_level2_count            ? AIMEE_DB2_EVENT_LEVEL2_COUNT
       : transport_expect_level3_count            ? AIMEE_DB2_EVENT_LEVEL3_COUNT
       : transport_expect_dimension_reset         ? AIMEE_DB2_EVENT_DIMENSION_RESET
       : transport_expect_serving_id              ? AIMEE_DB2_EVENT_EMBEDDER_SERVING_ID
       : transport_expect_reembed_maintenance     ? AIMEE_DB2_EVENT_REEMBED_MAINT_CLEAR
       : transport_expect_reembed_clear           ? AIMEE_DB2_EVENT_REEMBED_CLEAR
       : transport_expect_reembed                 ? AIMEE_DB2_EVENT_REEMBED_STATUS
       : transport_expect_postgres                ? AIMEE_DB2_EVENT_POSTGRES_STATUS
       : transport_expect_refusals                ? AIMEE_DB2_EVENT_EMBEDDING_REFUSALS
       : transport_expect_pool                    ? AIMEE_DB2_EVENT_POOL_STATUS
       : transport_expect_dimension               ? AIMEE_DB2_EVENT_EMBEDDING_DIMENSION
                                                  : AIMEE_DB2_EVENT_HEALTH;
   uint32_t expected_stage =
       transport_expect_health_record             ? AIMEE_DB2_STAGE_HEALTH_RECORD
       : transport_expect_l2_memory_ids           ? AIMEE_DB2_STAGE_L2_MEMORY_IDS
       : transport_expect_effectiveness_stats     ? AIMEE_DB2_STAGE_EFFECTIVENESS_STATS
       : transport_expect_effectiveness_demote    ? AIMEE_DB2_STAGE_EFFECTIVENESS_DEMOTE
       : transport_expect_retention_enforce       ? AIMEE_DB2_STAGE_RETENTION_ENFORCE
       : transport_expect_effectiveness_update    ? AIMEE_DB2_STAGE_EFFECTIVENESS_UPDATE
       : transport_expect_key_exists_in_tier_pair ? AIMEE_DB2_STAGE_KEY_EXISTS_IN_TIER_PAIR
       : transport_expect_find_id_by_key_kind     ? AIMEE_DB2_STAGE_FIND_ID_BY_KEY_KIND
       : transport_expect_key_exists              ? AIMEE_DB2_STAGE_KEY_EXISTS
       : transport_expect_session_l2_count        ? AIMEE_DB2_STAGE_SESSION_L2_COUNT
       : transport_expect_total_count             ? AIMEE_DB2_STAGE_TOTAL_COUNT
       : transport_expect_orphaned_l0_count       ? AIMEE_DB2_STAGE_ORPHANED_L0_COUNT
       : transport_expect_level2_count            ? AIMEE_DB2_STAGE_LEVEL2_COUNT
       : transport_expect_level3_count            ? AIMEE_DB2_STAGE_LEVEL3_COUNT
       : transport_expect_dimension_reset         ? AIMEE_DB2_STAGE_DIMENSION_RESET
       : transport_expect_serving_id              ? AIMEE_DB2_STAGE_EMBEDDER_SERVING_ID
       : transport_expect_reembed_maintenance     ? AIMEE_DB2_STAGE_REEMBED_MAINT_CLEAR
       : transport_expect_reembed_clear           ? AIMEE_DB2_STAGE_REEMBED_CLEAR
       : transport_expect_reembed                 ? AIMEE_DB2_STAGE_REEMBED_STATUS
       : transport_expect_postgres                ? AIMEE_DB2_STAGE_POSTGRES_STATUS
       : transport_expect_refusals                ? AIMEE_DB2_STAGE_EMBEDDING_REFUSALS
       : transport_expect_pool                    ? AIMEE_DB2_STAGE_POOL_STATUS
       : transport_expect_dimension               ? AIMEE_DB2_STAGE_EMBEDDING_DIMENSION
                                                  : AIMEE_DB2_STAGE_HEALTH;
   assert(event_kind == expected_event);
   assert(stage_id == expected_stage);
   assert(trace_id == 77);
   assert(deadline_ns == 88);
   if (transport_expect_health_record)
   {
      uint32_t promotions = 0u, demotions = 0u, expirations = 0u;
      assert(aimee_db2_health_record_request_decode(request_body, request_len, &promotions,
                                                    &demotions, &expirations) == 0);
      assert(promotions == 4u && demotions == 2u && expirations == 9u);
   }
   else if (transport_expect_l2_memory_ids)
      assert(aimee_db2_l2_memory_ids_request_decode(request_body, request_len) == 0);
   else if (transport_expect_effectiveness_stats)
      assert(aimee_db2_effectiveness_stats_request_decode(request_body, request_len) == 0);
   else if (transport_expect_effectiveness_demote)
      assert(aimee_db2_effectiveness_demote_request_decode(request_body, request_len) == 0);
   else if (transport_expect_retention_enforce)
      assert(aimee_db2_retention_enforce_request_decode(request_body, request_len) == 0);
   else if (transport_expect_effectiveness_update)
   {
      uint64_t memory_id = 0;
      uint32_t has_value = 0;
      double value = 0.0;
      assert(aimee_db2_effectiveness_update_request_decode(request_body, request_len, &memory_id,
                                                           &has_value, &value) == 0);
      assert(memory_id == 42 && has_value == 1 && value == 0.75);
   }
   else if (transport_expect_key_exists_in_tier_pair)
   {
      char key[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_KEY_MAX + 1u];
      char tier_a[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_A_MAX + 1u];
      char tier_b[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_B_MAX + 1u];
      assert(aimee_db2_key_exists_in_tier_pair_request_decode(request_body, request_len, key,
                                                              sizeof(key), tier_a, sizeof(tier_a),
                                                              tier_b, sizeof(tier_b)) == 0);
      assert(strcmp(key, "recovery:tool-a->tool-b") == 0);
      assert(strcmp(tier_a, "L3") == 0);
      assert(strcmp(tier_b, "L4") == 0);
   }
   else if (transport_expect_find_id_by_key_kind)
   {
      char key[AIMEE_DB2_FIND_ID_BY_KEY_KIND_KEY_MAX + 1u];
      char kind[AIMEE_DB2_FIND_ID_BY_KEY_KIND_KIND_MAX + 1u];
      assert(aimee_db2_find_id_by_key_kind_request_decode(request_body, request_len, key,
                                                          sizeof(key), kind, sizeof(kind)) == 0);
      assert(strcmp(key, "task:deploy-fix") == 0);
      assert(strcmp(kind, "task") == 0);
   }
   else if (transport_expect_key_exists)
   {
      char key[AIMEE_DB2_KEY_EXISTS_KEY_MAX + 1u];
      assert(aimee_db2_key_exists_request_decode(request_body, request_len, key, sizeof(key)) == 0);
      assert(strcmp(key, "recovery:tool-a->tool-b") == 0);
   }
   else if (transport_expect_session_l2_count)
   {
      char source_session[AIMEE_DB2_SESSION_L2_COUNT_SESSION_MAX + 1u];
      assert(aimee_db2_session_l2_count_request_decode(request_body, request_len, source_session,
                                                       sizeof(source_session)) == 0);
      assert(strcmp(source_session, "session-123") == 0);
   }
   else if (transport_expect_total_count)
      assert(aimee_db2_total_count_request_decode(request_body, request_len) == 0);
   else if (transport_expect_orphaned_l0_count)
      assert(aimee_db2_orphaned_l0_count_request_decode(request_body, request_len) == 0);
   else if (transport_expect_level2_count)
      assert(aimee_db2_level2_count_request_decode(request_body, request_len) == 0);
   else if (transport_expect_level3_count)
      assert(aimee_db2_level3_count_request_decode(request_body, request_len) == 0);
   else if (transport_expect_dimension_reset)
   {
      uint32_t target = 99, force = 99, dry_run = 99;
      assert(aimee_db2_dimension_reset_request_decode(request_body, request_len, &target, &force,
                                                      &dry_run) == 0);
      assert(target == 384 && force == 1 && dry_run == 0);
   }
   else if (transport_expect_serving_id)
      assert(aimee_db2_embedder_serving_id_request_decode(request_body, request_len) == 0);
   else if (transport_expect_reembed_maintenance)
   {
      uint32_t force = 99;
      assert(aimee_db2_reembed_clear_maintenance_request_decode(request_body, request_len,
                                                                &force) == 0);
      assert(force == 1);
   }
   else if (transport_expect_reembed_clear)
      assert(aimee_db2_reembed_clear_request_decode(request_body, request_len) == 0);
   else if (transport_expect_reembed)
      assert(aimee_db2_reembed_status_request_decode(request_body, request_len) == 0);
   else if (transport_expect_postgres)
      assert(aimee_db2_postgres_status_request_decode(request_body, request_len) == 0);
   else if (transport_expect_refusals)
      assert(aimee_db2_embedding_refusals_request_decode(request_body, request_len) == 0);
   else if (transport_expect_pool)
      assert(aimee_db2_pool_status_request_decode(request_body, request_len) == 0);
   else if (transport_expect_dimension)
      assert(aimee_db2_embedding_dimension_request_decode(request_body, request_len) == 0);
   else
      assert(aimee_db2_health_request_decode(request_body, request_len) == 0);
   assert(cancelled_fn == NULL && cancel_context == NULL);
   transport_calls++;
   if (transport_result != AIMEE_MODULE_CALL_OK)
      return transport_result;
   if (transport_response_len > response_capacity)
      return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
   memcpy(response_body, transport_response, transport_response_len);
   *response_len = transport_response_len;
   return AIMEE_MODULE_CALL_OK;
}

static void test_wire_contract(void)
{
   uint8_t request[AIMEE_DB2_REQUEST_LEN] = {0};
   assert(aimee_db2_health_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_health_request_decode(request, sizeof(request)) == 0);
   assert(aimee_db2_health_request_encode(NULL, sizeof(request)) == -1);
   assert(aimee_db2_health_request_encode(request, sizeof(request) - 1) == -1);
   assert(aimee_db2_health_request_decode(request, sizeof(request) - 1) == -1);
   request[0] ^= 1u;
   assert(aimee_db2_health_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_health_request_encode(request, sizeof(request)) == 0);
   request[4] ^= 1u;
   assert(aimee_db2_health_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_health_request_decode(NULL, sizeof(request)) == -1);

   for (uint32_t flags = 0; flags <= AIMEE_DB2_FLAG_ALL; ++flags)
   {
      uint8_t response[AIMEE_DB2_RESPONSE_LEN] = {0};
      int schema_ok = -1, have_pg_trgm = -1, kb_tables_ok = -1;
      assert(aimee_db2_health_response_encode(flags, response, sizeof(response)) == 0);
      assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                              &kb_tables_ok) == 0);
      assert(schema_ok == !!(flags & AIMEE_DB2_FLAG_SCHEMA));
      assert(have_pg_trgm == !!(flags & AIMEE_DB2_FLAG_PG_TRGM));
      assert(kb_tables_ok == !!(flags & AIMEE_DB2_FLAG_KB_TABLES));
   }

   uint8_t response[AIMEE_DB2_RESPONSE_LEN] = {0};
   int schema_ok = 1, have_pg_trgm = 1, kb_tables_ok = 1;
   assert(aimee_db2_health_response_encode(0, NULL, sizeof(response)) == -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response) - 1) == -1);
   assert(aimee_db2_health_response_encode(AIMEE_DB2_FLAG_ALL + 1u, response, sizeof(response)) ==
          -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response)) == 0);
   assert(aimee_db2_health_response_decode(NULL, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(aimee_db2_health_response_decode(response, sizeof(response) - 1, &schema_ok,
                                           &have_pg_trgm, &kb_tables_ok) == -1);
   response[0] ^= 1u;
   assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response)) == 0);
   response[4] ^= 1u;
   assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response)) == 0);
   aimee_db2_put_u32(response + 8, AIMEE_DB2_FLAG_ALL + 1u);
   assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(aimee_db2_health_response_encode(0, response, sizeof(response)) == 0);
   aimee_db2_put_u32(response + 12, 1u);
   assert(aimee_db2_health_response_decode(response, sizeof(response), &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == -1);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);
}

static void test_body_envelope(void)
{
   uint8_t frame[AIMEE_DB2_ENVELOPE_HEADER_LEN + 3] = {0};
   const uint32_t operation = 0x01020304u;
   assert(aimee_db2_request_header_encode(operation, 5u, 3u, frame, sizeof(frame)) == 0);
   frame[AIMEE_DB2_ENVELOPE_HEADER_LEN] = 0xaa;
   frame[AIMEE_DB2_ENVELOPE_HEADER_LEN + 1] = 0xbb;
   frame[AIMEE_DB2_ENVELOPE_HEADER_LEN + 2] = 0xcc;
   aimee_db2_request_header_t request = {9, 9, 9};
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == 0);
   assert(request.operation == operation && request.flags == 5 && request.payload_len == 3);

   assert(aimee_db2_request_header_encode(0, 0, 0, frame, sizeof(frame)) == -1);
   assert(aimee_db2_request_header_encode(1, 0, 0, NULL, sizeof(frame)) == -1);
   assert(aimee_db2_request_header_encode(1, 0, 0, frame, AIMEE_DB2_ENVELOPE_HEADER_LEN - 1) == -1);
   assert(aimee_db2_request_header_decode(NULL, sizeof(frame), &request) == -1);
   assert(request.operation == 0 && request.flags == 0 && request.payload_len == 0);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), NULL) == -1);

   assert(aimee_db2_request_header_encode(operation, 5u, 3u, frame, sizeof(frame)) == 0);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame) - 1, &request) == -1);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame) + 1, &request) == -1);
   frame[0] ^= 1u;
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   frame[0] ^= 1u;
   frame[4] ^= 1u;
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   frame[4] ^= 1u;
   frame[6] ^= 1u;
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   frame[6] ^= 1u;
   aimee_db2_put_u32(frame + 8, 0);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   aimee_db2_put_u32(frame + 8, operation);
   aimee_db2_put_u32(frame + 20, 1);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);
   assert(aimee_db2_request_header_encode(operation, 5u, 3u, frame, sizeof(frame)) == 0);
   aimee_db2_put_u32(frame + 16, 4);
   assert(aimee_db2_request_header_decode(frame, sizeof(frame), &request) == -1);

   for (uint32_t result = AIMEE_DB2_RESULT_OK; result <= AIMEE_DB2_RESULT_INVALID_STATE; ++result)
   {
      assert(aimee_db2_reply_header_encode(operation, result, 3u, frame, sizeof(frame)) == 0);
      aimee_db2_reply_header_t reply = {9, 9, 9};
      assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == 0);
      assert(reply.operation == operation && reply.result == result && reply.payload_len == 3);
   }
   assert(aimee_db2_reply_header_encode(0, 0, 0, frame, sizeof(frame)) == -1);
   assert(aimee_db2_reply_header_encode(operation, AIMEE_DB2_RESULT_INVALID_STATE + 1u, 0, frame,
                                        sizeof(frame)) == -1);
   assert(aimee_db2_reply_header_encode(operation, 0, 0, NULL, sizeof(frame)) == -1);
   assert(aimee_db2_reply_header_encode(operation, 0, 0, frame,
                                        AIMEE_DB2_ENVELOPE_HEADER_LEN - 1) == -1);
   assert(aimee_db2_reply_header_encode(operation, AIMEE_DB2_RESULT_OK, 3u, frame, sizeof(frame)) ==
          0);
   aimee_db2_put_u32(frame + 12, AIMEE_DB2_RESULT_INVALID_STATE + 1u);
   aimee_db2_reply_header_t reply = {9, 9, 9};
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   assert(reply.operation == 0 && reply.result == 0 && reply.payload_len == 0);
   assert(aimee_db2_reply_header_encode(operation, AIMEE_DB2_RESULT_OK, 3u, frame, sizeof(frame)) ==
          0);
   frame[0] ^= 1u;
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   frame[0] ^= 1u;
   frame[4] ^= 1u;
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   frame[4] ^= 1u;
   frame[6] ^= 1u;
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   frame[6] ^= 1u;
   aimee_db2_put_u32(frame + 8, 0);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   aimee_db2_put_u32(frame + 8, operation);
   aimee_db2_put_u32(frame + 16, 4);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   aimee_db2_put_u32(frame + 16, 3);
   aimee_db2_put_u32(frame + 20, 1);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), &reply) == -1);
   assert(aimee_db2_reply_header_encode(operation, AIMEE_DB2_RESULT_OK, 3u, frame, sizeof(frame)) ==
          0);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame) - 1, &reply) == -1);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame) + 1, &reply) == -1);
   assert(aimee_db2_reply_header_decode(NULL, sizeof(frame), &reply) == -1);
   assert(aimee_db2_reply_header_decode(frame, sizeof(frame), NULL) == -1);
}

static void test_embedding_dimension_wire(void)
{
   uint8_t request[AIMEE_DB2_EMBEDDING_DIMENSION_REQUEST_LEN] = {0};
   assert(aimee_db2_embedding_dimension_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedding_dimension_request_decode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedding_dimension_request_encode(NULL, sizeof(request)) == -1);
   assert(aimee_db2_embedding_dimension_request_encode(request, sizeof(request) - 1) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_embedding_dimension_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_embedding_dimension_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedding_dimension_request_decode(request, sizeof(request) - 1) == -1);

   uint8_t reply[AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, result = 99, dimension = 99;
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply, sizeof(reply),
                                                     &reply_len) == 0);
   assert(reply_len == sizeof(reply));
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, &result, &dimension) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && dimension == 384);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 0, reply,
                                                     sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_EMBEDDING_DIMENSION_ERROR_LEN);
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, &result, &dimension) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && dimension == 0);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 0, reply, sizeof(reply),
                                                     &reply_len) == -1);
   assert(reply_len == 0);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK,
                                                     AIMEE_DB2_EMBEDDING_DIMENSION_MAX + 1u, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, 0, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 1, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, NULL, sizeof(reply),
                                                     &reply_len) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply, sizeof(reply),
                                                     NULL) == -1);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply,
                                                     sizeof(reply) - 1, &reply_len) == -1);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply, sizeof(reply),
                                                     &reply_len) == 0);
   aimee_db2_put_u32(reply + 8, AIMEE_DB2_OPERATION_HEALTH);
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, &result, &dimension) == -1);
   assert(result == 0 && dimension == 0);
   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, reply, sizeof(reply),
                                                     &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 0);
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, &result, &dimension) == -1);
   assert(aimee_db2_embedding_dimension_reply_decode(NULL, reply_len, &result, &dimension) == -1);
   assert(aimee_db2_embedding_dimension_reply_decode(reply, reply_len, NULL, &dimension) == -1);
}

static void test_level3_count_wire(void)
{
   uint8_t request[AIMEE_DB2_LEVEL3_COUNT_REQUEST_LEN] = {0};
   assert(aimee_db2_level3_count_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_level3_count_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_level3_count_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_level3_count_request_encode(NULL, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_LEVEL3_COUNT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, count = 99;
   assert(aimee_db2_level3_count_reply_encode(42, reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == sizeof(reply));
   assert(aimee_db2_level3_count_reply_decode(reply, reply_len, &count) == 0 && count == 42);
   assert(aimee_db2_level3_count_reply_encode(AIMEE_DB2_LEVEL3_COUNT_MAX + 1u, reply, sizeof(reply),
                                              &reply_len) == -1);
   assert(reply_len == 0);
   assert(aimee_db2_level3_count_reply_encode(42, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_level3_count_reply_encode(42, NULL, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_level3_count_reply_encode(42, reply, sizeof(reply), NULL) == -1);
   assert(aimee_db2_level3_count_reply_encode(42, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_level3_count_reply_decode(reply, reply_len, &count) == -1 && count == 0);
   assert(aimee_db2_level3_count_reply_decode(NULL, reply_len, &count) == -1);
   assert(aimee_db2_level3_count_reply_decode(reply, reply_len, NULL) == -1);
}

static void test_level2_count_wire(void)
{
   uint8_t request[AIMEE_DB2_LEVEL2_COUNT_REQUEST_LEN] = {0};
   assert(aimee_db2_level2_count_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_level2_count_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_level2_count_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_LEVEL2_COUNT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, count = 99;
   assert(aimee_db2_level2_count_reply_encode(17, reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == sizeof(reply));
   assert(aimee_db2_level2_count_reply_decode(reply, reply_len, &count) == 0 && count == 17);
   assert(aimee_db2_level2_count_reply_encode(AIMEE_DB2_LEVEL2_COUNT_MAX + 1u, reply, sizeof(reply),
                                              &reply_len) == -1);
   assert(aimee_db2_level2_count_reply_encode(17, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_level2_count_reply_encode(17, NULL, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_level2_count_reply_encode(17, reply, sizeof(reply), NULL) == -1);
   assert(aimee_db2_level2_count_reply_encode(17, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_level2_count_reply_decode(reply, reply_len, &count) == -1 && count == 0);
}

static void test_orphaned_l0_count_wire(void)
{
   uint8_t request[AIMEE_DB2_ORPHANED_L0_COUNT_REQUEST_LEN] = {0};
   assert(aimee_db2_orphaned_l0_count_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_orphaned_l0_count_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_orphaned_l0_count_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_ORPHANED_L0_COUNT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, count = 99;
   assert(aimee_db2_orphaned_l0_count_reply_encode(5, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_orphaned_l0_count_reply_decode(reply, reply_len, &count) == 0 && count == 5);
   assert(aimee_db2_orphaned_l0_count_reply_encode(AIMEE_DB2_ORPHANED_L0_COUNT_MAX + 1u, reply,
                                                   sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_orphaned_l0_count_reply_encode(5, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_orphaned_l0_count_reply_encode(5, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_orphaned_l0_count_reply_decode(reply, reply_len, &count) == -1 && count == 0);
}

static void test_total_count_wire(void)
{
   uint8_t request[AIMEE_DB2_TOTAL_COUNT_REQUEST_LEN] = {0};
   assert(aimee_db2_total_count_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_total_count_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_total_count_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_TOTAL_COUNT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   uint64_t count = 99;
   assert(aimee_db2_total_count_reply_encode(1234567890123ULL, reply, sizeof(reply), &reply_len) ==
          0);
   assert(aimee_db2_total_count_reply_decode(reply, reply_len, &count) == 0 &&
          count == 1234567890123ULL);
   assert(aimee_db2_total_count_reply_encode(AIMEE_DB2_TOTAL_COUNT_MAX + 1ULL, reply, sizeof(reply),
                                             &reply_len) == -1);
   assert(aimee_db2_total_count_reply_encode(1, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_total_count_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_total_count_reply_decode(reply, reply_len, &count) == -1 && count == 0);
}

static void test_session_l2_count_wire(void)
{
   uint8_t request[AIMEE_DB2_SESSION_L2_COUNT_REQUEST_MAX_LEN] = {0};
   uint32_t request_len = 99;
   char source_session[AIMEE_DB2_SESSION_L2_COUNT_SESSION_MAX + 1u];
   assert(aimee_db2_session_l2_count_request_encode("session-123", request, sizeof(request),
                                                    &request_len) == 0);
   assert(aimee_db2_session_l2_count_request_decode(request, request_len, source_session,
                                                    sizeof(source_session)) == 0);
   assert(strcmp(source_session, "session-123") == 0);
   assert(aimee_db2_session_l2_count_request_encode("", request, sizeof(request), &request_len) ==
          -1);
   char oversized[AIMEE_DB2_SESSION_L2_COUNT_SESSION_MAX + 2u];
   memset(oversized, 'x', sizeof(oversized) - 1u);
   oversized[sizeof(oversized) - 1u] = '\0';
   assert(aimee_db2_session_l2_count_request_encode(oversized, request, sizeof(request),
                                                    &request_len) == -1);
   assert(aimee_db2_session_l2_count_request_encode("session-123", request, sizeof(request),
                                                    &request_len) == 0);
   request[AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u] = '\0';
   assert(aimee_db2_session_l2_count_request_decode(request, request_len, source_session,
                                                    sizeof(source_session)) == -1);

   uint8_t reply[AIMEE_DB2_SESSION_L2_COUNT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, count = 99;
   assert(aimee_db2_session_l2_count_reply_encode(3, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_session_l2_count_reply_decode(reply, reply_len, &count) == 0 && count == 3);
   assert(aimee_db2_session_l2_count_reply_encode(AIMEE_DB2_SESSION_L2_COUNT_MAX + 1u, reply,
                                                  sizeof(reply), &reply_len) == -1);
}

static void test_key_exists_wire(void)
{
   uint8_t request[AIMEE_DB2_KEY_EXISTS_REQUEST_MAX_LEN] = {0};
   uint32_t request_len = 99;
   char key[AIMEE_DB2_KEY_EXISTS_KEY_MAX + 1u];
   assert(aimee_db2_key_exists_request_encode("recovery:tool-a->tool-b", request, sizeof(request),
                                              &request_len) == 0);
   assert(aimee_db2_key_exists_request_decode(request, request_len, key, sizeof(key)) == 0);
   assert(strcmp(key, "recovery:tool-a->tool-b") == 0);
   assert(aimee_db2_key_exists_request_encode("", request, sizeof(request), &request_len) == -1);
   char oversized[AIMEE_DB2_KEY_EXISTS_KEY_MAX + 2u];
   memset(oversized, 'x', sizeof(oversized) - 1u);
   oversized[sizeof(oversized) - 1u] = '\0';
   assert(aimee_db2_key_exists_request_encode(oversized, request, sizeof(request), &request_len) ==
          -1);
   assert(aimee_db2_key_exists_request_encode("recovery:tool-a->tool-b", request, sizeof(request),
                                              &request_len) == 0);
   request[AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u] = '\0';
   assert(aimee_db2_key_exists_request_decode(request, request_len, key, sizeof(key)) == -1);

   uint8_t reply[AIMEE_DB2_KEY_EXISTS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, exists = 99;
   assert(aimee_db2_key_exists_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_key_exists_reply_decode(reply, reply_len, &exists) == 0 && exists == 1);
   assert(aimee_db2_key_exists_reply_encode(AIMEE_DB2_KEY_EXISTS_MAX + 1u, reply, sizeof(reply),
                                            &reply_len) == -1);
}

static void test_find_id_by_key_kind_wire(void)
{
   uint8_t request[AIMEE_DB2_FIND_ID_BY_KEY_KIND_REQUEST_MAX_LEN] = {0};
   uint32_t request_len = 99;
   char key[AIMEE_DB2_FIND_ID_BY_KEY_KIND_KEY_MAX + 1u];
   char kind[AIMEE_DB2_FIND_ID_BY_KEY_KIND_KIND_MAX + 1u];
   assert(aimee_db2_find_id_by_key_kind_request_encode("task:deploy-fix", "task", request,
                                                       sizeof(request), &request_len) == 0);
   assert(aimee_db2_find_id_by_key_kind_request_decode(request, request_len, key, sizeof(key), kind,
                                                       sizeof(kind)) == 0);
   assert(strcmp(key, "task:deploy-fix") == 0 && strcmp(kind, "task") == 0);
   assert(aimee_db2_find_id_by_key_kind_request_encode("", "task", request, sizeof(request),
                                                       &request_len) == -1);
   assert(aimee_db2_find_id_by_key_kind_request_encode("task:deploy-fix", "", request,
                                                       sizeof(request), &request_len) == -1);

   char oversized_key[AIMEE_DB2_FIND_ID_BY_KEY_KIND_KEY_MAX + 2u];
   memset(oversized_key, 'x', sizeof(oversized_key) - 1u);
   oversized_key[sizeof(oversized_key) - 1u] = '\0';
   assert(aimee_db2_find_id_by_key_kind_request_encode(oversized_key, "task", request,
                                                       sizeof(request), &request_len) == -1);
   char oversized_kind[AIMEE_DB2_FIND_ID_BY_KEY_KIND_KIND_MAX + 2u];
   memset(oversized_kind, 'x', sizeof(oversized_kind) - 1u);
   oversized_kind[sizeof(oversized_kind) - 1u] = '\0';
   assert(aimee_db2_find_id_by_key_kind_request_encode("task:deploy-fix", oversized_kind, request,
                                                       sizeof(request), &request_len) == -1);

   assert(aimee_db2_find_id_by_key_kind_request_encode("task:deploy-fix", "task", request,
                                                       sizeof(request), &request_len) == 0);
   request[AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u] = '\0';
   assert(aimee_db2_find_id_by_key_kind_request_decode(request, request_len, key, sizeof(key), kind,
                                                       sizeof(kind)) == -1);
   assert(aimee_db2_find_id_by_key_kind_request_encode("task:deploy-fix", "task", request,
                                                       sizeof(request), &request_len) == 0);
   request[request_len - 1u] = '\0';
   assert(aimee_db2_find_id_by_key_kind_request_decode(request, request_len, key, sizeof(key), kind,
                                                       sizeof(kind)) == -1);

   uint8_t reply[AIMEE_DB2_FIND_ID_BY_KEY_KIND_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, found = 99;
   uint64_t id = 99;
   assert(aimee_db2_find_id_by_key_kind_reply_encode(1, 42, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_find_id_by_key_kind_reply_decode(reply, reply_len, &found, &id) == 0 &&
          found == 1 && id == 42);
   assert(aimee_db2_find_id_by_key_kind_reply_encode(0, 0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_find_id_by_key_kind_reply_decode(reply, reply_len, &found, &id) == 0 &&
          found == 0 && id == 0);
   assert(aimee_db2_find_id_by_key_kind_reply_encode(0, 42, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(aimee_db2_find_id_by_key_kind_reply_encode(1, 0, reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_find_id_by_key_kind_reply_encode(2, 42, reply, sizeof(reply), &reply_len) ==
          -1);
}

static void test_key_exists_in_tier_pair_wire(void)
{
   uint8_t request[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_REQUEST_MAX_LEN] = {0};
   uint32_t request_len = 99;
   char key[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_KEY_MAX + 1u];
   char tier_a[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_A_MAX + 1u];
   char tier_b[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_B_MAX + 1u];
   assert(aimee_db2_key_exists_in_tier_pair_request_encode(
              "recovery:tool-a->tool-b", "L3", "L4", request, sizeof(request), &request_len) == 0);
   assert(aimee_db2_key_exists_in_tier_pair_request_decode(request, request_len, key, sizeof(key),
                                                           tier_a, sizeof(tier_a), tier_b,
                                                           sizeof(tier_b)) == 0);
   assert(strcmp(key, "recovery:tool-a->tool-b") == 0);
   assert(strcmp(tier_a, "L3") == 0 && strcmp(tier_b, "L4") == 0);
   assert(aimee_db2_key_exists_in_tier_pair_request_encode("", "L3", "L4", request, sizeof(request),
                                                           &request_len) == -1);
   assert(aimee_db2_key_exists_in_tier_pair_request_encode(
              "recovery:tool-a->tool-b", "", "L4", request, sizeof(request), &request_len) == -1);
   assert(aimee_db2_key_exists_in_tier_pair_request_encode(
              "recovery:tool-a->tool-b", "L3", "", request, sizeof(request), &request_len) == -1);
   assert(aimee_db2_key_exists_in_tier_pair_request_encode(
              "recovery:tool-a->tool-b", "L3", "L4", request, sizeof(request), &request_len) == 0);
   request[request_len - 1u] = '\0';
   assert(aimee_db2_key_exists_in_tier_pair_request_decode(request, request_len, key, sizeof(key),
                                                           tier_a, sizeof(tier_a), tier_b,
                                                           sizeof(tier_b)) == -1);

   uint8_t reply[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, exists = 99;
   assert(aimee_db2_key_exists_in_tier_pair_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_key_exists_in_tier_pair_reply_decode(reply, reply_len, &exists) == 0 &&
          exists == 1);
   assert(aimee_db2_key_exists_in_tier_pair_reply_encode(2, reply, sizeof(reply), &reply_len) ==
          -1);
}

static void test_effectiveness_update_wire(void)
{
   uint8_t request[AIMEE_DB2_EFFECTIVENESS_UPDATE_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   uint32_t has_value = 99;
   double value = 99.0;
   assert(aimee_db2_effectiveness_update_request_encode(42, 1, 0.75, request, sizeof(request)) ==
          0);
   assert(aimee_db2_effectiveness_update_request_decode(request, sizeof(request), &memory_id,
                                                        &has_value, &value) == 0);
   assert(memory_id == 42 && has_value == 1 && value == 0.75);
   assert(aimee_db2_effectiveness_update_request_encode(42, 0, 0.0, request, sizeof(request)) == 0);
   assert(aimee_db2_effectiveness_update_request_decode(request, sizeof(request), &memory_id,
                                                        &has_value, &value) == 0);
   assert(memory_id == 42 && has_value == 0 && value == 0.0);
   assert(aimee_db2_effectiveness_update_request_encode(0, 1, 0.75, request, sizeof(request)) ==
          -1);
   assert(aimee_db2_effectiveness_update_request_encode(42, 2, 0.75, request, sizeof(request)) ==
          -1);
   assert(aimee_db2_effectiveness_update_request_encode(42, 0, 0.75, request, sizeof(request)) ==
          -1);

   uint64_t nan_bits = 0x7ff8000000000042ULL;
   double nan_value = 0.0;
   memcpy(&nan_value, &nan_bits, sizeof(nan_bits));
   assert(aimee_db2_effectiveness_update_request_encode(42, 1, nan_value, request,
                                                        sizeof(request)) == 0);
   assert(aimee_db2_effectiveness_update_request_decode(request, sizeof(request), &memory_id,
                                                        &has_value, &value) == 0);
   uint64_t decoded_bits = 0;
   memcpy(&decoded_bits, &value, sizeof(decoded_bits));
   assert(decoded_bits == nan_bits);

   uint8_t reply[AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN] = {0};
   uint32_t result = 99;
   assert(aimee_db2_effectiveness_update_reply_encode(AIMEE_DB2_RESULT_OK, reply, sizeof(reply)) ==
          0);
   assert(aimee_db2_effectiveness_update_reply_decode(reply, sizeof(reply), &result) == 0 &&
          result == AIMEE_DB2_RESULT_OK);
   assert(aimee_db2_effectiveness_update_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, reply,
                                                      sizeof(reply)) == 0);
   assert(aimee_db2_effectiveness_update_reply_decode(reply, sizeof(reply), &result) == 0 &&
          result == AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_effectiveness_update_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, reply,
                                                      sizeof(reply)) == -1);
}

static void test_retention_enforce_wire(void)
{
   assert(strcmp(AIMEE_DB2_RETENTION_RESTRICTED, "restricted") == 0);
   assert(AIMEE_DB2_RETENTION_RESTRICTED_DAYS == 7u);
   assert(strcmp(AIMEE_DB2_RETENTION_SENSITIVE, "sensitive") == 0);
   assert(AIMEE_DB2_RETENTION_SENSITIVE_DAYS == 90u);
   uint8_t request[AIMEE_DB2_RETENTION_ENFORCE_REQUEST_LEN] = {0};
   assert(aimee_db2_retention_enforce_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_retention_enforce_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_retention_enforce_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_RETENTION_ENFORCE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, deleted_count = 99;
   assert(aimee_db2_retention_enforce_reply_encode(5, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_retention_enforce_reply_decode(reply, reply_len, &deleted_count) == 0 &&
          deleted_count == 5);
   assert(aimee_db2_retention_enforce_reply_encode(AIMEE_DB2_RETENTION_ENFORCE_MAX + 1u, reply,
                                                   sizeof(reply), &reply_len) == -1);
   assert(reply_len == 0);
}

static void test_effectiveness_demote_wire(void)
{
   uint64_t threshold_bits = 0;
   double threshold = AIMEE_DB2_EFFECTIVENESS_DEMOTE_THRESHOLD;
   memcpy(&threshold_bits, &threshold, sizeof(threshold_bits));
   assert(threshold_bits == 0x3fd3333333333333ULL);
   uint8_t request[AIMEE_DB2_EFFECTIVENESS_DEMOTE_REQUEST_LEN] = {0};
   assert(aimee_db2_effectiveness_demote_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_effectiveness_demote_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_effectiveness_demote_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_EFFECTIVENESS_DEMOTE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, demoted_count = 99;
   assert(aimee_db2_effectiveness_demote_reply_encode(2, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_effectiveness_demote_reply_decode(reply, reply_len, &demoted_count) == 0 &&
          demoted_count == 2);
   assert(aimee_db2_effectiveness_demote_reply_encode(AIMEE_DB2_EFFECTIVENESS_DEMOTE_MAX + 1u,
                                                      reply, sizeof(reply), &reply_len) == -1);
   assert(reply_len == 0);
}

static void test_effectiveness_stats_wire(void)
{
   uint64_t threshold_bits = 0;
   double threshold = AIMEE_DB2_EFFECTIVENESS_STATS_LOW_THRESHOLD;
   memcpy(&threshold_bits, &threshold, sizeof(threshold_bits));
   assert(threshold_bits == 0x3fd3333333333333ULL);
   uint8_t request[AIMEE_DB2_EFFECTIVENESS_STATS_REQUEST_LEN] = {0};
   assert(aimee_db2_effectiveness_stats_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_effectiveness_stats_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_effectiveness_stats_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   aimee_db2_effectiveness_stats_t stats = {
       .avg_effectiveness = 0.5, .low_effectiveness_count = 3, .high_impact_count = 1};
   aimee_db2_effectiveness_stats_t decoded = {0};
   assert(aimee_db2_effectiveness_stats_reply_encode(&stats, reply, sizeof(reply), &reply_len) ==
          0);
   assert(reply_len == AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN);
   assert(aimee_db2_effectiveness_stats_reply_decode(reply, reply_len, &decoded) == 0 &&
          decoded.avg_effectiveness == 0.5 && decoded.low_effectiveness_count == 3 &&
          decoded.high_impact_count == 1);

   /* The average is a probability: both bounds hold, and NaN is rejected. */
   aimee_db2_effectiveness_stats_t bounds = stats;
   bounds.avg_effectiveness = 0.0;
   assert(aimee_db2_effectiveness_stats_reply_encode(&bounds, reply, sizeof(reply), &reply_len) ==
          0);
   bounds.avg_effectiveness = 1.0;
   assert(aimee_db2_effectiveness_stats_reply_encode(&bounds, reply, sizeof(reply), &reply_len) ==
          0);
   bounds.avg_effectiveness = -0.5;
   assert(aimee_db2_effectiveness_stats_reply_encode(&bounds, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(reply_len == 0);
   bounds.avg_effectiveness = 1.5;
   assert(aimee_db2_effectiveness_stats_reply_encode(&bounds, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(reply_len == 0);

   aimee_db2_effectiveness_stats_t overflow = stats;
   overflow.low_effectiveness_count = AIMEE_DB2_EFFECTIVENESS_STATS_LOW_MAX + 1u;
   assert(aimee_db2_effectiveness_stats_reply_encode(&overflow, reply, sizeof(reply), &reply_len) ==
          -1);
   overflow = stats;
   overflow.high_impact_count = AIMEE_DB2_EFFECTIVENESS_STATS_HIGH_MAX + 1u;
   assert(aimee_db2_effectiveness_stats_reply_encode(&overflow, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(reply_len == 0);

   /* A NaN average on the wire must not survive decoding. */
   assert(aimee_db2_effectiveness_stats_reply_encode(&stats, reply, sizeof(reply), &reply_len) ==
          0);
   aimee_db2_put_u64(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 0x7ff8000000000000ULL);
   assert(aimee_db2_effectiveness_stats_reply_decode(reply, reply_len, &decoded) == -1);
   aimee_db2_put_u64(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 0x3ff0000000000001ULL);
   assert(aimee_db2_effectiveness_stats_reply_decode(reply, reply_len, &decoded) == -1);
}

static void test_l2_memory_ids_wire(void)
{
   uint8_t request[AIMEE_DB2_L2_MEMORY_IDS_REQUEST_LEN] = {0};
   assert(aimee_db2_l2_memory_ids_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_l2_memory_ids_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_l2_memory_ids_request_decode(request, sizeof(request)) == -1);

   static uint8_t reply[AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MAX_LEN];
   static uint64_t decoded[AIMEE_DB2_L2_MEMORY_IDS_MAX];
   const uint64_t ids[] = {7, 19, AIMEE_DB2_L2_MEMORY_ID_MAX};
   uint32_t reply_len = 99, count = 99;
   assert(aimee_db2_l2_memory_ids_reply_encode(ids, 3u, reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u + 3u * 8u);
   assert(aimee_db2_l2_memory_ids_reply_decode(reply, reply_len, decoded,
                                               AIMEE_DB2_L2_MEMORY_IDS_MAX, &count) == 0);
   assert(count == 3 && decoded[0] == 7 && decoded[1] == 19 &&
          decoded[2] == AIMEE_DB2_L2_MEMORY_ID_MAX);

   /* An empty list is the shortest legal reply, not an error. */
   assert(aimee_db2_l2_memory_ids_reply_encode(NULL, 0u, reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MIN_LEN);
   assert(aimee_db2_l2_memory_ids_reply_decode(reply, reply_len, decoded,
                                               AIMEE_DB2_L2_MEMORY_IDS_MAX, &count) == 0 &&
          count == 0);

   /* Identifiers are positive and bounded on both sides of the wire. */
   const uint64_t zero_id[] = {0};
   assert(aimee_db2_l2_memory_ids_reply_encode(zero_id, 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   const uint64_t huge_id[] = {(uint64_t)AIMEE_DB2_L2_MEMORY_ID_MAX + 1u};
   assert(aimee_db2_l2_memory_ids_reply_encode(huge_id, 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(reply_len == 0);

   /* The declared bound holds, and a short caller buffer is refused rather
    * than overrun. */
   assert(aimee_db2_l2_memory_ids_reply_encode(ids, AIMEE_DB2_L2_MEMORY_IDS_MAX + 1u, reply,
                                               sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_l2_memory_ids_reply_encode(ids, 3u, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_l2_memory_ids_reply_decode(reply, reply_len, decoded, 2u, &count) == -1 &&
          count == 0);

   /* A count that disagrees with the payload length must not be trusted. */
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 2u);
   assert(aimee_db2_l2_memory_ids_reply_decode(reply, reply_len, decoded,
                                               AIMEE_DB2_L2_MEMORY_IDS_MAX, &count) == -1);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 4u);
   assert(aimee_db2_l2_memory_ids_reply_decode(reply, reply_len, decoded,
                                               AIMEE_DB2_L2_MEMORY_IDS_MAX, &count) == -1);
}

static void test_health_record_wire(void)
{
   uint8_t request[AIMEE_DB2_HEALTH_RECORD_REQUEST_LEN] = {0};
   uint32_t promotions = 99, demotions = 99, expirations = 99;
   assert(aimee_db2_health_record_request_encode(4u, 2u, 9u, request, sizeof(request)) == 0);
   assert(aimee_db2_health_record_request_decode(request, sizeof(request), &promotions, &demotions,
                                                 &expirations) == 0);
   assert(promotions == 4u && demotions == 2u && expirations == 9u);

   /* Every counter is bounded on both sides of the wire. */
   assert(aimee_db2_health_record_request_encode(AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX + 1u, 2u, 9u,
                                                 request, sizeof(request)) == -1);
   assert(aimee_db2_health_record_request_encode(4u, AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX + 1u, 9u,
                                                 request, sizeof(request)) == -1);
   assert(aimee_db2_health_record_request_encode(4u, 2u, AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX + 1u,
                                                 request, sizeof(request)) == -1);
   assert(aimee_db2_health_record_request_encode(4u, 2u, 9u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + AIMEE_DB2_ENVELOPE_HEADER_LEN, 0x80000000u);
   assert(aimee_db2_health_record_request_decode(request, sizeof(request), &promotions, &demotions,
                                                 &expirations) == -1);
   assert(promotions == 0u && demotions == 0u && expirations == 0u);
   assert(aimee_db2_health_record_request_encode(4u, 2u, 9u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_health_record_request_decode(request, sizeof(request), &promotions, &demotions,
                                                 &expirations) == -1);

   uint8_t reply[AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN] = {0};
   assert(aimee_db2_health_record_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_health_record_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_health_record_reply_encode(reply, sizeof(reply) - 1) == -1);
}

static void test_pool_status_wire(void)
{
   uint8_t request[AIMEE_DB2_POOL_STATUS_REQUEST_LEN] = {0};
   assert(aimee_db2_pool_status_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_pool_status_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_pool_status_request_decode(request, sizeof(request)) == -1);

   const aimee_db2_pool_status_t expected = {16, 2, 1, 10, 3, 4, 5};
   uint8_t reply[AIMEE_DB2_POOL_STATUS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, result = 99;
   aimee_db2_pool_status_t decoded = {0};
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply, sizeof(reply),
                                             &reply_len) == 0);
   assert(reply_len == sizeof(reply));
   assert(aimee_db2_pool_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.size == 16 && decoded.in_use == 2 &&
          decoded.waiters == 1 && decoded.lease_grants == 10 && decoded.lease_timeouts == 3 &&
          decoded.stuck == 4 && decoded.poisoned == 5);

   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                             sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_POOL_STATUS_ERROR_LEN);
   assert(aimee_db2_pool_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded.size == 0);

   aimee_db2_pool_status_t bad = expected;
   bad.size = 0;
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                             &reply_len) == -1);
   bad = expected;
   bad.in_use = bad.size + 1;
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                             &reply_len) == -1);
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, NULL, reply, sizeof(reply),
                                             &reply_len) == -1);
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, &expected, reply,
                                             sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply,
                                             sizeof(reply) - 1, &reply_len) == -1);

   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply, sizeof(reply),
                                             &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 0);
   assert(aimee_db2_pool_status_reply_decode(reply, reply_len, &result, &decoded) == -1);
   assert(result == 0 && decoded.size == 0);
   assert(aimee_db2_pool_status_reply_decode(NULL, reply_len, &result, &decoded) == -1);
}

static void test_embedding_refusals_wire(void)
{
   uint8_t request[AIMEE_DB2_EMBEDDING_REFUSALS_REQUEST_LEN] = {0};
   assert(aimee_db2_embedding_refusals_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedding_refusals_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1);
   assert(aimee_db2_embedding_refusals_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN] = {0};
   const aimee_db2_embedding_refusals_t expected = {7, 768};
   aimee_db2_embedding_refusals_t decoded = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply,
                                                    sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_embedding_refusals_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.refused_count == 7 &&
          decoded.last_offered == 768);
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                                    sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_embedding_refusals_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded.refused_count == 0);

   aimee_db2_embedding_refusals_t bad = {7, 0};
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                    &reply_len) == -1);
   bad = (aimee_db2_embedding_refusals_t){0, 768};
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                    &reply_len) == -1);
}

static void test_postgres_status_wire(void)
{
   uint8_t request[AIMEE_DB2_POSTGRES_STATUS_REQUEST_LEN] = {0};
   assert(aimee_db2_postgres_status_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_postgres_status_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1);
   assert(aimee_db2_postgres_status_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN] = {0};
   const aimee_db2_postgres_status_t expected = {15, 12, 100, 1, 1048576};
   aimee_db2_postgres_status_t decoded = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply,
                                                 sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_postgres_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.available == 15 &&
          decoded.active_connections == 12 && decoded.max_connections == 100 &&
          decoded.is_replica == 1 && decoded.replica_lag_bytes == 1048576);

   const aimee_db2_postgres_status_t partial = {
       AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE | AIMEE_DB2_POSTGRES_AVAILABLE_MAX, 12, 100, 0, 0};
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &partial, reply,
                                                 sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_postgres_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(decoded.available == 3 && decoded.is_replica == 0 && decoded.replica_lag_bytes == 0);

   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                                 sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_postgres_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded.available == 0);

   aimee_db2_postgres_status_t bad = expected;
   bad.available = 16;
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                 &reply_len) == -1);
   bad = expected;
   bad.available &= ~AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE;
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                 &reply_len) == -1);
   bad = expected;
   bad.is_replica = 0;
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                 &reply_len) == -1);
}

static void test_reembed_status_wire(void)
{
   uint8_t request[AIMEE_DB2_REEMBED_STATUS_REQUEST_LEN] = {0};
   assert(aimee_db2_reembed_status_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_reembed_status_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1);
   assert(aimee_db2_reembed_status_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN] = {0};
   const aimee_db2_reembed_status_t expected = {384, 1700000000};
   aimee_db2_reembed_status_t decoded = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, reply,
                                                sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.target_dimension == 384 &&
          decoded.started_epoch == 1700000000);
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, NULL, reply,
                                                sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND && decoded.target_dimension == 0);
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                                sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_status_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);

   aimee_db2_reembed_status_t bad = {0, 1700000000};
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                &reply_len) == -1);
   bad = (aimee_db2_reembed_status_t){384, 0};
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                &reply_len) == -1);
}

static void test_reembed_clear_wire(void)
{
   uint8_t request[AIMEE_DB2_REEMBED_CLEAR_REQUEST_LEN] = {0};
   assert(aimee_db2_reembed_clear_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_reembed_clear_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1);
   assert(aimee_db2_reembed_clear_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_reembed_clear_reply_encode(AIMEE_DB2_RESULT_OK, reply, sizeof(reply),
                                               &reply_len) == 0);
   assert(aimee_db2_reembed_clear_reply_decode(reply, reply_len, &result) == 0);
   assert(result == AIMEE_DB2_RESULT_OK);
   assert(aimee_db2_reembed_clear_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, reply, sizeof(reply),
                                               &reply_len) == 0);
   assert(aimee_db2_reembed_clear_reply_decode(reply, reply_len, &result) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_reembed_clear_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, reply, sizeof(reply),
                                               &reply_len) == -1);
}

static void test_reembed_clear_maintenance_wire(void)
{
   uint8_t request[AIMEE_DB2_REEMBED_MAINT_CLEAR_REQUEST_LEN] = {0};
   uint32_t force = 99;
   assert(aimee_db2_reembed_clear_maintenance_request_encode(1, request, sizeof(request)) == 0);
   assert(aimee_db2_reembed_clear_maintenance_request_decode(request, sizeof(request), &force) ==
          0);
   assert(force == 1);
   assert(aimee_db2_reembed_clear_maintenance_request_encode(2, request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + AIMEE_DB2_ENVELOPE_HEADER_LEN, 2);
   assert(aimee_db2_reembed_clear_maintenance_request_decode(request, sizeof(request), &force) ==
          -1);
   assert(force == 0);

   uint8_t reply[AIMEE_DB2_REEMBED_MAINT_CLEAR_RESPONSE_LEN] = {0};
   const aimee_db2_reembed_clear_maintenance_t ok = {1, 384, 384};
   const aimee_db2_reembed_clear_maintenance_t conflict = {1, 768, 384};
   aimee_db2_reembed_clear_maintenance_t decoded = {0};
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_reembed_clear_maintenance_reply_encode(AIMEE_DB2_RESULT_OK, &ok, reply,
                                                           sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_clear_maintenance_reply_decode(reply, reply_len, &result, &decoded) ==
          0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded.was_in_progress == 1 &&
          decoded.recorded_dimension == 384 && decoded.running_dimension == 384);
   assert(aimee_db2_reembed_clear_maintenance_reply_encode(AIMEE_DB2_RESULT_CONFLICT, &conflict,
                                                           reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_clear_maintenance_reply_decode(reply, reply_len, &result, &decoded) ==
          0);
   assert(result == AIMEE_DB2_RESULT_CONFLICT && decoded.recorded_dimension == 768);
   assert(aimee_db2_reembed_clear_maintenance_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL,
                                                           reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reembed_clear_maintenance_reply_decode(reply, reply_len, &result, &decoded) ==
          0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded.running_dimension == 0);

   assert(aimee_db2_reembed_clear_maintenance_reply_encode(AIMEE_DB2_RESULT_CONFLICT, &ok, reply,
                                                           sizeof(reply), &reply_len) == -1);
   aimee_db2_reembed_clear_maintenance_t bad = ok;
   bad.running_dimension = 0;
   assert(aimee_db2_reembed_clear_maintenance_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply,
                                                           sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_reembed_clear_maintenance_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, &ok,
                                                           reply, sizeof(reply), &reply_len) == -1);
}

static void test_embedder_serving_id_wire(void)
{
   uint8_t request[AIMEE_DB2_EMBEDDER_SERVING_ID_REQUEST_LEN] = {0};
   assert(aimee_db2_embedder_serving_id_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_embedder_serving_id_request_decode(request, sizeof(request)) == 0);
   request[12] = 1;
   assert(aimee_db2_embedder_serving_id_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MAX_LEN] = {0};
   uint32_t reply_len = 99, result = 99;
   char decoded[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1] = "stale";
   const char *expected = "bekko-a25m/8721341054416418";
   assert(aimee_db2_embedder_serving_id_reply_encode(AIMEE_DB2_RESULT_OK, expected, reply,
                                                     sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MIN_LEN + strlen(expected));
   assert(aimee_db2_embedder_serving_id_reply_decode(reply, reply_len, &result, decoded,
                                                     sizeof(decoded)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && strcmp(decoded, expected) == 0);

   assert(aimee_db2_embedder_serving_id_reply_encode(AIMEE_DB2_RESULT_OK, "", reply, sizeof(reply),
                                                     &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MIN_LEN);
   assert(aimee_db2_embedder_serving_id_reply_decode(reply, reply_len, &result, decoded,
                                                     sizeof(decoded)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && decoded[0] == '\0');
   assert(aimee_db2_embedder_serving_id_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                                     sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_embedder_serving_id_reply_decode(reply, reply_len, &result, decoded,
                                                     sizeof(decoded)) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded[0] == '\0');

   char maximum[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1];
   memset(maximum, 'x', sizeof(maximum) - 1);
   maximum[sizeof(maximum) - 1] = '\0';
   assert(aimee_db2_embedder_serving_id_reply_encode(AIMEE_DB2_RESULT_OK, maximum, reply,
                                                     sizeof(reply), &reply_len) == 0);
   assert(reply_len == sizeof(reply));
   char too_long[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 2];
   memset(too_long, 'x', sizeof(too_long) - 1);
   too_long[sizeof(too_long) - 1] = '\0';
   assert(aimee_db2_embedder_serving_id_reply_encode(AIMEE_DB2_RESULT_OK, too_long, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_embedder_serving_id_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, expected,
                                                     reply, sizeof(reply), &reply_len) == -1);
}

static void test_dimension_reset_wire(void)
{
   uint8_t request[AIMEE_DB2_DIMENSION_RESET_REQUEST_LEN] = {0};
   uint32_t target = 99, force = 99, dry_run = 99;
   assert(aimee_db2_dimension_reset_request_encode(384, 1, 1, request, sizeof(request)) == 0);
   assert(aimee_db2_dimension_reset_request_decode(request, sizeof(request), &target, &force,
                                                   &dry_run) == 0);
   assert(target == 384 && force == 1 && dry_run == 1);
   assert(aimee_db2_dimension_reset_request_encode(0, 0, 0, request, sizeof(request)) == -1);
   assert(aimee_db2_dimension_reset_request_encode(4001, 0, 0, request, sizeof(request)) == -1);
   assert(aimee_db2_dimension_reset_request_encode(384, 2, 0, request, sizeof(request)) == -1);
   assert(aimee_db2_dimension_reset_request_encode(384, 0, 2, request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_DIMENSION_RESET_RESPONSE_LEN] = {0};
   const aimee_db2_dimension_reset_t expected = {768, 384, 6, 0, 1234, -1, 7};
   aimee_db2_dimension_reset_t decoded = {0};
   uint32_t reply_len = 99, result = 99;
   for (uint32_t code = AIMEE_DB2_RESULT_OK; code <= AIMEE_DB2_RESULT_DENIED; code++)
   {
      if (code == AIMEE_DB2_RESULT_NOT_FOUND)
         continue;
      assert(aimee_db2_dimension_reset_reply_encode(code, &expected, reply, sizeof(reply),
                                                    &reply_len) == 0);
      assert(aimee_db2_dimension_reset_reply_decode(reply, reply_len, &result, &decoded) == 0);
      assert(result == code && decoded.recorded_dimension == 768 &&
             decoded.target_dimension == 384 && decoded.tables_discovered == 6 &&
             decoded.tables_dropped == 0 && decoded.rows_cleared == 1234 &&
             decoded.curator_requeued == -1 && decoded.evidence_requeued == 7);
   }
   assert(aimee_db2_dimension_reset_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, NULL, reply,
                                                 sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_dimension_reset_reply_decode(reply, reply_len, &result, &decoded) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && decoded.target_dimension == 0);
   aimee_db2_dimension_reset_t bad = expected;
   bad.tables_dropped = 7;
   assert(aimee_db2_dimension_reset_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                 &reply_len) == -1);
   bad = expected;
   bad.curator_requeued = -2;
   assert(aimee_db2_dimension_reset_reply_encode(AIMEE_DB2_RESULT_OK, &bad, reply, sizeof(reply),
                                                 &reply_len) == -1);
}

static aimee_module_status_t invoke(const aimee_db2_module_backend_t *backend,
                                    aimee_module_invocation_t *invocation, uint8_t *request,
                                    uint32_t request_len, uint8_t *response,
                                    uint32_t response_capacity, uint32_t *response_len)
{
   return aimee_module_handler(invocation, request, request_len, response, response_capacity,
                               response_len, (void *)backend);
}

static void test_handler_success_and_failures(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .is_initialized = is_initialized,
       .health_probe = health_probe,
       .kb_health_probe = kb_health_probe,
   };
   uint8_t request[AIMEE_DB2_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_HEALTH};
   assert(aimee_db2_health_request_encode(request, sizeof(request)) == 0);
   memset(response, 0xa5, sizeof(response));

   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(response_len == AIMEE_DB2_RESPONSE_LEN);
   assert(initialized_calls == 1 && health_calls == 1 && kb_health_calls == 1);
   int schema_ok = 0, have_pg_trgm = 0, kb_tables_ok = 0;
   assert(aimee_db2_health_response_decode(response, response_len, &schema_ok, &have_pg_trgm,
                                           &kb_tables_ok) == 0);
   assert(schema_ok && have_pg_trgm && kb_tables_ok);

   response_len = 99;
   invocation.stage_id++;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(response_len == 0);
   invocation.stage_id = AIMEE_DB2_STAGE_HEALTH;
   assert(invoke(&backend, &invocation, request, sizeof(request) - 1, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);

   health_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   health_result = 0;
   initialized_value = 0;
   int prior_health_calls = health_calls;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   assert(health_calls == prior_health_calls);
   initialized_value = 1;
   kb_health_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   kb_health_result = 0;
   cancelled = 1;
   prior_health_calls = health_calls;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CANCELLED);
   assert(health_calls == prior_health_calls);

   cancelled = 0;
   cancel_after = 2;
   cancel_checks = 0;
   prior_health_calls = health_calls;
   int prior_kb_health_calls = kb_health_calls;
   response_len = 99;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CANCELLED);
   assert(response_len == 0);
   assert(health_calls == prior_health_calls + 1);
   assert(kb_health_calls == prior_kb_health_calls + 1);
}

static void test_embedding_dimension_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .is_initialized = is_initialized,
       .health_probe = health_probe,
       .kb_health_probe = kb_health_probe,
       .embedding_dimension = embedding_dimension,
   };
   uint8_t request[AIMEE_DB2_EMBEDDING_DIMENSION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99, dimension = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EMBEDDING_DIMENSION};
   assert(aimee_db2_embedding_dimension_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(embedding_dimension_calls == 1);
   assert(aimee_db2_embedding_dimension_reply_decode(response, response_len, &result, &dimension) ==
          0);
   assert(result == AIMEE_DB2_RESULT_OK && dimension == 384);

   embedding_dimension_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedding_dimension_reply_decode(response, response_len, &result, &dimension) ==
          0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && dimension == 0);
   embedding_dimension_value = 4001;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedding_dimension_reply_decode(response, response_len, &result, &dimension) ==
          0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && dimension == 0);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   embedding_dimension_value = 384;
   cancel_after = 2;
   cancel_checks = 0;
   int prior_calls = embedding_dimension_calls;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CANCELLED);
   assert(response_len == 0 && embedding_dimension_calls == prior_calls + 1);
}

static void test_level3_count_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.level3_count = level3_count};
   uint8_t request[AIMEE_DB2_LEVEL3_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_LEVEL3_COUNT_RESPONSE_LEN];
   uint32_t response_len = 99, count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_LEVEL3_COUNT};
   assert(aimee_db2_level3_count_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(level3_count_calls == 1);
   assert(aimee_db2_level3_count_reply_decode(response, response_len, &count) == 0 && count == 42);

   level3_count_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   level3_count_value = 42;
   cancel_after = 2;
   cancel_checks = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CANCELLED);
   assert(response_len == 0);

   cancel_after = 0;
   invocation.stage_id = AIMEE_DB2_STAGE_HEALTH;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_level2_count_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.level2_count = level2_count};
   uint8_t request[AIMEE_DB2_LEVEL2_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_LEVEL2_COUNT_RESPONSE_LEN];
   uint32_t response_len = 99, count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_LEVEL2_COUNT};
   assert(aimee_db2_level2_count_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(level2_count_calls == 1);
   assert(aimee_db2_level2_count_reply_decode(response, response_len, &count) == 0 && count == 17);

   level2_count_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_orphaned_l0_count_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.orphaned_l0_count = orphaned_l0_count};
   uint8_t request[AIMEE_DB2_ORPHANED_L0_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_ORPHANED_L0_COUNT_RESPONSE_LEN];
   uint32_t response_len = 99, count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_ORPHANED_L0_COUNT};
   assert(aimee_db2_orphaned_l0_count_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(orphaned_l0_count_calls == 1);
   assert(aimee_db2_orphaned_l0_count_reply_decode(response, response_len, &count) == 0 &&
          count == 5);
   orphaned_l0_count_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_total_count_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.total_count = total_count};
   uint8_t request[AIMEE_DB2_TOTAL_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_TOTAL_COUNT_RESPONSE_LEN];
   uint32_t response_len = 99;
   uint64_t count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_TOTAL_COUNT};
   assert(aimee_db2_total_count_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(total_count_calls == 1);
   assert(aimee_db2_total_count_reply_decode(response, response_len, &count) == 0 &&
          count == 1234567890123ULL);
   total_count_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_session_l2_count_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.session_l2_count = session_l2_count};
   uint8_t request[AIMEE_DB2_SESSION_L2_COUNT_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_SESSION_L2_COUNT_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99, count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_SESSION_L2_COUNT};
   assert(aimee_db2_session_l2_count_request_encode("session-123", request, sizeof(request),
                                                    &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(session_l2_count_calls == 1);
   assert(aimee_db2_session_l2_count_reply_decode(response, response_len, &count) == 0 &&
          count == 3);
   session_l2_count_value = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_key_exists_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.key_exists = key_exists};
   uint8_t request[AIMEE_DB2_KEY_EXISTS_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_KEY_EXISTS_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99, exists = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_KEY_EXISTS};
   assert(aimee_db2_key_exists_request_encode("recovery:tool-a->tool-b", request, sizeof(request),
                                              &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(key_exists_calls == 1);
   assert(aimee_db2_key_exists_reply_decode(response, response_len, &exists) == 0 && exists == 1);
   key_exists_value = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_find_id_by_key_kind_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.find_id_by_key_kind = find_id_by_key_kind};
   uint8_t request[AIMEE_DB2_FIND_ID_BY_KEY_KIND_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_FIND_ID_BY_KEY_KIND_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99, found = 99;
   uint64_t id = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_FIND_ID_BY_KEY_KIND};
   assert(aimee_db2_find_id_by_key_kind_request_encode("task:deploy-fix", "task", request,
                                                       sizeof(request), &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(find_id_by_key_kind_calls == 1);
   assert(aimee_db2_find_id_by_key_kind_reply_decode(response, response_len, &found, &id) == 0 &&
          found == 1 && id == 42);
   find_id_by_key_kind_value = 0;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_find_id_by_key_kind_reply_decode(response, response_len, &found, &id) == 0 &&
          found == 0 && id == 0);
   find_id_by_key_kind_value = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_key_exists_in_tier_pair_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.key_exists_in_tier_pair = key_exists_in_tier_pair};
   uint8_t request[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99, exists = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_KEY_EXISTS_IN_TIER_PAIR};
   assert(aimee_db2_key_exists_in_tier_pair_request_encode(
              "recovery:tool-a->tool-b", "L3", "L4", request, sizeof(request), &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(key_exists_in_tier_pair_calls == 1);
   assert(aimee_db2_key_exists_in_tier_pair_reply_decode(response, response_len, &exists) == 0 &&
          exists == 1);
   key_exists_in_tier_pair_value = 0;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_key_exists_in_tier_pair_reply_decode(response, response_len, &exists) == 0 &&
          exists == 0);
   key_exists_in_tier_pair_value = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_effectiveness_update_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.clear_effectiveness = clear_effectiveness,
                                               .set_effectiveness = set_effectiveness};
   uint8_t request[AIMEE_DB2_EFFECTIVENESS_UPDATE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EFFECTIVENESS_UPDATE};
   assert(aimee_db2_effectiveness_update_request_encode(42, 1, 0.75, request, sizeof(request)) ==
          0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(set_effectiveness_calls == 1 && effectiveness_memory_id == 42 &&
          effectiveness_value == 0.75);
   assert(aimee_db2_effectiveness_update_reply_decode(response, response_len, &result) == 0 &&
          result == AIMEE_DB2_RESULT_OK);

   assert(aimee_db2_effectiveness_update_request_encode(42, 0, 0.0, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(clear_effectiveness_calls == 1 && effectiveness_memory_id == 42);

   effectiveness_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_effectiveness_update_reply_decode(response, response_len, &result) == 0 &&
          result == AIMEE_DB2_RESULT_INVALID_STATE);
   effectiveness_result = 2;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_retention_enforce_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.retention_delete = retention_delete};
   uint8_t request[AIMEE_DB2_RETENTION_ENFORCE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RETENTION_ENFORCE_RESPONSE_LEN];
   uint32_t response_len = 99, deleted_count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_RETENTION_ENFORCE};
   assert(aimee_db2_retention_enforce_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(retention_delete_calls == 2);
   assert(aimee_db2_retention_enforce_reply_decode(response, response_len, &deleted_count) == 0 &&
          deleted_count == 5);

   retention_restricted_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   retention_restricted_value = (int)AIMEE_DB2_RETENTION_ENFORCE_MAX;
   retention_sensitive_value = 1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_effectiveness_demote_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.demote_effectiveness = demote_effectiveness};
   uint8_t request[AIMEE_DB2_EFFECTIVENESS_DEMOTE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EFFECTIVENESS_DEMOTE_RESPONSE_LEN];
   uint32_t response_len = 99, demoted_count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EFFECTIVENESS_DEMOTE};
   assert(aimee_db2_effectiveness_demote_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(demote_effectiveness_calls == 1 &&
          demote_effectiveness_threshold == AIMEE_DB2_EFFECTIVENESS_DEMOTE_THRESHOLD);
   assert(aimee_db2_effectiveness_demote_reply_decode(response, response_len, &demoted_count) ==
              0 &&
          demoted_count == 2);

   demote_effectiveness_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_effectiveness_stats_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.effectiveness_stats = effectiveness_stats};
   uint8_t request[AIMEE_DB2_EFFECTIVENESS_STATS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_db2_effectiveness_stats_t decoded = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EFFECTIVENESS_STATS};
   assert(aimee_db2_effectiveness_stats_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(effectiveness_stats_calls == 1 &&
          effectiveness_stats_low_threshold == AIMEE_DB2_EFFECTIVENESS_STATS_LOW_THRESHOLD);
   assert(aimee_db2_effectiveness_stats_reply_decode(response, response_len, &decoded) == 0 &&
          decoded.avg_effectiveness == 0.5 && decoded.low_effectiveness_count == 3 &&
          decoded.high_impact_count == 1);

   effectiveness_stats_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   /* Counts outside the declared bound never reach the wire. */
   effectiveness_stats_result = 0;
   effectiveness_stats_low_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   effectiveness_stats_low_value = 3;
   effectiveness_stats_high_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   effectiveness_stats_high_value = 1;

   /* An average outside the declared probability range is refused, not truncated. */
   effectiveness_stats_average_value = 1.5;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   effectiveness_stats_average_value = 0.5;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_l2_memory_ids_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.list_l2_memory_ids = list_l2_memory_ids};
   uint8_t request[AIMEE_DB2_L2_MEMORY_IDS_REQUEST_LEN];
   static uint8_t response[AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MAX_LEN];
   static uint64_t decoded[AIMEE_DB2_L2_MEMORY_IDS_MAX];
   uint32_t response_len = 99, count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_L2_MEMORY_IDS};
   assert(aimee_db2_l2_memory_ids_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(list_l2_memory_ids_calls == 1);
   assert(aimee_db2_l2_memory_ids_reply_decode(response, response_len, decoded,
                                               AIMEE_DB2_L2_MEMORY_IDS_MAX, &count) == 0);
   assert(count == 3 && decoded[0] == 7 && decoded[1] == 22 && decoded[2] == 33);

   /* An empty corpus still answers, with the shortest legal reply. */
   list_l2_memory_ids_result = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(response_len == AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MIN_LEN);

   /* A backend error and a non-positive identifier are both refused. */
   list_l2_memory_ids_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   list_l2_memory_ids_result = 3;
   list_l2_memory_ids_first = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   list_l2_memory_ids_first = -5;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   list_l2_memory_ids_first = 7;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_health_record_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.count_memories = count_memories,
                                               .count_recent_conflicts = count_recent_conflicts,
                                               .health_record = health_record};
   uint8_t request[AIMEE_DB2_HEALTH_RECORD_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_HEALTH_RECORD};
   assert(aimee_db2_health_record_request_encode(4u, 2u, 9u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_health_record_reply_decode(response, response_len) == 0);

   /* DB2 supplies the corpus total and the fixed conflict window; only the
    * three cycle counters come from the caller. */
   assert(health_record_calls == 1 && health_record_total == 512 &&
          health_record_contradictions == 6 && health_record_promotions == 4 &&
          health_record_demotions == 2 && health_record_expirations == 9);
   assert(count_recent_conflicts_days == AIMEE_DB2_HEALTH_RECORD_CONFLICT_WINDOW_DAYS);

   /* A failed pre-step must not record a cycle at all. */
   count_memories_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   assert(health_record_calls == 1);
   count_memories_value = 512;
   count_recent_conflicts_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   assert(health_record_calls == 1);
   count_recent_conflicts_value = 6;

   /* Every composed capability is required, not just the insert. */
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   const aimee_db2_module_backend_t no_counts = {.health_record = health_record};
   assert(invoke(&no_counts, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   const aimee_db2_module_backend_t no_insert = {.count_memories = count_memories,
                                                 .count_recent_conflicts = count_recent_conflicts};
   assert(invoke(&no_insert, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_pool_status_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .pool_status = pool_status,
   };
   uint8_t request[AIMEE_DB2_POOL_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_POOL_STATUS_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_pool_status_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_POOL_STATUS};
   assert(aimee_db2_pool_status_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_pool_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.size == 16 && status.in_use == 2);

   pool_status_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_pool_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && status.size == 0);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_embedding_refusals_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.embedding_refusals = embedding_refusals};
   uint8_t request[AIMEE_DB2_EMBEDDING_REFUSALS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_embedding_refusals_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EMBEDDING_REFUSALS};
   assert(aimee_db2_embedding_refusals_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedding_refusals_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.refused_count == 7 && status.last_offered == 768);
   embedding_refusals_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedding_refusals_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
}

static void test_postgres_status_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.postgres_status = postgres_status};
   uint8_t request[AIMEE_DB2_POSTGRES_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_postgres_status_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_POSTGRES_STATUS};
   assert(aimee_db2_postgres_status_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_postgres_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.available == 15 &&
          status.replica_lag_bytes == 1048576);
   postgres_status_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_postgres_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
}

static void test_reembed_status_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.reembed_status = reembed_status};
   uint8_t request[AIMEE_DB2_REEMBED_STATUS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_reembed_status_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_REEMBED_STATUS};
   assert(aimee_db2_reembed_status_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.target_dimension == 384);
   reembed_status_result = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND);
   reembed_status_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_status_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
}

static void test_reembed_clear_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.reembed_clear = db2_reembed_in_progress_clear};
   uint8_t request[AIMEE_DB2_REEMBED_CLEAR_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_REEMBED_CLEAR};
   assert(aimee_db2_reembed_clear_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_clear_reply_decode(response, response_len, &result) == 0);
   assert(result == AIMEE_DB2_RESULT_OK);
   reembed_clear_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_clear_reply_decode(response, response_len, &result) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
}

static void test_reembed_clear_maintenance_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.reembed_clear_maintenance =
                                                   db2_reembed_clear_maintenance};
   uint8_t request[AIMEE_DB2_REEMBED_MAINT_CLEAR_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REEMBED_MAINT_CLEAR_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_reembed_clear_maintenance_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_REEMBED_MAINT_CLEAR};
   assert(aimee_db2_reembed_clear_maintenance_request_encode(1, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(reembed_maintenance_calls == 1 && reembed_maintenance_force == 1);
   assert(aimee_db2_reembed_clear_maintenance_reply_decode(response, response_len, &result,
                                                           &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.was_in_progress == 1 &&
          status.recorded_dimension == 384 && status.running_dimension == 384);

   reembed_maintenance_result = -1;
   reembed_maintenance_recorded = 768;
   assert(aimee_db2_reembed_clear_maintenance_request_encode(0, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_clear_maintenance_reply_decode(response, response_len, &result,
                                                           &status) == 0);
   assert(result == AIMEE_DB2_RESULT_CONFLICT && status.recorded_dimension == 768 &&
          status.running_dimension == 384);

   reembed_maintenance_result = -2;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_clear_maintenance_reply_decode(response, response_len, &result,
                                                           &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && status.running_dimension == 0);

   reembed_maintenance_result = 0;
   reembed_maintenance_running = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_reembed_clear_maintenance_reply_decode(response, response_len, &result,
                                                           &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_embedder_serving_id_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.embedder_serving_id = db2_embedder_serving_id};
   uint8_t request[AIMEE_DB2_EMBEDDER_SERVING_ID_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MAX_LEN];
   uint32_t response_len = 99, result = 99;
   char serving_id[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1] = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EMBEDDER_SERVING_ID};
   assert(aimee_db2_embedder_serving_id_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedder_serving_id_reply_decode(response, response_len, &result, serving_id,
                                                     sizeof(serving_id)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && strcmp(serving_id, serving_id_value) == 0);

   serving_id_value = "";
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedder_serving_id_reply_decode(response, response_len, &result, serving_id,
                                                     sizeof(serving_id)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && serving_id[0] == '\0');
   serving_id_value = NULL;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_embedder_serving_id_reply_decode(response, response_len, &result, serving_id,
                                                     sizeof(serving_id)) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && serving_id[0] == '\0');

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_dimension_reset_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.dimension_reset = dimension_reset};
   uint8_t request[AIMEE_DB2_DIMENSION_RESET_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DIMENSION_RESET_RESPONSE_LEN];
   uint32_t response_len = 99, result = 99;
   aimee_db2_dimension_reset_t status = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_DIMENSION_RESET};
   assert(aimee_db2_dimension_reset_request_encode(384, 1, 0, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(dimension_reset_calls == 1 && dimension_reset_target == 384 &&
          dimension_reset_force == 1 && dimension_reset_dry_run == 0);
   assert(aimee_db2_dimension_reset_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && status.recorded_dimension == 768 &&
          status.target_dimension == 384 && status.rows_cleared == 1234);

   dimension_reset_result = -2;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_dimension_reset_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_CONFLICT);
   dimension_reset_result = -3;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_dimension_reset_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_DENIED);
   dimension_reset_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_dimension_reset_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && status.target_dimension == 0);

   dimension_reset_result = 0;
   dimension_reset_status.tables_discovered = 17;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_dimension_reset_reply_decode(response, response_len, &result, &status) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
}

static void test_typed_client(void)
{
   reset();
   int schema_ok = 9, have_pg_trgm = 9, kb_tables_ok = 9;
   assert(aimee_db2_health_call(NULL, NULL, 77, 88, &schema_ok, &have_pg_trgm, &kb_tables_ok, NULL,
                                NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);

   assert(aimee_db2_health_call(transport, (void *)0x1234, 77, 88, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(schema_ok && !have_pg_trgm && kb_tables_ok);
   assert(transport_calls == 1);

   transport_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   schema_ok = have_pg_trgm = kb_tables_ok = 9;
   assert(aimee_db2_health_call(transport, (void *)0x1234, 77, 88, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);

   transport_result = AIMEE_MODULE_CALL_OK;
   transport_response[0] ^= 1u;
   assert(aimee_db2_health_call(transport, (void *)0x1234, 77, 88, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_PROTOCOL);
   assert(!schema_ok && !have_pg_trgm && !kb_tables_ok);
   transport_response[0] ^= 1u;
   transport_response_len--;
   assert(aimee_db2_health_call(transport, (void *)0x1234, 77, 88, NULL, NULL, NULL, NULL, NULL) ==
          AIMEE_MODULE_CALL_PROTOCOL);
}

static void test_embedding_dimension_typed_client(void)
{
   reset();
   transport_expect_dimension = 1;
   uint32_t domain_result = 9, dimension = 9;
   assert(aimee_db2_embedding_dimension_call(NULL, NULL, 77, 88, &domain_result, &dimension, NULL,
                                             NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(domain_result == 0 && dimension == 0);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_OK, 384, transport_response,
                                                     sizeof(transport_response),
                                                     &transport_response_len) == 0);
   assert(aimee_db2_embedding_dimension_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && dimension == 384 && transport_calls == 1);

   assert(aimee_db2_embedding_dimension_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 0,
                                                     transport_response, sizeof(transport_response),
                                                     &transport_response_len) == 0);
   assert(aimee_db2_embedding_dimension_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_INVALID_STATE && dimension == 0);

   transport_result = AIMEE_MODULE_CALL_CANCELLED;
   domain_result = dimension = 9;
   assert(aimee_db2_embedding_dimension_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             &dimension, NULL,
                                             NULL) == AIMEE_MODULE_CALL_CANCELLED);
   assert(domain_result == 0 && dimension == 0);
   transport_result = AIMEE_MODULE_CALL_OK;
   transport_response[0] ^= 1u;
   assert(aimee_db2_embedding_dimension_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_PROTOCOL);
   assert(domain_result == 0 && dimension == 0);
}

static void test_level3_count_typed_client(void)
{
   reset();
   transport_expect_level3_count = 1;
   uint32_t count = 99;
   assert(aimee_db2_level3_count_call(NULL, NULL, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(count == 0);
   assert(aimee_db2_level3_count_reply_encode(42, transport_response, sizeof(transport_response),
                                              &transport_response_len) == 0);
   assert(aimee_db2_level3_count_call(transport, (void *)0x1234, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(count == 42 && transport_calls == 1);

   transport_result = AIMEE_MODULE_CALL_CANCELLED;
   count = 99;
   assert(aimee_db2_level3_count_call(transport, (void *)0x1234, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_CANCELLED);
   assert(count == 0);
   transport_result = AIMEE_MODULE_CALL_OK;
   transport_response[0] ^= 1u;
   assert(aimee_db2_level3_count_call(transport, (void *)0x1234, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_PROTOCOL);
   assert(count == 0);
}

static void test_level2_count_typed_client(void)
{
   reset();
   transport_expect_level2_count = 1;
   uint32_t count = 99;
   assert(aimee_db2_level2_count_call(NULL, NULL, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(count == 0);
   assert(aimee_db2_level2_count_reply_encode(17, transport_response, sizeof(transport_response),
                                              &transport_response_len) == 0);
   assert(aimee_db2_level2_count_call(transport, (void *)0x1234, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(count == 17 && transport_calls == 1);
   transport_result = AIMEE_MODULE_CALL_CANCELLED;
   count = 99;
   assert(aimee_db2_level2_count_call(transport, (void *)0x1234, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_CANCELLED);
   assert(count == 0);
}

static void test_orphaned_l0_count_typed_client(void)
{
   reset();
   transport_expect_orphaned_l0_count = 1;
   uint32_t count = 99;
   assert(aimee_db2_orphaned_l0_count_call(NULL, NULL, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(count == 0);
   assert(aimee_db2_orphaned_l0_count_reply_encode(
              5, transport_response, sizeof(transport_response), &transport_response_len) == 0);
   assert(aimee_db2_orphaned_l0_count_call(transport, (void *)0x1234, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(count == 5 && transport_calls == 1);
}

static void test_total_count_typed_client(void)
{
   reset();
   transport_expect_total_count = 1;
   uint64_t count = 99;
   assert(aimee_db2_total_count_call(NULL, NULL, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(count == 0);
   assert(aimee_db2_total_count_reply_encode(1234567890123ULL, transport_response,
                                             sizeof(transport_response),
                                             &transport_response_len) == 0);
   assert(aimee_db2_total_count_call(transport, (void *)0x1234, 77, 88, &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(count == 1234567890123ULL && transport_calls == 1);
}

static void test_session_l2_count_typed_client(void)
{
   reset();
   transport_expect_session_l2_count = 1;
   uint32_t count = 99;
   assert(aimee_db2_session_l2_count_call(NULL, NULL, 77, 88, "session-123", &count, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(count == 0);
   assert(aimee_db2_session_l2_count_call(transport, (void *)0x1234, 77, 88, "", &count, NULL,
                                          NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(aimee_db2_session_l2_count_reply_encode(3, transport_response, sizeof(transport_response),
                                                  &transport_response_len) == 0);
   assert(aimee_db2_session_l2_count_call(transport, (void *)0x1234, 77, 88, "session-123", &count,
                                          NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(count == 3 && transport_calls == 1);
}

static void test_key_exists_typed_client(void)
{
   reset();
   transport_expect_key_exists = 1;
   uint32_t exists = 99;
   assert(aimee_db2_key_exists_call(NULL, NULL, 77, 88, "recovery:tool-a->tool-b", &exists, NULL,
                                    NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(exists == 0);
   assert(aimee_db2_key_exists_call(transport, (void *)0x1234, 77, 88, "", &exists, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(aimee_db2_key_exists_reply_encode(1, transport_response, sizeof(transport_response),
                                            &transport_response_len) == 0);
   assert(aimee_db2_key_exists_call(transport, (void *)0x1234, 77, 88, "recovery:tool-a->tool-b",
                                    &exists, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(exists == 1 && transport_calls == 1);
}

static void test_find_id_by_key_kind_typed_client(void)
{
   reset();
   transport_expect_find_id_by_key_kind = 1;
   uint32_t found = 99;
   uint64_t id = 99;
   assert(aimee_db2_find_id_by_key_kind_call(NULL, NULL, 77, 88, "task:deploy-fix", "task", &found,
                                             &id, NULL,
                                             NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(found == 0 && id == 0);
   assert(aimee_db2_find_id_by_key_kind_call(transport, (void *)0x1234, 77, 88, "", "task", &found,
                                             &id, NULL,
                                             NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(aimee_db2_find_id_by_key_kind_reply_encode(
              1, 42, transport_response, sizeof(transport_response), &transport_response_len) == 0);
   assert(aimee_db2_find_id_by_key_kind_call(transport, (void *)0x1234, 77, 88, "task:deploy-fix",
                                             "task", &found, &id, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(found == 1 && id == 42 && transport_calls == 1);
}

static void test_key_exists_in_tier_pair_typed_client(void)
{
   reset();
   transport_expect_key_exists_in_tier_pair = 1;
   uint32_t exists = 99;
   assert(aimee_db2_key_exists_in_tier_pair_call(NULL, NULL, 77, 88, "recovery:tool-a->tool-b",
                                                 "L3", "L4", &exists, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(exists == 0);
   assert(aimee_db2_key_exists_in_tier_pair_call(transport, (void *)0x1234, 77, 88,
                                                 "recovery:tool-a->tool-b", "", "L4", &exists, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(aimee_db2_key_exists_in_tier_pair_reply_encode(
              1, transport_response, sizeof(transport_response), &transport_response_len) == 0);
   assert(aimee_db2_key_exists_in_tier_pair_call(transport, (void *)0x1234, 77, 88,
                                                 "recovery:tool-a->tool-b", "L3", "L4", &exists,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(exists == 1 && transport_calls == 1);
}

static void test_effectiveness_update_typed_client(void)
{
   reset();
   transport_expect_effectiveness_update = 1;
   uint32_t result = 99;
   assert(aimee_db2_effectiveness_update_call(NULL, NULL, 77, 88, 42, 1, 0.75, &result, NULL,
                                              NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(result == 0);
   assert(aimee_db2_effectiveness_update_call(transport, (void *)0x1234, 77, 88, 0, 1, 0.75,
                                              &result, NULL,
                                              NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(aimee_db2_effectiveness_update_reply_encode(AIMEE_DB2_RESULT_OK, transport_response,
                                                      sizeof(transport_response)) == 0);
   transport_response_len = AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN;
   assert(aimee_db2_effectiveness_update_call(transport, (void *)0x1234, 77, 88, 42, 1, 0.75,
                                              &result, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(result == AIMEE_DB2_RESULT_OK && transport_calls == 1);
}

static void test_retention_enforce_typed_client(void)
{
   reset();
   transport_expect_retention_enforce = 1;
   uint32_t deleted_count = 99;
   assert(aimee_db2_retention_enforce_call(NULL, NULL, 77, 88, &deleted_count, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(deleted_count == 0);
   assert(aimee_db2_retention_enforce_reply_encode(
              5, transport_response, sizeof(transport_response), &transport_response_len) == 0);
   assert(aimee_db2_retention_enforce_call(transport, (void *)0x1234, 77, 88, &deleted_count, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(deleted_count == 5 && transport_calls == 1);
}

static void test_effectiveness_demote_typed_client(void)
{
   reset();
   transport_expect_effectiveness_demote = 1;
   uint32_t demoted_count = 99;
   assert(aimee_db2_effectiveness_demote_call(NULL, NULL, 77, 88, &demoted_count, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(demoted_count == 0);
   assert(aimee_db2_effectiveness_demote_reply_encode(
              2, transport_response, sizeof(transport_response), &transport_response_len) == 0);
   assert(aimee_db2_effectiveness_demote_call(transport, (void *)0x1234, 77, 88, &demoted_count,
                                              NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(demoted_count == 2 && transport_calls == 1);
}

static void test_effectiveness_stats_typed_client(void)
{
   reset();
   transport_expect_effectiveness_stats = 1;
   aimee_db2_effectiveness_stats_t stats = {
       .avg_effectiveness = 0.5, .low_effectiveness_count = 3, .high_impact_count = 1};
   aimee_db2_effectiveness_stats_t received = {
       .avg_effectiveness = 9.0, .low_effectiveness_count = 99, .high_impact_count = 99};
   assert(aimee_db2_effectiveness_stats_call(NULL, NULL, 77, 88, &received, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(received.avg_effectiveness == 0.0 && received.low_effectiveness_count == 0 &&
          received.high_impact_count == 0);
   assert(aimee_db2_effectiveness_stats_reply_encode(&stats, transport_response,
                                                     sizeof(transport_response),
                                                     &transport_response_len) == 0);
   assert(aimee_db2_effectiveness_stats_call(transport, (void *)0x1234, 77, 88, &received, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(received.avg_effectiveness == 0.5 && received.low_effectiveness_count == 3 &&
          received.high_impact_count == 1 && transport_calls == 1);
}

static void test_l2_memory_ids_typed_client(void)
{
   reset();
   transport_expect_l2_memory_ids = 1;
   static uint64_t received[AIMEE_DB2_L2_MEMORY_IDS_MAX];
   const uint64_t ids[] = {7, 19, 4242};
   uint32_t count = 99;
   assert(aimee_db2_l2_memory_ids_call(NULL, NULL, 77, 88, received, AIMEE_DB2_L2_MEMORY_IDS_MAX,
                                       &count, NULL, NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(count == 0);
   assert(aimee_db2_l2_memory_ids_reply_encode(ids, 3u, transport_response,
                                               sizeof(transport_response),
                                               &transport_response_len) == 0);
   assert(aimee_db2_l2_memory_ids_call(transport, (void *)0x1234, 77, 88, received,
                                       AIMEE_DB2_L2_MEMORY_IDS_MAX, &count, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(count == 3 && received[0] == 7 && received[1] == 19 && received[2] == 4242 &&
          transport_calls == 1);
}

static void test_health_record_typed_client(void)
{
   reset();
   transport_expect_health_record = 1;
   assert(aimee_db2_health_record_call(NULL, NULL, 77, 88, 4u, 2u, 9u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(aimee_db2_health_record_call(transport, (void *)0x1234, 77, 88,
                                       AIMEE_DB2_HEALTH_RECORD_COUNTER_MAX + 1u, 2u, 9u, NULL,
                                       NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(transport_calls == 0);
   assert(aimee_db2_health_record_reply_encode(transport_response, sizeof(transport_response)) ==
          0);
   transport_response_len = AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN;
   assert(aimee_db2_health_record_call(transport, (void *)0x1234, 77, 88, 4u, 2u, 9u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(transport_calls == 1);
}

static void test_pool_status_typed_client(void)
{
   reset();
   transport_expect_pool = 1;
   const aimee_db2_pool_status_t expected = {16, 2, 1, 10, 3, 4, 5};
   uint32_t domain_result = 9;
   aimee_db2_pool_status_t status = {.size = 9};
   assert(aimee_db2_pool_status_call(NULL, NULL, 77, 88, &domain_result, &status, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(domain_result == 0 && status.size == 0);

   assert(aimee_db2_pool_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, transport_response,
                                             sizeof(transport_response),
                                             &transport_response_len) == 0);
   assert(aimee_db2_pool_status_call(transport, (void *)0x1234, 77, 88, &domain_result, &status,
                                     NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && status.size == 16 && status.poisoned == 5);

   transport_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   domain_result = 9;
   status.size = 9;
   assert(aimee_db2_pool_status_call(transport, (void *)0x1234, 77, 88, &domain_result, &status,
                                     NULL, NULL) == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(domain_result == 0 && status.size == 0);
}

static void test_embedding_refusals_typed_client(void)
{
   reset();
   transport_expect_refusals = 1;
   const aimee_db2_embedding_refusals_t expected = {7, 768};
   uint32_t domain_result = 9;
   aimee_db2_embedding_refusals_t status = {0};
   assert(aimee_db2_embedding_refusals_reply_encode(AIMEE_DB2_RESULT_OK, &expected,
                                                    transport_response, sizeof(transport_response),
                                                    &transport_response_len) == 0);
   assert(aimee_db2_embedding_refusals_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                            &status, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && status.refused_count == 7 &&
          status.last_offered == 768);
}

static void test_postgres_status_typed_client(void)
{
   reset();
   transport_expect_postgres = 1;
   const aimee_db2_postgres_status_t expected = {15, 12, 100, 1, 1048576};
   uint32_t domain_result = 9;
   aimee_db2_postgres_status_t status = {0};
   assert(aimee_db2_postgres_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, transport_response,
                                                 sizeof(transport_response),
                                                 &transport_response_len) == 0);
   assert(aimee_db2_postgres_status_call(transport, (void *)0x1234, 77, 88, &domain_result, &status,
                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && status.available == 15 &&
          status.active_connections == 12 && status.replica_lag_bytes == 1048576);
}

static void test_reembed_status_typed_client(void)
{
   reset();
   transport_expect_reembed = 1;
   const aimee_db2_reembed_status_t expected = {384, 1700000000};
   uint32_t domain_result = 9;
   aimee_db2_reembed_status_t status = {0};
   assert(aimee_db2_reembed_status_reply_encode(AIMEE_DB2_RESULT_OK, &expected, transport_response,
                                                sizeof(transport_response),
                                                &transport_response_len) == 0);
   assert(aimee_db2_reembed_status_call(transport, (void *)0x1234, 77, 88, &domain_result, &status,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && status.target_dimension == 384 &&
          status.started_epoch == 1700000000);
}

static void test_reembed_clear_typed_client(void)
{
   reset();
   transport_expect_reembed_clear = 1;
   uint32_t domain_result = 9;
   assert(aimee_db2_reembed_clear_reply_encode(AIMEE_DB2_RESULT_OK, transport_response,
                                               sizeof(transport_response),
                                               &transport_response_len) == 0);
   assert(aimee_db2_reembed_clear_call(transport, (void *)0x1234, 77, 88, &domain_result, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK);
}

static void test_reembed_clear_maintenance_typed_client(void)
{
   reset();
   transport_expect_reembed_maintenance = 1;
   const aimee_db2_reembed_clear_maintenance_t expected = {1, 768, 384};
   uint32_t domain_result = 9;
   aimee_db2_reembed_clear_maintenance_t status = {0};
   assert(aimee_db2_reembed_clear_maintenance_reply_encode(
              AIMEE_DB2_RESULT_CONFLICT, &expected, transport_response, sizeof(transport_response),
              &transport_response_len) == 0);
   assert(aimee_db2_reembed_clear_maintenance_call(transport, (void *)0x1234, 77, 88, 1,
                                                   &domain_result, &status, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_CONFLICT && status.was_in_progress == 1 &&
          status.recorded_dimension == 768 && status.running_dimension == 384);

   domain_result = 9;
   status.running_dimension = 9;
   assert(aimee_db2_reembed_clear_maintenance_call(NULL, NULL, 77, 88, 1, &domain_result, &status,
                                                   NULL,
                                                   NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(domain_result == 0 && status.running_dimension == 0);
   assert(aimee_db2_reembed_clear_maintenance_call(transport, (void *)0x1234, 77, 88, 2,
                                                   &domain_result, &status, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
}

static void test_embedder_serving_id_typed_client(void)
{
   reset();
   transport_expect_serving_id = 1;
   uint32_t domain_result = 9;
   char serving_id[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1] = "stale";
   const char *expected = "bekko-a25m/8721341054416418";
   assert(aimee_db2_embedder_serving_id_reply_encode(AIMEE_DB2_RESULT_OK, expected,
                                                     transport_response, sizeof(transport_response),
                                                     &transport_response_len) == 0);
   assert(aimee_db2_embedder_serving_id_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             serving_id, sizeof(serving_id), NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && strcmp(serving_id, expected) == 0);

   domain_result = 9;
   strcpy(serving_id, "stale");
   assert(aimee_db2_embedder_serving_id_call(NULL, NULL, 77, 88, &domain_result, serving_id,
                                             sizeof(serving_id), NULL,
                                             NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(domain_result == 0 && serving_id[0] == '\0');
   assert(aimee_db2_embedder_serving_id_call(transport, (void *)0x1234, 77, 88, &domain_result,
                                             serving_id, AIMEE_DB2_EMBEDDER_SERVING_ID_MAX, NULL,
                                             NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
}

static void test_dimension_reset_typed_client(void)
{
   reset();
   transport_expect_dimension_reset = 1;
   uint32_t domain_result = 9;
   const aimee_db2_dimension_reset_t expected = {768, 384, 6, 0, 1234, -1, 7};
   aimee_db2_dimension_reset_t status = {0};
   assert(aimee_db2_dimension_reset_reply_encode(AIMEE_DB2_RESULT_DENIED, &expected,
                                                 transport_response, sizeof(transport_response),
                                                 &transport_response_len) == 0);
   assert(aimee_db2_dimension_reset_call(transport, (void *)0x1234, 77, 88, 384, 1, 0,
                                         &domain_result, &status, NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_DENIED && status.recorded_dimension == 768 &&
          status.target_dimension == 384 && status.curator_requeued == -1);

   domain_result = 9;
   status.target_dimension = 9;
   assert(aimee_db2_dimension_reset_call(NULL, NULL, 77, 88, 384, 1, 0, &domain_result, &status,
                                         NULL, NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(domain_result == 0 && status.target_dimension == 0);
   assert(aimee_db2_dimension_reset_call(transport, (void *)0x1234, 77, 88, 4001, 0, 0,
                                         &domain_result, &status, NULL,
                                         NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
}

int main(void)
{
   test_wire_contract();
   test_body_envelope();
   test_embedding_dimension_wire();
   test_level3_count_wire();
   test_level2_count_wire();
   test_orphaned_l0_count_wire();
   test_total_count_wire();
   test_session_l2_count_wire();
   test_key_exists_wire();
   test_find_id_by_key_kind_wire();
   test_key_exists_in_tier_pair_wire();
   test_effectiveness_update_wire();
   test_retention_enforce_wire();
   test_effectiveness_demote_wire();
   test_effectiveness_stats_wire();
   test_l2_memory_ids_wire();
   test_health_record_wire();
   test_pool_status_wire();
   test_embedding_refusals_wire();
   test_postgres_status_wire();
   test_reembed_status_wire();
   test_reembed_clear_wire();
   test_reembed_clear_maintenance_wire();
   test_embedder_serving_id_wire();
   test_dimension_reset_wire();
   test_handler_success_and_failures();
   test_embedding_dimension_handler();
   test_level3_count_handler();
   test_level2_count_handler();
   test_orphaned_l0_count_handler();
   test_total_count_handler();
   test_session_l2_count_handler();
   test_key_exists_handler();
   test_find_id_by_key_kind_handler();
   test_key_exists_in_tier_pair_handler();
   test_effectiveness_update_handler();
   test_retention_enforce_handler();
   test_effectiveness_demote_handler();
   test_effectiveness_stats_handler();
   test_l2_memory_ids_handler();
   test_health_record_handler();
   test_pool_status_handler();
   test_embedding_refusals_handler();
   test_postgres_status_handler();
   test_reembed_status_handler();
   test_reembed_clear_handler();
   test_reembed_clear_maintenance_handler();
   test_embedder_serving_id_handler();
   test_dimension_reset_handler();
   test_typed_client();
   test_embedding_dimension_typed_client();
   test_level3_count_typed_client();
   test_level2_count_typed_client();
   test_orphaned_l0_count_typed_client();
   test_total_count_typed_client();
   test_session_l2_count_typed_client();
   test_key_exists_typed_client();
   test_find_id_by_key_kind_typed_client();
   test_key_exists_in_tier_pair_typed_client();
   test_effectiveness_update_typed_client();
   test_retention_enforce_typed_client();
   test_effectiveness_demote_typed_client();
   test_effectiveness_stats_typed_client();
   test_l2_memory_ids_typed_client();
   test_health_record_typed_client();
   test_pool_status_typed_client();
   test_embedding_refusals_typed_client();
   test_postgres_status_typed_client();
   test_reembed_status_typed_client();
   test_reembed_clear_typed_client();
   test_reembed_clear_maintenance_typed_client();
   test_embedder_serving_id_typed_client();
   test_dimension_reset_typed_client();
   puts("test_db2_module_contract: ok");
   return 0;
}
