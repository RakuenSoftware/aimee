#include "module_adapter.h"

#include <aimee/db2/module_api.h>

#include "c/db2.h"
#include "c/db2_internal.h"
#include "c/db2_pool.h"
#include "c/code_index.h"
#include "c/entity_edges.h"
#include "c/kind_lifecycle.h"
#include "c/memory_health.h"
#include "c/memory_lifecycle.h"
#include "c/memory_payload.h"
#include "c/memory_promotion.h"
#include "c/memory_query.h"
#include "c/memory_relations.h"

static int production_health_counters(int promote_use_count, double promote_confidence,
                                      aimee_db2_health_counters_t *counters)
{
   if (!counters)
      return -1;
   db2_memory_health_query_counters_t raw = {0};
   if (db2_memory_health_query_counters(promote_use_count, promote_confidence, &raw) != 0)
      return -1;
   const int values[] = {
       raw.cycles,
       raw.total_contradictions,
       raw.total_promotions,
       raw.total_demotions,
       raw.total_expirations,
       raw.new_memories,
       raw.l1_eligible,
       raw.l2_total,
       raw.l2_stale_30_days,
   };
   for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++)
      if (values[index] < 0 || values[index] > (int)AIMEE_DB2_HEALTH_COUNTERS_MAX)
         return -1;
   *counters = (aimee_db2_health_counters_t){
       .cycles = (uint32_t)raw.cycles,
       .total_contradictions = (uint32_t)raw.total_contradictions,
       .total_promotions = (uint32_t)raw.total_promotions,
       .total_demotions = (uint32_t)raw.total_demotions,
       .total_expirations = (uint32_t)raw.total_expirations,
       .new_memories = (uint32_t)raw.new_memories,
       .l1_eligible = (uint32_t)raw.l1_eligible,
       .l2_total = (uint32_t)raw.l2_total,
       .l2_stale_30_days = (uint32_t)raw.l2_stale_30_days,
   };
   return 0;
}

static int production_stats_counts(aimee_db2_memory_stats_t *stats)
{
   if (!stats)
      return -1;
   memory_stats_t raw;
   memset(&raw, 0, sizeof(raw));
   if (db2_memory_stats_counts(&raw) != 0)
      return -1;
   if ((int)AIMEE_DB2_STATS_COUNTS_TIERS !=
           (int)(sizeof(raw.tier_counts) / sizeof(raw.tier_counts[0])) ||
       (int)AIMEE_DB2_STATS_COUNTS_KINDS !=
           (int)(sizeof(raw.kind_counts) / sizeof(raw.kind_counts[0])))
      return -1;
   *stats = (aimee_db2_memory_stats_t){0};
   for (uint32_t index = 0u; index < AIMEE_DB2_STATS_COUNTS_TIERS; index++)
   {
      if (raw.tier_counts[index] < 0 || raw.tier_counts[index] > (int)AIMEE_DB2_STATS_COUNTS_MAX)
         return -1;
      stats->tier_counts[index] = (uint32_t)raw.tier_counts[index];
   }
   for (uint32_t index = 0u; index < AIMEE_DB2_STATS_COUNTS_KINDS; index++)
   {
      if (raw.kind_counts[index] < 0 || raw.kind_counts[index] > (int)AIMEE_DB2_STATS_COUNTS_MAX)
         return -1;
      stats->kind_counts[index] = (uint32_t)raw.kind_counts[index];
   }
   if (raw.total < 0 || raw.total > (int)AIMEE_DB2_STATS_COUNTS_MAX || raw.conflicts < 0 ||
       raw.conflicts > (int)AIMEE_DB2_STATS_COUNTS_MAX)
      return -1;
   stats->total = (uint32_t)raw.total;
   stats->conflicts = (uint32_t)raw.conflicts;
   return 0;
}

static int production_list_kinds_in_tier(const char *tier, char (*kinds)[16], int max)
{
   if (!kinds || max <= 0)
      return 0;
   db2_memory_promotion_kind_t rows[AIMEE_DB2_EXPIRE_KINDS_MAX];
   if (max > (int)(sizeof(rows) / sizeof(rows[0])))
      max = (int)(sizeof(rows) / sizeof(rows[0]));
   int found = db2_memory_promotion_list_kinds_in_tier(tier, rows, max);
   if (found < 0)
      return -1;
   if (found > max)
      found = max;
   for (int index = 0; index < found; index++)
      snprintf(kinds[index], sizeof(kinds[index]), "%s", rows[index].kind);
   return found;
}

static void production_now_utc(char *buf, size_t len)
{
   db2_now_utc(buf, len);
}

static int production_kind_demote_policy(const char *kind, double *confidence, int *days)
{
   if (!confidence || !days)
      return -1;
   kind_lifecycle_t lifecycle;
   memset(&lifecycle, 0, sizeof(lifecycle));
   /* A miss fills the default fact lifecycle, which is still a usable policy. */
   (void)db2_kind_lifecycle_load(kind, &lifecycle);
   *confidence = lifecycle.demote_confidence;
   /* Resistance stretches the idle window a kind must sit through before it
    * demotes; it lives with the thresholds it scales. */
   *days = (int)(lifecycle.demote_days * lifecycle.demotion_resistance);
   return 0;
}

static int production_kind_expire_days(const char *kind)
{
   kind_lifecycle_t lifecycle;
   memset(&lifecycle, 0, sizeof(lifecycle));
   /* A miss fills the default fact lifecycle, which is still a usable window. */
   (void)db2_kind_lifecycle_load(kind, &lifecycle);
   return lifecycle.expire_days;
}

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

/* The backend takes a rejection reason and discards it -- the parameter is
 * explicitly unused and nothing above it persists one either. The wire
 * operation therefore carries no reason, and this shim is what makes that
 * visible instead of quietly forwarding an empty string as though a rationale
 * had been recorded. */
static int production_reject(int64_t memory_id)
{
   return db2_memory_reject(memory_id, NULL);
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
       .prune_health = db2_memory_health_prune_old,
       .prune_contradictions = db2_memory_health_prune_old_contradictions,
       .health_counters = production_health_counters,
       .stats_counts = production_stats_counts,
       .delete_l0_provenance = db2_memory_promotion_delete_l0_provenance,
       .delete_l0 = db2_memory_promotion_delete_l0,
       .list_kinds_in_tier = production_list_kinds_in_tier,
       .kind_expire_days = production_kind_expire_days,
       .delete_stale_l1_provenance = db2_memory_promotion_delete_stale_l1_provenance,
       .delete_stale_l1 = db2_memory_promotion_delete_stale_l1,
       .now_utc = production_now_utc,
       .kind_demote_policy = production_kind_demote_policy,
       .demote_kind = db2_memory_promotion_demote_kind,
       .demote_cascade = db2_memory_promotion_demote_cascade,
       .promote_stable = db2_memory_promotion_promote_stable_l2_to_l3,
       .reclassify_directives = db2_memory_promotion_reclassify_directives,
       .record_l4_approval = db2_memory_promotion_record_l4_approval,
       .prune_orphaned_l0 = db2_memory_prune_orphaned_l0,
       .lifecycle_sweep_expired = db2_memory_lifecycle_sweep_expired,
       .demote_id = db2_memory_promotion_demote_id,
       .has_workspace_tag = db2_memory_has_any_workspace_tag,
       .delete_row = db2_memory_delete_row,
       .touch = db2_memory_touch,
       .link_delete = db2_memory_link_delete,
       .valid_at = db2_memory_valid_at,
       .has_scope_type = db2_memory_has_scope_type,
       .reject = production_reject,
       .update_content = db2_memory_update_content,
       .decay_confidence = db2_memory_decay_confidence,
       .workspace_tag_insert = db2_memory_workspace_tag_insert,
       .set_cognified_kind = db2_memory_set_cognified_kind,
       .set_source_session = db2_memory_set_source_session,
       .negation_tokens_update = db2_memory_negation_tokens_update,
       .get_content = db2_memory_get_content,
       .get_source_session = db2_memory_get_source_session,
       .pick_first_temporal_ref = db2_memory_pick_first_temporal_ref,
       .count_and_max_updated = db2_memory_count_and_max_updated,
       .entity_edge_prune_orphans = db2_entity_edge_prune_orphans,
       .entity_edge_normalize_weights = db2_entity_edge_normalize_weights,
       .project_count = db2_code_index_project_count,
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
   /* Operation ids are unique per family, not globally, so the stage id is
    * the outer discriminator: an index-family request and a lifecycle-family
    * request can carry the same operation number and are told apart only by
    * the stage they arrive on. Every accepted family needs its own branch
    * below, and nothing may fall through to another family's decoders. */
   if (!invocation || !response_len || !response_body ||
       (invocation->stage_id != AIMEE_DB2_STAGE_HEALTH &&
        invocation->stage_id != AIMEE_DB2_STAGE_LEVEL3_COUNT &&
        invocation->stage_id != AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS &&
        invocation->stage_id != AIMEE_DB2_STAGE_ENTITY_EDGE_NORMALIZE_WEIGHTS &&
        invocation->stage_id != AIMEE_DB2_STAGE_PROJECT_COUNT))
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
      if (aimee_db2_health_retention_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_HEALTH_RETENTION_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->prune_health || !backend->prune_contradictions)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int snapshots = backend->prune_health(AIMEE_DB2_HEALTH_RETENTION_SNAPSHOT_DAYS);
         int contradictions =
             backend->prune_contradictions(AIMEE_DB2_HEALTH_RETENTION_CONTRADICTION_DAYS);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (snapshots < 0 || snapshots > (int)AIMEE_DB2_HEALTH_RETENTION_MAX ||
             contradictions < 0 || contradictions > (int)AIMEE_DB2_HEALTH_RETENTION_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_health_retention_reply_encode((uint32_t)snapshots, (uint32_t)contradictions,
                                                     response_body, response_capacity,
                                                     response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_health_counters_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->health_counters)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         aimee_db2_health_counters_t counters = {0};
         if (backend->health_counters(AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_USE_COUNT,
                                      AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_CONFIDENCE, &counters) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_health_counters_reply_encode(&counters, response_body, response_capacity,
                                                    response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_stats_counts_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->stats_counts)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         aimee_db2_memory_stats_t stats = {0};
         if (backend->stats_counts(&stats) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_stats_counts_reply_encode(&stats, response_body, response_capacity,
                                                 response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_expire_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_EXPIRE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->delete_l0_provenance || !backend->delete_l0 ||
             !backend->list_kinds_in_tier || !backend->kind_expire_days ||
             !backend->delete_stale_l1_provenance || !backend->delete_stale_l1)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;

         /* Provenance goes first in each stage so no row outlives its record. */
         backend->delete_l0_provenance();
         int level0 = backend->delete_l0();
         if (level0 < 0 || level0 > (int)AIMEE_DB2_EXPIRE_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;

         char kinds[AIMEE_DB2_EXPIRE_KINDS_MAX][16];
         int kind_count = backend->list_kinds_in_tier(AIMEE_DB2_EXPIRE_STALE_TIER, kinds,
                                                      (int)AIMEE_DB2_EXPIRE_KINDS_MAX);
         if (kind_count < 0 || kind_count > (int)AIMEE_DB2_EXPIRE_KINDS_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;

         long stale = 0;
         for (int index = 0; index < kind_count; index++)
         {
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            int days = backend->kind_expire_days(kinds[index]);
            if (days <= 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            char window[16];
            if (snprintf(window, sizeof(window), "-%d", days) >= (int)sizeof(window))
               return AIMEE_MODULE_STATUS_INTERNAL;
            backend->delete_stale_l1_provenance(kinds[index], window);
            int deleted = backend->delete_stale_l1(kinds[index], window);
            if (deleted < 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            stale += deleted;
            if (stale > (long)AIMEE_DB2_EXPIRE_MAX)
               return AIMEE_MODULE_STATUS_INTERNAL;
         }
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_expire_reply_encode((uint32_t)level0, (uint32_t)stale, response_body,
                                           response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_demote_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_DEMOTE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->now_utc || !backend->list_kinds_in_tier ||
             !backend->kind_demote_policy || !backend->demote_kind || !backend->demote_cascade)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;

         /* One stamp for the whole action: the cascade matches dependants of
          * exactly the rows this call demoted. */
         char stamp[32] = "";
         backend->now_utc(stamp, sizeof(stamp));
         if (!stamp[0])
            return AIMEE_MODULE_STATUS_INTERNAL;

         char kinds[AIMEE_DB2_DEMOTE_KINDS_MAX][16];
         int kind_count = backend->list_kinds_in_tier(AIMEE_DB2_DEMOTE_TIER, kinds,
                                                      (int)AIMEE_DB2_DEMOTE_KINDS_MAX);
         if (kind_count < 0 || kind_count > (int)AIMEE_DB2_DEMOTE_KINDS_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;

         long demoted = 0;
         for (int index = 0; index < kind_count; index++)
         {
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            double confidence = 0.0;
            int days = 0;
            if (backend->kind_demote_policy(kinds[index], &confidence, &days) != 0 || days <= 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            char window[16];
            if (snprintf(window, sizeof(window), "-%d", days) >= (int)sizeof(window))
               return AIMEE_MODULE_STATUS_INTERNAL;
            int changed = backend->demote_kind(stamp, kinds[index], confidence, window);
            if (changed < 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            demoted += changed;
            if (demoted > (long)AIMEE_DB2_DEMOTE_MAX)
               return AIMEE_MODULE_STATUS_INTERNAL;
         }

         /* Nothing demoted means nothing to cascade to. */
         long cascaded = 0;
         if (demoted > 0)
         {
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            int changed = backend->demote_cascade(stamp);
            if (changed < 0 || changed > (int)AIMEE_DB2_DEMOTE_MAX)
               return AIMEE_MODULE_STATUS_INTERNAL;
            cascaded = changed;
         }
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_demote_reply_encode((uint32_t)demoted, (uint32_t)cascaded, response_body,
                                           response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_promote_stable_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_PROMOTE_STABLE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->now_utc || !backend->promote_stable)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         char stamp[32] = "";
         backend->now_utc(stamp, sizeof(stamp));
         if (!stamp[0])
            return AIMEE_MODULE_STATUS_INTERNAL;
         int promoted = backend->promote_stable(stamp);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (promoted < 0 || promoted > (int)AIMEE_DB2_PROMOTE_STABLE_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_promote_stable_reply_encode((uint32_t)promoted, response_body,
                                                   response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint32_t require_approval = 0u;
      if (aimee_db2_reclassify_directives_request_decode(request_body, request_len,
                                                         &require_approval) == 0)
      {
         if (response_capacity < AIMEE_DB2_RECLASSIFY_DIRECTIVES_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->reclassify_directives)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int reclassified = backend->reclassify_directives((int)require_approval);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (reclassified < 0 || reclassified > (int)AIMEE_DB2_RECLASSIFY_DIRECTIVES_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_reclassify_directives_reply_encode((uint32_t)reclassified, response_body,
                                                          response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t approval_memory_id = 0u;
      char approver[AIMEE_DB2_RECORD_L4_APPROVAL_APPROVER_MAX + 1];
      char note[AIMEE_DB2_RECORD_L4_APPROVAL_NOTE_MAX + 1];
      if (aimee_db2_record_l4_approval_request_decode(request_body, request_len,
                                                      &approval_memory_id, approver,
                                                      sizeof(approver), note, sizeof(note)) == 0)
      {
         if (response_capacity < AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->record_l4_approval)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         if (backend->record_l4_approval((int64_t)approval_memory_id, approver, note) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_record_l4_approval_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_prune_orphaned_l0_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_PRUNE_ORPHANED_L0_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->prune_orphaned_l0)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The backend returns -1 on connection or statement failure and the
          * affected-row count otherwise, so a negative value is a fault rather
          * than an empty sweep. */
         int deleted = backend->prune_orphaned_l0();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (deleted < 0 || (uint32_t)deleted > AIMEE_DB2_PRUNE_ORPHANED_L0_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_prune_orphaned_l0_reply_encode((uint32_t)deleted, response_body,
                                                      response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_lifecycle_sweep_expired_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->lifecycle_sweep_expired)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* This backend reports a connection or statement failure as zero
          * rather than a negative value, so a fault and an empty sweep are the
          * same answer here. That is the existing contract of the symbol and
          * of the already-migrated promote_stable and demote operations; it is
          * preserved rather than quietly changed under a bus migration. A
          * negative value is still refused in case that contract is tightened. */
         int archived = backend->lifecycle_sweep_expired();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (archived < 0 || (uint32_t)archived > AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_lifecycle_sweep_expired_reply_encode((uint32_t)archived, response_body,
                                                            response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t decay_memory_id = 0u;
      if (aimee_db2_demote_id_request_decode(request_body, request_len, &decay_memory_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_DEMOTE_ID_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->demote_id)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The predicate is an equality on the primary key, so a count above
          * one means the statement no longer matches the reviewed operation
          * and the reply would misreport how much of the tier moved. */
         int demoted = backend->demote_id((int64_t)decay_memory_id);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (demoted < 0 || (uint32_t)demoted > AIMEE_DB2_DEMOTE_ID_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_demote_id_reply_encode((uint32_t)demoted, response_body, response_capacity,
                                              response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t tag_memory_id = 0u;
      if (aimee_db2_has_workspace_tag_request_decode(request_body, request_len, &tag_memory_id) ==
          0)
      {
         if (response_capacity < AIMEE_DB2_HAS_WORKSPACE_TAG_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->has_workspace_tag)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The probe is LIMIT 1, so anything but zero or one means the
          * statement drifted and the flag would no longer be a Boolean. */
         int tagged = backend->has_workspace_tag((int64_t)tag_memory_id);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (tagged < 0 || (uint32_t)tagged > AIMEE_DB2_HAS_WORKSPACE_TAG_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_has_workspace_tag_reply_encode((uint32_t)tagged, response_body,
                                                      response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t delete_memory_id = 0u;
      if (aimee_db2_delete_row_request_decode(request_body, request_len, &delete_memory_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_DELETE_ROW_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->delete_row)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Equality on the primary key: the row existed or it did not, so a
          * wider count means the statement no longer matches the operation. */
         int deleted = backend->delete_row((int64_t)delete_memory_id);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (deleted < 0 || (uint32_t)deleted > AIMEE_DB2_DELETE_ROW_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_delete_row_reply_encode((uint32_t)deleted, response_body, response_capacity,
                                               response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t touch_memory_id = 0u;
      if (aimee_db2_touch_request_decode(request_body, request_len, &touch_memory_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_TOUCH_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->touch)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The backend reports both an absent row and a statement failure as
          * non-zero, so this is an acknowledgement rather than evidence the
          * memory existed. */
         if (backend->touch((int64_t)touch_memory_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_touch_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_TOUCH_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t delete_link_id = 0u;
      if (aimee_db2_link_delete_request_decode(request_body, request_len, &delete_link_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_LINK_DELETE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->link_delete)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         if (backend->link_delete((int64_t)delete_link_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_link_delete_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_LINK_DELETE_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t valid_memory_id = 0u;
      char valid_as_of[AIMEE_DB2_VALID_AT_AS_OF_MAX + 1];
      if (aimee_db2_valid_at_request_decode(request_body, request_len, &valid_memory_id,
                                            valid_as_of, sizeof(valid_as_of)) == 0)
      {
         if (response_capacity < AIMEE_DB2_VALID_AT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->valid_at)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int verdict = backend->valid_at((int64_t)valid_memory_id, valid_as_of);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         /* A negative verdict means the comparison could not be evaluated, not
          * that the memory was out of force. It is carried as invalid_state so
          * a caller cannot read an unanswered question as a "no". */
         uint32_t domain = (verdict < 0) ? AIMEE_DB2_RESULT_INVALID_STATE : AIMEE_DB2_RESULT_OK;
         uint32_t in_force = (verdict > 0) ? 1u : 0u;
         if (aimee_db2_valid_at_reply_encode(domain, in_force, response_body, response_capacity,
                                             response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t scope_memory_id = 0u;
      char scope_kind[AIMEE_DB2_HAS_SCOPE_TYPE_SCOPE_MAX + 1];
      if (aimee_db2_has_scope_type_request_decode(request_body, request_len, &scope_memory_id,
                                                  scope_kind, sizeof(scope_kind)) == 0)
      {
         if (response_capacity < AIMEE_DB2_HAS_SCOPE_TYPE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->has_scope_type)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* LIMIT 1, so anything but zero or one means the probe changed shape. */
         int present = backend->has_scope_type((int64_t)scope_memory_id, scope_kind);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (present < 0 || (uint32_t)present > AIMEE_DB2_HAS_SCOPE_TYPE_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_has_scope_type_reply_encode((uint32_t)present, response_body,
                                                   response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t reject_memory_id = 0u;
      if (aimee_db2_reject_request_decode(request_body, request_len, &reject_memory_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_REJECT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->reject)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         if (backend->reject((int64_t)reject_memory_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_reject_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_REJECT_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t content_memory_id = 0u;
      static _Thread_local char new_content[AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX + 1];
      if (aimee_db2_update_content_request_decode(request_body, request_len, &content_memory_id,
                                                  new_content, sizeof(new_content)) == 0)
      {
         if (response_capacity < AIMEE_DB2_UPDATE_CONTENT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->update_content)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int updated = backend->update_content((int64_t)content_memory_id, new_content);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (updated < 0 || (uint32_t)updated > AIMEE_DB2_UPDATE_CONTENT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_update_content_reply_encode((uint32_t)updated, response_body,
                                                   response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t decay_confidence_memory_id = 0u;
      if (aimee_db2_decay_confidence_request_decode(request_body, request_len,
                                                    &decay_confidence_memory_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_DECAY_CONFIDENCE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->decay_confidence)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The backend returns void: it cannot report whether the row existed
          * or whether the statement ran, so there is nothing to check here and
          * nothing honest to put in the reply beyond the acknowledgement. */
         backend->decay_confidence((int64_t)decay_confidence_memory_id);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_decay_confidence_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_DECAY_CONFIDENCE_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t tag_insert_memory_id = 0u;
      char tag_workspace[AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX + 1];
      if (aimee_db2_workspace_tag_insert_request_decode(request_body, request_len,
                                                        &tag_insert_memory_id, tag_workspace,
                                                        sizeof(tag_workspace)) == 0)
      {
         if (response_capacity < AIMEE_DB2_WORKSPACE_TAG_INSERT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->workspace_tag_insert)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Void backend over an ON CONFLICT DO NOTHING insert: a repeat is a
          * no-op and a fault is silent, so the acknowledgement is all there is
          * to report and a count would have to be invented. */
         backend->workspace_tag_insert((int64_t)tag_insert_memory_id, tag_workspace);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_workspace_tag_insert_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_WORKSPACE_TAG_INSERT_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t cognified_memory_id = 0u;
      char cognified_kind[AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX + 1];
      if (aimee_db2_set_cognified_kind_request_decode(request_body, request_len,
                                                      &cognified_memory_id, cognified_kind,
                                                      sizeof(cognified_kind)) == 0)
      {
         if (response_capacity < AIMEE_DB2_SET_COGNIFIED_KIND_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->set_cognified_kind)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         backend->set_cognified_kind((int64_t)cognified_memory_id, cognified_kind);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_set_cognified_kind_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_SET_COGNIFIED_KIND_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t session_memory_id = 0u;
      char assigned_session[AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX + 1];
      if (aimee_db2_set_source_session_request_decode(request_body, request_len, &session_memory_id,
                                                      assigned_session,
                                                      sizeof(assigned_session)) == 0)
      {
         if (response_capacity < AIMEE_DB2_SET_SOURCE_SESSION_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->set_source_session)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* An empty session reaches the backend as an empty string and clears
          * the column; it is not filtered out here. */
         backend->set_source_session((int64_t)session_memory_id, assigned_session);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_set_source_session_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_SET_SOURCE_SESSION_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t negation_memory_id = 0u;
      static _Thread_local char negation_tokens[AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX + 1];
      if (aimee_db2_negation_tokens_update_request_decode(request_body, request_len,
                                                          &negation_memory_id, negation_tokens,
                                                          sizeof(negation_tokens)) == 0)
      {
         if (response_capacity < AIMEE_DB2_NEGATION_TOKENS_UPDATE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->negation_tokens_update)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* An empty token set reaches the backend and clears the column: a
          * memory with no negations is a real extraction result. */
         backend->negation_tokens_update((int64_t)negation_memory_id, negation_tokens);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_negation_tokens_update_reply_encode(response_body, response_capacity) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         *response_len = AIMEE_DB2_NEGATION_TOKENS_UPDATE_RESPONSE_LEN;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t content_read_memory_id = 0u;
      if (aimee_db2_get_content_request_decode(request_body, request_len,
                                               &content_read_memory_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_GET_CONTENT_ERROR_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->get_content)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         static _Thread_local char read_content[AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1];
         read_content[0] = '\0';
         int hit = backend->get_content((int64_t)content_read_memory_id, read_content,
                                        (int)sizeof(read_content));
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         /* A miss carries no content: reporting it as an empty payload would
          * be indistinguishable from a row that genuinely holds "". */
         if (aimee_db2_get_content_reply_encode(
                 hit ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_NOT_FOUND, hit ? read_content : NULL,
                 response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t session_read_memory_id = 0u;
      if (aimee_db2_get_source_session_request_decode(request_body, request_len,
                                                      &session_read_memory_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_GET_SOURCE_SESSION_ERROR_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->get_source_session)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         char read_session[AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX + 1];
         read_session[0] = '\0';
         int found = backend->get_source_session((int64_t)session_read_memory_id, read_session,
                                                 (int)sizeof(read_session)) == 0;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         /* The backend succeeds only for a non-empty session, so a success
          * with an empty buffer would be a contract violation rather than a
          * blank session -- refuse it instead of encoding an empty ok. */
         if (found && read_session[0] == '\0')
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_get_source_session_reply_encode(
                 found ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_NOT_FOUND,
                 found ? read_session : NULL, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t temporal_ref_memory_id = 0u;
      if (aimee_db2_pick_first_temporal_ref_request_decode(request_body, request_len,
                                                           &temporal_ref_memory_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_ERROR_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->pick_first_temporal_ref)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         char picked_ref[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX + 1];
         picked_ref[0] = '\0';
         int picked = backend->pick_first_temporal_ref((int64_t)temporal_ref_memory_id, picked_ref,
                                                       (int)sizeof(picked_ref));
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         /* The backend reports a hit only for a non-empty key, so a hit with
          * an empty buffer is a broken contract rather than a blank key. */
         if (picked && picked_ref[0] == '\0')
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_pick_first_temporal_ref_reply_encode(
                 picked ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_NOT_FOUND,
                 picked ? picked_ref : NULL, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_count_and_max_updated_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_COUNT_AND_MAX_UPDATED_ERROR_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->count_and_max_updated)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         int corpus_count = 0;
         char corpus_stamp[AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX + 1];
         corpus_stamp[0] = '\0';
         int computed =
             backend->count_and_max_updated(&corpus_count, corpus_stamp, (int)sizeof(corpus_stamp));
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (computed && (corpus_count < 0 ||
                          (uint32_t)corpus_count > AIMEE_DB2_COUNT_AND_MAX_UPDATED_COUNT_MAX))
            return AIMEE_MODULE_STATUS_INTERNAL;
         /* The aggregate always yields a row when it runs, so a failure means
          * it did not run -- invalid_state, carrying neither number. */
         if (aimee_db2_count_and_max_updated_reply_encode(
                 computed ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_INVALID_STATE,
                 computed ? (uint32_t)corpus_count : 0u, computed ? corpus_stamp : NULL,
                 response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }

   if (invocation->stage_id == AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS ||
       invocation->stage_id == AIMEE_DB2_STAGE_ENTITY_EDGE_NORMALIZE_WEIGHTS ||
       invocation->stage_id == AIMEE_DB2_STAGE_PROJECT_COUNT)
   {
      if (aimee_db2_entity_edge_prune_orphans_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->entity_edge_prune_orphans)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The backend reports both an empty prune and a statement failure as
          * zero, the same limitation the memory sweeps carry. */
         int pruned = backend->entity_edge_prune_orphans();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (pruned < 0 || (uint32_t)pruned > AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_entity_edge_prune_orphans_reply_encode((uint32_t)pruned, response_body,
                                                              response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_entity_edge_normalize_weights_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->entity_edge_normalize_weights)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Zero here means a converged graph, not a failed pass: the statement
          * excludes rows that already hold their normalised weight. */
         int normalized = backend->entity_edge_normalize_weights();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (normalized < 0 ||
             (uint32_t)normalized > AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_entity_edge_normalize_weights_reply_encode(
                 (uint32_t)normalized, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_project_count_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_PROJECT_COUNT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->project_count)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The backend reports both an empty index and a failed statement as
          * zero, so this count cannot distinguish them -- the same limitation
          * the memory counts carry. */
         int projects = backend->project_count();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (projects < 0 || (uint32_t)projects > AIMEE_DB2_PROJECT_COUNT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_project_count_reply_encode((uint32_t)projects, response_body,
                                                  response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
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
