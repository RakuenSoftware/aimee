#include "module_adapter.h"

#include <aimee/db2/module_api.h>

#include "c/db2.h"
#include "c/db2_internal.h"
#include "c/db2_pool.h"
#include "c/anti_patterns.h"
#include "c/artifacts.h"
#include "c/code_index.h"
#include "c/code_index_ops.h"
#include "c/cross_repo_build.h"
#include "c/cross_repo_identity.h"
#include "c/cross_repo_route.h"
#include "c/curiosity.h"
#include "c/db2_learning.h"
#include "c/decision_log.h"
#include "c/entity_edges.h"
#include "c/epistemic_directives.h"
#include "c/evidence_vectors.h"
#include "c/kb_docs.h"
#include "c/kb_payload.h"
#include "c/kb_releases.h"
#include "c/kb_runtime_state.h"
#include "c/kb_service_backend.h"
#include "c/kind_lifecycle.h"
#include "c/learning_synth_ops.h"
#include "c/memory_health.h"
#include "c/memory_lifecycle.h"
#include "c/memory_payload.h"
#include "c/memory_promotion.h"
#include "c/memory_query.h"
#include "c/memory_scope_query.h"
#include "c/memory_relations.h"
#include "c/mining.h"
#include "c/prospective_memories.h"
#include "c/rel_types_store.h"
#include "c/rules.h"
#include "c/tasks.h"
#include "c/trace_mining.h"

#include <stdlib.h>
#include <string.h>

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

/* Read a scoped row list and hand back only the identifiers.
 *
 * The scope is set and then put back the way it was found rather than cleared:
 * while the adapter still runs in the caller's process it shares the same
 * thread-local as the caller, and clearing it would silently rescope whatever
 * the caller does next. */
static int production_scoped_ids(int (*read)(memory_t *out, int max), int scope_active,
                                 int include_all, const char *workspace, const char *project,
                                 int64_t *out, int max)
{
   if (!read || !out || max <= 0)
      return -1;
   memory_t *rows = calloc((size_t)max, sizeof(*rows));
   if (!rows)
      return -1;

   db2_memory_scope_context_t saved;
   memset(&saved, 0, sizeof(saved));
   db2_memory_scope_context_get(&saved);
   if (scope_active)
      db2_memory_scope_context_set(workspace, project, include_all);
   else
      db2_memory_scope_context_clear();

   int listed = read(rows, max);

   if (saved.active)
      db2_memory_scope_context_set(saved.workspace, saved.project, saved.include_all);
   else
      db2_memory_scope_context_clear();

   if (listed > max)
      listed = max;
   for (int index = 0; index < listed; index++)
      out[index] = rows[index].id;
   free(rows);
   return listed;
}

/* Same shape as production_scoped_ids, with a search term and an explicit
 * limit the statement binds. */
static int
production_scoped_term_ids(int (*read)(const char *term, int limit, memory_t *out, int max),
                           const char *term, int limit, int scope_active, int include_all,
                           const char *workspace, const char *project, int64_t *out, int max)
{
   if (!read || !term || !out || max <= 0 || limit <= 0)
      return -1;
   memory_t *rows = calloc((size_t)max, sizeof(*rows));
   if (!rows)
      return -1;

   db2_memory_scope_context_t saved;
   memset(&saved, 0, sizeof(saved));
   db2_memory_scope_context_get(&saved);
   if (scope_active)
      db2_memory_scope_context_set(workspace, project, include_all);
   else
      db2_memory_scope_context_clear();

   int listed = read(term, limit, rows, max);

   if (saved.active)
      db2_memory_scope_context_set(saved.workspace, saved.project, saved.include_all);
   else
      db2_memory_scope_context_clear();

   if (listed > max)
      listed = max;
   for (int index = 0; index < listed; index++)
      out[index] = rows[index].id;
   free(rows);
   return listed;
}

static int production_find_facts_like(const char *term, int limit, int scope_active,
                                      int include_all, const char *workspace, const char *project,
                                      int64_t *out, int max)
{
   return production_scoped_term_ids(db2_memory_find_facts_like, term, limit, scope_active,
                                     include_all, workspace, project, out, max);
}

/* This one takes no separate limit: its statement binds the caller's buffer
 * size as the LIMIT, so the wire limit arrives as the buffer size and the two
 * are necessarily the same number. */
static int list_session_scope_priority_like_read(const char *pattern, int limit, memory_t *out,
                                                 int max)
{
   (void)limit;
   return db2_memory_list_session_scope_priority_like(pattern, out, max);
}

static int production_list_session_scope_priority_like(const char *term, int limit,
                                                       int scope_active, int include_all,
                                                       const char *workspace, const char *project,
                                                       int64_t *out, int max)
{
   return production_scoped_term_ids(list_session_scope_priority_like_read, term, limit,
                                     scope_active, include_all, workspace, project, out, max);
}

static int production_negation_fts_search(const char *term, int limit, int scope_active,
                                          int include_all, const char *workspace,
                                          const char *project, int64_t *out, int max)
{
   return production_scoped_term_ids(db2_memory_negation_fts_search, term, limit, scope_active,
                                     include_all, workspace, project, out, max);
}

/* A session walk returns rows the same way the scoped reads do, so it takes the
 * same detour through the heap and hands back only the identifiers. There is no
 * scope to set: the session identifier in the statement is the filter. */
static int production_session_ids(int (*read)(const char *session_id, int64_t anchor_id, int limit,
                                              memory_t *out, int max),
                                  const char *session_id, int64_t anchor_id, int limit,
                                  int64_t *out, int max)
{
   if (!read || !session_id || !out || max <= 0 || limit <= 0)
      return -1;
   memory_t *rows = calloc((size_t)max, sizeof(*rows));
   if (!rows)
      return -1;
   int listed = read(session_id, anchor_id, limit, rows, max);
   if (listed > max)
      listed = max;
   for (int index = 0; index < listed; index++)
      out[index] = rows[index].id;
   free(rows);
   return listed;
}

static int production_session_neighbors_before(const char *session_id, int64_t anchor_id, int limit,
                                               int64_t *out, int max)
{
   return production_session_ids(db2_memory_session_neighbors_before, session_id, anchor_id, limit,
                                 out, max);
}

static int production_session_neighbors_after(const char *session_id, int64_t anchor_id, int limit,
                                              int64_t *out, int max)
{
   return production_session_ids(db2_memory_session_neighbors_after, session_id, anchor_id, limit,
                                 out, max);
}

static int production_collect_alias_matches(const char *term, int limit, int scope_active,
                                            int include_all, const char *workspace,
                                            const char *project, int64_t *out, int max)
{
   return production_scoped_term_ids(db2_memory_collect_alias_matches, term, limit, scope_active,
                                     include_all, workspace, project, out, max);
}

static int production_collect_entity_matches(const char *term, int limit, int scope_active,
                                             int include_all, const char *workspace,
                                             const char *project, int64_t *out, int max)
{
   return production_scoped_term_ids(db2_memory_collect_entity_matches, term, limit, scope_active,
                                     include_all, workspace, project, out, max);
}

static int production_collect_event_frame_matches(const char *term, int limit, int scope_active,
                                                  int include_all, const char *workspace,
                                                  const char *project, int64_t *out, int max)
{
   return production_scoped_term_ids(db2_memory_collect_event_frame_matches, term, limit,
                                     scope_active, include_all, workspace, project, out, max);
}

static int production_collect_relation_token_matches(const char *term, int limit, int scope_active,
                                                     int include_all, const char *workspace,
                                                     const char *project, int64_t *out, int max)
{
   return production_scoped_term_ids(db2_memory_collect_relation_token_matches, term, limit,
                                     scope_active, include_all, workspace, project, out, max);
}

static int production_collect_summary_matches(const char *term, int limit, int scope_active,
                                              int include_all, const char *workspace,
                                              const char *project, int64_t *out, int max)
{
   return production_scoped_term_ids(db2_memory_collect_summary_matches, term, limit, scope_active,
                                     include_all, workspace, project, out, max);
}

static int production_collect_temporal_matches(const char *term, int limit, int scope_active,
                                               int include_all, const char *workspace,
                                               const char *project, int64_t *out, int max)
{
   return production_scoped_term_ids(db2_memory_collect_temporal_matches, term, limit, scope_active,
                                     include_all, workspace, project, out, max);
}

static int production_top_l2_facts(int scope_active, int include_all, const char *workspace,
                                   const char *project, int64_t *out, int max)
{
   return production_scoped_ids(db2_memory_top_l2_facts, scope_active, include_all, workspace,
                                project, out, max);
}

static int production_list_session_scope_priority(int scope_active, int include_all,
                                                  const char *workspace, const char *project,
                                                  int64_t *out, int max)
{
   return production_scoped_ids(db2_memory_list_session_scope_priority, scope_active, include_all,
                                workspace, project, out, max);
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
       .top_l2_facts = production_top_l2_facts,
       .list_session_scope_priority = production_list_session_scope_priority,
       .collect_alias_matches = production_collect_alias_matches,
       .collect_entity_matches = production_collect_entity_matches,
       .collect_event_frame_matches = production_collect_event_frame_matches,
       .collect_relation_token_matches = production_collect_relation_token_matches,
       .collect_summary_matches = production_collect_summary_matches,
       .collect_temporal_matches = production_collect_temporal_matches,
       .find_facts_like = production_find_facts_like,
       .list_session_scope_priority_like = production_list_session_scope_priority_like,
       .negation_fts_search = production_negation_fts_search,
       .session_neighbors_before = production_session_neighbors_before,
       .session_neighbors_after = production_session_neighbors_after,
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
       .purge_hidden_pollution = db2_code_index_purge_hidden_pollution,
       .requeue_drifted = db2_code_index_requeue_drifted,
       .cross_repo_rebuild_routes = db2_cross_repo_rebuild_routes,
       .cross_repo_rebuild_identities = db2_cross_repo_rebuild_identities,
       .cross_repo_rebuild_build_deps = db2_cross_repo_rebuild_build_deps,
       .drift_candidates = db2_code_index_drift_candidates,
       .rules_decay = db2_rules_decay,
       .curiosity_rescore_all = db2_curiosity_rescore_all,
       .mining_seed_job_defaults = db2_mining_seed_job_defaults,
       .proposals_archive_expired = db2_learning_proposals_archive_expired,
       .trace_mining_last_id = db2_trace_mining_last_id,
       .rel_types_ensure_seed = db2_rel_types_ensure_seed,
       .vector_rebuild_lock_try_acquire = db2_kb_runtime_state_vector_rebuild_lock_try_acquire,
       .vector_rebuild_lock_release = db2_kb_runtime_state_vector_rebuild_lock_release,
       .release_get_active = db2_kb_release_get_active,
       .prospective_sweep_expired = db2_prospective_sweep_expired,
       /* Of the two identical sweeps, bind the one that reports failure:
        * db2_directive_sweep_expired collapses a failed statement into
        * the same zero an empty sweep produces. */
       .directive_sweep_expired = db2_kb_service_directive_sweep_expired,
       /* Of the two suppressions, bind the one whose single statement
        * carries the state guard rather than checking it separately. */
       .directive_suppress = db2_directive_suppress,
       .directive_record_surface = db2_directive_record_surface,
       .anti_pattern_bump = db2_anti_pattern_bump,
       .anti_pattern_delete = db2_anti_pattern_delete,
       .doc_delete = db2_kb_doc_delete,
       .task_delete = db2_task_delete,
       .file_index_delete_project = db2_kb_file_index_delete_project,
       .clear_project = db2_kb_service_clear_project,
       .clear_current_project = db2_kb_service_clear_current_project,
       .mark_revisit_due = db2_decision_log_mark_revisit_due,
       .ingest_queue_reset_running = db2_kb_ingest_queue_reset_running,
       .evidence_reembed_all = db2_evidence_reembed_all,
       .curator_reembed_all = db2_curator_reembed_all,
       .synth_reenqueue_all = db2_synth_reenqueue_all,
       .curator_reenqueue_extract_all = db2_curator_reenqueue_extract_all,
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
        invocation->stage_id != AIMEE_DB2_STAGE_PROJECT_COUNT &&
        invocation->stage_id != AIMEE_DB2_STAGE_PURGE_HIDDEN_POLLUTION &&
        invocation->stage_id != AIMEE_DB2_STAGE_REQUEUE_DRIFTED &&
        invocation->stage_id != AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_ROUTES &&
        invocation->stage_id != AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_IDENTITIES &&
        invocation->stage_id != AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_BUILD_DEPS &&
        invocation->stage_id != AIMEE_DB2_STAGE_DRIFT_CANDIDATES &&
        invocation->stage_id != AIMEE_DB2_STAGE_RULES_DECAY &&
        invocation->stage_id != AIMEE_DB2_STAGE_CURIOSITY_RESCORE_ALL &&
        invocation->stage_id != AIMEE_DB2_STAGE_MINING_SEED_JOB_DEFAULTS &&
        invocation->stage_id != AIMEE_DB2_STAGE_PROPOSALS_ARCHIVE_EXPIRED &&
        invocation->stage_id != AIMEE_DB2_STAGE_TRACE_MINING_LAST_ID &&
        invocation->stage_id != AIMEE_DB2_STAGE_REL_TYPES_ENSURE_SEED &&
        invocation->stage_id != AIMEE_DB2_STAGE_VECTOR_REBUILD_LOCK_TRY_ACQUIRE &&
        invocation->stage_id != AIMEE_DB2_STAGE_VECTOR_REBUILD_LOCK_RELEASE &&
        invocation->stage_id != AIMEE_DB2_STAGE_RELEASE_GET_ACTIVE &&
        invocation->stage_id != AIMEE_DB2_STAGE_PROSPECTIVE_SWEEP_EXPIRED &&
        invocation->stage_id != AIMEE_DB2_STAGE_DIRECTIVE_SWEEP_EXPIRED &&
        invocation->stage_id != AIMEE_DB2_STAGE_DIRECTIVE_SUPPRESS &&
        invocation->stage_id != AIMEE_DB2_STAGE_DIRECTIVE_RECORD_SURFACE &&
        invocation->stage_id != AIMEE_DB2_STAGE_ANTI_PATTERN_BUMP &&
        invocation->stage_id != AIMEE_DB2_STAGE_ANTI_PATTERN_DELETE &&
        invocation->stage_id != AIMEE_DB2_STAGE_DOC_DELETE &&
        invocation->stage_id != AIMEE_DB2_STAGE_TASK_DELETE &&
        invocation->stage_id != AIMEE_DB2_STAGE_FILE_INDEX_DELETE_PROJECT &&
        invocation->stage_id != AIMEE_DB2_STAGE_CLEAR_PROJECT &&
        invocation->stage_id != AIMEE_DB2_STAGE_CLEAR_CURRENT_PROJECT &&
        invocation->stage_id != AIMEE_DB2_STAGE_MARK_REVISIT_DUE &&
        invocation->stage_id != AIMEE_DB2_STAGE_INGEST_QUEUE_RESET_RUNNING &&
        invocation->stage_id != AIMEE_DB2_STAGE_EVIDENCE_REEMBED_ALL &&
        invocation->stage_id != AIMEE_DB2_STAGE_CURATOR_REEMBED_ALL &&
        invocation->stage_id != AIMEE_DB2_STAGE_SYNTH_REENQUEUE_ALL &&
        invocation->stage_id != AIMEE_DB2_STAGE_CURATOR_REENQUEUE_EXTRACT_ALL))
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
      {
         /* Both scoped identifier lists decode the same way and differ only in
          * which reviewed backend they reach, so one branch serves both. */
         uint32_t limit = 0u, scope_flags = 0u;
         char workspace[AIMEE_DB2_TOP_L2_FACTS_WORKSPACE_MAX + 1];
         char project[AIMEE_DB2_TOP_L2_FACTS_PROJECT_MAX + 1];
         int (*read)(int, int, const char *, const char *, int64_t *, int) = NULL;
         int (*encode)(const uint64_t *, uint32_t, uint8_t *, size_t, uint32_t *) = NULL;
         if (aimee_db2_top_l2_facts_request_decode(request_body, request_len, &limit, &scope_flags,
                                                   workspace, sizeof(workspace), project,
                                                   sizeof(project)) == 0)
         {
            read = backend ? backend->top_l2_facts : NULL;
            encode = aimee_db2_top_l2_facts_reply_encode;
         }
         else if (aimee_db2_list_session_scope_priority_request_decode(
                      request_body, request_len, &limit, &scope_flags, workspace, sizeof(workspace),
                      project, sizeof(project)) == 0)
         {
            read = backend ? backend->list_session_scope_priority : NULL;
            encode = aimee_db2_list_session_scope_priority_reply_encode;
         }
         if (encode)
         {
            if (response_capacity < AIMEE_DB2_TOP_L2_FACTS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!read)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            int64_t rows[AIMEE_DB2_TOP_L2_FACTS_MAX];
            int listed = read((int)(scope_flags & 1u), (int)((scope_flags >> 1) & 1u), workspace,
                              project, rows, (int)limit);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (listed < 0 || listed > (int)limit)
               return AIMEE_MODULE_STATUS_INTERNAL;
            uint64_t memory_ids[AIMEE_DB2_TOP_L2_FACTS_MAX];
            for (int index = 0; index < listed; index++)
            {
               if (rows[index] < (int64_t)AIMEE_DB2_TOP_L2_FACTS_ID_MIN)
                  return AIMEE_MODULE_STATUS_INTERNAL;
               memory_ids[index] = (uint64_t)rows[index];
            }
            if (encode(memory_ids, (uint32_t)listed, response_body, response_capacity,
                       response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         /* All six term probes decode the same way and differ only in which
          * reviewed backend they reach, so one branch serves them all. */
         uint32_t limit = 0u, scope_flags = 0u;
         char term[AIMEE_DB2_COLLECT_ALIAS_MATCHES_TERM_MAX + 1];
         char workspace[AIMEE_DB2_COLLECT_ALIAS_MATCHES_WORKSPACE_MAX + 1];
         char project[AIMEE_DB2_COLLECT_ALIAS_MATCHES_PROJECT_MAX + 1];
         int (*read)(const char *, int, int, int, const char *, const char *, int64_t *, int) =
             NULL;
         int (*encode)(const uint64_t *, uint32_t, uint8_t *, size_t, uint32_t *) = NULL;
         if (!encode && aimee_db2_collect_alias_matches_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->collect_alias_matches : NULL;
            encode = aimee_db2_collect_alias_matches_reply_encode;
         }
         if (!encode && aimee_db2_collect_entity_matches_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->collect_entity_matches : NULL;
            encode = aimee_db2_collect_entity_matches_reply_encode;
         }
         if (!encode && aimee_db2_collect_event_frame_matches_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->collect_event_frame_matches : NULL;
            encode = aimee_db2_collect_event_frame_matches_reply_encode;
         }
         if (!encode && aimee_db2_collect_relation_token_matches_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->collect_relation_token_matches : NULL;
            encode = aimee_db2_collect_relation_token_matches_reply_encode;
         }
         if (!encode && aimee_db2_collect_summary_matches_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->collect_summary_matches : NULL;
            encode = aimee_db2_collect_summary_matches_reply_encode;
         }
         if (!encode && aimee_db2_collect_temporal_matches_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->collect_temporal_matches : NULL;
            encode = aimee_db2_collect_temporal_matches_reply_encode;
         }
         if (!encode && aimee_db2_find_facts_like_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->find_facts_like : NULL;
            encode = aimee_db2_find_facts_like_reply_encode;
         }
         if (!encode && aimee_db2_list_session_scope_priority_like_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->list_session_scope_priority_like : NULL;
            encode = aimee_db2_list_session_scope_priority_like_reply_encode;
         }
         if (!encode && aimee_db2_negation_fts_search_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->negation_fts_search : NULL;
            encode = aimee_db2_negation_fts_search_reply_encode;
         }
         if (encode)
         {
            if (response_capacity < AIMEE_DB2_COLLECT_ALIAS_MATCHES_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!read)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            int64_t rows[AIMEE_DB2_COLLECT_ALIAS_MATCHES_MAX];
            int listed = read(term, (int)limit, (int)(scope_flags & 1u),
                              (int)((scope_flags >> 1) & 1u), workspace, project, rows, (int)limit);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (listed < 0 || listed > (int)limit)
               return AIMEE_MODULE_STATUS_INTERNAL;
            uint64_t memory_ids[AIMEE_DB2_COLLECT_ALIAS_MATCHES_MAX];
            for (int index = 0; index < listed; index++)
            {
               if (rows[index] < (int64_t)AIMEE_DB2_COLLECT_ALIAS_MATCHES_ID_MIN)
                  return AIMEE_MODULE_STATUS_INTERNAL;
               memory_ids[index] = (uint64_t)rows[index];
            }
            if (encode(memory_ids, (uint32_t)listed, response_body, response_capacity,
                       response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         /* The two session walks decode the same way and differ only in which
          * reviewed backend they reach, so one branch serves both. */
         uint64_t anchor_id = 0u;
         uint32_t limit = 0u;
         char session_id[AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_SESSION_MAX + 1];
         int (*read)(const char *, int64_t, int, int64_t *, int) = NULL;
         int (*encode)(const uint64_t *, uint32_t, uint8_t *, size_t, uint32_t *) = NULL;
         if (!encode && aimee_db2_session_neighbors_before_request_decode(
                            request_body, request_len, session_id, sizeof(session_id), &anchor_id,
                            &limit) == 0)
         {
            read = backend ? backend->session_neighbors_before : NULL;
            encode = aimee_db2_session_neighbors_before_reply_encode;
         }
         if (!encode && aimee_db2_session_neighbors_after_request_decode(
                            request_body, request_len, session_id, sizeof(session_id), &anchor_id,
                            &limit) == 0)
         {
            read = backend ? backend->session_neighbors_after : NULL;
            encode = aimee_db2_session_neighbors_after_reply_encode;
         }
         if (encode)
         {
            if (response_capacity < AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!read)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            int64_t rows[AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_MAX];
            int listed = read(session_id, (int64_t)anchor_id, (int)limit, rows, (int)limit);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (listed < 0 || listed > (int)limit)
               return AIMEE_MODULE_STATUS_INTERNAL;
            uint64_t memory_ids[AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_MAX];
            for (int index = 0; index < listed; index++)
            {
               if (rows[index] < (int64_t)AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_ID_MIN)
                  return AIMEE_MODULE_STATUS_INTERNAL;
               memory_ids[index] = (uint64_t)rows[index];
            }
            if (encode(memory_ids, (uint32_t)listed, response_body, response_capacity,
                       response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
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
       invocation->stage_id == AIMEE_DB2_STAGE_PROJECT_COUNT ||
       invocation->stage_id == AIMEE_DB2_STAGE_PURGE_HIDDEN_POLLUTION ||
       invocation->stage_id == AIMEE_DB2_STAGE_REQUEUE_DRIFTED ||
       invocation->stage_id == AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_ROUTES ||
       invocation->stage_id == AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_IDENTITIES ||
       invocation->stage_id == AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_BUILD_DEPS ||
       invocation->stage_id == AIMEE_DB2_STAGE_DRIFT_CANDIDATES)
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
      if (aimee_db2_purge_hidden_pollution_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_PURGE_HIDDEN_POLLUTION_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->purge_hidden_pollution)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Unlike the counts above, this backend does separate failure from an
          * empty result: no connection or no statement is -1, a clean index is
          * zero. The negative arrives as internal rather than as a purge that
          * removed nothing. */
         int purged = backend->purge_hidden_pollution();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (purged < 0 || (uint32_t)purged > AIMEE_DB2_PURGE_HIDDEN_POLLUTION_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_purge_hidden_pollution_reply_encode((uint32_t)purged, response_body,
                                                           response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_requeue_drifted_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_REQUEUE_DRIFTED_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->requeue_drifted)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Nothing drifted and no connection both arrive as zero here, so the
          * count cannot separate an idle graph from a failed enqueue. The
          * statement returns the rows it inserted rather than counting them
          * again, so the number that does arrive is at least exact. */
         int requeued = backend->requeue_drifted();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (requeued < 0 || (uint32_t)requeued > AIMEE_DB2_REQUEUE_DRIFTED_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_requeue_drifted_reply_encode((uint32_t)requeued, response_body,
                                                    response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_cross_repo_rebuild_routes_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_CROSS_REPO_REBUILD_ROUTES_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->cross_repo_rebuild_routes)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The first operation here to own a real transaction: the table is
          * emptied and refilled inside one BEGIN/COMMIT, so no caller ever
          * observes a half-rebuilt routing table. A rolled-back rebuild is -1
          * and stays a failure -- reporting it as an empty table would claim
          * every cross-repo include had stopped resolving. The number itself
          * is the rebuilt table's size, from its own query. */
         int route_count = backend->cross_repo_rebuild_routes();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (route_count < 0 || (uint32_t)route_count > AIMEE_DB2_CROSS_REPO_REBUILD_ROUTES_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_cross_repo_rebuild_routes_reply_encode((uint32_t)route_count, response_body,
                                                              response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_cross_repo_rebuild_identities_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_CROSS_REPO_REBUILD_IDENTITIES_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->cross_repo_rebuild_identities)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Transactional like the route rebuild beside it, and -1 stays a
          * failure for the same reason. The number differs in kind though: it
          * counts inserts attempted, and a duplicate identity is counted and
          * then discarded by ON CONFLICT, so it can exceed the table size.
          * The catalog says so; the wire carries a number, not a claim. */
         int identities_written = backend->cross_repo_rebuild_identities();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (identities_written < 0 ||
             (uint32_t)identities_written > AIMEE_DB2_CROSS_REPO_REBUILD_IDENTITIES_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_cross_repo_rebuild_identities_reply_encode(
                 (uint32_t)identities_written, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_cross_repo_rebuild_build_deps_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_CROSS_REPO_REBUILD_BUILD_DEPS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->cross_repo_rebuild_build_deps)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The last of the three cross-repo rebuilds and the strictest about
          * partial results: a mid-cursor error on either the project list or
          * the manifest scan rolls back rather than committing a table that is
          * missing the repositories it never reached. That arrives as -1 and
          * stays a failure. Insert attempts again, not rows stored. */
         int build_deps_written = backend->cross_repo_rebuild_build_deps();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (build_deps_written < 0 ||
             (uint32_t)build_deps_written > AIMEE_DB2_CROSS_REPO_REBUILD_BUILD_DEPS_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_cross_repo_rebuild_build_deps_reply_encode(
                 (uint32_t)build_deps_written, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_drift_candidates_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_DRIFT_CANDIDATES_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->drift_candidates)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The read-only half of a pair: this counts exactly what
          * index.requeue_drifted would enqueue, because both statements use
          * the same predicate. An empty corpus and a failed statement are both
          * zero, as they are for the requeue itself. */
         int64_t drift = backend->drift_candidates();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (drift < 0 || (uint64_t)drift > AIMEE_DB2_DRIFT_CANDIDATES_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_drift_candidates_reply_encode((uint64_t)drift, response_body,
                                                     response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      char project[AIMEE_DB2_FILE_INDEX_DELETE_PROJECT_PROJECT_MAX + 1] = {0};
      if (aimee_db2_file_index_delete_project_request_decode(request_body, request_len, project,
                                                             sizeof(project)) == 0)
      {
         if (response_capacity < AIMEE_DB2_FILE_INDEX_DELETE_PROJECT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->file_index_delete_project)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* One statement over one table, so nothing here can half-succeed.
          * It normalises the project name before matching, so the name that
          * arrives is not necessarily the one deleted, and it does not check
          * whether the statement finished -- a failure reports zero rather
          * than an error. */
         int deleted_entries = backend->file_index_delete_project(project);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (deleted_entries < 0 ||
             (uint32_t)deleted_entries > AIMEE_DB2_FILE_INDEX_DELETE_PROJECT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_file_index_delete_project_reply_encode(
                 (uint32_t)deleted_entries, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }

   if (invocation->stage_id == AIMEE_DB2_STAGE_VECTOR_REBUILD_LOCK_TRY_ACQUIRE)
   {
      if (aimee_db2_vector_rebuild_lock_try_acquire_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_VECTOR_REBUILD_LOCK_TRY_ACQUIRE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->vector_rebuild_lock_try_acquire)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The custody family's first stage. A one here means this caller
          * wrote the lock row, not that it is the only holder: the backend
          * reads the row and writes it as two separate statements, so two
          * callers that both read before either writes are both told they
          * acquired it. Serving the operation on one stage narrows that window
          * but does not close it while unmigrated callers still reach the
          * backend directly. The catalog says mutually_exclusive is false. */
         int acquired = backend->vector_rebuild_lock_try_acquire();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (acquired < 0 || (uint32_t)acquired > AIMEE_DB2_VECTOR_REBUILD_LOCK_TRY_ACQUIRE_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_vector_rebuild_lock_try_acquire_reply_encode(
                 (uint32_t)acquired, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_vector_rebuild_lock_release_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_VECTOR_REBUILD_LOCK_RELEASE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->vector_rebuild_lock_release)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The release does not check who holds the lock, so any caller can
          * release another's, and it returns void so no failure can reach the
          * wire. Both are recorded in the catalog rather than left to be
          * discovered by whoever loses a rebuild to a stranger's release. */
         backend->vector_rebuild_lock_release();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_vector_rebuild_lock_release_reply_encode(response_body, response_capacity,
                                                                response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_release_get_active_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_RELEASE_GET_ACTIVE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->release_get_active)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Zero is three answers at once: no key, a value that would not
          * parse, and a value that was not positive. The parse runs through
          * atoll, which returns zero for garbage without saying so, and the
          * boundary cannot recover what the backend threw away. Recorded in
          * the catalog so a zero is not read as a confident "no release". */
         int64_t release_id = backend->release_get_active();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (release_id < 0 || (uint64_t)release_id > AIMEE_DB2_RELEASE_GET_ACTIVE_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_release_get_active_reply_encode((uint64_t)release_id, response_body,
                                                       response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }

   if (invocation->stage_id == AIMEE_DB2_STAGE_REL_TYPES_ENSURE_SEED)
   {
      if (aimee_db2_rel_types_ensure_seed_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_REL_TYPES_ENSURE_SEED_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->rel_types_ensure_seed)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The organization family's first stage. What this seeds is not
          * DB2's to decide: the relation-type ontology is declared by the
          * ontology module and only persisted here, so the policy pins the
          * persistence rule and stops there. Any failed statement fails the
          * whole pass, and the acknowledgement shape leaves no room to report
          * that as a partial success. */
         if (backend->rel_types_ensure_seed() != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_rel_types_ensure_seed_reply_encode(response_body, response_capacity,
                                                          response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t row_id = 0u;
      if (aimee_db2_doc_delete_request_decode(request_body, request_len, &row_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_DOC_DELETE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->doc_delete)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Deleting a document that is not there is an error, matching
          * the anti-pattern delete rather than the bump. */
         if (backend->doc_delete((int64_t)row_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_doc_delete_reply_encode(response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_task_delete_request_decode(request_body, request_len, &row_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_TASK_DELETE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->task_delete)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Two statements with no transaction: the edge delete runs
          * first and its failure is ignored, then the task delete
          * decides the result. A caller can be told the task is gone
          * while its edges remain. The schema's foreign key cascades
          * them anyway, which is why that looked safe; the catalog
          * records that the pair is not atomic. */
         if (backend->task_delete((int64_t)row_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_task_delete_reply_encode(response_body, response_capacity, response_len) !=
             0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      char project[AIMEE_DB2_CLEAR_PROJECT_PROJECT_MAX + 1] = {0};
      if (aimee_db2_clear_project_request_decode(request_body, request_len, project,
                                                 sizeof(project)) == 0)
      {
         if (response_capacity < AIMEE_DB2_CLEAR_PROJECT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->clear_project)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Deletes a project's documents and the vector index operations
          * pointing at them. No foreign key cascades the second table,
          * which is why it is cleared explicitly; that statement's failure
          * is ignored here and fatal in the current-generation clear below,
          * and neither runs in a transaction. */
         int deleted_documents = backend->clear_project(project);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (deleted_documents < 0 || (uint32_t)deleted_documents > AIMEE_DB2_CLEAR_PROJECT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_clear_project_reply_encode((uint32_t)deleted_documents, response_body,
                                                  response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_clear_current_project_request_decode(request_body, request_len, project,
                                                         sizeof(project)) == 0)
      {
         if (response_capacity < AIMEE_DB2_CLEAR_CURRENT_PROJECT_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->clear_current_project)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The same work scoped to the project's current generation, and
          * the one that treats a failed index-operation delete as fatal. */
         int deleted_documents = backend->clear_current_project(project);
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (deleted_documents < 0 ||
             (uint32_t)deleted_documents > AIMEE_DB2_CLEAR_CURRENT_PROJECT_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_clear_current_project_reply_encode(
                 (uint32_t)deleted_documents, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }

   if (invocation->stage_id == AIMEE_DB2_STAGE_RULES_DECAY ||
       invocation->stage_id == AIMEE_DB2_STAGE_CURIOSITY_RESCORE_ALL ||
       invocation->stage_id == AIMEE_DB2_STAGE_MINING_SEED_JOB_DEFAULTS ||
       invocation->stage_id == AIMEE_DB2_STAGE_PROPOSALS_ARCHIVE_EXPIRED)
   {
      if (aimee_db2_rules_decay_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_RULES_DECAY_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->rules_decay)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The learning family's first stage. Every decay constant stays here:
          * the amount, the two intervals, the archive threshold and its grace
          * period. A caller able to send any of them could age a hard
          * directive out in one call, or delete rules still in force. The
          * number sums three statements, so a decayed rule and an archived one
          * are not separable in it. */
         int rules_touched = backend->rules_decay();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (rules_touched < 0 || (uint32_t)rules_touched > AIMEE_DB2_RULES_DECAY_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_rules_decay_reply_encode((uint32_t)rules_touched, response_body,
                                                response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_curiosity_rescore_all_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_CURIOSITY_RESCORE_ALL_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->curiosity_rescore_all)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The scoring weights stay here for the same reason the decay
          * constants next door do, though what they buy is different: these
          * decide what the system becomes curious about next, so a caller able
          * to send them could steer its attention. Nothing open to rescore and
          * a failed statement are both zero. */
         int items_rescored = backend->curiosity_rescore_all();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (items_rescored < 0 || (uint32_t)items_rescored > AIMEE_DB2_CURIOSITY_RESCORE_ALL_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_curiosity_rescore_all_reply_encode((uint32_t)items_rescored, response_body,
                                                          response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_mining_seed_job_defaults_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_MINING_SEED_JOB_DEFAULTS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->mining_seed_job_defaults)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The first acknowledgement-only reply on this bus. A count would be
          * the seeds attempted rather than the rows created -- the do-nothing
          * conflict rule makes those different numbers -- so this operation
          * reports only whether the seed pass completed. */
         if (backend->mining_seed_job_defaults() != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_mining_seed_job_defaults_reply_encode(response_body, response_capacity,
                                                             response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_proposals_archive_expired_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_PROPOSALS_ARCHIVE_EXPIRED_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->proposals_archive_expired)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* This backend returns void and logs whatever goes wrong, so there is
          * nothing here to turn into a failure: the acknowledgement is always
          * ok and promises only that the sweep was attempted. The catalog
          * records that as reports_failure false. Giving the wire a real answer
          * means changing the backend signature, which is a separate change
          * and should look like one rather than arriving inside a migration. */
         backend->proposals_archive_expired();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_proposals_archive_expired_reply_encode(response_body, response_capacity,
                                                              response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_trace_mining_last_id_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_TRACE_MINING_LAST_ID_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->trace_mining_last_id)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* A never-mined corpus and a failed read are both zero, and here the
          * two cost very different things: a zero sends the next mining pass
          * back to the start, so a failed read buys a full rescan rather than
          * losing anything. Cheap one way, expensive the other, invisible
          * either way, and recorded in the catalog for that reason. */
         int64_t last_trace_id = backend->trace_mining_last_id();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (last_trace_id < 0 || (uint64_t)last_trace_id > AIMEE_DB2_TRACE_MINING_LAST_ID_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_trace_mining_last_id_reply_encode((uint64_t)last_trace_id, response_body,
                                                         response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t anti_pattern_id = 0u;
      if (aimee_db2_anti_pattern_bump_request_decode(request_body, request_len, &anti_pattern_id) ==
          0)
      {
         if (response_capacity < AIMEE_DB2_ANTI_PATTERN_BUMP_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->anti_pattern_bump)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Reports success whenever the statement ran, so bumping an
          * identifier that does not exist is indistinguishable from
          * bumping one that does. The delete below makes the opposite
          * choice on the same table; both are in the catalog. */
         if (backend->anti_pattern_bump((int64_t)anti_pattern_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_anti_pattern_bump_reply_encode(response_body, response_capacity,
                                                      response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_anti_pattern_delete_request_decode(request_body, request_len,
                                                       &anti_pattern_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_ANTI_PATTERN_DELETE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->anti_pattern_delete)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The opposite choice to the bump above: nothing deleted is
          * reported as an error. The backend does not separate that
          * from a failed statement, so both arrive as internal. */
         if (backend->anti_pattern_delete((int64_t)anti_pattern_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_anti_pattern_delete_reply_encode(response_body, response_capacity,
                                                        response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }

   if (invocation->stage_id == AIMEE_DB2_STAGE_PROSPECTIVE_SWEEP_EXPIRED ||
       invocation->stage_id == AIMEE_DB2_STAGE_DIRECTIVE_SWEEP_EXPIRED ||
       invocation->stage_id == AIMEE_DB2_STAGE_MARK_REVISIT_DUE ||
       invocation->stage_id == AIMEE_DB2_STAGE_INGEST_QUEUE_RESET_RUNNING ||
       invocation->stage_id == AIMEE_DB2_STAGE_EVIDENCE_REEMBED_ALL ||
       invocation->stage_id == AIMEE_DB2_STAGE_CURATOR_REEMBED_ALL ||
       invocation->stage_id == AIMEE_DB2_STAGE_SYNTH_REENQUEUE_ALL ||
       invocation->stage_id == AIMEE_DB2_STAGE_CURATOR_REENQUEUE_EXTRACT_ALL)
   {
      if (aimee_db2_prospective_sweep_expired_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->prospective_sweep_expired)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Nothing armed has expired yet and a failed statement are both zero
          * from this backend, so the count cannot separate them. What the
          * boundary does keep is the clock: the comparison happens in the
          * database, so a caller with a wrong clock cannot retire a memory
          * that is still inside its window. */
         int expired = backend->prospective_sweep_expired();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (expired < 0 || (uint32_t)expired > AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_prospective_sweep_expired_reply_encode((uint32_t)expired, response_body,
                                                              response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_directive_sweep_expired_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->directive_sweep_expired)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Same collapse as the sweep above: nothing open has passed its
          * window, and a failed statement, both arrive as zero. The clock
          * stays in the database for the same reason it does there, and it
          * matters more here -- a directive is a rule the system holds itself
          * to, so retiring one early drops a constraint still in force. */
         int directives = backend->directive_sweep_expired();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (directives < 0 || (uint32_t)directives > AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_directive_sweep_expired_reply_encode((uint32_t)directives, response_body,
                                                            response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      uint64_t directive_id = 0u;
      if (aimee_db2_directive_suppress_request_decode(request_body, request_len, &directive_id) ==
          0)
      {
         if (response_capacity < AIMEE_DB2_DIRECTIVE_SUPPRESS_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->directive_suppress)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The statement carries its own state guard, so deciding and writing
          * are one action rather than a check a caller could lose a race
          * against. A directive that was not open and a failed statement come
          * back the same way, and the backend keeps nothing that would let the
          * boundary say not_found instead -- recorded in the catalog. */
         if (backend->directive_suppress((int64_t)directive_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_directive_suppress_reply_encode(response_body, response_capacity,
                                                       response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_directive_record_surface_request_decode(request_body, request_len,
                                                            &directive_id) == 0)
      {
         if (response_capacity < AIMEE_DB2_DIRECTIVE_RECORD_SURFACE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->directive_record_surface)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Same guard and same collapse as the suppression above. */
         if (backend->directive_record_surface((int64_t)directive_id) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (aimee_db2_directive_record_surface_reply_encode(response_body, response_capacity,
                                                             response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_mark_revisit_due_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_MARK_REVISIT_DUE_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->mark_revisit_due)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Unlike the two sweeps above, this backend separates a failed
          * statement from an empty one: no connection, no statement and a step
          * that did not finish are all -1, while nothing being due is zero.
          * The boundary keeps that apart rather than reporting an untouched
          * decision log as a successful sweep. */
         int marked = backend->mark_revisit_due();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (marked < 0 || (uint32_t)marked > AIMEE_DB2_MARK_REVISIT_DUE_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_mark_revisit_due_reply_encode((uint32_t)marked, response_body,
                                                     response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_ingest_queue_reset_running_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_INGEST_QUEUE_RESET_RUNNING_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->ingest_queue_reset_running)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* No connection and no statement are -1 here, and the boundary keeps
          * that as a failure. Reporting it as zero would claim a clean queue
          * while rows abandoned by a dead worker are still stranded, which is
          * exactly the state this recovery exists to leave behind. */
         int reset_rows = backend->ingest_queue_reset_running();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (reset_rows < 0 || (uint32_t)reset_rows > AIMEE_DB2_INGEST_QUEUE_RESET_RUNNING_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_ingest_queue_reset_running_reply_encode(
                 (uint32_t)reset_rows, response_body, response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_evidence_reembed_all_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_EVIDENCE_REEMBED_ALL_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->evidence_reembed_all)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The only operation on this stage whose reach is every row, and it
          * discards each row's attempt count and last error along the way.
          * None of that reach travels, so a caller cannot narrow or widen it.
          * An empty index and a failed statement are both zero here. */
         int evidence_rows = backend->evidence_reembed_all();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (evidence_rows < 0 || (uint32_t)evidence_rows > AIMEE_DB2_EVIDENCE_REEMBED_ALL_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_evidence_reembed_all_reply_encode((uint32_t)evidence_rows, response_body,
                                                         response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_curator_reembed_all_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_CURATOR_REEMBED_ALL_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->curator_reembed_all)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Narrower than the evidence reset beside it, and the narrowing is
          * the point: only the six re-derivable artifact kinds move back to
          * proposed. The list never travels, so a caller cannot demote an
          * artifact that nothing will ever propose again. An empty set and a
          * failed statement are both zero here. */
         int demoted_artifacts = backend->curator_reembed_all();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (demoted_artifacts < 0 ||
             (uint32_t)demoted_artifacts > AIMEE_DB2_CURATOR_REEMBED_ALL_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_curator_reembed_all_reply_encode((uint32_t)demoted_artifacts, response_body,
                                                        response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_synth_reenqueue_all_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_SYNTH_REENQUEUE_ALL_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->synth_reenqueue_all)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* The same total reach and the same attempt-and-error discard as the
          * evidence reset above, because the same version bump drives both.
          * Neither travels. An empty table and a failed statement are both
          * zero here, as they are there. */
         int reenqueued_ops = backend->synth_reenqueue_all();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (reenqueued_ops < 0 || (uint32_t)reenqueued_ops > AIMEE_DB2_SYNTH_REENQUEUE_ALL_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_synth_reenqueue_all_reply_encode((uint32_t)reenqueued_ops, response_body,
                                                        response_capacity, response_len) != 0)
            return AIMEE_MODULE_STATUS_INTERNAL;
         return AIMEE_MODULE_STATUS_OK;
      }
      if (aimee_db2_curator_reenqueue_extract_all_request_decode(request_body, request_len) == 0)
      {
         if (response_capacity < AIMEE_DB2_CURATOR_REENQUEUE_EXTRACT_ALL_RESPONSE_LEN)
            return AIMEE_MODULE_STATUS_INVALID_REQUEST;
         if (!backend || !backend->curator_reenqueue_extract_all)
            return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
         /* Three autocommitting statements with no surrounding transaction, so
          * a concurrent reader can observe the state between them. The number
          * that comes back is the size of the extract queue afterwards, from
          * its own query, not the rows the two mutations touched. The catalog
          * says both, and the wire carries neither claim. */
         int extract_jobs = backend->curator_reenqueue_extract_all();
         if (aimee_module_invocation_cancelled(invocation))
            return AIMEE_MODULE_STATUS_CANCELLED;
         if (extract_jobs < 0 ||
             (uint32_t)extract_jobs > AIMEE_DB2_CURATOR_REENQUEUE_EXTRACT_ALL_MAX)
            return AIMEE_MODULE_STATUS_INTERNAL;
         if (aimee_db2_curator_reenqueue_extract_all_reply_encode(
                 (uint32_t)extract_jobs, response_body, response_capacity, response_len) != 0)
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
