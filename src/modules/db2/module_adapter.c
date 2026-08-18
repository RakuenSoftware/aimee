#include "module_adapter.h"

#include <aimee/db2/module_api.h>

#include "c/db2.h"
#include "c/db2_pool.h"
#include "c/memory_health.h"
#include "c/memory_payload.h"
#include "c/memory_query.h"

static int production_pool_status(aimee_db2_pool_status_t *status)
{
   int size = 0, in_use = 0, waiters = 0;
   long grants = 0, timeouts = 0, stuck = 0, poisoned = 0;
   db2_pool_stats(&size, &in_use, &waiters, &grants, &timeouts, &stuck, &poisoned);
   if (!status || size < 1 || size > (int)AIMEE_DB2_POOL_SIZE_MAX || in_use < 0 || in_use > size ||
       waiters < 0 || grants < 0 || timeouts < 0 || stuck < 0 || poisoned < 0)
      return -1;
   *status = (aimee_db2_pool_status_t){
       .size = (uint32_t)size,
       .in_use = (uint32_t)in_use,
       .waiters = (uint32_t)waiters,
       .lease_grants = (uint64_t)grants,
       .lease_timeouts = (uint64_t)timeouts,
       .stuck = (uint64_t)stuck,
       .poisoned = (uint64_t)poisoned,
   };
   return 0;
}

static int production_embedding_refusals(aimee_db2_embedding_refusals_t *status)
{
   long long refused = db2_embedding_dim_refused_count();
   int offered = db2_embedding_dim_last_offered();
   if (!status || refused < 0 || offered < 0 ||
       (uint32_t)offered > AIMEE_DB2_EMBEDDING_OFFERED_MAX || ((refused == 0) != (offered == 0)))
      return -1;
   *status = (aimee_db2_embedding_refusals_t){
       .refused_count = (uint64_t)refused,
       .last_offered = (uint32_t)offered,
   };
   return 0;
}

static int production_postgres_status(aimee_db2_postgres_status_t *status)
{
   int active = -1, maximum = -1, replica = -1;
   int64_t lag = -1;
   if (!status || db2_pg_stat_summary(&active, &maximum, &replica, &lag) != 0 || active < -1 ||
       maximum < -1 || replica < -1 || replica > 1 || lag < -1 || (lag >= 0 && replica != 1))
      return -1;
   *status = (aimee_db2_postgres_status_t){0};
   if (active >= 0)
   {
      status->available |= AIMEE_DB2_POSTGRES_AVAILABLE_ACTIVE;
      status->active_connections = (uint32_t)active;
   }
   if (maximum >= 0)
   {
      status->available |= AIMEE_DB2_POSTGRES_AVAILABLE_MAX;
      status->max_connections = (uint32_t)maximum;
   }
   if (replica >= 0)
   {
      status->available |= AIMEE_DB2_POSTGRES_AVAILABLE_ROLE;
      status->is_replica = (uint32_t)replica;
   }
   if (lag >= 0)
   {
      status->available |= AIMEE_DB2_POSTGRES_AVAILABLE_LAG;
      status->replica_lag_bytes = (uint64_t)lag;
   }
   return aimee_db2_postgres_status_valid(status) ? 0 : -1;
}

static int production_dimension_reset(uint32_t target_dimension, uint32_t force, uint32_t dry_run,
                                      aimee_db2_dimension_reset_t *status)
{
   if (!status)
      return -1;
   db2_reembed_plan_t plan = {0};
   int result = db2_dim_change_reset((int)target_dimension, (int)force, (int)dry_run, &plan);
   status->recorded_dimension = plan.recorded_dim >= 0 ? (uint32_t)plan.recorded_dim : UINT32_MAX;
   status->target_dimension = plan.target_dim >= 0 ? (uint32_t)plan.target_dim : UINT32_MAX;
   status->tables_discovered = plan.n_tables >= 0 ? (uint32_t)plan.n_tables : UINT32_MAX;
   status->tables_dropped = plan.n_dropped >= 0 ? (uint32_t)plan.n_dropped : UINT32_MAX;
   status->rows_cleared = plan.rows_cleared >= 0 ? (uint64_t)plan.rows_cleared : UINT64_MAX;
   status->curator_requeued = plan.curator_requeued;
   status->evidence_requeued = plan.evidence_requeued;
   return result;
}

static int production_reembed_status(aimee_db2_reembed_status_t *status)
{
   if (!status)
      return -1;
   int target = 0;
   long started = 0;
   int result = db2_reembed_in_progress_get(&target, &started);
   *status = (aimee_db2_reembed_status_t){0};
   if (result == 0)
      return 0;
   if (result != 1 || target < (int)AIMEE_DB2_REEMBED_DIMENSION_MIN ||
       target > (int)AIMEE_DB2_REEMBED_DIMENSION_MAX || started <= 0)
      return -1;
   status->target_dimension = (uint32_t)target;
   status->started_epoch = (uint64_t)started;
   return 1;
}

static const aimee_db2_module_backend_t *production_backend(void)
{
   static const aimee_db2_module_backend_t backend = {
       .is_initialized = db2_is_initialized,
       .health_probe = db2_health_probe,
       .kb_health_probe = db2_kb_health_probe,
       .embedding_dimension = db2_embedding_dim,
       .level3_count = db2_memory_count_l3,
       .level2_count = db2_memory_count_l2,
       .orphaned_l0_count = db2_memory_count_orphaned_l0,
       .total_count = db2_memory_count,
       .session_l2_count = db2_memory_count_l2_for_session,
       .key_exists = db2_memory_key_exists,
       .find_id_by_key_kind = db2_memory_find_id_by_key_kind,
       .key_exists_in_tier_pair = db2_memory_key_exists_in_tier_pair,
       .clear_effectiveness = db2_memory_health_clear_effectiveness,
       .set_effectiveness = db2_memory_health_set_effectiveness,
       .retention_delete = db2_memory_health_delete_by_sensitivity,
       .demote_effectiveness = db2_memory_health_demote_low_effectiveness,
       .effectiveness_stats = db2_memory_health_effectiveness_stats,
       .list_l2_memory_ids = db2_memory_health_list_l2_memory_ids,
       .count_memories = db2_memory_health_count_memories,
       .count_recent_conflicts = db2_memory_health_count_recent_conflicts,
       .health_record = db2_memory_health_record,
       .pool_status = production_pool_status,
       .embedding_refusals = production_embedding_refusals,
       .postgres_status = production_postgres_status,
       .reembed_status = production_reembed_status,
       .reembed_clear = db2_reembed_in_progress_clear,
       .reembed_clear_maintenance = db2_reembed_clear_maintenance,
       .embedder_serving_id = db2_embedder_serving_id,
       .dimension_reset = production_dimension_reset,
   };
   return &backend;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   if (response_len)
      *response_len = 0;
   if (!invocation || !response_len || !response_body ||
       (invocation->stage_id != AIMEE_DB2_STAGE_HEALTH &&
        invocation->stage_id != AIMEE_DB2_STAGE_LEVEL3_COUNT))
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   const aimee_db2_module_backend_t *backend = user_data;
   if (!backend)
      backend = production_backend();

   if (invocation->stage_id == AIMEE_DB2_STAGE_LEVEL3_COUNT)
   {
      if (aimee_db2_level3_count_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_LEVEL3_COUNT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->level3_count)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int raw_count = backend->level3_count();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_count < 0 || (uint32_t)raw_count > AIMEE_DB2_LEVEL3_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_level3_count_reply_encode((uint32_t)raw_count, response_body,
                                                 response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_level2_count_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_LEVEL2_COUNT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->level2_count)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int raw_count = backend->level2_count();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_count < 0 || (uint32_t)raw_count > AIMEE_DB2_LEVEL2_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_level2_count_reply_encode((uint32_t)raw_count, response_body,
                                                 response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_orphaned_l0_count_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_ORPHANED_L0_COUNT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->orphaned_l0_count)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int raw_count = backend->orphaned_l0_count();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_count < 0 || (uint32_t)raw_count > AIMEE_DB2_ORPHANED_L0_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_orphaned_l0_count_reply_encode((uint32_t)raw_count, response_body,
                                                      response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_total_count_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_TOTAL_COUNT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->total_count)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int64_t raw_count = backend->total_count();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_count < 0 || (uint64_t)raw_count > AIMEE_DB2_TOTAL_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_total_count_reply_encode((uint64_t)raw_count, response_body,
                                                response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      char source_session[AIMEE_DB2_SESSION_L2_COUNT_SESSION_MAX + 1u];
      if (aimee_db2_session_l2_count_request_decode(request_body, request_len, source_session,
                                                    sizeof(source_session)) == 0)
      {
         if (response_capacity < AIMEE_DB2_SESSION_L2_COUNT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->session_l2_count)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int raw_count = backend->session_l2_count(source_session);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_count < 0 || (uint32_t)raw_count > AIMEE_DB2_SESSION_L2_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_session_l2_count_reply_encode((uint32_t)raw_count, response_body,
                                                     response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      char key[AIMEE_DB2_KEY_EXISTS_KEY_MAX + 1u];
      if (aimee_db2_key_exists_request_decode(request_body, request_len, key, sizeof(key)) == 0)
      {
         if (response_capacity < AIMEE_DB2_KEY_EXISTS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->key_exists)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int raw_exists = backend->key_exists(key);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_exists < 0 || (uint32_t)raw_exists > AIMEE_DB2_KEY_EXISTS_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_key_exists_reply_encode((uint32_t)raw_exists, response_body,
                                               response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      char lookup_key[AIMEE_DB2_FIND_ID_BY_KEY_KIND_KEY_MAX + 1u];
      char lookup_kind[AIMEE_DB2_FIND_ID_BY_KEY_KIND_KIND_MAX + 1u];
      if (aimee_db2_find_id_by_key_kind_request_decode(request_body, request_len, lookup_key,
                                                       sizeof(lookup_key), lookup_kind,
                                                       sizeof(lookup_kind)) == 0)
      {
         if (response_capacity < AIMEE_DB2_FIND_ID_BY_KEY_KIND_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->find_id_by_key_kind)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int64_t raw_id = backend->find_id_by_key_kind(lookup_key, lookup_kind);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_id < 0 || (uint64_t)raw_id > AIMEE_DB2_FIND_ID_BY_KEY_KIND_ID_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         uint32_t found = raw_id > 0 ? 1u : 0u;
         if (aimee_db2_find_id_by_key_kind_reply_encode(found, (uint64_t)raw_id, response_body,
                                                        response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      char tier_pair_key[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_KEY_MAX + 1u];
      char tier_a[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_A_MAX + 1u];
      char tier_b[AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_TIER_B_MAX + 1u];
      if (aimee_db2_key_exists_in_tier_pair_request_decode(
              request_body, request_len, tier_pair_key, sizeof(tier_pair_key), tier_a,
              sizeof(tier_a), tier_b, sizeof(tier_b)) == 0)
      {
         if (response_capacity < AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->key_exists_in_tier_pair)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int raw_exists = backend->key_exists_in_tier_pair(tier_pair_key, tier_a, tier_b);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (raw_exists < 0 || raw_exists > (int)AIMEE_DB2_KEY_EXISTS_IN_TIER_PAIR_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_key_exists_in_tier_pair_reply_encode((uint32_t)raw_exists, response_body,
                                                            response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t effectiveness_memory_id = 0u;
      uint32_t effectiveness_has_value = 0u;
      double effectiveness_value = 0.0;
      if (aimee_db2_effectiveness_update_request_decode(
              request_body, request_len, &effectiveness_memory_id, &effectiveness_has_value,
              &effectiveness_value) == 0)
      {
         if (response_capacity < AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || (effectiveness_has_value && !backend->set_effectiveness) ||
             (!effectiveness_has_value && !backend->clear_effectiveness))
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int rc =
             effectiveness_has_value
                 ? backend->set_effectiveness((int64_t)effectiveness_memory_id, effectiveness_value)
                 : backend->clear_effectiveness((int64_t)effectiveness_memory_id);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (rc != 0 && rc != -1)
            return AIMEE_MODULE_STATUS_INTERNAL;
         uint32_t domain_result = rc == 0 ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_INVALID_STATE;
         if (aimee_db2_effectiveness_update_reply_encode(domain_result, response_body,
                                                         response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_EFFECTIVENESS_UPDATE_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_retention_enforce_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_RETENTION_ENFORCE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->retention_delete)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int restricted = backend->retention_delete(AIMEE_DB2_RETENTION_RESTRICTED,
                                                    (int)AIMEE_DB2_RETENTION_RESTRICTED_DAYS);
         int sensitive = backend->retention_delete(AIMEE_DB2_RETENTION_SENSITIVE,
                                                   (int)AIMEE_DB2_RETENTION_SENSITIVE_DAYS);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         int64_t deleted_count = (int64_t)restricted + (int64_t)sensitive;
         if (restricted < 0 || sensitive < 0 ||
             deleted_count > (int64_t)AIMEE_DB2_RETENTION_ENFORCE_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_retention_enforce_reply_encode((uint32_t)deleted_count, response_body,
                                                      response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_effectiveness_demote_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_EFFECTIVENESS_DEMOTE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->demote_effectiveness)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int demoted_count =
             backend->demote_effectiveness(AIMEE_DB2_EFFECTIVENESS_DEMOTE_THRESHOLD);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (demoted_count < 0 || demoted_count > (int)AIMEE_DB2_EFFECTIVENESS_DEMOTE_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_effectiveness_demote_reply_encode((uint32_t)demoted_count, response_body,
                                                         response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_effectiveness_stats_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_EFFECTIVENESS_STATS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->effectiveness_stats)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         double average = 0.0;
         int low_effectiveness = 0;
         int high_impact = 0;
         if (backend->effectiveness_stats(AIMEE_DB2_EFFECTIVENESS_STATS_LOW_THRESHOLD, &average,
                                          &low_effectiveness, &high_impact) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (low_effectiveness < 0 ||
             low_effectiveness > (int)AIMEE_DB2_EFFECTIVENESS_STATS_LOW_MAX || high_impact < 0 ||
             high_impact > (int)AIMEE_DB2_EFFECTIVENESS_STATS_HIGH_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         aimee_db2_effectiveness_stats_t stats = {
             .avg_effectiveness = average,
             .low_effectiveness_count = (uint32_t)low_effectiveness,
             .high_impact_count = (uint32_t)high_impact,
         };
         if (aimee_db2_effectiveness_stats_reply_encode(&stats, response_body, response_capacity,
                                                        response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_l2_memory_ids_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_L2_MEMORY_IDS_RESPONSE_MAX_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->list_l2_memory_ids)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int64_t rows[AIMEE_DB2_L2_MEMORY_IDS_MAX];
         int listed = backend->list_l2_memory_ids(rows, (int)AIMEE_DB2_L2_MEMORY_IDS_MAX);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (listed < 0 || listed > (int)AIMEE_DB2_L2_MEMORY_IDS_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         uint64_t memory_ids[AIMEE_DB2_L2_MEMORY_IDS_MAX];
         for (int index = 0; index < listed; index++)
         {
            if (rows[index] < (int64_t)AIMEE_DB2_L2_MEMORY_ID_MIN)
               return AIMEE_MODULE_STATUS_INTERNAL;
            memory_ids[index] = (uint64_t)rows[index];
         }
         if (aimee_db2_l2_memory_ids_reply_encode(memory_ids, (uint32_t)listed, response_body,
                                                  response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint32_t promotions = 0u, demotions = 0u, expirations = 0u;
      if (aimee_db2_health_record_request_decode(request_body, request_len, &promotions, &demotions,
                                                 &expirations) == 0)
      {
         if (response_capacity < AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->count_memories || !backend->count_recent_conflicts ||
             !backend->health_record)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int total_memories = backend->count_memories();
         int contradictions =
             backend->count_recent_conflicts(AIMEE_DB2_HEALTH_RECORD_CONFLICT_WINDOW_DAYS);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (total_memories < 0 || contradictions < 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         backend->health_record(total_memories, contradictions, (int)promotions, (int)demotions,
                                (int)expirations);
         if (aimee_db2_health_record_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_HEALTH_RECORD_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }

   if (aimee_db2_health_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->is_initialized || !backend->health_probe ||
          !backend->kb_health_probe)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int schema_ok = 0, have_pg_trgm = 0, kb_tables_ok = 0;
      if (backend->is_initialized() <= 0 || backend->health_probe(&schema_ok, &have_pg_trgm) != 0 ||
          backend->kb_health_probe(&kb_tables_ok) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;

      uint32_t flags = (schema_ok ? AIMEE_DB2_FLAG_SCHEMA : 0u) |
                       (have_pg_trgm ? AIMEE_DB2_FLAG_PG_TRGM : 0u) |
                       (kb_tables_ok ? AIMEE_DB2_FLAG_KB_TABLES : 0u);
      if (aimee_db2_health_response_encode(flags, response_body, response_capacity) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      *response_len = AIMEE_DB2_RESPONSE_LEN;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_embedding_dimension_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_EMBEDDING_DIMENSION_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->embedding_dimension)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int raw_dimension = backend->embedding_dimension();
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;

      uint32_t result = AIMEE_DB2_RESULT_INVALID_STATE;
      uint32_t dimension = 0u;
      if (raw_dimension >= (int)AIMEE_DB2_EMBEDDING_DIMENSION_MIN &&
          raw_dimension <= (int)AIMEE_DB2_EMBEDDING_DIMENSION_MAX)
      {
         result = AIMEE_DB2_RESULT_OK;
         dimension = (uint32_t)raw_dimension;
      }
      if (aimee_db2_embedding_dimension_reply_encode(result, dimension, response_body,
                                                     response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_pool_status_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_POOL_STATUS_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->pool_status)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_pool_status_t status = {0};
      uint32_t result =
          backend->pool_status(&status) == 0 ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_pool_status_reply_encode(result, result == AIMEE_DB2_RESULT_OK ? &status : NULL,
                                             response_body, response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_embedding_refusals_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_EMBEDDING_REFUSALS_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->embedding_refusals)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_embedding_refusals_t status = {0};
      uint32_t result = backend->embedding_refusals(&status) == 0 ? AIMEE_DB2_RESULT_OK
                                                                  : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_embedding_refusals_reply_encode(
              result, result == AIMEE_DB2_RESULT_OK ? &status : NULL, response_body,
              response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_postgres_status_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_POSTGRES_STATUS_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->postgres_status)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_postgres_status_t status = {0};
      uint32_t result = backend->postgres_status(&status) == 0 ? AIMEE_DB2_RESULT_OK
                                                               : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_postgres_status_reply_encode(
              result, result == AIMEE_DB2_RESULT_OK ? &status : NULL, response_body,
              response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_reembed_status_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_REEMBED_STATUS_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->reembed_status)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_reembed_status_t status = {0};
      int backend_result = backend->reembed_status(&status);
      uint32_t result = backend_result == 1   ? AIMEE_DB2_RESULT_OK
                        : backend_result == 0 ? AIMEE_DB2_RESULT_NOT_FOUND
                                              : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_reembed_status_reply_encode(
              result, result == AIMEE_DB2_RESULT_OK ? &status : NULL, response_body,
              response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_embedder_serving_id_request_decode(request_body, request_len) == 0)
   {
      if (response_capacity < AIMEE_DB2_EMBEDDER_SERVING_ID_RESPONSE_MAX_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->embedder_serving_id)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      const char *serving_id = backend->embedder_serving_id();
      uint32_t result = serving_id ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_INVALID_STATE;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_embedder_serving_id_reply_encode(
              result, result == AIMEE_DB2_RESULT_OK ? serving_id : NULL, response_body,
              response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   uint32_t maintenance_force = 0u;
   if (aimee_db2_reembed_clear_maintenance_request_decode(request_body, request_len,
                                                          &maintenance_force) == 0)
   {
      if (response_capacity < AIMEE_DB2_REEMBED_MAINT_CLEAR_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->reembed_clear_maintenance)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int was = 0, recorded = 0, running = 0;
      int backend_result =
          backend->reembed_clear_maintenance((int)maintenance_force, &was, &recorded, &running);
      aimee_db2_reembed_clear_maintenance_t status = {
          .was_in_progress = was >= 0 ? (uint32_t)was : 2u,
          .recorded_dimension = recorded >= 0 ? (uint32_t)recorded : UINT32_MAX,
          .running_dimension = running >= 0 ? (uint32_t)running : UINT32_MAX,
      };
      uint32_t result = AIMEE_DB2_RESULT_INVALID_STATE;
      const aimee_db2_reembed_clear_maintenance_t *reply_status = NULL;
      if (aimee_db2_reembed_clear_maintenance_valid(&status))
      {
         if (backend_result == 0)
         {
            result = AIMEE_DB2_RESULT_OK;
            reply_status = &status;
         }
         else if (backend_result == -1 && status.recorded_dimension > 0u &&
                  status.recorded_dimension != status.running_dimension)
         {
            result = AIMEE_DB2_RESULT_CONFLICT;
            reply_status = &status;
         }
      }
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_reembed_clear_maintenance_reply_encode(result, reply_status, response_body,
                                                           response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   uint32_t reset_target = 0u, reset_force = 0u, reset_dry_run = 0u;
   if (aimee_db2_dimension_reset_request_decode(request_body, request_len, &reset_target,
                                                &reset_force, &reset_dry_run) == 0)
   {
      if (response_capacity < AIMEE_DB2_DIMENSION_RESET_RESPONSE_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!backend || !backend->dimension_reset)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      aimee_db2_dimension_reset_t status = {0};
      int backend_result =
          backend->dimension_reset(reset_target, reset_force, reset_dry_run, &status);
      uint32_t result = backend_result == 0    ? AIMEE_DB2_RESULT_OK
                        : backend_result == -2 ? AIMEE_DB2_RESULT_CONFLICT
                        : backend_result == -3 ? AIMEE_DB2_RESULT_DENIED
                                               : AIMEE_DB2_RESULT_INVALID_STATE;
      const aimee_db2_dimension_reset_t *reply_status =
          result == AIMEE_DB2_RESULT_INVALID_STATE ? NULL : &status;
      if (reply_status && !aimee_db2_dimension_reset_valid(reply_status))
      {
         result = AIMEE_DB2_RESULT_INVALID_STATE;
         reply_status = NULL;
      }
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (aimee_db2_dimension_reset_reply_encode(result, reply_status, response_body,
                                                 response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }

   if (aimee_db2_reembed_clear_request_decode(request_body, request_len) != 0 ||
       response_capacity < AIMEE_DB2_REEMBED_CLEAR_RESPONSE_LEN)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (!backend || !backend->reembed_clear)
      return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
   uint32_t result =
       backend->reembed_clear() == 0 ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_INVALID_STATE;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (aimee_db2_reembed_clear_reply_encode(result, response_body, response_capacity,
                                            response_len) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   return AIMEE_MODULE_STATUS_OK;
}
