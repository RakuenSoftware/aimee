#include "module_adapter.h"

#include <aimee/db2/module_api.h>

#include "c/db2.h"
#include "c/cross_repo_stats.h"
#include "c/feature_rows.h"
#include "c/kb_audit_worm.h"
#include "c/css_render.h"
#include "c/demotion.h"
#include "c/agent_outcomes.h"
#include "c/entity_nodes.h"
#include "c/memory_lint.h"
#include "c/typed_facts.h"
#include "c/canonical_index.h"
#include "c/corpus_jobs.h"
#include "c/memory_briefing.h"
#include "c/css_migration.h"
#include "c/sketch.h"
#include "c/memory_scenes.h"
#include "c/calibration.h"
#include "c/collab_rules.h"
#include "c/bandit.h"
#include "c/code_projection.h"
#include "c/ontology_evolution.h"
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
#include "c/entity_profiles.h"
#include "c/enrollments.h"
#include "c/epistemic_directives.h"
#include "c/evidence_vectors.h"
#include "c/fidelity.h"
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

/* Copy a memory_t into the bounded row the wire carries. The row's text fields
 * are one byte narrower than the columns they come from, so a value that filled
 * its column exactly would not fit; snprintf truncates rather than overruns,
 * and the bound is checked before the row is encoded. */
static void production_fill_row(const memory_t *from, aimee_db2_memory_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->id = (uint64_t)from->id;
   row->confidence = from->confidence;
   row->salience = from->salience;
   row->use_count = from->use_count < 0 ? 0u : (uint32_t)from->use_count;
   snprintf(row->tier, sizeof(row->tier), "%s", from->tier);
   snprintf(row->kind, sizeof(row->kind), "%s", from->kind);
   snprintf(row->key, sizeof(row->key), "%s", from->key);
   snprintf(row->content, sizeof(row->content), "%s", from->content);
   snprintf(row->use_cases, sizeof(row->use_cases), "%s", from->use_cases);
   snprintf(row->last_used_at, sizeof(row->last_used_at), "%s", from->last_used_at);
   snprintf(row->created_at, sizeof(row->created_at), "%s", from->created_at);
   snprintf(row->updated_at, sizeof(row->updated_at), "%s", from->updated_at);
   snprintf(row->source_session, sizeof(row->source_session), "%s", from->source_session);
   snprintf(row->provenance_category, sizeof(row->provenance_category), "%s",
            from->provenance_category);
}

static int production_row_read(int (*read)(int64_t, memory_t *), int64_t identifier,
                               aimee_db2_memory_row_t *row)
{
   if (!read || !row)
      return -1;
   memory_t found;
   if (read(identifier, &found) != 0)
      return -1;
   production_fill_row(&found, row);
   return 0;
}

static int production_row_get(int64_t memory_id, aimee_db2_memory_row_t *row)
{
   return production_row_read(db2_memory_get, memory_id, row);
}

static int production_row_get_by_unit_id(int64_t unit_id, aimee_db2_memory_row_t *row)
{
   return production_row_read(db2_memory_get_by_unit_id, unit_id, row);
}

/* The keyword search takes the caller's buffer size as its own LIMIT, the way
 * list_session_scope_priority_like does. */
static int search_facts_patterns_by_keyword_read(const char *keyword, int limit, memory_t *out,
                                                 int max)
{
   (void)limit;
   return db2_memory_search_facts_patterns_by_keyword(keyword, out, max);
}

static int production_search_facts_patterns_by_keyword(const char *term, int limit,
                                                       int scope_active, int include_all,
                                                       const char *workspace, const char *project,
                                                       int64_t *out, int max)
{
   return production_scoped_term_ids(search_facts_patterns_by_keyword_read, term, limit,
                                     scope_active, include_all, workspace, project, out, max);
}

/* The history takes no limit and no scope; the caller's buffer size is the only
 * bound, and the statement itself has none. */
static int production_fact_history(const char *normalized_key, int limit, int64_t *out, int max)
{
   if (!normalized_key || !out || max <= 0 || limit <= 0)
      return -1;
   memory_t *rows = calloc((size_t)max, sizeof(*rows));
   if (!rows)
      return -1;
   int listed = db2_memory_fact_history(normalized_key, rows, max);
   if (listed > max)
      listed = max;
   for (int index = 0; index < listed; index++)
      out[index] = rows[index].id;
   free(rows);
   return listed;
}

static int production_list_rows(const char *tier, const char *kind, int hide_archived, int limit,
                                int scope_active, int include_all, const char *workspace,
                                const char *project, int64_t *out, int max)
{
   if (!tier || !kind || !out || max <= 0 || limit <= 0)
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

   int listed = db2_memory_list(tier, kind, hide_archived, limit, rows, max);

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

static int production_aggregate(const char *entity_seed, const char *keyword, int limit,
                                int *truncated_out, int64_t *out, int max)
{
   if (truncated_out)
      *truncated_out = 0;
   if (!entity_seed || !keyword || !out || max <= 0 || limit <= 0)
      return -1;
   memory_t *rows = calloc((size_t)max, sizeof(*rows));
   if (!rows)
      return -1;
   int listed =
       db2_memory_aggregate(entity_seed, keyword, rows, limit < max ? limit : max, truncated_out);
   if (listed > max)
      listed = max;
   for (int index = 0; index < listed; index++)
      out[index] = rows[index].id;
   free(rows);
   return listed;
}

static int production_load_eval_corpus(int limit, char *label_out, size_t label_len, int64_t *out,
                                       int max)
{
   if (label_out && label_len)
      label_out[0] = '\0';
   if (!label_out || !out || max <= 0 || limit <= 0)
      return -1;
   memory_t *rows = calloc((size_t)max, sizeof(*rows));
   if (!rows)
      return -1;
   int listed = db2_memory_load_eval_corpus(rows, limit < max ? limit : max, label_out, label_len);
   if (listed > max)
      listed = max;
   for (int index = 0; index < listed; index++)
      out[index] = rows[index].id;
   free(rows);
   return listed;
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
       .search_facts_patterns_by_keyword = production_search_facts_patterns_by_keyword,
       .fact_history = production_fact_history,
       .list_rows = production_list_rows,
       .aggregate = production_aggregate,
       .load_eval_corpus = production_load_eval_corpus,
       .record_exists = db2_kb_service_memory_record_exists,
       .document_exists = db2_kb_service_kb_document_exists,
       .trace_mining_record = db2_trace_mining_record,
       .anti_pattern_exists_exact = db2_anti_pattern_exists_exact,
       .anti_pattern_exists_by_source_ref = db2_anti_pattern_exists_by_source_ref,
       .artifact_citation_count = db2_artifact_citation_count,
       .commits_in_last_7_days = db2_learning_commits_in_last_7_days,
       .entity_observation_count = db2_entity_count_observations,
       .fidelity_attribution_count = db2_fidelity_attribution_count_by_turn,
       .blob_referenced = db2_kb_blob_ref_referenced,
       .async_pending_count = db2_kb_async_count_kind_pending,
       .artifact_stamp_reflected = db2_artifact_stamp_reflected,
       .failed_query_bump = db2_failed_query_bump,
       .fence_active = db2_kb_purge_fence_active,
       .runtime_state_touch = db2_kb_runtime_state_set_now,
       .synth_enqueue = db2_synth_enqueue,
       .synth_mark_done = db2_synth_mark_done,
       .reembed_mark_finished = db2_kb_service_mark_reembed_finished,
       .mining_job_try_lock = db2_mining_job_try_lock,
       .artifact_set_state = db2_artifact_set_state,
       .artifact_register_exemplar = db2_artifact_register_exemplar,
       .evidence_enqueue = db2_evidence_enqueue,
       .evidence_mark_failed = db2_evidence_mark_failed,
       .synth_mark_failed = db2_synth_mark_failed,
       .runtime_state_set = db2_kb_runtime_state_set,
       .set_active_embedder_version = db2_kb_service_set_active_embedder_version,
       .entity_profile_fresh = db2_entity_profile_is_fresh,
       .doc_exists_by_hash = db2_kb_doc_exists_by_hash_scope,
       .pdf_quarantine_confirm = db2_kb_pdf_quarantine_confirm,
       .pdf_quarantine_reject = db2_kb_pdf_quarantine_reject,
       .enrollment_active = db2_enrollment_is_active_by_key,
       .runtime_state_get = db2_kb_runtime_state_get,
       .session_neighbors_before = production_session_neighbors_before,
       .session_neighbors_after = production_session_neighbors_after,
       .row_get = production_row_get,
       .row_get_by_unit_id = production_row_get_by_unit_id,
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
       .bandit_arms_list = db2_bandit_arms_list,
       .bandit_promotion_get = db2_bandit_promotion_get,
       .project_fingerprint = db2_code_projection_project_fingerprint,
       .visible_source_hash = db2_code_projection_visible_source_hash,
       .entity_profile_card = db2_entity_profile_get_card,
       .ontology_eval_status = db2_ontology_eval_status,
       .decision_log_set_outcome = db2_decision_log_set_outcome,
       .decision_log_set_status = db2_decision_log_set_status,
       .decision_log_set_revisit = db2_decision_log_set_revisit,
       .prospective_set_state = db2_prospective_set_state,
       .task_update_state = db2_task_update_state,
       .ingest_queue_fail = db2_kb_ingest_queue_fail,
       .generation_abort = db2_code_projection_generation_abort,
       .generation_set_source_hash = db2_code_projection_generation_set_source_hash,
       .generation_publish = db2_code_projection_generation_publish,
       .purge_files_matching = db2_code_index_purge_files_matching,
       .collab_rule_approve = db2_collab_rules_approve,
       .collab_rule_reject = db2_collab_rules_reject,
       .collab_rule_retire = db2_collab_rules_retire,
       .proposal_bump_corroboration = db2_learning_proposal_bump_corroboration,
       .proposal_mark_committed = db2_learning_proposal_mark_committed,
       .rules_delete_by_id = db2_rules_delete,
       .calibration_surfaces_with_data = db2_calibration_surfaces_with_data,
       .reset_stuck_vector_ops = db2_kb_service_reset_stuck_vector_ops,
       .dedupe_by_key = db2_memory_dedupe_by_key,
       .directive_resolve = db2_directive_resolve,
       .release_add_doc = db2_kb_release_add_doc,
       .scene_member_exists = db2_memory_scene_member_exists,
       .unit_edge_exists = db2_memory_unit_edge_exists,
       .artifact_cite = db2_artifact_cite,
       .artifact_link = db2_artifact_link,
       .bandit_promotion_set = db2_bandit_promotion_set,
       .collab_rule_propose = db2_collab_rules_propose,
       .file_index_delete_current_generation = db2_kb_file_index_delete_current_project,
       .project_delete = db2_code_index_project_delete,
       .minhash_delete_current_generation = db2_sketch_minhash_signature_delete_project,
       .css_migration_enumerate = db2_css_migration_enumerate,
       .ontology_approve = db2_ontology_approve,
       .ontology_reject = db2_ontology_reject,
       .rules_delete_by_directive_type = db2_rules_delete_by_directive_type,
       .artifact_flag_review = db2_artifact_flag_review,
       .verdict_suppressed = db2_artifact_verdict_suppressed,
       .css_migration_assert_conventions = db2_css_migration_assert_conventions,
       .curator_invalidate_doc = db2_curator_invalidate_doc,
       .doc_assets_delete_for_doc = db2_kb_doc_assets_delete_for_doc,
       .ontology_map = db2_ontology_map,
       .minhash_delete_file = db2_sketch_minhash_signature_delete,
       .project_current_generation = db2_code_index_project_current_generation,
       .projection_generation_create = db2_code_projection_generation_create,
       .projection_visible_id = db2_code_projection_visible_id,
       .release_create = db2_kb_release_create,
       .css_migration_rules_doc = db2_css_migration_rules_doc,
       .unique_file_basename = db2_code_index_unique_file_basename,
       .purge_fence_heartbeat = db2_kb_purge_fence_heartbeat,
       .purge_fence_clear = db2_kb_purge_fence_clear,
       .document_stored_hash = db2_kb_documents_get_stored_hash,
       .document_hash_exists = db2_kb_documents_hash_exists,
       .pdf_tsr_state = db2_kb_pdf_tsr_state,
       .match_error_keys = db2_memory_promotion_match_error_keys,
       .document_chunk_ids = db2_kb_documents_list_chunk_ids_for_file,
       .memory_ids_by_updated = db2_kb_service_list_memory_ids_by_updated,
       .unit_ids_for_memory = db2_memory_unit_list_ids,
       .retryable_index_failures = db2_memory_list_retryable_index_failures,
       .entity_neighbors = db2_entity_edge_neighbors,
       .entity_neighbors_filtered = db2_entity_edge_neighbors_filtered,
       .entity_outbound_neighbors = db2_entity_edge_outbound_neighbors,
       .entity_top_partners = db2_entity_edge_top_partners_by_relation,
       .entity_top_targets = db2_entity_edge_top_targets_by_relation,
       .file_definitions = db2_code_index_file_definitions,
       .code_search = db2_code_index_code_search,
       .code_search_excluding_project = db2_code_index_code_search_excluding_project,
       .project_last_scan = db2_code_index_project_last_scan,
       .active_embedder_version = db2_kb_service_get_active_embedder_version,
       .bandit_decision_points = db2_bandit_decision_points_list,
       .corpus_pipeline_stage_counts = db2_corpus_pipeline_stage_counts,
       .briefing_active_entities = db2_memory_briefing_list_active_entities,
       .entity_walk_step_typed = db2_entity_edge_walk_step_typed,
       .projection_generations_list = db2_code_projection_generations_list,
       .entity_edge_bump_utility = db2_entity_edge_bump_utility,
       .bandit_decision_close = db2_bandit_decision_close,
       .entity_neighbors_weighted = db2_entity_edge_neighbors_weighted,
       .prospective_list = db2_prospective_list,
       .prospective_list_armed = db2_prospective_list_armed,
       .prospective_by_entity = db2_prospective_list_by_entity,
       .prospective_by_file = db2_prospective_list_by_file,
       .prospective_by_trigger_terms = db2_prospective_list_by_trigger_terms,
       .directive_list = db2_directive_list,
       .directive_by_entity = db2_directive_match_by_entity,
       .directive_by_file = db2_directive_match_by_file,
       .directive_by_lexical = db2_directive_match_by_lexical,
       .relations_for_entity = db2_memory_relations_for_entity,
       .relations_search = db2_memory_relations_search,
       .relations_search_as_of = db2_memory_relations_search_as_of,
       .relations_supporting = db2_memory_relations_supporting,
       .entity_edges_for_entity = db2_entity_edge_list_by_entity,
       .entity_edges_by_token = db2_entity_edge_search_by_token,
       .entity_top_triples = db2_entity_edge_top_distinct_triples,
       .projection_edges = db2_code_projection_list_edges,
       .projection_edges_for_generation = db2_code_projection_list_edges_for_gen,
       .task_edges = db2_task_get_edges,
       .term_find = db2_code_index_term_find,
       .term_find_in_project = canonical_index_find_project,
       .term_find_excluding_project = canonical_index_find_excluding_project,
       .callers_find = db2_code_index_callers_find,
       .callers_find_scoped = canonical_index_find_callers,
       .callers_find_excluding_project = canonical_index_find_callers_excluding_project,
       .rules_list = db2_rules_list,
       .rules_list_by_tier = db2_rules_list_by_tier,
       .rules_list_hard = db2_rules_list_hard,
       .anti_pattern_list = db2_anti_pattern_list,
       .anti_pattern_list_hot = db2_anti_pattern_list_hot,
       .anti_pattern_check = db2_anti_pattern_check,
       .task_list = db2_task_list,
       .task_subtasks = db2_task_get_subtasks,
       .typed_fact_recall = db2_typed_fact_recall,
       .memory_lint = memory_lint_run,
       .decision_log_list = db2_decision_log_list,
       .decision_log_list_scoped = db2_decision_log_list_scoped,
       .global_constraints = db2_memory_list_global_constraints,
       .kv_section = db2_memory_list_kv_section,
       .memories_by_key = db2_memory_list_by_key,
       .session_memories = db2_memory_session_id_content_list,
       .memory_candidates = db2_memory_list_candidates,
       .recall_section = db2_memory_list_recall_section,
       .l2_cross_key_pairs = db2_memory_l2_cross_key_pairs,
       .l2_fact_decision_pairs = db2_memory_l2_fact_vs_decision_pairs,
       .kb_directive_resolve = db2_kb_service_directive_resolve,
       .memory_link_create = db2_memory_link_create,
       .task_add_edge = db2_task_add_edge,
       .decision_log_active_id = db2_decision_log_active_id,
       .entity_node_get = db2_entity_node_get,
       .entity_node_alias_upsert = db2_entity_node_alias_upsert,
       .entity_edge_upsert = db2_entity_edge_upsert,
       .bandit_decision_insert = db2_bandit_decision_insert,
       .artifact_write = db2_artifact_write,
       .artifact_write_ex = db2_artifact_write_ex,
       .artifact_target_surface = db2_artifact_target_surface,
       .agent_outcome_record = db2_agent_outcome_record,
       .artifact_reject = db2_artifact_reject,
       .audit_event_write = db2_audit_event_write,
       .audit_latest_before = db2_audit_read_latest_before,
       .bandit_arm_stats_update = db2_bandit_arm_stats_update,
       .code_file_hash = db2_code_file_hash,
       .file_modified_since = db2_code_index_file_modified_since,
       .code_file_upsert = db2_code_index_file_upsert,
       .code_index_op_record = db2_code_index_op_record,
       .code_project_upsert = db2_code_index_project_upsert,
       .demotion_profile_read = db2_demotion_profile_read,
       .demotion_profile_write = db2_demotion_profile_write,
       .retrieval_attribution_write = db2_demotion_retrieval_attribution_write,
       .css_render_snapshot_store = db2_css_render_snapshot_store,
       .entity_node_upsert = db2_entity_node_upsert,
       .entity_profile_upsert = db2_entity_profile_upsert,
       .resolve_contradiction = db2_directive_resolve_contradiction,
       .enrollment_touch_last_seen = db2_enrollment_touch_last_seen,
       .retrieval_event_by_turn = db2_demotion_retrieval_event_by_turn,
       .kb_audit_append = db2_kb_audit_append,
       .feature_row_upsert = db2_feature_row_upsert,
       .feature_row_read = db2_feature_row_read,
       .async_enqueue = db2_kb_async_enqueue,
       .console_oidc_get = db2_console_oidc_get,
       .console_oidc_put = db2_console_oidc_put,
       .corpus_pipeline_status = db2_corpus_pipeline_status,
       .corpus_pipeline_drain = db2_corpus_pipeline_drain,
       .cross_repo_set_trust = db2_cross_repo_set_trust,
       .bandit_explore_stats = db2_bandit_explore_stats,
       .bandit_arm_stats_read = db2_bandit_arm_stats_read,
       .project_stats = canonical_index_project_stats,
       .recompute_blocked_symbols = db2_cross_repo_recompute_blocked_symbols,
   };
   return &backend;
}

/* db2-envelope-string-u32-v1: one bounded string in, one bounded number out.
 * Every operation on this format is handled the same way, so the shape lives
 * here and each family below carries a table of its own rows. `bound` is the
 * operation's own maximum, applied to whatever the backend answers -- an
 * existence probe must not put a two on a wire whose contract says zero or
 * one, whatever its backend returns. */
typedef struct
{
   int (*decode)(const uint8_t *input, size_t input_len, char *argument, size_t capacity);
   int (*encode)(uint32_t answer, uint8_t *output, size_t capacity, uint32_t *output_len);
   int (*read)(const char *argument);
   uint32_t bound;
} db2_string_count_binding_t;

/* The buffers below are sized by the generator from every operation on their
 * format, because sizing them by naming one operation's bound is correct only
 * until a wider one joins -- which is how a decode that should have succeeded
 * was refused by its own capacity check. */
#define DB2_STRING_COUNT_ARGUMENT_MAX AIMEE_DB2_STRING_COUNT_ARGUMENT_MAX

static aimee_module_status_t
db2_dispatch_string_count(const db2_string_count_binding_t *bindings, size_t count,
                          const uint8_t *request_body, uint32_t request_len, uint8_t *response_body,
                          size_t response_capacity, uint32_t *response_len,
                          const aimee_module_invocation_t *invocation, int *handled)
{
   char argument[DB2_STRING_COUNT_ARGUMENT_MAX + 1];
   *handled = 0;
   for (size_t index = 0; index < count; index++)
   {
      const db2_string_count_binding_t *binding = &bindings[index];
      if (binding->decode(request_body, request_len, argument, sizeof(argument)) != 0)
         continue;
      *handled = 1;
      if (response_capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!binding->read)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int counted = binding->read(argument);
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (counted < 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      uint32_t value = (uint32_t)counted;
      if (value > binding->bound)
         value = binding->bound;
      if (binding->encode(value, response_body, response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }
   return AIMEE_MODULE_STATUS_OK;
}

/* db2-envelope-string-ack-v1: one bounded string in, an acknowledgement out. */
typedef struct
{
   int (*decode)(const uint8_t *input, size_t input_len, char *argument, size_t capacity);
   int (*encode)(uint8_t *output, size_t capacity, uint32_t *output_len);
   int (*write)(const char *argument);
} db2_string_ack_binding_t;

static aimee_module_status_t
db2_dispatch_string_ack(const db2_string_ack_binding_t *bindings, size_t count,
                        const uint8_t *request_body, uint32_t request_len, uint8_t *response_body,
                        size_t response_capacity, uint32_t *response_len,
                        const aimee_module_invocation_t *invocation, int *handled)
{
   char argument[AIMEE_DB2_STRING_ACK_ARGUMENT_MAX + 1];
   *handled = 0;
   for (size_t index = 0; index < count; index++)
   {
      const db2_string_ack_binding_t *binding = &bindings[index];
      if (binding->decode(request_body, request_len, argument, sizeof(argument)) != 0)
         continue;
      *handled = 1;
      if (response_capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!binding->write)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int written = binding->write(argument);
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (written != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      if (binding->encode(response_body, response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }
   return AIMEE_MODULE_STATUS_OK;
}

/* db2-envelope-string-pair-ack-v1: two bounded strings in, an acknowledgement
 * out. The second buffer is sized independently of the first because these
 * carry an error text or a reason, which is wider than the identifier beside
 * it. */
typedef struct
{
   int (*decode)(const uint8_t *input, size_t input_len, char *first, size_t first_capacity,
                 char *second, size_t second_capacity);
   int (*encode)(uint8_t *output, size_t capacity, uint32_t *output_len);
   int (*write)(const char *first, const char *second);
} db2_string_pair_ack_binding_t;

#define DB2_STRING_PAIR_FIRST_MAX  AIMEE_DB2_STRING_PAIR_FIRST_MAX
#define DB2_STRING_PAIR_SECOND_MAX AIMEE_DB2_STRING_PAIR_SECOND_MAX

static aimee_module_status_t db2_dispatch_string_pair_ack(
    const db2_string_pair_ack_binding_t *bindings, size_t count, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, size_t response_capacity, uint32_t *response_len,
    const aimee_module_invocation_t *invocation, int *handled)
{
   char first[DB2_STRING_PAIR_FIRST_MAX + 1];
   char second[DB2_STRING_PAIR_SECOND_MAX + 1];
   *handled = 0;
   for (size_t index = 0; index < count; index++)
   {
      const db2_string_pair_ack_binding_t *binding = &bindings[index];
      if (binding->decode(request_body, request_len, first, sizeof(first), second,
                          sizeof(second)) != 0)
         continue;
      *handled = 1;
      if (response_capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!binding->write)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int written = binding->write(first, second);
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (written != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      if (binding->encode(response_body, response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }
   return AIMEE_MODULE_STATUS_OK;
}

/* db2-envelope-string-pair-u32-v1: two bounded strings in, one bounded number
 * out. Same arguments as the pair acknowledgement above; the difference is that
 * these answer something. */
typedef struct
{
   int (*decode)(const uint8_t *input, size_t input_len, char *first, size_t first_capacity,
                 char *second, size_t second_capacity);
   int (*encode)(uint32_t answer, uint8_t *output, size_t capacity, uint32_t *output_len);
   int (*read)(const char *first, const char *second);
   uint32_t bound;
} db2_string_pair_count_binding_t;

static aimee_module_status_t db2_dispatch_string_pair_count(
    const db2_string_pair_count_binding_t *bindings, size_t count, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, size_t response_capacity, uint32_t *response_len,
    const aimee_module_invocation_t *invocation, int *handled)
{
   char first[DB2_STRING_PAIR_FIRST_MAX + 1];
   char second[DB2_STRING_PAIR_SECOND_MAX + 1];
   *handled = 0;
   for (size_t index = 0; index < count; index++)
   {
      const db2_string_pair_count_binding_t *binding = &bindings[index];
      if (binding->decode(request_body, request_len, first, sizeof(first), second,
                          sizeof(second)) != 0)
         continue;
      *handled = 1;
      if (response_capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!binding->read)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int counted = binding->read(first, second);
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (counted < 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      uint32_t value = (uint32_t)counted;
      if (value > binding->bound)
         value = binding->bound;
      if (binding->encode(value, response_body, response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }
   return AIMEE_MODULE_STATUS_OK;
}

/* db2-envelope-u64-u32-v1: one positive identifier in, one Boolean out. A
 * backend that cannot run its statement answers negative, which is reported as
 * false rather than invented as true. */
typedef struct
{
   int (*decode)(const uint8_t *input, size_t input_len, uint64_t *identifier);
   int (*encode)(uint32_t exists, uint8_t *output, size_t capacity, uint32_t *output_len);
   int (*read)(int64_t identifier);
} db2_u64_probe_binding_t;

static aimee_module_status_t
db2_dispatch_u64_probe(const db2_u64_probe_binding_t *bindings, size_t count,
                       const uint8_t *request_body, uint32_t request_len, uint8_t *response_body,
                       size_t response_capacity, uint32_t *response_len,
                       const aimee_module_invocation_t *invocation, int *handled)
{
   uint64_t identifier = 0u;
   *handled = 0;
   for (size_t index = 0; index < count; index++)
   {
      const db2_u64_probe_binding_t *binding = &bindings[index];
      if (binding->decode(request_body, request_len, &identifier) != 0)
         continue;
      *handled = 1;
      if (response_capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!binding->read)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int found = binding->read((int64_t)identifier);
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (binding->encode(found > 0 ? 1u : 0u, response_body, response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }
   return AIMEE_MODULE_STATUS_OK;
}

/* db2-envelope-u64-ack-v1: one positive identifier in, an acknowledgement out. */
typedef struct
{
   int (*decode)(const uint8_t *input, size_t input_len, uint64_t *identifier);
   int (*encode)(uint8_t *output, size_t capacity, uint32_t *output_len);
   int (*write)(int64_t identifier);
} db2_u64_ack_binding_t;

static aimee_module_status_t db2_dispatch_u64_ack(const db2_u64_ack_binding_t *bindings,
                                                  size_t count, const uint8_t *request_body,
                                                  uint32_t request_len, uint8_t *response_body,
                                                  size_t response_capacity, uint32_t *response_len,
                                                  const aimee_module_invocation_t *invocation,
                                                  int *handled)
{
   uint64_t identifier = 0u;
   *handled = 0;
   for (size_t index = 0; index < count; index++)
   {
      const db2_u64_ack_binding_t *binding = &bindings[index];
      if (binding->decode(request_body, request_len, &identifier) != 0)
         continue;
      *handled = 1;
      if (response_capacity < AIMEE_DB2_ENVELOPE_HEADER_LEN)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (!binding->write)
         return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
      int written = binding->write((int64_t)identifier);
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      if (written != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      if (binding->encode(response_body, response_capacity, response_len) != 0)
         return AIMEE_MODULE_STATUS_INTERNAL;
      return AIMEE_MODULE_STATUS_OK;
   }
   return AIMEE_MODULE_STATUS_OK;
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
         if (!encode && aimee_db2_search_facts_patterns_by_keyword_request_decode(
                            request_body, request_len, term, sizeof(term), &limit, &scope_flags,
                            workspace, sizeof(workspace), project, sizeof(project)) == 0)
         {
            read = backend ? backend->search_facts_patterns_by_keyword : NULL;
            encode = aimee_db2_search_facts_patterns_by_keyword_reply_encode;
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
      {
         /* Both row getters decode the same way and differ only in which
          * reviewed backend they reach, so one branch serves both. */
         uint64_t identifier = 0u;
         int (*read)(int64_t, aimee_db2_memory_row_t *) = NULL;
         int (*encode)(uint32_t, const aimee_db2_memory_row_t *, uint8_t *, size_t, uint32_t *) =
             NULL;
         if (!encode &&
             aimee_db2_row_get_request_decode(request_body, request_len, &identifier) == 0)
         {
            read = backend ? backend->row_get : NULL;
            encode = aimee_db2_row_get_reply_encode;
         }
         if (!encode && aimee_db2_row_get_by_unit_id_request_decode(request_body, request_len,
                                                                    &identifier) == 0)
         {
            read = backend ? backend->row_get_by_unit_id : NULL;
            encode = aimee_db2_row_get_by_unit_id_reply_encode;
         }
         if (encode)
         {
            if (response_capacity < AIMEE_DB2_ROW_GET_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!read)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_memory_row_t row;
            int found = read((int64_t)identifier, &row);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            /* The backend cannot separate an absent row from a statement that
             * did not run, so not_found is the honest report for both. */
            if (encode(found == 0 ? AIMEE_DB2_RESULT_OK : AIMEE_DB2_RESULT_NOT_FOUND,
                       found == 0 ? &row : NULL, response_body, response_capacity,
                       response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t history_limit = 0u;
         char history_key[AIMEE_DB2_FACT_HISTORY_KEY_MAX + 1];
         if (aimee_db2_fact_history_request_decode(request_body, request_len, history_key,
                                                   sizeof(history_key), &history_limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_FACT_HISTORY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->fact_history)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            int64_t rows[AIMEE_DB2_FACT_HISTORY_MAX];
            int listed =
                backend->fact_history(history_key, (int)history_limit, rows, (int)history_limit);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (listed < 0 || listed > (int)history_limit)
               return AIMEE_MODULE_STATUS_INTERNAL;
            uint64_t memory_ids[AIMEE_DB2_FACT_HISTORY_MAX];
            for (int index = 0; index < listed; index++)
            {
               if (rows[index] < (int64_t)AIMEE_DB2_FACT_HISTORY_ID_MIN)
                  return AIMEE_MODULE_STATUS_INTERNAL;
               memory_ids[index] = (uint64_t)rows[index];
            }
            if (aimee_db2_fact_history_reply_encode(memory_ids, (uint32_t)listed, response_body,
                                                    response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t list_limit = 0u, list_scope = 0u, list_hide = 0u;
         char list_tier[AIMEE_DB2_LIST_ROWS_TIER_MAX + 1];
         char list_kind[AIMEE_DB2_LIST_ROWS_KIND_MAX + 1];
         char list_workspace[AIMEE_DB2_LIST_ROWS_WORKSPACE_MAX + 1];
         char list_project[AIMEE_DB2_LIST_ROWS_PROJECT_MAX + 1];
         if (aimee_db2_list_rows_request_decode(
                 request_body, request_len, &list_limit, &list_scope, &list_hide, list_tier,
                 sizeof(list_tier), list_kind, sizeof(list_kind), list_workspace,
                 sizeof(list_workspace), list_project, sizeof(list_project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_LIST_ROWS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->list_rows)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            int64_t rows[AIMEE_DB2_LIST_ROWS_MAX];
            /* An empty tier or kind reaches the backend as an empty string,
             * which is exactly how it decides to leave that clause out. */
            int listed = backend->list_rows(list_tier, list_kind, (int)list_hide, (int)list_limit,
                                            (int)(list_scope & 1u), (int)((list_scope >> 1) & 1u),
                                            list_workspace, list_project, rows, (int)list_limit);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (listed < 0 || listed > (int)list_limit)
               return AIMEE_MODULE_STATUS_INTERNAL;
            uint64_t memory_ids[AIMEE_DB2_LIST_ROWS_MAX];
            for (int index = 0; index < listed; index++)
            {
               if (rows[index] < (int64_t)AIMEE_DB2_LIST_ROWS_ID_MIN)
                  return AIMEE_MODULE_STATUS_INTERNAL;
               memory_ids[index] = (uint64_t)rows[index];
            }
            if (aimee_db2_list_rows_reply_encode(memory_ids, (uint32_t)listed, response_body,
                                                 response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t aggregate_limit = 0u;
         char aggregate_entity[AIMEE_DB2_AGGREGATE_ENTITY_SEED_MAX + 1];
         char aggregate_keyword[AIMEE_DB2_AGGREGATE_KEYWORD_MAX + 1];
         if (aimee_db2_aggregate_request_decode(request_body, request_len, aggregate_entity,
                                                sizeof(aggregate_entity), aggregate_keyword,
                                                sizeof(aggregate_keyword), &aggregate_limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_AGGREGATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->aggregate)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            int64_t rows[AIMEE_DB2_AGGREGATE_MAX];
            int truncated = 0;
            int listed =
                backend->aggregate(aggregate_entity, aggregate_keyword, (int)aggregate_limit,
                                   &truncated, rows, (int)aggregate_limit);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (listed < 0 || listed > (int)aggregate_limit)
               return AIMEE_MODULE_STATUS_INTERNAL;
            uint64_t memory_ids[AIMEE_DB2_AGGREGATE_MAX];
            for (int index = 0; index < listed; index++)
            {
               if (rows[index] < (int64_t)AIMEE_DB2_AGGREGATE_ID_MIN)
                  return AIMEE_MODULE_STATUS_INTERNAL;
               memory_ids[index] = (uint64_t)rows[index];
            }
            if (aimee_db2_aggregate_reply_encode(truncated ? 1u : 0u, memory_ids, (uint32_t)listed,
                                                 response_body, response_capacity,
                                                 response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t corpus_limit = 0u;
         if (aimee_db2_load_eval_corpus_request_decode(request_body, request_len, &corpus_limit) ==
             0)
         {
            if (response_capacity < AIMEE_DB2_LOAD_EVAL_CORPUS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->load_eval_corpus)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            int64_t rows[AIMEE_DB2_LOAD_EVAL_CORPUS_MAX];
            char label[AIMEE_DB2_LOAD_EVAL_CORPUS_LABEL_MAX + 1] = "";
            int listed = backend->load_eval_corpus((int)corpus_limit, label, sizeof(label), rows,
                                                   (int)corpus_limit);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (listed < 0 || listed > (int)corpus_limit)
               return AIMEE_MODULE_STATUS_INTERNAL;
            uint64_t memory_ids[AIMEE_DB2_LOAD_EVAL_CORPUS_MAX];
            for (int index = 0; index < listed; index++)
            {
               if (rows[index] < (int64_t)AIMEE_DB2_LOAD_EVAL_CORPUS_ID_MIN)
                  return AIMEE_MODULE_STATUS_INTERNAL;
               memory_ids[index] = (uint64_t)rows[index];
            }
            if (aimee_db2_load_eval_corpus_reply_encode(label, memory_ids, (uint32_t)listed,
                                                        response_body, response_capacity,
                                                        response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         /* Every db2-envelope-u64-u32-v1 operation this family owns. */
         const db2_u64_probe_binding_t bindings[] = {
             {aimee_db2_record_exists_request_decode, aimee_db2_record_exists_reply_encode,
              backend ? backend->record_exists : NULL},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_u64_probe(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         uint64_t prospective_id = 0u;
         char new_state[AIMEE_DB2_PROSPECTIVE_SET_STATE_NEW_STATE_MAX + 1] = "";
         if (aimee_db2_prospective_set_state_request_decode(
                 request_body, request_len, &prospective_id, new_state, sizeof(new_state)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROSPECTIVE_SET_STATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->prospective_set_state)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->prospective_set_state((int64_t)prospective_id, new_state) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_prospective_set_state_reply_encode(acknowledged, response_body,
                                                             response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t dry_run = 0u;
         if (aimee_db2_dedupe_by_key_request_decode(request_body, request_len, &dry_run) == 0)
         {
            if (response_capacity < AIMEE_DB2_DEDUPE_BY_KEY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->dedupe_by_key)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t merged = 0u;
            {
               int merged_rows = backend->dedupe_by_key((int)dry_run);
               merged = merged_rows < 0 ? 0u : (uint32_t)merged_rows;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_dedupe_by_key_reply_encode(merged, response_body, response_capacity,
                                                     response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t scene_memory_id = 0u;
         uint64_t scene_id = 0u;
         if (aimee_db2_scene_member_exists_request_decode(request_body, request_len,
                                                          &scene_memory_id, &scene_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_SCENE_MEMBER_EXISTS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->scene_member_exists)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t member = 0u;
            {
               int found =
                   backend->scene_member_exists((int64_t)scene_memory_id, (int64_t)scene_id);
               member = found > 0 ? 1u : 0u;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_scene_member_exists_reply_encode(member, response_body, response_capacity,
                                                           response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t unit_id_a = 0u;
         uint64_t unit_id_b = 0u;
         if (aimee_db2_unit_edge_exists_request_decode(request_body, request_len, &unit_id_a,
                                                       &unit_id_b) == 0)
         {
            if (response_capacity < AIMEE_DB2_UNIT_EDGE_EXISTS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->unit_edge_exists)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t connected = 0u;
            {
               int found = backend->unit_edge_exists((int64_t)unit_id_a, (int64_t)unit_id_b);
               connected = found > 0 ? 1u : 0u;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_unit_edge_exists_reply_encode(connected, response_body, response_capacity,
                                                        response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char error_lowered[AIMEE_DB2_MATCH_ERROR_KEYS_ERROR_LOWERED_MAX + 1] = "";
         if (aimee_db2_match_error_keys_request_decode(request_body, request_len, error_lowered,
                                                       sizeof(error_lowered)) == 0)
         {
            if (response_capacity < AIMEE_DB2_MATCH_ERROR_KEYS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->match_error_keys)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_match_error_keys_row_t rows[AIMEE_DB2_MATCH_ERROR_KEYS_MAX_ROWS];
            uint32_t count = 0u;
            {
               int64_t ids[AIMEE_DB2_MATCH_ERROR_KEYS_MAX_ROWS];
               int found = backend->match_error_keys(error_lowered, ids,
                                                     AIMEE_DB2_MATCH_ERROR_KEYS_MAX_ROWS);
               for (int index = 0; index < found; index++)
                  rows[index].memory_id = (uint64_t)ids[index];
               count = found < 0 ? 0u : (uint32_t)found;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_match_error_keys_reply_encode(rows, count, response_body,
                                                        response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t limit = 0u;
         if (aimee_db2_memory_ids_by_updated_request_decode(request_body, request_len, &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_MEMORY_IDS_BY_UPDATED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->memory_ids_by_updated)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_memory_ids_by_updated_row_t rows[AIMEE_DB2_MEMORY_IDS_BY_UPDATED_MAX_ROWS];
            uint32_t count = 0u;
            {
               int64_t ids[AIMEE_DB2_MEMORY_IDS_BY_UPDATED_MAX_ROWS];
               int found = backend->memory_ids_by_updated((int)limit, ids,
                                                          AIMEE_DB2_MEMORY_IDS_BY_UPDATED_MAX_ROWS);
               for (int index = 0; index < found; index++)
                  rows[index].memory_id = (uint64_t)ids[index];
               count = found < 0 ? 0u : (uint32_t)found;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_memory_ids_by_updated_reply_encode(rows, count, response_body,
                                                             response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t memory_id = 0u;
         if (aimee_db2_unit_ids_for_memory_request_decode(request_body, request_len, &memory_id) ==
             0)
         {
            if (response_capacity < AIMEE_DB2_UNIT_IDS_FOR_MEMORY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->unit_ids_for_memory)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_unit_ids_for_memory_row_t rows[AIMEE_DB2_UNIT_IDS_FOR_MEMORY_MAX_ROWS];
            uint32_t count = 0u;
            {
               int64_t ids[AIMEE_DB2_UNIT_IDS_FOR_MEMORY_MAX_ROWS];
               int found = backend->unit_ids_for_memory((int64_t)memory_id, ids,
                                                        AIMEE_DB2_UNIT_IDS_FOR_MEMORY_MAX_ROWS);
               for (int index = 0; index < found; index++)
                  rows[index].unit_id = (uint64_t)ids[index];
               count = found < 0 ? 0u : (uint32_t)found;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_unit_ids_for_memory_reply_encode(rows, count, response_body,
                                                           response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t limit = 0u;
         if (aimee_db2_briefing_active_entities_request_decode(request_body, request_len, &limit) ==
             0)
         {
            if (response_capacity < AIMEE_DB2_BRIEFING_ACTIVE_ENTITIES_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->briefing_active_entities)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_briefing_active_entities_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_BRIEFING_ACTIVE_ENTITIES_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_briefing_entity_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_BRIEFING_ACTIVE_ENTITIES_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int requested = (int)limit;
               if (requested > (int)AIMEE_DB2_BRIEFING_ACTIVE_ENTITIES_MAX_ROWS)
                  requested = (int)AIMEE_DB2_BRIEFING_ACTIVE_ENTITIES_MAX_ROWS;
               int written = backend->briefing_active_entities(found, requested);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].entity, sizeof(rows[index].entity), "%s", found[index].name);
                  rows[index].mentions =
                      found[index].mentions < 0 ? 0u : (uint32_t)found[index].mentions;
                  snprintf(rows[index].last_seen, sizeof(rows[index].last_seen), "%s",
                           found[index].last_seen);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_briefing_active_entities_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char state_filter[AIMEE_DB2_PROSPECTIVE_LIST_STATE_FILTER_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_prospective_list_request_decode(request_body, request_len, state_filter,
                                                       sizeof(state_filter), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROSPECTIVE_LIST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->prospective_list)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_prospective_list_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_PROSPECTIVE_LIST_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_prospective_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_PROSPECTIVE_LIST_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->prospective_list(state_filter, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].prospective_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].trigger_count =
                      found[index].trigger_count < 0 ? 0u : (uint32_t)found[index].trigger_count;
                  snprintf(rows[index].trigger_text, sizeof(rows[index].trigger_text), "%s",
                           found[index].trigger_text);
                  snprintf(rows[index].action_text, sizeof(rows[index].action_text), "%s",
                           found[index].action_text);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].recurrence, sizeof(rows[index].recurrence), "%s",
                           found[index].recurrence);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_triggered_at, sizeof(rows[index].last_triggered_at),
                           "%s", found[index].last_triggered_at);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_prospective_list_reply_encode(rows, count, response_body,
                                                        response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         if (aimee_db2_prospective_list_armed_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROSPECTIVE_LIST_ARMED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->prospective_list_armed)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_prospective_list_armed_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_PROSPECTIVE_LIST_ARMED_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_prospective_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_PROSPECTIVE_LIST_ARMED_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->prospective_list_armed(
                   found, AIMEE_DB2_PROSPECTIVE_LIST_ARMED_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].prospective_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].trigger_count =
                      found[index].trigger_count < 0 ? 0u : (uint32_t)found[index].trigger_count;
                  snprintf(rows[index].trigger_text, sizeof(rows[index].trigger_text), "%s",
                           found[index].trigger_text);
                  snprintf(rows[index].action_text, sizeof(rows[index].action_text), "%s",
                           found[index].action_text);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].recurrence, sizeof(rows[index].recurrence), "%s",
                           found[index].recurrence);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_triggered_at, sizeof(rows[index].last_triggered_at),
                           "%s", found[index].last_triggered_at);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_prospective_list_armed_reply_encode(rows, count, response_body,
                                                              response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity_lowered[AIMEE_DB2_PROSPECTIVE_BY_ENTITY_ENTITY_LOWERED_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_prospective_by_entity_request_decode(
                 request_body, request_len, entity_lowered, sizeof(entity_lowered), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROSPECTIVE_BY_ENTITY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->prospective_by_entity)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_prospective_by_entity_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_PROSPECTIVE_BY_ENTITY_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_prospective_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_PROSPECTIVE_BY_ENTITY_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->prospective_by_entity(entity_lowered, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].prospective_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].trigger_count =
                      found[index].trigger_count < 0 ? 0u : (uint32_t)found[index].trigger_count;
                  snprintf(rows[index].trigger_text, sizeof(rows[index].trigger_text), "%s",
                           found[index].trigger_text);
                  snprintf(rows[index].action_text, sizeof(rows[index].action_text), "%s",
                           found[index].action_text);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].recurrence, sizeof(rows[index].recurrence), "%s",
                           found[index].recurrence);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_triggered_at, sizeof(rows[index].last_triggered_at),
                           "%s", found[index].last_triggered_at);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_prospective_by_entity_reply_encode(rows, count, response_body,
                                                             response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char file_anchor[AIMEE_DB2_PROSPECTIVE_BY_FILE_FILE_ANCHOR_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_prospective_by_file_request_decode(request_body, request_len, file_anchor,
                                                          sizeof(file_anchor), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROSPECTIVE_BY_FILE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->prospective_by_file)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_prospective_by_file_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_PROSPECTIVE_BY_FILE_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_prospective_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_PROSPECTIVE_BY_FILE_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->prospective_by_file(file_anchor, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].prospective_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].trigger_count =
                      found[index].trigger_count < 0 ? 0u : (uint32_t)found[index].trigger_count;
                  snprintf(rows[index].trigger_text, sizeof(rows[index].trigger_text), "%s",
                           found[index].trigger_text);
                  snprintf(rows[index].action_text, sizeof(rows[index].action_text), "%s",
                           found[index].action_text);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].recurrence, sizeof(rows[index].recurrence), "%s",
                           found[index].recurrence);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_triggered_at, sizeof(rows[index].last_triggered_at),
                           "%s", found[index].last_triggered_at);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_prospective_by_file_reply_encode(rows, count, response_body,
                                                           response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char turn_text[AIMEE_DB2_PROSPECTIVE_BY_TRIGGER_TERMS_TURN_TEXT_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_prospective_by_trigger_terms_request_decode(
                 request_body, request_len, turn_text, sizeof(turn_text), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROSPECTIVE_BY_TRIGGER_TERMS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->prospective_by_trigger_terms)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_prospective_by_trigger_terms_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_PROSPECTIVE_BY_TRIGGER_TERMS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_prospective_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_PROSPECTIVE_BY_TRIGGER_TERMS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->prospective_by_trigger_terms(turn_text, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].prospective_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].trigger_count =
                      found[index].trigger_count < 0 ? 0u : (uint32_t)found[index].trigger_count;
                  snprintf(rows[index].trigger_text, sizeof(rows[index].trigger_text), "%s",
                           found[index].trigger_text);
                  snprintf(rows[index].action_text, sizeof(rows[index].action_text), "%s",
                           found[index].action_text);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].recurrence, sizeof(rows[index].recurrence), "%s",
                           found[index].recurrence);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_triggered_at, sizeof(rows[index].last_triggered_at),
                           "%s", found[index].last_triggered_at);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_prospective_by_trigger_terms_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_RELATIONS_FOR_ENTITY_ENTITY_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_relations_for_entity_request_decode(request_body, request_len, entity,
                                                           sizeof(entity), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_RELATIONS_FOR_ENTITY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->relations_for_entity)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_relations_for_entity_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_RELATIONS_FOR_ENTITY_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_relation_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_RELATIONS_FOR_ENTITY_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->relations_for_entity(entity, (int)limit, found,
                                                           AIMEE_DB2_RELATIONS_FOR_ENTITY_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].relation_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].relation_memory_id =
                      found[index].memory_id < 0 ? 0u : (uint64_t)found[index].memory_id;
                  rows[index].episode_id =
                      found[index].episode_id < 0 ? 0u : (uint64_t)found[index].episode_id;
                  rows[index].relation_weight = found[index].weight;
                  snprintf(rows[index].src_entity, sizeof(rows[index].src_entity), "%s",
                           found[index].src_entity);
                  snprintf(rows[index].relation_name, sizeof(rows[index].relation_name), "%s",
                           found[index].relation);
                  snprintf(rows[index].dst_entity, sizeof(rows[index].dst_entity), "%s",
                           found[index].dst_entity);
                  snprintf(rows[index].fact_text, sizeof(rows[index].fact_text), "%s",
                           found[index].fact_text);
                  snprintf(rows[index].valid_at, sizeof(rows[index].valid_at), "%s",
                           found[index].valid_at);
                  snprintf(rows[index].invalid_at, sizeof(rows[index].invalid_at), "%s",
                           found[index].invalid_at);
                  snprintf(rows[index].relation_created_at, sizeof(rows[index].relation_created_at),
                           "%s", found[index].created_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_relations_for_entity_reply_encode(rows, count, response_body,
                                                            response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char relation_query[AIMEE_DB2_RELATIONS_SEARCH_RELATION_QUERY_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_relations_search_request_decode(request_body, request_len, relation_query,
                                                       sizeof(relation_query), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_RELATIONS_SEARCH_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->relations_search)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_relations_search_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_RELATIONS_SEARCH_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_relation_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_RELATIONS_SEARCH_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->relations_search(relation_query, (int)limit, found,
                                                       AIMEE_DB2_RELATIONS_SEARCH_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].relation_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].relation_memory_id =
                      found[index].memory_id < 0 ? 0u : (uint64_t)found[index].memory_id;
                  rows[index].episode_id =
                      found[index].episode_id < 0 ? 0u : (uint64_t)found[index].episode_id;
                  rows[index].relation_weight = found[index].weight;
                  snprintf(rows[index].src_entity, sizeof(rows[index].src_entity), "%s",
                           found[index].src_entity);
                  snprintf(rows[index].relation_name, sizeof(rows[index].relation_name), "%s",
                           found[index].relation);
                  snprintf(rows[index].dst_entity, sizeof(rows[index].dst_entity), "%s",
                           found[index].dst_entity);
                  snprintf(rows[index].fact_text, sizeof(rows[index].fact_text), "%s",
                           found[index].fact_text);
                  snprintf(rows[index].valid_at, sizeof(rows[index].valid_at), "%s",
                           found[index].valid_at);
                  snprintf(rows[index].invalid_at, sizeof(rows[index].invalid_at), "%s",
                           found[index].invalid_at);
                  snprintf(rows[index].relation_created_at, sizeof(rows[index].relation_created_at),
                           "%s", found[index].created_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_relations_search_reply_encode(rows, count, response_body,
                                                        response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char relation_query[AIMEE_DB2_RELATIONS_SEARCH_AS_OF_RELATION_QUERY_MAX + 1] = "";
         char as_of[AIMEE_DB2_RELATIONS_SEARCH_AS_OF_AS_OF_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_relations_search_as_of_request_decode(request_body, request_len,
                                                             relation_query, sizeof(relation_query),
                                                             as_of, sizeof(as_of), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_RELATIONS_SEARCH_AS_OF_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->relations_search_as_of)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_relations_search_as_of_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_RELATIONS_SEARCH_AS_OF_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_relation_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_RELATIONS_SEARCH_AS_OF_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written =
                   backend->relations_search_as_of(relation_query, as_of, (int)limit, found,
                                                   AIMEE_DB2_RELATIONS_SEARCH_AS_OF_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].relation_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].relation_memory_id =
                      found[index].memory_id < 0 ? 0u : (uint64_t)found[index].memory_id;
                  rows[index].episode_id =
                      found[index].episode_id < 0 ? 0u : (uint64_t)found[index].episode_id;
                  rows[index].relation_weight = found[index].weight;
                  snprintf(rows[index].src_entity, sizeof(rows[index].src_entity), "%s",
                           found[index].src_entity);
                  snprintf(rows[index].relation_name, sizeof(rows[index].relation_name), "%s",
                           found[index].relation);
                  snprintf(rows[index].dst_entity, sizeof(rows[index].dst_entity), "%s",
                           found[index].dst_entity);
                  snprintf(rows[index].fact_text, sizeof(rows[index].fact_text), "%s",
                           found[index].fact_text);
                  snprintf(rows[index].valid_at, sizeof(rows[index].valid_at), "%s",
                           found[index].valid_at);
                  snprintf(rows[index].invalid_at, sizeof(rows[index].invalid_at), "%s",
                           found[index].invalid_at);
                  snprintf(rows[index].relation_created_at, sizeof(rows[index].relation_created_at),
                           "%s", found[index].created_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_relations_search_as_of_reply_encode(rows, count, response_body,
                                                              response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity_token[AIMEE_DB2_RELATIONS_SUPPORTING_ENTITY_TOKEN_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_relations_supporting_request_decode(request_body, request_len, entity_token,
                                                           sizeof(entity_token), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_RELATIONS_SUPPORTING_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->relations_supporting)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_relations_supporting_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_RELATIONS_SUPPORTING_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_relation_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_RELATIONS_SUPPORTING_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->relations_supporting(entity_token, (int)limit, found,
                                                           AIMEE_DB2_RELATIONS_SUPPORTING_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].relation_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].relation_memory_id =
                      found[index].memory_id < 0 ? 0u : (uint64_t)found[index].memory_id;
                  rows[index].episode_id =
                      found[index].episode_id < 0 ? 0u : (uint64_t)found[index].episode_id;
                  rows[index].relation_weight = found[index].weight;
                  snprintf(rows[index].src_entity, sizeof(rows[index].src_entity), "%s",
                           found[index].src_entity);
                  snprintf(rows[index].relation_name, sizeof(rows[index].relation_name), "%s",
                           found[index].relation);
                  snprintf(rows[index].dst_entity, sizeof(rows[index].dst_entity), "%s",
                           found[index].dst_entity);
                  snprintf(rows[index].fact_text, sizeof(rows[index].fact_text), "%s",
                           found[index].fact_text);
                  snprintf(rows[index].valid_at, sizeof(rows[index].valid_at), "%s",
                           found[index].valid_at);
                  snprintf(rows[index].invalid_at, sizeof(rows[index].invalid_at), "%s",
                           found[index].invalid_at);
                  snprintf(rows[index].relation_created_at, sizeof(rows[index].relation_created_at),
                           "%s", found[index].created_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_relations_supporting_reply_encode(rows, count, response_body,
                                                            response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char fact_subject[AIMEE_DB2_TYPED_FACT_RECALL_FACT_SUBJECT_MAX + 1] = "";
         char relation_filter[AIMEE_DB2_TYPED_FACT_RECALL_RELATION_FILTER_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_typed_fact_recall_request_decode(request_body, request_len, fact_subject,
                                                        sizeof(fact_subject), relation_filter,
                                                        sizeof(relation_filter), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_TYPED_FACT_RECALL_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->typed_fact_recall)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_typed_fact_recall_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_TYPED_FACT_RECALL_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               typed_fact_t *found = malloc(sizeof(*found) * AIMEE_DB2_TYPED_FACT_RECALL_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written =
                   backend->typed_fact_recall(fact_subject, relation_filter, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].fact_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].fact_confidence =
                      found[index].confidence < 0 ? 0u : (uint32_t)found[index].confidence;
                  snprintf(rows[index].subject, sizeof(rows[index].subject), "%s",
                           found[index].subject);
                  snprintf(rows[index].subject_kind, sizeof(rows[index].subject_kind), "%s",
                           found[index].subject_kind);
                  snprintf(rows[index].fact_relation, sizeof(rows[index].fact_relation), "%s",
                           found[index].relation);
                  snprintf(rows[index].object, sizeof(rows[index].object), "%s",
                           found[index].object);
                  snprintf(rows[index].object_kind, sizeof(rows[index].object_kind), "%s",
                           found[index].object_kind);
                  snprintf(rows[index].fact_source, sizeof(rows[index].fact_source), "%s",
                           found[index].source);
                  snprintf(rows[index].asserted_at, sizeof(rows[index].asserted_at), "%s",
                           found[index].asserted_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_typed_fact_recall_reply_encode(rows, count, response_body,
                                                         response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         if (aimee_db2_global_constraints_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_GLOBAL_CONSTRAINTS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->global_constraints)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_global_constraints_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_GLOBAL_CONSTRAINTS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_kv_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_GLOBAL_CONSTRAINTS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written =
                   backend->global_constraints(found, AIMEE_DB2_GLOBAL_CONSTRAINTS_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].memory_key, sizeof(rows[index].memory_key), "%s",
                           found[index].key);
                  snprintf(rows[index].memory_content, sizeof(rows[index].memory_content), "%s",
                           found[index].content);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_global_constraints_reply_encode(rows, count, response_body,
                                                          response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t kv_section = 0u;
         if (aimee_db2_kv_section_request_decode(request_body, request_len, &kv_section) == 0)
         {
            if (response_capacity < AIMEE_DB2_KV_SECTION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->kv_section)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_kv_section_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_KV_SECTION_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_kv_row_t *found = malloc(sizeof(*found) * AIMEE_DB2_KV_SECTION_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->kv_section((db2_memory_section_t)kv_section, found,
                                                 AIMEE_DB2_KV_SECTION_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].memory_key, sizeof(rows[index].memory_key), "%s",
                           found[index].key);
                  snprintf(rows[index].memory_content, sizeof(rows[index].memory_content), "%s",
                           found[index].content);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_kv_section_reply_encode(rows, count, response_body, response_capacity,
                                                  response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char memory_key_exact[AIMEE_DB2_MEMORIES_BY_KEY_MEMORY_KEY_EXACT_MAX + 1] = "";
         if (aimee_db2_memories_by_key_request_decode(request_body, request_len, memory_key_exact,
                                                      sizeof(memory_key_exact)) == 0)
         {
            if (response_capacity < AIMEE_DB2_MEMORIES_BY_KEY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->memories_by_key)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_memories_by_key_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_MEMORIES_BY_KEY_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_id_content_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_MEMORIES_BY_KEY_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->memories_by_key(memory_key_exact, found,
                                                      AIMEE_DB2_MEMORIES_BY_KEY_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].memory_row_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  snprintf(rows[index].memory_content, sizeof(rows[index].memory_content), "%s",
                           found[index].content);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_memories_by_key_reply_encode(rows, count, response_body,
                                                       response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char memory_session_id[AIMEE_DB2_SESSION_MEMORIES_MEMORY_SESSION_ID_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_session_memories_request_decode(request_body, request_len, memory_session_id,
                                                       sizeof(memory_session_id), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_SESSION_MEMORIES_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->session_memories)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_session_memories_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_SESSION_MEMORIES_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_id_content_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_SESSION_MEMORIES_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->session_memories(memory_session_id, (int)limit, found,
                                                       AIMEE_DB2_SESSION_MEMORIES_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].memory_row_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  snprintf(rows[index].memory_content, sizeof(rows[index].memory_content), "%s",
                           found[index].content);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_session_memories_reply_encode(rows, count, response_body,
                                                        response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t candidate_filter = 0u;
         if (aimee_db2_memory_candidates_request_decode(request_body, request_len,
                                                        &candidate_filter) == 0)
         {
            if (response_capacity < AIMEE_DB2_MEMORY_CANDIDATES_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->memory_candidates)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_memory_candidates_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_MEMORY_CANDIDATES_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_cand_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_MEMORY_CANDIDATES_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written =
                   backend->memory_candidates((db2_memory_cand_filter_t)candidate_filter, found,
                                              AIMEE_DB2_MEMORY_CANDIDATES_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].memory_row_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].use_count =
                      found[index].use_count < 0 ? 0u : (uint32_t)found[index].use_count;
                  rows[index].memory_confidence = found[index].confidence;
                  snprintf(rows[index].memory_tier, sizeof(rows[index].memory_tier), "%s",
                           found[index].tier);
                  snprintf(rows[index].memory_key, sizeof(rows[index].memory_key), "%s",
                           found[index].key);
                  snprintf(rows[index].memory_content, sizeof(rows[index].memory_content), "%s",
                           found[index].content);
                  snprintf(rows[index].memory_kind, sizeof(rows[index].memory_kind), "%s",
                           found[index].kind);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_memory_candidates_reply_encode(rows, count, response_body,
                                                         response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t recall_section = 0u;
         if (aimee_db2_recall_section_request_decode(request_body, request_len, &recall_section) ==
             0)
         {
            if (response_capacity < AIMEE_DB2_RECALL_SECTION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->recall_section)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_recall_section_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_RECALL_SECTION_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_cand_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_RECALL_SECTION_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->recall_section((db2_memory_recall_section_t)recall_section,
                                                     found, AIMEE_DB2_RECALL_SECTION_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].memory_row_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  snprintf(rows[index].memory_tier, sizeof(rows[index].memory_tier), "%s",
                           found[index].tier);
                  snprintf(rows[index].memory_key, sizeof(rows[index].memory_key), "%s",
                           found[index].key);
                  snprintf(rows[index].memory_content, sizeof(rows[index].memory_content), "%s",
                           found[index].content);
                  snprintf(rows[index].memory_kind, sizeof(rows[index].memory_kind), "%s",
                           found[index].kind);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_recall_section_reply_encode(rows, count, response_body, response_capacity,
                                                      response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t max_pairs = 0u;
         if (aimee_db2_l2_cross_key_pairs_request_decode(request_body, request_len, &max_pairs) ==
             0)
         {
            if (response_capacity < AIMEE_DB2_L2_CROSS_KEY_PAIRS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->l2_cross_key_pairs)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_l2_cross_key_pairs_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_L2_CROSS_KEY_PAIRS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_pair_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_L2_CROSS_KEY_PAIRS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->l2_cross_key_pairs((int)max_pairs, found,
                                                         AIMEE_DB2_L2_CROSS_KEY_PAIRS_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].memory_id_a =
                      found[index].id_a < 0 ? 0u : (uint64_t)found[index].id_a;
                  rows[index].memory_id_b =
                      found[index].id_b < 0 ? 0u : (uint64_t)found[index].id_b;
                  snprintf(rows[index].content_a, sizeof(rows[index].content_a), "%s",
                           found[index].content_a);
                  snprintf(rows[index].content_b, sizeof(rows[index].content_b), "%s",
                           found[index].content_b);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_l2_cross_key_pairs_reply_encode(rows, count, response_body,
                                                          response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t max_pairs = 0u;
         if (aimee_db2_l2_fact_decision_pairs_request_decode(request_body, request_len,
                                                             &max_pairs) == 0)
         {
            if (response_capacity < AIMEE_DB2_L2_FACT_DECISION_PAIRS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->l2_fact_decision_pairs)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_l2_fact_decision_pairs_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_L2_FACT_DECISION_PAIRS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_memory_pair_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_L2_FACT_DECISION_PAIRS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->l2_fact_decision_pairs(
                   (int)max_pairs, found, AIMEE_DB2_L2_FACT_DECISION_PAIRS_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].memory_id_a =
                      found[index].id_a < 0 ? 0u : (uint64_t)found[index].id_a;
                  rows[index].memory_id_b =
                      found[index].id_b < 0 ? 0u : (uint64_t)found[index].id_b;
                  snprintf(rows[index].content_a, sizeof(rows[index].content_a), "%s",
                           found[index].content_a);
                  snprintf(rows[index].content_b, sizeof(rows[index].content_b), "%s",
                           found[index].content_b);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_l2_fact_decision_pairs_reply_encode(rows, count, response_body,
                                                              response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t link_source_id = 0u;
         uint64_t link_target_id = 0u;
         char link_relation[AIMEE_DB2_MEMORY_LINK_CREATE_LINK_RELATION_MAX + 1] = "";
         if (aimee_db2_memory_link_create_request_decode(request_body, request_len, &link_source_id,
                                                         &link_target_id, link_relation,
                                                         sizeof(link_relation)) == 0)
         {
            if (response_capacity < AIMEE_DB2_MEMORY_LINK_CREATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->memory_link_create)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->memory_link_create((int64_t)link_source_id,
                                                       (int64_t)link_target_id, link_relation) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_memory_link_create_reply_encode(acknowledged, response_body,
                                                          response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
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
      {
         /* Every db2-envelope-string-u32-v1 operation this family owns. */
         const db2_string_count_binding_t bindings[] = {
             {aimee_db2_entity_observation_count_request_decode,
              aimee_db2_entity_observation_count_reply_encode,
              backend ? backend->entity_observation_count : NULL,
              AIMEE_DB2_ENTITY_OBSERVATION_COUNT_MAX},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_count(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         /* Every db2-envelope-string-pair-u32-v1 operation this family owns. */
         const db2_string_pair_count_binding_t bindings[] = {
             {aimee_db2_entity_profile_fresh_request_decode,
              aimee_db2_entity_profile_fresh_reply_encode,
              backend ? backend->entity_profile_fresh : NULL, AIMEE_DB2_ENTITY_PROFILE_FRESH_MAX},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_pair_count(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         char project[AIMEE_DB2_PROJECT_FINGERPRINT_PROJECT_MAX + 1] = "";
         if (aimee_db2_project_fingerprint_request_decode(request_body, request_len, project,
                                                          sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECT_FINGERPRINT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->project_fingerprint)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char fingerprint[AIMEE_DB2_PROJECT_FINGERPRINT_FINGERPRINT_MAX + 1] = "";
            backend->project_fingerprint(project, fingerprint, sizeof(fingerprint));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_project_fingerprint_reply_encode(fingerprint, response_body,
                                                           response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_VISIBLE_SOURCE_HASH_PROJECT_MAX + 1] = "";
         if (aimee_db2_visible_source_hash_request_decode(request_body, request_len, project,
                                                          sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_VISIBLE_SOURCE_HASH_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->visible_source_hash)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char source_hash[AIMEE_DB2_VISIBLE_SOURCE_HASH_SOURCE_HASH_MAX + 1] = "";
            backend->visible_source_hash(project, source_hash, sizeof(source_hash));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_visible_source_hash_reply_encode(source_hash, response_body,
                                                           response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity_id[AIMEE_DB2_ENTITY_PROFILE_CARD_ENTITY_ID_MAX + 1] = "";
         if (aimee_db2_entity_profile_card_request_decode(request_body, request_len, entity_id,
                                                          sizeof(entity_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_PROFILE_CARD_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_profile_card)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char card_json[AIMEE_DB2_ENTITY_PROFILE_CARD_CARD_JSON_MAX + 1] = "";
            backend->entity_profile_card(entity_id, card_json, sizeof(card_json));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_entity_profile_card_reply_encode(card_json, response_body,
                                                           response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t generation_id = 0u;
         char error_message[AIMEE_DB2_GENERATION_ABORT_ERROR_MESSAGE_MAX + 1] = "";
         if (aimee_db2_generation_abort_request_decode(request_body, request_len, &generation_id,
                                                       error_message, sizeof(error_message)) == 0)
         {
            if (response_capacity < AIMEE_DB2_GENERATION_ABORT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->generation_abort)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->generation_abort((int64_t)generation_id, error_message) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_generation_abort_reply_encode(acknowledged, response_body,
                                                        response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t generation_id = 0u;
         char source_hash[AIMEE_DB2_GENERATION_SET_SOURCE_HASH_SOURCE_HASH_MAX + 1] = "";
         if (aimee_db2_generation_set_source_hash_request_decode(
                 request_body, request_len, &generation_id, source_hash, sizeof(source_hash)) == 0)
         {
            if (response_capacity < AIMEE_DB2_GENERATION_SET_SOURCE_HASH_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->generation_set_source_hash)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->generation_set_source_hash((int64_t)generation_id, source_hash) == 0 ? 1u
                                                                                              : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_generation_set_source_hash_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t generation_id = 0u;
         char project[AIMEE_DB2_GENERATION_PUBLISH_PROJECT_MAX + 1] = "";
         if (aimee_db2_generation_publish_request_decode(request_body, request_len, &generation_id,
                                                         project, sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_GENERATION_PUBLISH_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->generation_publish)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->generation_publish((int64_t)generation_id, project) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_generation_publish_reply_encode(acknowledged, response_body,
                                                          response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t project_id = 0u;
         char path_glob[AIMEE_DB2_PURGE_FILES_MATCHING_PATH_GLOB_MAX + 1] = "";
         if (aimee_db2_purge_files_matching_request_decode(request_body, request_len, &project_id,
                                                           path_glob, sizeof(path_glob)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PURGE_FILES_MATCHING_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->purge_files_matching)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t deleted = 0u;
            {
               int purged = backend->purge_files_matching((int64_t)project_id, path_glob);
               deleted = purged < 0 ? 0u : (uint32_t)purged;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_purge_files_matching_reply_encode(deleted, response_body,
                                                            response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_FILE_INDEX_DELETE_CURRENT_GENERATION_PROJECT_MAX + 1] = "";
         if (aimee_db2_file_index_delete_current_generation_request_decode(
                 request_body, request_len, project, sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_FILE_INDEX_DELETE_CURRENT_GENERATION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->file_index_delete_current_generation)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t deleted = 0u;
            {
               int removed = backend->file_index_delete_current_generation(project);
               deleted = removed < 0 ? 0u : (uint32_t)removed;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_file_index_delete_current_generation_reply_encode(
                    deleted, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PROJECT_DELETE_PROJECT_MAX + 1] = "";
         if (aimee_db2_project_delete_request_decode(request_body, request_len, project,
                                                     sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECT_DELETE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->project_delete)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t deleted = 0u;
            {
               int removed = backend->project_delete(project);
               deleted = removed < 0 ? 0u : (uint32_t)removed;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_project_delete_reply_encode(deleted, response_body, response_capacity,
                                                      response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_MINHASH_DELETE_CURRENT_GENERATION_PROJECT_MAX + 1] = "";
         if (aimee_db2_minhash_delete_current_generation_request_decode(
                 request_body, request_len, project, sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_MINHASH_DELETE_CURRENT_GENERATION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->minhash_delete_current_generation)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->minhash_delete_current_generation(project) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_minhash_delete_current_generation_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_MINHASH_DELETE_FILE_PROJECT_MAX + 1] = "";
         char file_path[AIMEE_DB2_MINHASH_DELETE_FILE_FILE_PATH_MAX + 1] = "";
         if (aimee_db2_minhash_delete_file_request_decode(request_body, request_len, project,
                                                          sizeof(project), file_path,
                                                          sizeof(file_path)) == 0)
         {
            if (response_capacity < AIMEE_DB2_MINHASH_DELETE_FILE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->minhash_delete_file)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->minhash_delete_file(project, file_path) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_minhash_delete_file_reply_encode(acknowledged, response_body,
                                                           response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PROJECT_CURRENT_GENERATION_PROJECT_MAX + 1] = "";
         if (aimee_db2_project_current_generation_request_decode(request_body, request_len, project,
                                                                 sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECT_CURRENT_GENERATION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->project_current_generation)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t generation = 0u;
            {
               int64_t current = 0;
               if (backend->project_current_generation(project, &current) == 0 && current > 0)
                  generation = (uint64_t)current;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_project_current_generation_reply_encode(
                    generation, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PROJECTION_GENERATION_CREATE_PROJECT_MAX + 1] = "";
         if (aimee_db2_projection_generation_create_request_decode(request_body, request_len,
                                                                   project, sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECTION_GENERATION_CREATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->projection_generation_create)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t generation = 0u;
            {
               int64_t created = backend->projection_generation_create(project);
               generation = created < 0 ? 0u : (uint64_t)created;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_projection_generation_create_reply_encode(
                    generation, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PROJECTION_VISIBLE_ID_PROJECT_MAX + 1] = "";
         if (aimee_db2_projection_visible_id_request_decode(request_body, request_len, project,
                                                            sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECTION_VISIBLE_ID_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->projection_visible_id)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t generation = 0u;
            {
               int64_t visible = backend->projection_visible_id(project);
               generation = visible < 0 ? 0u : (uint64_t)visible;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_projection_visible_id_reply_encode(generation, response_body,
                                                             response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_UNIQUE_FILE_BASENAME_PROJECT_MAX + 1] = "";
         char basename[AIMEE_DB2_UNIQUE_FILE_BASENAME_BASENAME_MAX + 1] = "";
         if (aimee_db2_unique_file_basename_request_decode(request_body, request_len, project,
                                                           sizeof(project), basename,
                                                           sizeof(basename)) == 0)
         {
            if (response_capacity < AIMEE_DB2_UNIQUE_FILE_BASENAME_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->unique_file_basename)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char file_path[AIMEE_DB2_UNIQUE_FILE_BASENAME_FILE_PATH_MAX + 1] = "";
            (void)backend->unique_file_basename(project, basename, file_path, sizeof(file_path));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_unique_file_basename_reply_encode(file_path, response_body,
                                                            response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_ENTITY_NEIGHBORS_ENTITY_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_entity_neighbors_request_decode(request_body, request_len, entity,
                                                       sizeof(entity), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_NEIGHBORS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_neighbors)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_neighbors_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_NEIGHBORS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_entity_neighbor_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ENTITY_NEIGHBORS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->entity_neighbors(
                   entity, found, AIMEE_DB2_ENTITY_NEIGHBORS_MAX_ROWS, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].node, sizeof(rows[index].node), "%s", found[index].node);
                  rows[index].weight = found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_neighbors_reply_encode(rows, count, response_body,
                                                        response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_ENTITY_NEIGHBORS_FILTERED_ENTITY_MAX + 1] = "";
         char relation_a[AIMEE_DB2_ENTITY_NEIGHBORS_FILTERED_RELATION_A_MAX + 1] = "";
         char relation_b[AIMEE_DB2_ENTITY_NEIGHBORS_FILTERED_RELATION_B_MAX + 1] = "";
         uint32_t order_by_weight = 0u;
         uint32_t limit = 0u;
         if (aimee_db2_entity_neighbors_filtered_request_decode(
                 request_body, request_len, entity, sizeof(entity), relation_a, sizeof(relation_a),
                 relation_b, sizeof(relation_b), &order_by_weight, &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_NEIGHBORS_FILTERED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_neighbors_filtered)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_neighbors_filtered_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_NEIGHBORS_FILTERED_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_entity_neighbor_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ENTITY_NEIGHBORS_FILTERED_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->entity_neighbors_filtered(
                   entity, relation_a, relation_b, (int)order_by_weight, found,
                   AIMEE_DB2_ENTITY_NEIGHBORS_FILTERED_MAX_ROWS, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].node, sizeof(rows[index].node), "%s", found[index].node);
                  rows[index].weight = found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_neighbors_filtered_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_ENTITY_OUTBOUND_NEIGHBORS_ENTITY_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_entity_outbound_neighbors_request_decode(request_body, request_len, entity,
                                                                sizeof(entity), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_OUTBOUND_NEIGHBORS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_outbound_neighbors)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_outbound_neighbors_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_OUTBOUND_NEIGHBORS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_entity_neighbor_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ENTITY_OUTBOUND_NEIGHBORS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->entity_outbound_neighbors(
                   entity, found, AIMEE_DB2_ENTITY_OUTBOUND_NEIGHBORS_MAX_ROWS, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].node, sizeof(rows[index].node), "%s", found[index].node);
                  rows[index].weight = found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_outbound_neighbors_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_ENTITY_TOP_PARTNERS_ENTITY_MAX + 1] = "";
         char relation[AIMEE_DB2_ENTITY_TOP_PARTNERS_RELATION_MAX + 1] = "";
         if (aimee_db2_entity_top_partners_request_decode(request_body, request_len, entity,
                                                          sizeof(entity), relation,
                                                          sizeof(relation)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_TOP_PARTNERS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_top_partners)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_top_partners_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_TOP_PARTNERS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_entity_neighbor_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ENTITY_TOP_PARTNERS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->entity_top_partners(entity, relation, found,
                                                          AIMEE_DB2_ENTITY_TOP_PARTNERS_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].node, sizeof(rows[index].node), "%s", found[index].node);
                  rows[index].weight = found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_top_partners_reply_encode(rows, count, response_body,
                                                           response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_ENTITY_TOP_TARGETS_ENTITY_MAX + 1] = "";
         char relation[AIMEE_DB2_ENTITY_TOP_TARGETS_RELATION_MAX + 1] = "";
         if (aimee_db2_entity_top_targets_request_decode(request_body, request_len, entity,
                                                         sizeof(entity), relation,
                                                         sizeof(relation)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_TOP_TARGETS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_top_targets)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_top_targets_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_TOP_TARGETS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_entity_neighbor_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ENTITY_TOP_TARGETS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->entity_top_targets(entity, relation, found,
                                                         AIMEE_DB2_ENTITY_TOP_TARGETS_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].node, sizeof(rows[index].node), "%s", found[index].node);
                  rows[index].weight = found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_top_targets_reply_encode(rows, count, response_body,
                                                          response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_FILE_DEFINITIONS_PROJECT_MAX + 1] = "";
         char file_path[AIMEE_DB2_FILE_DEFINITIONS_FILE_PATH_MAX + 1] = "";
         if (aimee_db2_file_definitions_request_decode(request_body, request_len, project,
                                                       sizeof(project), file_path,
                                                       sizeof(file_path)) == 0)
         {
            if (response_capacity < AIMEE_DB2_FILE_DEFINITIONS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->file_definitions)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_file_definitions_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_FILE_DEFINITIONS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               definition_t *found = malloc(sizeof(*found) * AIMEE_DB2_FILE_DEFINITIONS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->file_definitions(project, file_path, found,
                                                       AIMEE_DB2_FILE_DEFINITIONS_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].symbol_name, sizeof(rows[index].symbol_name), "%s",
                           found[index].name);
                  snprintf(rows[index].symbol_kind, sizeof(rows[index].symbol_kind), "%s",
                           found[index].kind);
                  rows[index].line = found[index].line < 0 ? 0u : (uint32_t)found[index].line;
                  rows[index].line_end =
                      found[index].line_end < 0 ? 0u : (uint32_t)found[index].line_end;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_file_definitions_reply_encode(rows, count, response_body,
                                                        response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char query[AIMEE_DB2_CODE_SEARCH_QUERY_MAX + 1] = "";
         char project[AIMEE_DB2_CODE_SEARCH_PROJECT_MAX + 1] = "";
         uint32_t enrich = 0u;
         if (aimee_db2_code_search_request_decode(request_body, request_len, query, sizeof(query),
                                                  project, sizeof(project), &enrich) == 0)
         {
            if (response_capacity < AIMEE_DB2_CODE_SEARCH_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->code_search)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_code_search_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_CODE_SEARCH_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               code_search_hit_t *found = malloc(sizeof(*found) * AIMEE_DB2_CODE_SEARCH_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->code_search(query, project, found,
                                                  AIMEE_DB2_CODE_SEARCH_MAX_ROWS, (int)enrich);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].project, sizeof(rows[index].project), "%s",
                           found[index].project);
                  snprintf(rows[index].file_path, sizeof(rows[index].file_path), "%s",
                           found[index].file_path);
                  snprintf(rows[index].snippet, sizeof(rows[index].snippet), "%s",
                           found[index].snippet);
                  rows[index].rank = found[index].rank < 0.0 ? 0.0 : found[index].rank;
                  snprintf(rows[index].content_hash, sizeof(rows[index].content_hash), "%s",
                           found[index].content_hash);
                  rows[index].line = found[index].line < 0 ? 0u : (uint32_t)found[index].line;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_code_search_reply_encode(rows, count, response_body, response_capacity,
                                                   response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char query[AIMEE_DB2_CODE_SEARCH_EXCLUDING_PROJECT_QUERY_MAX + 1] = "";
         char excluded_project[AIMEE_DB2_CODE_SEARCH_EXCLUDING_PROJECT_EXCLUDED_PROJECT_MAX + 1] =
             "";
         uint32_t enrich = 0u;
         if (aimee_db2_code_search_excluding_project_request_decode(
                 request_body, request_len, query, sizeof(query), excluded_project,
                 sizeof(excluded_project), &enrich) == 0)
         {
            if (response_capacity < AIMEE_DB2_CODE_SEARCH_EXCLUDING_PROJECT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->code_search_excluding_project)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_code_search_excluding_project_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_CODE_SEARCH_EXCLUDING_PROJECT_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               code_search_hit_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_CODE_SEARCH_EXCLUDING_PROJECT_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->code_search_excluding_project(
                   query, excluded_project, found, AIMEE_DB2_CODE_SEARCH_EXCLUDING_PROJECT_MAX_ROWS,
                   (int)enrich);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].project, sizeof(rows[index].project), "%s",
                           found[index].project);
                  snprintf(rows[index].file_path, sizeof(rows[index].file_path), "%s",
                           found[index].file_path);
                  snprintf(rows[index].snippet, sizeof(rows[index].snippet), "%s",
                           found[index].snippet);
                  rows[index].rank = found[index].rank < 0.0 ? 0.0 : found[index].rank;
                  snprintf(rows[index].content_hash, sizeof(rows[index].content_hash), "%s",
                           found[index].content_hash);
                  rows[index].line = found[index].line < 0 ? 0u : (uint32_t)found[index].line;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_code_search_excluding_project_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {

         if (aimee_db2_project_last_scan_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECT_LAST_SCAN_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->project_last_scan)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char last_scan[AIMEE_DB2_PROJECT_LAST_SCAN_LAST_SCAN_MAX + 1] = "";
            (void)backend->project_last_scan(last_scan, sizeof(last_scan));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_project_last_scan_reply_encode(last_scan, response_body,
                                                         response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char node[AIMEE_DB2_ENTITY_WALK_STEP_TYPED_NODE_MAX + 1] = "";
         if (aimee_db2_entity_walk_step_typed_request_decode(request_body, request_len, node,
                                                             sizeof(node)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_WALK_STEP_TYPED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_walk_step_typed)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_walk_step_typed_row_t rows[AIMEE_DB2_ENTITY_WALK_STEP_TYPED_MAX_ROWS];
            uint32_t count = 0u;
            {
               db2_entity_edge_typed_t found[AIMEE_DB2_ENTITY_WALK_STEP_TYPED_MAX_ROWS];
               int written = backend->entity_walk_step_typed(
                   node, found, AIMEE_DB2_ENTITY_WALK_STEP_TYPED_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].source, sizeof(rows[index].source), "%s",
                           found[index].source);
                  snprintf(rows[index].relation, sizeof(rows[index].relation), "%s",
                           found[index].relation);
                  snprintf(rows[index].target, sizeof(rows[index].target), "%s",
                           found[index].target);
                  rows[index].relation_id =
                      found[index].relation_id < 0 ? 0u : (uint32_t)found[index].relation_id;
                  rows[index].subject_kind =
                      found[index].subject_kind < 0 ? 0u : (uint32_t)found[index].subject_kind;
                  rows[index].object_kind =
                      found[index].object_kind < 0 ? 0u : (uint32_t)found[index].object_kind;
                  rows[index].weight = found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
               }
               count = written < 0 ? 0u : (uint32_t)written;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_entity_walk_step_typed_reply_encode(rows, count, response_body,
                                                              response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PROJECTION_GENERATIONS_LIST_PROJECT_MAX + 1] = "";
         if (aimee_db2_projection_generations_list_request_decode(request_body, request_len,
                                                                  project, sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECTION_GENERATIONS_LIST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->projection_generations_list)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_projection_generations_list_row_t
                rows[AIMEE_DB2_PROJECTION_GENERATIONS_LIST_MAX_ROWS];
            uint32_t count = 0u;
            {
               code_projection_generation_row_t
                   found[AIMEE_DB2_PROJECTION_GENERATIONS_LIST_MAX_ROWS];
               int written = backend->projection_generations_list(
                   project, found, AIMEE_DB2_PROJECTION_GENERATIONS_LIST_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].generation = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].started_at, sizeof(rows[index].started_at), "%s",
                           found[index].started_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_projection_generations_list_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_ENTITY_EDGE_BUMP_UTILITY_ENTITY_MAX + 1] = "";
         double utility_delta = 0.0;
         if (aimee_db2_entity_edge_bump_utility_request_decode(request_body, request_len, entity,
                                                               sizeof(entity), &utility_delta) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_EDGE_BUMP_UTILITY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_edge_bump_utility)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->entity_edge_bump_utility(entity, utility_delta) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_entity_edge_bump_utility_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_ENTITY_NEIGHBORS_WEIGHTED_ENTITY_MAX + 1] = "";
         uint32_t limit = 0u;
         uint32_t utility_scoring_enabled = 0u;
         if (aimee_db2_entity_neighbors_weighted_request_decode(request_body, request_len, entity,
                                                                sizeof(entity), &limit,
                                                                &utility_scoring_enabled) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_NEIGHBORS_WEIGHTED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_neighbors_weighted)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_neighbors_weighted_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_NEIGHBORS_WEIGHTED_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_entity_edge_weighted_neighbor_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ENTITY_NEIGHBORS_WEIGHTED_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->entity_neighbors_weighted(
                   entity, found, AIMEE_DB2_ENTITY_NEIGHBORS_WEIGHTED_MAX_ROWS, (int)limit,
                   (int)utility_scoring_enabled);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].node, sizeof(rows[index].node), "%s", found[index].node);
                  rows[index].weight = found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
                  rows[index].utility_score = found[index].utility_score;
                  rows[index].effective_utility = found[index].effective_utility;
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_neighbors_weighted_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity[AIMEE_DB2_ENTITY_EDGES_FOR_ENTITY_ENTITY_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_entity_edges_for_entity_request_decode(request_body, request_len, entity,
                                                              sizeof(entity), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_EDGES_FOR_ENTITY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_edges_for_entity)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_edges_for_entity_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_EDGES_FOR_ENTITY_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               edge_t *found = malloc(sizeof(*found) * AIMEE_DB2_ENTITY_EDGES_FOR_ENTITY_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->entity_edges_for_entity(entity, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].edge_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].edge_weight =
                      found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
                  snprintf(rows[index].edge_source, sizeof(rows[index].edge_source), "%s",
                           found[index].source);
                  snprintf(rows[index].edge_relation, sizeof(rows[index].edge_relation), "%s",
                           found[index].relation);
                  snprintf(rows[index].edge_target, sizeof(rows[index].edge_target), "%s",
                           found[index].target);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_edges_for_entity_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char token[AIMEE_DB2_ENTITY_EDGES_BY_TOKEN_TOKEN_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_entity_edges_by_token_request_decode(request_body, request_len, token,
                                                            sizeof(token), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_EDGES_BY_TOKEN_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_edges_by_token)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_edges_by_token_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_EDGES_BY_TOKEN_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               edge_t *found = malloc(sizeof(*found) * AIMEE_DB2_ENTITY_EDGES_BY_TOKEN_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->entity_edges_by_token(
                   token, found, AIMEE_DB2_ENTITY_EDGES_BY_TOKEN_MAX_ROWS, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].edge_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].edge_weight =
                      found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
                  snprintf(rows[index].edge_source, sizeof(rows[index].edge_source), "%s",
                           found[index].source);
                  snprintf(rows[index].edge_relation, sizeof(rows[index].edge_relation), "%s",
                           found[index].relation);
                  snprintf(rows[index].edge_target, sizeof(rows[index].edge_target), "%s",
                           found[index].target);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_edges_by_token_reply_encode(rows, count, response_body,
                                                             response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         if (aimee_db2_entity_top_triples_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_TOP_TRIPLES_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_top_triples)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_entity_top_triples_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ENTITY_TOP_TRIPLES_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               edge_t *found = malloc(sizeof(*found) * AIMEE_DB2_ENTITY_TOP_TRIPLES_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written =
                   backend->entity_top_triples(found, AIMEE_DB2_ENTITY_TOP_TRIPLES_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].edge_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].edge_weight =
                      found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
                  snprintf(rows[index].edge_source, sizeof(rows[index].edge_source), "%s",
                           found[index].source);
                  snprintf(rows[index].edge_relation, sizeof(rows[index].edge_relation), "%s",
                           found[index].relation);
                  snprintf(rows[index].edge_target, sizeof(rows[index].edge_target), "%s",
                           found[index].target);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_top_triples_reply_encode(rows, count, response_body,
                                                          response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PROJECTION_EDGES_PROJECT_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_projection_edges_request_decode(request_body, request_len, project,
                                                       sizeof(project), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECTION_EDGES_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->projection_edges)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_projection_edges_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_PROJECTION_EDGES_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               code_projection_edge_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_PROJECTION_EDGES_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->projection_edges(project, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].structural_weight = found[index].structural_weight < 0
                                                      ? 0u
                                                      : (uint32_t)found[index].structural_weight;
                  snprintf(rows[index].projection_source, sizeof(rows[index].projection_source),
                           "%s", found[index].source);
                  snprintf(rows[index].projection_relation, sizeof(rows[index].projection_relation),
                           "%s", found[index].relation);
                  snprintf(rows[index].projection_target, sizeof(rows[index].projection_target),
                           "%s", found[index].target);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_projection_edges_reply_encode(rows, count, response_body,
                                                        response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t projection_generation = 0u;
         uint32_t limit = 0u;
         if (aimee_db2_projection_edges_for_generation_request_decode(
                 request_body, request_len, &projection_generation, &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECTION_EDGES_FOR_GENERATION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->projection_edges_for_generation)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_projection_edges_for_generation_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_PROJECTION_EDGES_FOR_GENERATION_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               code_projection_edge_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_PROJECTION_EDGES_FOR_GENERATION_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->projection_edges_for_generation(
                   (int64_t)projection_generation, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].structural_weight = found[index].structural_weight < 0
                                                      ? 0u
                                                      : (uint32_t)found[index].structural_weight;
                  snprintf(rows[index].projection_source, sizeof(rows[index].projection_source),
                           "%s", found[index].source);
                  snprintf(rows[index].projection_relation, sizeof(rows[index].projection_relation),
                           "%s", found[index].relation);
                  snprintf(rows[index].projection_target, sizeof(rows[index].projection_target),
                           "%s", found[index].target);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_projection_edges_for_generation_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char identifier[AIMEE_DB2_TERM_FIND_IDENTIFIER_MAX + 1] = "";
         if (aimee_db2_term_find_request_decode(request_body, request_len, identifier,
                                                sizeof(identifier)) == 0)
         {
            if (response_capacity < AIMEE_DB2_TERM_FIND_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->term_find)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_term_find_row_t *rows = malloc(sizeof(*rows) * AIMEE_DB2_TERM_FIND_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               term_hit_t *found = malloc(sizeof(*found) * AIMEE_DB2_TERM_FIND_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->term_find(identifier, found, AIMEE_DB2_TERM_FIND_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].line = found[index].line < 0 ? 0u : (uint32_t)found[index].line;
                  rows[index].line_end =
                      found[index].line_end < 0 ? 0u : (uint32_t)found[index].line_end;
                  snprintf(rows[index].hit_project, sizeof(rows[index].hit_project), "%s",
                           found[index].project);
                  snprintf(rows[index].hit_file_path, sizeof(rows[index].hit_file_path), "%s",
                           found[index].file_path);
                  snprintf(rows[index].term_kind, sizeof(rows[index].term_kind), "%s",
                           found[index].kind);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_term_find_reply_encode(rows, count, response_body, response_capacity,
                                                 response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_TERM_FIND_IN_PROJECT_PROJECT_MAX + 1] = "";
         char identifier[AIMEE_DB2_TERM_FIND_IN_PROJECT_IDENTIFIER_MAX + 1] = "";
         if (aimee_db2_term_find_in_project_request_decode(request_body, request_len, project,
                                                           sizeof(project), identifier,
                                                           sizeof(identifier)) == 0)
         {
            if (response_capacity < AIMEE_DB2_TERM_FIND_IN_PROJECT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->term_find_in_project)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_term_find_in_project_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_TERM_FIND_IN_PROJECT_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               term_hit_t *found = malloc(sizeof(*found) * AIMEE_DB2_TERM_FIND_IN_PROJECT_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->term_find_in_project(project, identifier, found,
                                                           AIMEE_DB2_TERM_FIND_IN_PROJECT_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].line = found[index].line < 0 ? 0u : (uint32_t)found[index].line;
                  rows[index].line_end =
                      found[index].line_end < 0 ? 0u : (uint32_t)found[index].line_end;
                  snprintf(rows[index].hit_project, sizeof(rows[index].hit_project), "%s",
                           found[index].project);
                  snprintf(rows[index].hit_file_path, sizeof(rows[index].hit_file_path), "%s",
                           found[index].file_path);
                  snprintf(rows[index].term_kind, sizeof(rows[index].term_kind), "%s",
                           found[index].kind);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_term_find_in_project_reply_encode(rows, count, response_body,
                                                            response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char excluded_project[AIMEE_DB2_TERM_FIND_EXCLUDING_PROJECT_EXCLUDED_PROJECT_MAX + 1] = "";
         char identifier[AIMEE_DB2_TERM_FIND_EXCLUDING_PROJECT_IDENTIFIER_MAX + 1] = "";
         if (aimee_db2_term_find_excluding_project_request_decode(
                 request_body, request_len, excluded_project, sizeof(excluded_project), identifier,
                 sizeof(identifier)) == 0)
         {
            if (response_capacity < AIMEE_DB2_TERM_FIND_EXCLUDING_PROJECT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->term_find_excluding_project)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_term_find_excluding_project_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_TERM_FIND_EXCLUDING_PROJECT_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               term_hit_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_TERM_FIND_EXCLUDING_PROJECT_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->term_find_excluding_project(
                   excluded_project, identifier, found,
                   AIMEE_DB2_TERM_FIND_EXCLUDING_PROJECT_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].line = found[index].line < 0 ? 0u : (uint32_t)found[index].line;
                  rows[index].line_end =
                      found[index].line_end < 0 ? 0u : (uint32_t)found[index].line_end;
                  snprintf(rows[index].hit_project, sizeof(rows[index].hit_project), "%s",
                           found[index].project);
                  snprintf(rows[index].hit_file_path, sizeof(rows[index].hit_file_path), "%s",
                           found[index].file_path);
                  snprintf(rows[index].term_kind, sizeof(rows[index].term_kind), "%s",
                           found[index].kind);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_term_find_excluding_project_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_CALLERS_FIND_PROJECT_MAX + 1] = "";
         char callee[AIMEE_DB2_CALLERS_FIND_CALLEE_MAX + 1] = "";
         if (aimee_db2_callers_find_request_decode(request_body, request_len, project,
                                                   sizeof(project), callee, sizeof(callee)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CALLERS_FIND_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->callers_find)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_callers_find_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_CALLERS_FIND_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               caller_hit_t *found = malloc(sizeof(*found) * AIMEE_DB2_CALLERS_FIND_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written =
                   backend->callers_find(project, callee, found, AIMEE_DB2_CALLERS_FIND_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].caller_line =
                      found[index].line < 0 ? 0u : (uint32_t)found[index].line;
                  snprintf(rows[index].caller_project, sizeof(rows[index].caller_project), "%s",
                           found[index].project);
                  snprintf(rows[index].caller_file_path, sizeof(rows[index].caller_file_path), "%s",
                           found[index].file_path);
                  snprintf(rows[index].caller_symbol, sizeof(rows[index].caller_symbol), "%s",
                           found[index].caller);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_callers_find_reply_encode(rows, count, response_body, response_capacity,
                                                    response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_CALLERS_FIND_SCOPED_PROJECT_MAX + 1] = "";
         char callee[AIMEE_DB2_CALLERS_FIND_SCOPED_CALLEE_MAX + 1] = "";
         if (aimee_db2_callers_find_scoped_request_decode(
                 request_body, request_len, project, sizeof(project), callee, sizeof(callee)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CALLERS_FIND_SCOPED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->callers_find_scoped)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_callers_find_scoped_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_CALLERS_FIND_SCOPED_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               caller_hit_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_CALLERS_FIND_SCOPED_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->callers_find_scoped(project, callee, found,
                                                          AIMEE_DB2_CALLERS_FIND_SCOPED_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].caller_line =
                      found[index].line < 0 ? 0u : (uint32_t)found[index].line;
                  snprintf(rows[index].caller_project, sizeof(rows[index].caller_project), "%s",
                           found[index].project);
                  snprintf(rows[index].caller_file_path, sizeof(rows[index].caller_file_path), "%s",
                           found[index].file_path);
                  snprintf(rows[index].caller_symbol, sizeof(rows[index].caller_symbol), "%s",
                           found[index].caller);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_callers_find_scoped_reply_encode(rows, count, response_body,
                                                           response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char excluded_project[AIMEE_DB2_CALLERS_FIND_EXCLUDING_PROJECT_EXCLUDED_PROJECT_MAX + 1] =
             "";
         char callee[AIMEE_DB2_CALLERS_FIND_EXCLUDING_PROJECT_CALLEE_MAX + 1] = "";
         if (aimee_db2_callers_find_excluding_project_request_decode(
                 request_body, request_len, excluded_project, sizeof(excluded_project), callee,
                 sizeof(callee)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CALLERS_FIND_EXCLUDING_PROJECT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->callers_find_excluding_project)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_callers_find_excluding_project_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_CALLERS_FIND_EXCLUDING_PROJECT_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               caller_hit_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_CALLERS_FIND_EXCLUDING_PROJECT_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->callers_find_excluding_project(
                   excluded_project, callee, found,
                   AIMEE_DB2_CALLERS_FIND_EXCLUDING_PROJECT_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].caller_line =
                      found[index].line < 0 ? 0u : (uint32_t)found[index].line;
                  snprintf(rows[index].caller_project, sizeof(rows[index].caller_project), "%s",
                           found[index].project);
                  snprintf(rows[index].caller_file_path, sizeof(rows[index].caller_file_path), "%s",
                           found[index].file_path);
                  snprintf(rows[index].caller_symbol, sizeof(rows[index].caller_symbol), "%s",
                           found[index].caller);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_callers_find_excluding_project_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char node_key[AIMEE_DB2_ENTITY_NODE_GET_NODE_KEY_MAX + 1] = "";
         if (aimee_db2_entity_node_get_request_decode(request_body, request_len, node_key,
                                                      sizeof(node_key)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_NODE_GET_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_node_get)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t found = 0u;
            uint32_t node_kind = 0u;
            uint64_t last_seen_generation = 0u;
            char node_project[AIMEE_DB2_ENTITY_NODE_GET_NODE_PROJECT_MAX + 1] = "";
            char display_name[AIMEE_DB2_ENTITY_NODE_GET_DISPLAY_NAME_MAX + 1] = "";
            char full_key[AIMEE_DB2_ENTITY_NODE_GET_FULL_KEY_MAX + 1] = "";
            char node_file_path[AIMEE_DB2_ENTITY_NODE_GET_NODE_FILE_PATH_MAX + 1] = "";
            char node_symbol[AIMEE_DB2_ENTITY_NODE_GET_NODE_SYMBOL_MAX + 1] = "";
            char node_origin[AIMEE_DB2_ENTITY_NODE_GET_NODE_ORIGIN_MAX + 1] = "";
            {
               db2_entity_node_t node;
               memset(&node, 0, sizeof(node));
               if (backend->entity_node_get(node_key, &node) == 0)
               {
                  found = 1u;
                  node_kind = node.node_kind < 0 ? 0u : (uint32_t)node.node_kind;
                  last_seen_generation = node.last_seen_generation_id < 0
                                             ? 0u
                                             : (uint64_t)node.last_seen_generation_id;
                  snprintf(node_project, sizeof(node_project), "%s", node.project);
                  snprintf(display_name, sizeof(display_name), "%s", node.display_name);
                  snprintf(full_key, sizeof(full_key), "%s", node.full_key);
                  snprintf(node_file_path, sizeof(node_file_path), "%s", node.file_path);
                  snprintf(node_symbol, sizeof(node_symbol), "%s", node.symbol);
                  snprintf(node_origin, sizeof(node_origin), "%s", node.node_origin);
               }
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_node_get_reply_encode(
                    found, node_kind, last_seen_generation, node_project, display_name, full_key,
                    node_file_path, node_symbol, node_origin, response_body, response_capacity,
                    response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char alias[AIMEE_DB2_ENTITY_NODE_ALIAS_UPSERT_ALIAS_MAX + 1] = "";
         char node_key[AIMEE_DB2_ENTITY_NODE_ALIAS_UPSERT_NODE_KEY_MAX + 1] = "";
         char alias_kind[AIMEE_DB2_ENTITY_NODE_ALIAS_UPSERT_ALIAS_KIND_MAX + 1] = "";
         char alias_project[AIMEE_DB2_ENTITY_NODE_ALIAS_UPSERT_ALIAS_PROJECT_MAX + 1] = "";
         uint64_t alias_generation = 0u;
         if (aimee_db2_entity_node_alias_upsert_request_decode(
                 request_body, request_len, alias, sizeof(alias), node_key, sizeof(node_key),
                 alias_kind, sizeof(alias_kind), alias_project, sizeof(alias_project),
                 &alias_generation) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_NODE_ALIAS_UPSERT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_node_alias_upsert)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->entity_node_alias_upsert(alias, node_key, alias_kind, alias_project,
                                                  (int64_t)alias_generation) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_node_alias_upsert_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char edge_source[AIMEE_DB2_ENTITY_EDGE_UPSERT_EDGE_SOURCE_MAX + 1] = "";
         char edge_relation[AIMEE_DB2_ENTITY_EDGE_UPSERT_EDGE_RELATION_MAX + 1] = "";
         char edge_target[AIMEE_DB2_ENTITY_EDGE_UPSERT_EDGE_TARGET_MAX + 1] = "";
         uint64_t window_id = 0u;
         uint32_t relation_id = 0u;
         uint32_t subject_kind = 0u;
         uint32_t object_kind = 0u;
         if (aimee_db2_entity_edge_upsert_request_decode(
                 request_body, request_len, edge_source, sizeof(edge_source), edge_relation,
                 sizeof(edge_relation), edge_target, sizeof(edge_target), &window_id, &relation_id,
                 &subject_kind, &object_kind) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_EDGE_UPSERT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_edge_upsert)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            uint32_t edge_added = 0u;
            {
               int added = 0;
               acknowledged =
                   backend->entity_edge_upsert(edge_source, edge_relation, edge_target,
                                               (int64_t)window_id, (int)relation_id,
                                               (int)subject_kind, (int)object_kind, &added) == 0
                       ? 1u
                       : 0u;
               edge_added = added > 0 ? 1u : 0u;
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_edge_upsert_reply_encode(acknowledged, edge_added, response_body,
                                                          response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_CODE_FILE_HASH_PROJECT_MAX + 1] = "";
         char file_path[AIMEE_DB2_CODE_FILE_HASH_FILE_PATH_MAX + 1] = "";
         if (aimee_db2_code_file_hash_request_decode(request_body, request_len, project,
                                                     sizeof(project), file_path,
                                                     sizeof(file_path)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CODE_FILE_HASH_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->code_file_hash)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char file_hash[AIMEE_DB2_CODE_FILE_HASH_FILE_HASH_MAX + 1] = "";
            (void)backend->code_file_hash(project, file_path, file_hash, (int)sizeof(file_hash));
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_code_file_hash_reply_encode(file_hash, response_body, response_capacity,
                                                      response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t project_id = 0u;
         char file_path[AIMEE_DB2_FILE_MODIFIED_SINCE_FILE_PATH_MAX + 1] = "";
         uint64_t modified_since = 0u;
         if (aimee_db2_file_modified_since_request_decode(request_body, request_len, &project_id,
                                                          file_path, sizeof(file_path),
                                                          &modified_since) == 0)
         {
            if (response_capacity < AIMEE_DB2_FILE_MODIFIED_SINCE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->file_modified_since)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t modified = 0u;
            modified = backend->file_modified_since((int64_t)project_id, file_path,
                                                    (time_t)modified_since) != 0
                           ? 1u
                           : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_file_modified_since_reply_encode(modified, response_body,
                                                           response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t project_id = 0u;
         char file_path[AIMEE_DB2_CODE_FILE_UPSERT_FILE_PATH_MAX + 1] = "";
         char scanned_at[AIMEE_DB2_CODE_FILE_UPSERT_SCANNED_AT_MAX + 1] = "";
         if (aimee_db2_code_file_upsert_request_decode(request_body, request_len, &project_id,
                                                       file_path, sizeof(file_path), scanned_at,
                                                       sizeof(scanned_at)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CODE_FILE_UPSERT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->code_file_upsert)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t file_id = 0u;
            {
               int64_t written =
                   backend->code_file_upsert((int64_t)project_id, file_path, scanned_at);
               file_id = written < 0 ? 0u : (uint64_t)written;
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_code_file_upsert_reply_encode(file_id, response_body, response_capacity,
                                                        response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t point_id = 0u;
         char project[AIMEE_DB2_CODE_INDEX_OP_RECORD_PROJECT_MAX + 1] = "";
         char node_key[AIMEE_DB2_CODE_INDEX_OP_RECORD_NODE_KEY_MAX + 1] = "";
         char file_path[AIMEE_DB2_CODE_INDEX_OP_RECORD_FILE_PATH_MAX + 1] = "";
         uint32_t index_ok = 0u;
         char error_message[AIMEE_DB2_CODE_INDEX_OP_RECORD_ERROR_MESSAGE_MAX + 1] = "";
         if (aimee_db2_code_index_op_record_request_decode(
                 request_body, request_len, &point_id, project, sizeof(project), node_key,
                 sizeof(node_key), file_path, sizeof(file_path), &index_ok, error_message,
                 sizeof(error_message)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CODE_INDEX_OP_RECORD_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->code_index_op_record)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t recorded = 0u;
            backend->code_index_op_record((int64_t)point_id, project, node_key, file_path,
                                          (int)index_ok, error_message);
            recorded = 1u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_code_index_op_record_reply_encode(recorded, response_body,
                                                            response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_CODE_PROJECT_UPSERT_PROJECT_MAX + 1] = "";
         char project_root[AIMEE_DB2_CODE_PROJECT_UPSERT_PROJECT_ROOT_MAX + 1] = "";
         if (aimee_db2_code_project_upsert_request_decode(request_body, request_len, project,
                                                          sizeof(project), project_root,
                                                          sizeof(project_root)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CODE_PROJECT_UPSERT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->code_project_upsert)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t project_id = 0u;
            {
               int64_t written = backend->code_project_upsert(project, project_root);
               project_id = written < 0 ? 0u : (uint64_t)written;
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_code_project_upsert_reply_encode(project_id, response_body,
                                                           response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char node_key[AIMEE_DB2_ENTITY_NODE_UPSERT_NODE_KEY_MAX + 1] = "";
         uint32_t node_kind = 0u;
         char node_project[AIMEE_DB2_ENTITY_NODE_UPSERT_NODE_PROJECT_MAX + 1] = "";
         char display_name[AIMEE_DB2_ENTITY_NODE_UPSERT_DISPLAY_NAME_MAX + 1] = "";
         char full_key[AIMEE_DB2_ENTITY_NODE_UPSERT_FULL_KEY_MAX + 1] = "";
         char node_file_path[AIMEE_DB2_ENTITY_NODE_UPSERT_NODE_FILE_PATH_MAX + 1] = "";
         char node_symbol[AIMEE_DB2_ENTITY_NODE_UPSERT_NODE_SYMBOL_MAX + 1] = "";
         char node_origin[AIMEE_DB2_ENTITY_NODE_UPSERT_NODE_ORIGIN_MAX + 1] = "";
         uint64_t node_generation = 0u;
         if (aimee_db2_entity_node_upsert_request_decode(
                 request_body, request_len, node_key, sizeof(node_key), &node_kind, node_project,
                 sizeof(node_project), display_name, sizeof(display_name), full_key,
                 sizeof(full_key), node_file_path, sizeof(node_file_path), node_symbol,
                 sizeof(node_symbol), node_origin, sizeof(node_origin), &node_generation) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_NODE_UPSERT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_node_upsert)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->entity_node_upsert(node_key, (int)node_kind, node_project, display_name,
                                            full_key, node_file_path, node_symbol, node_origin,
                                            (int64_t)node_generation) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_node_upsert_reply_encode(acknowledged, response_body,
                                                          response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity_id[AIMEE_DB2_ENTITY_PROFILE_UPSERT_ENTITY_ID_MAX + 1] = "";
         char canonical_name[AIMEE_DB2_ENTITY_PROFILE_UPSERT_CANONICAL_NAME_MAX + 1] = "";
         uint32_t observation_count = 0u;
         char card_json[AIMEE_DB2_ENTITY_PROFILE_UPSERT_CARD_JSON_MAX + 1] = "";
         if (aimee_db2_entity_profile_upsert_request_decode(
                 request_body, request_len, entity_id, sizeof(entity_id), canonical_name,
                 sizeof(canonical_name), &observation_count, card_json, sizeof(card_json)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENTITY_PROFILE_UPSERT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->entity_profile_upsert)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->entity_profile_upsert(entity_id, canonical_name,
                                                          (int)observation_count, card_json) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_entity_profile_upsert_reply_encode(acknowledged, response_body,
                                                             response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project_name[AIMEE_DB2_PROJECT_STATS_PROJECT_NAME_MAX + 1] = "";
         if (aimee_db2_project_stats_request_decode(request_body, request_len, project_name,
                                                    sizeof(project_name)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROJECT_STATS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->project_stats)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t file_count = 0u;
            uint32_t definition_count = 0u;
            {
               int files = 0;
               int defs = 0;
               if (backend->project_stats(project_name, &files, &defs) == 0)
               {
                  file_count = files > 0 ? (uint32_t)files : 0u;
                  definition_count = defs > 0 ? (uint32_t)defs : 0u;
               }
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_project_stats_reply_encode(file_count, definition_count, response_body,
                                                     response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
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
      {
         if (aimee_db2_console_oidc_get_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_CONSOLE_OIDC_GET_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->console_oidc_get)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t configured = 0u;
            char oidc_issuer[AIMEE_DB2_CONSOLE_OIDC_GET_OIDC_ISSUER_MAX + 1] = "";
            char oidc_audience[AIMEE_DB2_CONSOLE_OIDC_GET_OIDC_AUDIENCE_MAX + 1] = "";
            char oidc_jwks_url[AIMEE_DB2_CONSOLE_OIDC_GET_OIDC_JWKS_URL_MAX + 1] = "";
            char oidc_admin_claim[AIMEE_DB2_CONSOLE_OIDC_GET_OIDC_ADMIN_CLAIM_MAX + 1] = "";
            char oidc_admin_values[AIMEE_DB2_CONSOLE_OIDC_GET_OIDC_ADMIN_VALUES_MAX + 1] = "";
            char oidc_updated_at[AIMEE_DB2_CONSOLE_OIDC_GET_OIDC_UPDATED_AT_MAX + 1] = "";
            db2_console_oidc_t oidc;
            memset(&oidc, 0, sizeof(oidc));
            if (backend->console_oidc_get(&oidc) == 0)
            {
               configured = 1u;
               snprintf(oidc_issuer, sizeof(oidc_issuer), "%s", oidc.issuer);
               snprintf(oidc_audience, sizeof(oidc_audience), "%s", oidc.audience);
               snprintf(oidc_jwks_url, sizeof(oidc_jwks_url), "%s", oidc.jwks_url);
               snprintf(oidc_admin_claim, sizeof(oidc_admin_claim), "%s", oidc.admin_claim);
               snprintf(oidc_admin_values, sizeof(oidc_admin_values), "%s", oidc.admin_values);
               snprintf(oidc_updated_at, sizeof(oidc_updated_at), "%s", oidc.updated_at);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_console_oidc_get_reply_encode(
                    configured, oidc_issuer, oidc_audience, oidc_jwks_url, oidc_admin_claim,
                    oidc_admin_values, oidc_updated_at, response_body, response_capacity,
                    response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char oidc_issuer[AIMEE_DB2_CONSOLE_OIDC_PUT_OIDC_ISSUER_MAX + 1] = "";
         char oidc_audience[AIMEE_DB2_CONSOLE_OIDC_PUT_OIDC_AUDIENCE_MAX + 1] = "";
         char oidc_jwks_url[AIMEE_DB2_CONSOLE_OIDC_PUT_OIDC_JWKS_URL_MAX + 1] = "";
         char oidc_admin_claim[AIMEE_DB2_CONSOLE_OIDC_PUT_OIDC_ADMIN_CLAIM_MAX + 1] = "";
         char oidc_admin_values[AIMEE_DB2_CONSOLE_OIDC_PUT_OIDC_ADMIN_VALUES_MAX + 1] = "";
         if (aimee_db2_console_oidc_put_request_decode(
                 request_body, request_len, oidc_issuer, sizeof(oidc_issuer), oidc_audience,
                 sizeof(oidc_audience), oidc_jwks_url, sizeof(oidc_jwks_url), oidc_admin_claim,
                 sizeof(oidc_admin_claim), oidc_admin_values, sizeof(oidc_admin_values)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CONSOLE_OIDC_PUT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->console_oidc_put)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            db2_console_oidc_t oidc;
            memset(&oidc, 0, sizeof(oidc));
            snprintf(oidc.issuer, sizeof(oidc.issuer), "%s", oidc_issuer);
            snprintf(oidc.audience, sizeof(oidc.audience), "%s", oidc_audience);
            snprintf(oidc.jwks_url, sizeof(oidc.jwks_url), "%s", oidc_jwks_url);
            snprintf(oidc.admin_claim, sizeof(oidc.admin_claim), "%s", oidc_admin_claim);
            snprintf(oidc.admin_values, sizeof(oidc.admin_values), "%s", oidc_admin_values);
            acknowledged = backend->console_oidc_put(&oidc) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_console_oidc_put_reply_encode(acknowledged, response_body,
                                                        response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char actor_role[AIMEE_DB2_KB_AUDIT_APPEND_ACTOR_ROLE_MAX + 1] = "";
         char actor_principal[AIMEE_DB2_KB_AUDIT_APPEND_ACTOR_PRINCIPAL_MAX + 1] = "";
         char audit_action[AIMEE_DB2_KB_AUDIT_APPEND_AUDIT_ACTION_MAX + 1] = "";
         char audit_subject[AIMEE_DB2_KB_AUDIT_APPEND_AUDIT_SUBJECT_MAX + 1] = "";
         char audit_verdict[AIMEE_DB2_KB_AUDIT_APPEND_AUDIT_VERDICT_MAX + 1] = "";
         char audit_detail[AIMEE_DB2_KB_AUDIT_APPEND_AUDIT_DETAIL_MAX + 1] = "";
         if (aimee_db2_kb_audit_append_request_decode(
                 request_body, request_len, actor_role, sizeof(actor_role), actor_principal,
                 sizeof(actor_principal), audit_action, sizeof(audit_action), audit_subject,
                 sizeof(audit_subject), audit_verdict, sizeof(audit_verdict), audit_detail,
                 sizeof(audit_detail)) == 0)
         {
            if (response_capacity < AIMEE_DB2_KB_AUDIT_APPEND_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->kb_audit_append)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->kb_audit_append(actor_role, actor_principal, audit_action,
                                                    audit_subject, audit_verdict, audit_detail) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_kb_audit_append_reply_encode(acknowledged, response_body,
                                                       response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char cert_fingerprint[AIMEE_DB2_ENROLLMENT_TOUCH_LAST_SEEN_CERT_FINGERPRINT_MAX + 1] = "";
         char enrollment_scope[AIMEE_DB2_ENROLLMENT_TOUCH_LAST_SEEN_ENROLLMENT_SCOPE_MAX + 1] = "";
         if (aimee_db2_enrollment_touch_last_seen_request_decode(
                 request_body, request_len, cert_fingerprint, sizeof(cert_fingerprint),
                 enrollment_scope, sizeof(enrollment_scope)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ENROLLMENT_TOUCH_LAST_SEEN_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->enrollment_touch_last_seen)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t recorded = 0u;
            backend->enrollment_touch_last_seen(cert_fingerprint, enrollment_scope);
            recorded = 1u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_enrollment_touch_last_seen_reply_encode(
                    recorded, response_body, response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         /* Every db2-envelope-string-pair-u32-v1 operation this family owns. */
         const db2_string_pair_count_binding_t bindings[] = {
             {aimee_db2_enrollment_active_request_decode, aimee_db2_enrollment_active_reply_encode,
              backend ? backend->enrollment_active : NULL, AIMEE_DB2_ENROLLMENT_ACTIVE_MAX},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_pair_count(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
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
      {
         /* Every db2-envelope-string-u32-v1 operation this family owns. */
         const db2_string_count_binding_t bindings[] = {
             {aimee_db2_blob_referenced_request_decode, aimee_db2_blob_referenced_reply_encode,
              backend ? backend->blob_referenced : NULL, AIMEE_DB2_BLOB_REFERENCED_MAX},
             {aimee_db2_fence_active_request_decode, aimee_db2_fence_active_reply_encode,
              backend ? backend->fence_active : NULL, AIMEE_DB2_FENCE_ACTIVE_MAX},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_count(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         /* Every db2-envelope-u64-u32-v1 operation this family owns. */
         const db2_u64_probe_binding_t bindings[] = {
             {aimee_db2_document_exists_request_decode, aimee_db2_document_exists_reply_encode,
              backend ? backend->document_exists : NULL},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_u64_probe(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         /* Every db2-envelope-string-pair-u32-v1 operation this family owns. */
         const db2_string_pair_count_binding_t bindings[] = {
             {aimee_db2_doc_exists_by_hash_request_decode,
              aimee_db2_doc_exists_by_hash_reply_encode,
              backend ? backend->doc_exists_by_hash : NULL, AIMEE_DB2_DOC_EXISTS_BY_HASH_MAX},
             {aimee_db2_pdf_quarantine_confirm_request_decode,
              aimee_db2_pdf_quarantine_confirm_reply_encode,
              backend ? backend->pdf_quarantine_confirm : NULL,
              AIMEE_DB2_PDF_QUARANTINE_CONFIRM_MAX},
             {aimee_db2_pdf_quarantine_reject_request_decode,
              aimee_db2_pdf_quarantine_reject_reply_encode,
              backend ? backend->pdf_quarantine_reject : NULL, AIMEE_DB2_PDF_QUARANTINE_REJECT_MAX},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_pair_count(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         char rel_type[AIMEE_DB2_ONTOLOGY_EVAL_STATUS_REL_TYPE_MAX + 1] = "";
         if (aimee_db2_ontology_eval_status_request_decode(request_body, request_len, rel_type,
                                                           sizeof(rel_type)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ONTOLOGY_EVAL_STATUS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->ontology_eval_status)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char status[AIMEE_DB2_ONTOLOGY_EVAL_STATUS_STATUS_MAX + 1] = "";
            backend->ontology_eval_status(rel_type, status, sizeof(status));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_ontology_eval_status_reply_encode(status, response_body,
                                                            response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t task_id = 0u;
         char state[AIMEE_DB2_TASK_UPDATE_STATE_STATE_MAX + 1] = "";
         if (aimee_db2_task_update_state_request_decode(request_body, request_len, &task_id, state,
                                                        sizeof(state)) == 0)
         {
            if (response_capacity < AIMEE_DB2_TASK_UPDATE_STATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->task_update_state)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t changed = 0u;
            changed = backend->task_update_state((int64_t)task_id, state) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_task_update_state_reply_encode(changed, response_body, response_capacity,
                                                         response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t release_id = 0u;
         uint64_t doc_id = 0u;
         if (aimee_db2_release_add_doc_request_decode(request_body, request_len, &release_id,
                                                      &doc_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_RELEASE_ADD_DOC_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->release_add_doc)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->release_add_doc((int64_t)release_id, (int64_t)doc_id) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_release_add_doc_reply_encode(acknowledged, response_body,
                                                       response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char rel_type[AIMEE_DB2_ONTOLOGY_APPROVE_REL_TYPE_MAX + 1] = "";
         if (aimee_db2_ontology_approve_request_decode(request_body, request_len, rel_type,
                                                       sizeof(rel_type)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ONTOLOGY_APPROVE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->ontology_approve)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->ontology_approve(rel_type) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_ontology_approve_reply_encode(acknowledged, response_body,
                                                        response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char rel_type[AIMEE_DB2_ONTOLOGY_REJECT_REL_TYPE_MAX + 1] = "";
         if (aimee_db2_ontology_reject_request_decode(request_body, request_len, rel_type,
                                                      sizeof(rel_type)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ONTOLOGY_REJECT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->ontology_reject)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->ontology_reject(rel_type) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_ontology_reject_reply_encode(acknowledged, response_body,
                                                       response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_DOC_ASSETS_DELETE_FOR_DOC_PROJECT_MAX + 1] = "";
         char document_key[AIMEE_DB2_DOC_ASSETS_DELETE_FOR_DOC_DOCUMENT_KEY_MAX + 1] = "";
         if (aimee_db2_doc_assets_delete_for_doc_request_decode(request_body, request_len, project,
                                                                sizeof(project), document_key,
                                                                sizeof(document_key)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DOC_ASSETS_DELETE_FOR_DOC_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->doc_assets_delete_for_doc)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t deleted = 0u;
            {
               int removed = backend->doc_assets_delete_for_doc(project, document_key);
               deleted = removed < 0 ? 0u : (uint32_t)removed;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_doc_assets_delete_for_doc_reply_encode(
                    deleted, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char rel_type[AIMEE_DB2_ONTOLOGY_MAP_REL_TYPE_MAX + 1] = "";
         char mapped_to[AIMEE_DB2_ONTOLOGY_MAP_MAPPED_TO_MAX + 1] = "";
         if (aimee_db2_ontology_map_request_decode(request_body, request_len, rel_type,
                                                   sizeof(rel_type), mapped_to,
                                                   sizeof(mapped_to)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ONTOLOGY_MAP_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->ontology_map)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->ontology_map(rel_type, mapped_to) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_ontology_map_reply_encode(acknowledged, response_body, response_capacity,
                                                    response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char release_name[AIMEE_DB2_RELEASE_CREATE_RELEASE_NAME_MAX + 1] = "";
         if (aimee_db2_release_create_request_decode(request_body, request_len, release_name,
                                                     sizeof(release_name)) == 0)
         {
            if (response_capacity < AIMEE_DB2_RELEASE_CREATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->release_create)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t release_id = 0u;
            {
               int64_t created = backend->release_create(release_name);
               release_id = created < 0 ? 0u : (uint64_t)created;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_release_create_reply_encode(release_id, response_body, response_capacity,
                                                      response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PURGE_FENCE_HEARTBEAT_PROJECT_MAX + 1] = "";
         char generation[AIMEE_DB2_PURGE_FENCE_HEARTBEAT_GENERATION_MAX + 1] = "";
         char purge_id[AIMEE_DB2_PURGE_FENCE_HEARTBEAT_PURGE_ID_MAX + 1] = "";
         if (aimee_db2_purge_fence_heartbeat_request_decode(
                 request_body, request_len, project, sizeof(project), generation,
                 sizeof(generation), purge_id, sizeof(purge_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PURGE_FENCE_HEARTBEAT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->purge_fence_heartbeat)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t applied = 0u;
            applied = backend->purge_fence_heartbeat(project, generation, purge_id) == 1 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_purge_fence_heartbeat_reply_encode(applied, response_body,
                                                             response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PURGE_FENCE_CLEAR_PROJECT_MAX + 1] = "";
         char generation[AIMEE_DB2_PURGE_FENCE_CLEAR_GENERATION_MAX + 1] = "";
         char purge_id[AIMEE_DB2_PURGE_FENCE_CLEAR_PURGE_ID_MAX + 1] = "";
         if (aimee_db2_purge_fence_clear_request_decode(
                 request_body, request_len, project, sizeof(project), generation,
                 sizeof(generation), purge_id, sizeof(purge_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PURGE_FENCE_CLEAR_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->purge_fence_clear)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t applied = 0u;
            applied = backend->purge_fence_clear(project, generation, purge_id) == 1 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_purge_fence_clear_reply_encode(applied, response_body, response_capacity,
                                                         response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_DOCUMENT_STORED_HASH_PROJECT_MAX + 1] = "";
         char file_path[AIMEE_DB2_DOCUMENT_STORED_HASH_FILE_PATH_MAX + 1] = "";
         if (aimee_db2_document_stored_hash_request_decode(request_body, request_len, project,
                                                           sizeof(project), file_path,
                                                           sizeof(file_path)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DOCUMENT_STORED_HASH_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->document_stored_hash)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char file_hash[AIMEE_DB2_DOCUMENT_STORED_HASH_FILE_HASH_MAX + 1] = "";
            (void)backend->document_stored_hash(project, file_path, file_hash, sizeof(file_hash));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_document_stored_hash_reply_encode(file_hash, response_body,
                                                            response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_DOCUMENT_HASH_EXISTS_PROJECT_MAX + 1] = "";
         char file_hash[AIMEE_DB2_DOCUMENT_HASH_EXISTS_FILE_HASH_MAX + 1] = "";
         if (aimee_db2_document_hash_exists_request_decode(request_body, request_len, project,
                                                           sizeof(project), file_hash,
                                                           sizeof(file_hash)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DOCUMENT_HASH_EXISTS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->document_hash_exists)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t exists = 0u;
            char sample_path[AIMEE_DB2_DOCUMENT_HASH_EXISTS_SAMPLE_PATH_MAX + 1] = "";
            exists = backend->document_hash_exists(project, file_hash, sample_path,
                                                   sizeof(sample_path)) > 0
                         ? 1u
                         : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_document_hash_exists_reply_encode(exists, sample_path, response_body,
                                                            response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_PDF_TSR_STATE_PROJECT_MAX + 1] = "";
         char document_key[AIMEE_DB2_PDF_TSR_STATE_DOCUMENT_KEY_MAX + 1] = "";
         if (aimee_db2_pdf_tsr_state_request_decode(request_body, request_len, project,
                                                    sizeof(project), document_key,
                                                    sizeof(document_key)) == 0)
         {
            if (response_capacity < AIMEE_DB2_PDF_TSR_STATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->pdf_tsr_state)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char tsr_state[AIMEE_DB2_PDF_TSR_STATE_TSR_STATE_MAX + 1] = "";
            (void)backend->pdf_tsr_state(project, document_key, tsr_state, sizeof(tsr_state));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_pdf_tsr_state_reply_encode(tsr_state, response_body, response_capacity,
                                                     response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_DOCUMENT_CHUNK_IDS_PROJECT_MAX + 1] = "";
         char file_path[AIMEE_DB2_DOCUMENT_CHUNK_IDS_FILE_PATH_MAX + 1] = "";
         if (aimee_db2_document_chunk_ids_request_decode(request_body, request_len, project,
                                                         sizeof(project), file_path,
                                                         sizeof(file_path)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DOCUMENT_CHUNK_IDS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->document_chunk_ids)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_document_chunk_ids_row_t rows[AIMEE_DB2_DOCUMENT_CHUNK_IDS_MAX_ROWS];
            uint32_t count = 0u;
            {
               int64_t ids[AIMEE_DB2_DOCUMENT_CHUNK_IDS_MAX_ROWS];
               int found = backend->document_chunk_ids(project, file_path, ids,
                                                       AIMEE_DB2_DOCUMENT_CHUNK_IDS_MAX_ROWS);
               for (int index = 0; index < found; index++)
                  rows[index].document_id = (uint64_t)ids[index];
               count = found < 0 ? 0u : (uint32_t)found;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_document_chunk_ids_reply_encode(rows, count, response_body,
                                                          response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t task_id = 0u;
         uint32_t limit = 0u;
         if (aimee_db2_task_edges_request_decode(request_body, request_len, &task_id, &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_TASK_EDGES_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->task_edges)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_task_edges_row_t rows[AIMEE_DB2_TASK_EDGES_MAX_ROWS];
            uint32_t count = 0u;
            {
               task_edge_t *found = malloc(sizeof(*found) * AIMEE_DB2_TASK_EDGES_MAX_ROWS);
               if (!found)
                  return AIMEE_MODULE_STATUS_INTERNAL;
               int written = backend->task_edges((int64_t)task_id, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].task_edge_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].source_task_id =
                      found[index].source_id < 0 ? 0u : (uint64_t)found[index].source_id;
                  rows[index].target_task_id =
                      found[index].target_id < 0 ? 0u : (uint64_t)found[index].target_id;
                  snprintf(rows[index].task_relation, sizeof(rows[index].task_relation), "%s",
                           found[index].relation);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_task_edges_reply_encode(rows, count, response_body, response_capacity,
                                                  response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char task_state_filter[AIMEE_DB2_TASK_LIST_TASK_STATE_FILTER_MAX + 1] = "";
         char task_session_filter[AIMEE_DB2_TASK_LIST_TASK_SESSION_FILTER_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_task_list_request_decode(request_body, request_len, task_state_filter,
                                                sizeof(task_state_filter), task_session_filter,
                                                sizeof(task_session_filter), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_TASK_LIST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->task_list)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_task_list_row_t *rows = malloc(sizeof(*rows) * AIMEE_DB2_TASK_LIST_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               aimee_task_t *found = malloc(sizeof(*found) * AIMEE_DB2_TASK_LIST_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->task_list(task_state_filter, task_session_filter, (int)limit,
                                                found, AIMEE_DB2_TASK_LIST_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].task_row_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].parent_task_id =
                      found[index].parent_id < 0 ? 0u : (uint64_t)found[index].parent_id;
                  rows[index].task_confidence = found[index].confidence;
                  snprintf(rows[index].task_title, sizeof(rows[index].task_title), "%s",
                           found[index].title);
                  snprintf(rows[index].task_state, sizeof(rows[index].task_state), "%s",
                           found[index].state);
                  snprintf(rows[index].task_created_at, sizeof(rows[index].task_created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].task_updated_at, sizeof(rows[index].task_updated_at), "%s",
                           found[index].updated_at);
                  snprintf(rows[index].task_session_id, sizeof(rows[index].task_session_id), "%s",
                           found[index].session_id);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_task_list_reply_encode(rows, count, response_body, response_capacity,
                                                 response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t parent_task = 0u;
         if (aimee_db2_task_subtasks_request_decode(request_body, request_len, &parent_task) == 0)
         {
            if (response_capacity < AIMEE_DB2_TASK_SUBTASKS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->task_subtasks)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_task_subtasks_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_TASK_SUBTASKS_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               aimee_task_t *found = malloc(sizeof(*found) * AIMEE_DB2_TASK_SUBTASKS_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->task_subtasks((int64_t)parent_task, found,
                                                    AIMEE_DB2_TASK_SUBTASKS_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].task_row_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].parent_task_id =
                      found[index].parent_id < 0 ? 0u : (uint64_t)found[index].parent_id;
                  rows[index].task_confidence = found[index].confidence;
                  snprintf(rows[index].task_title, sizeof(rows[index].task_title), "%s",
                           found[index].title);
                  snprintf(rows[index].task_state, sizeof(rows[index].task_state), "%s",
                           found[index].state);
                  snprintf(rows[index].task_created_at, sizeof(rows[index].task_created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].task_updated_at, sizeof(rows[index].task_updated_at), "%s",
                           found[index].updated_at);
                  snprintf(rows[index].task_session_id, sizeof(rows[index].task_session_id), "%s",
                           found[index].session_id);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_task_subtasks_reply_encode(rows, count, response_body, response_capacity,
                                                     response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t edge_source_task = 0u;
         uint64_t edge_target_task = 0u;
         char edge_relation[AIMEE_DB2_TASK_ADD_EDGE_EDGE_RELATION_MAX + 1] = "";
         if (aimee_db2_task_add_edge_request_decode(request_body, request_len, &edge_source_task,
                                                    &edge_target_task, edge_relation,
                                                    sizeof(edge_relation)) == 0)
         {
            if (response_capacity < AIMEE_DB2_TASK_ADD_EDGE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->task_add_edge)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->task_add_edge((int64_t)edge_source_task,
                                                  (int64_t)edge_target_task, edge_relation) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_task_add_edge_reply_encode(acknowledged, response_body, response_capacity,
                                                     response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project_name[AIMEE_DB2_CROSS_REPO_SET_TRUST_PROJECT_NAME_MAX + 1] = "";
         char new_trust[AIMEE_DB2_CROSS_REPO_SET_TRUST_NEW_TRUST_MAX + 1] = "";
         char trust_actor[AIMEE_DB2_CROSS_REPO_SET_TRUST_TRUST_ACTOR_MAX + 1] = "";
         char trust_request_id[AIMEE_DB2_CROSS_REPO_SET_TRUST_TRUST_REQUEST_ID_MAX + 1] = "";
         if (aimee_db2_cross_repo_set_trust_request_decode(
                 request_body, request_len, project_name, sizeof(project_name), new_trust,
                 sizeof(new_trust), trust_actor, sizeof(trust_actor), trust_request_id,
                 sizeof(trust_request_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CROSS_REPO_SET_TRUST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->cross_repo_set_trust)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t trust_result = 0u;
            char prior_trust[AIMEE_DB2_CROSS_REPO_SET_TRUST_PRIOR_TRUST_MAX + 1] = "";
            uint32_t trust_changed = 0u;
            {
               int changed = 0;
               int rc = backend->cross_repo_set_trust(project_name, new_trust, trust_actor,
                                                      trust_request_id, prior_trust,
                                                      sizeof(prior_trust), &changed);
               trust_result = rc == 0 ? 0u : (rc == 1 ? 1u : 2u);
               trust_changed = changed ? 1u : 0u;
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_cross_repo_set_trust_reply_encode(trust_result, prior_trust,
                                                            trust_changed, response_body,
                                                            response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t callee_repo_min = 0u;
         uint32_t definition_repo_min = 0u;
         uint32_t symbol_length_min = 0u;
         if (aimee_db2_recompute_blocked_symbols_request_decode(
                 request_body, request_len, &callee_repo_min, &definition_repo_min,
                 &symbol_length_min) == 0)
         {
            if (response_capacity < AIMEE_DB2_RECOMPUTE_BLOCKED_SYMBOLS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->recompute_blocked_symbols)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t blocked_count = 0u;
            {
               int blocked = backend->recompute_blocked_symbols(
                   (int)callee_repo_min, (int)definition_repo_min, (int)symbol_length_min);
               blocked_count = blocked > 0 ? (uint32_t)blocked : 0u;
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_recompute_blocked_symbols_reply_encode(
                    blocked_count, response_body, response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
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
      {
         /* Four reads that take one string and answer one number. They share an
          * argument buffer because the widest bound covers them all, and each
          * clamps its own answer: an existence probe must not report two. */
         char argument[AIMEE_DB2_ANTI_PATTERN_EXISTS_EXACT_ARGUMENT_MAX + 1];
         int (*read)(const char *) = NULL;
         int (*answer)(uint32_t, uint8_t *, size_t, uint32_t *) = NULL;
         uint32_t bound = 0u;
         if (!answer && aimee_db2_anti_pattern_exists_exact_request_decode(
                            request_body, request_len, argument, sizeof(argument)) == 0)
         {
            read = backend ? backend->anti_pattern_exists_exact : NULL;
            answer = aimee_db2_anti_pattern_exists_exact_reply_encode;
            bound = AIMEE_DB2_ANTI_PATTERN_EXISTS_EXACT_MAX;
         }
         if (!answer && aimee_db2_anti_pattern_exists_by_source_ref_request_decode(
                            request_body, request_len, argument, sizeof(argument)) == 0)
         {
            read = backend ? backend->anti_pattern_exists_by_source_ref : NULL;
            answer = aimee_db2_anti_pattern_exists_by_source_ref_reply_encode;
            bound = AIMEE_DB2_ANTI_PATTERN_EXISTS_BY_SOURCE_REF_MAX;
         }
         if (!answer && aimee_db2_artifact_citation_count_request_decode(
                            request_body, request_len, argument, sizeof(argument)) == 0)
         {
            read = backend ? backend->artifact_citation_count : NULL;
            answer = aimee_db2_artifact_citation_count_reply_encode;
            bound = AIMEE_DB2_ARTIFACT_CITATION_COUNT_MAX;
         }
         if (!answer && aimee_db2_commits_in_last_7_days_request_decode(
                            request_body, request_len, argument, sizeof(argument)) == 0)
         {
            read = backend ? backend->commits_in_last_7_days : NULL;
            answer = aimee_db2_commits_in_last_7_days_reply_encode;
            bound = AIMEE_DB2_COMMITS_IN_LAST_7_DAYS_MAX;
         }
         if (answer)
         {
            if (response_capacity < AIMEE_DB2_ANTI_PATTERN_EXISTS_EXACT_RESPONSE_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!read)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            int counted = read(argument);
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (counted < 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            uint32_t value = (uint32_t)counted;
            if (value > bound)
               value = bound;
            if (answer(value, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         /* Every db2-envelope-string-u32-v1 operation this family owns. */
         const db2_string_count_binding_t bindings[] = {
             {aimee_db2_anti_pattern_exists_exact_request_decode,
              aimee_db2_anti_pattern_exists_exact_reply_encode,
              backend ? backend->anti_pattern_exists_exact : NULL,
              AIMEE_DB2_ANTI_PATTERN_EXISTS_EXACT_MAX},
             {aimee_db2_anti_pattern_exists_by_source_ref_request_decode,
              aimee_db2_anti_pattern_exists_by_source_ref_reply_encode,
              backend ? backend->anti_pattern_exists_by_source_ref : NULL,
              AIMEE_DB2_ANTI_PATTERN_EXISTS_BY_SOURCE_REF_MAX},
             {aimee_db2_artifact_citation_count_request_decode,
              aimee_db2_artifact_citation_count_reply_encode,
              backend ? backend->artifact_citation_count : NULL,
              AIMEE_DB2_ARTIFACT_CITATION_COUNT_MAX},
             {aimee_db2_commits_in_last_7_days_request_decode,
              aimee_db2_commits_in_last_7_days_reply_encode,
              backend ? backend->commits_in_last_7_days : NULL,
              AIMEE_DB2_COMMITS_IN_LAST_7_DAYS_MAX},
             {aimee_db2_fidelity_attribution_count_request_decode,
              aimee_db2_fidelity_attribution_count_reply_encode,
              backend ? backend->fidelity_attribution_count : NULL,
              AIMEE_DB2_FIDELITY_ATTRIBUTION_COUNT_MAX},
             {aimee_db2_failed_query_bump_request_decode, aimee_db2_failed_query_bump_reply_encode,
              backend ? backend->failed_query_bump : NULL, AIMEE_DB2_FAILED_QUERY_BUMP_MAX},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_count(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         /* Every db2-envelope-u64-ack-v1 operation this family owns. */
         const db2_u64_ack_binding_t bindings[] = {
             {aimee_db2_trace_mining_record_request_decode,
              aimee_db2_trace_mining_record_reply_encode,
              backend ? backend->trace_mining_record : NULL},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_u64_ack(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         /* Every db2-envelope-string-ack-v1 operation this family owns. */
         const db2_string_ack_binding_t bindings[] = {
             {aimee_db2_artifact_stamp_reflected_request_decode,
              aimee_db2_artifact_stamp_reflected_reply_encode,
              backend ? backend->artifact_stamp_reflected : NULL},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_ack(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         /* Every db2-envelope-string-pair-ack-v1 operation this family owns. */
         const db2_string_pair_ack_binding_t bindings[] = {
             {aimee_db2_artifact_set_state_request_decode,
              aimee_db2_artifact_set_state_reply_encode,
              backend ? backend->artifact_set_state : NULL},
             {aimee_db2_artifact_register_exemplar_request_decode,
              aimee_db2_artifact_register_exemplar_reply_encode,
              backend ? backend->artifact_register_exemplar : NULL},
             {aimee_db2_evidence_enqueue_request_decode, aimee_db2_evidence_enqueue_reply_encode,
              backend ? backend->evidence_enqueue : NULL},
             {aimee_db2_evidence_mark_failed_request_decode,
              aimee_db2_evidence_mark_failed_reply_encode,
              backend ? backend->evidence_mark_failed : NULL},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_pair_ack(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         char decision_point[AIMEE_DB2_BANDIT_ARMS_LIST_DECISION_POINT_MAX + 1] = "";
         if (aimee_db2_bandit_arms_list_request_decode(request_body, request_len, decision_point,
                                                       sizeof(decision_point)) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_ARMS_LIST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_arms_list)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char arms[AIMEE_DB2_BANDIT_ARMS_LIST_ARMS_MAX + 1] = "";
            backend->bandit_arms_list(decision_point, arms, sizeof(arms));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_bandit_arms_list_reply_encode(arms, response_body, response_capacity,
                                                        response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char decision_point[AIMEE_DB2_BANDIT_PROMOTION_GET_DECISION_POINT_MAX + 1] = "";
         if (aimee_db2_bandit_promotion_get_request_decode(
                 request_body, request_len, decision_point, sizeof(decision_point)) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_PROMOTION_GET_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_promotion_get)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char arm_id[AIMEE_DB2_BANDIT_PROMOTION_GET_ARM_ID_MAX + 1] = "";
            backend->bandit_promotion_get(decision_point, arm_id, sizeof(arm_id));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_bandit_promotion_get_reply_encode(arm_id, response_body,
                                                            response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t decision_id = 0u;
         char outcome[AIMEE_DB2_DECISION_LOG_SET_OUTCOME_OUTCOME_MAX + 1] = "";
         if (aimee_db2_decision_log_set_outcome_request_decode(
                 request_body, request_len, &decision_id, outcome, sizeof(outcome)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DECISION_LOG_SET_OUTCOME_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->decision_log_set_outcome)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->decision_log_set_outcome((int64_t)decision_id, outcome) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_decision_log_set_outcome_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t decision_id = 0u;
         char status[AIMEE_DB2_DECISION_LOG_SET_STATUS_STATUS_MAX + 1] = "";
         if (aimee_db2_decision_log_set_status_request_decode(
                 request_body, request_len, &decision_id, status, sizeof(status)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DECISION_LOG_SET_STATUS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->decision_log_set_status)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->decision_log_set_status((int64_t)decision_id, status) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_decision_log_set_status_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t decision_id = 0u;
         char revisit_when[AIMEE_DB2_DECISION_LOG_SET_REVISIT_REVISIT_WHEN_MAX + 1] = "";
         if (aimee_db2_decision_log_set_revisit_request_decode(
                 request_body, request_len, &decision_id, revisit_when, sizeof(revisit_when)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DECISION_LOG_SET_REVISIT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->decision_log_set_revisit)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->decision_log_set_revisit((int64_t)decision_id, revisit_when) == 0 ? 1u
                                                                                           : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_decision_log_set_revisit_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t rule_id = 0u;
         if (aimee_db2_collab_rule_approve_request_decode(request_body, request_len, &rule_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_COLLAB_RULE_APPROVE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->collab_rule_approve)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->collab_rule_approve((int)rule_id) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_collab_rule_approve_reply_encode(acknowledged, response_body,
                                                           response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t rule_id = 0u;
         if (aimee_db2_collab_rule_reject_request_decode(request_body, request_len, &rule_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_COLLAB_RULE_REJECT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->collab_rule_reject)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t changed = 0u;
            changed = backend->collab_rule_reject((int)rule_id) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_collab_rule_reject_reply_encode(changed, response_body, response_capacity,
                                                          response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t rule_id = 0u;
         if (aimee_db2_collab_rule_retire_request_decode(request_body, request_len, &rule_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_COLLAB_RULE_RETIRE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->collab_rule_retire)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->collab_rule_retire((int)rule_id) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_collab_rule_retire_reply_encode(acknowledged, response_body,
                                                          response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t proposal_id = 0u;
         if (aimee_db2_proposal_bump_corroboration_request_decode(request_body, request_len,
                                                                  &proposal_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROPOSAL_BUMP_CORROBORATION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->proposal_bump_corroboration)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->proposal_bump_corroboration((int)proposal_id) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_proposal_bump_corroboration_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t proposal_id = 0u;
         if (aimee_db2_proposal_mark_committed_request_decode(request_body, request_len,
                                                              &proposal_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_PROPOSAL_MARK_COMMITTED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->proposal_mark_committed)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->proposal_mark_committed((int)proposal_id) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_proposal_mark_committed_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t rule_row_id = 0u;
         if (aimee_db2_rules_delete_by_id_request_decode(request_body, request_len, &rule_row_id) ==
             0)
         {
            if (response_capacity < AIMEE_DB2_RULES_DELETE_BY_ID_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->rules_delete_by_id)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t deleted = 0u;
            deleted = backend->rules_delete_by_id((int)rule_row_id) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_rules_delete_by_id_reply_encode(deleted, response_body, response_capacity,
                                                          response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t min_rows = 0u;
         if (aimee_db2_calibration_surfaces_with_data_request_decode(request_body, request_len,
                                                                     &min_rows) == 0)
         {
            if (response_capacity < AIMEE_DB2_CALIBRATION_SURFACES_WITH_DATA_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->calibration_surfaces_with_data)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t surfaces = 0u;
            {
               int counted = backend->calibration_surfaces_with_data((int)min_rows);
               surfaces = counted < 0 ? 0u : (uint32_t)counted;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_calibration_surfaces_with_data_reply_encode(
                    surfaces, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char citing_artifact_id[AIMEE_DB2_ARTIFACT_CITE_CITING_ARTIFACT_ID_MAX + 1] = "";
         char source_kind[AIMEE_DB2_ARTIFACT_CITE_SOURCE_KIND_MAX + 1] = "";
         char source_id[AIMEE_DB2_ARTIFACT_CITE_SOURCE_ID_MAX + 1] = "";
         if (aimee_db2_artifact_cite_request_decode(
                 request_body, request_len, citing_artifact_id, sizeof(citing_artifact_id),
                 source_kind, sizeof(source_kind), source_id, sizeof(source_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ARTIFACT_CITE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->artifact_cite)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->artifact_cite(citing_artifact_id, source_kind, source_id) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_artifact_cite_reply_encode(acknowledged, response_body, response_capacity,
                                                     response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char from_artifact_id[AIMEE_DB2_ARTIFACT_LINK_FROM_ARTIFACT_ID_MAX + 1] = "";
         char to_artifact_id[AIMEE_DB2_ARTIFACT_LINK_TO_ARTIFACT_ID_MAX + 1] = "";
         char link_kind[AIMEE_DB2_ARTIFACT_LINK_LINK_KIND_MAX + 1] = "";
         if (aimee_db2_artifact_link_request_decode(
                 request_body, request_len, from_artifact_id, sizeof(from_artifact_id),
                 to_artifact_id, sizeof(to_artifact_id), link_kind, sizeof(link_kind)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ARTIFACT_LINK_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->artifact_link)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->artifact_link(from_artifact_id, to_artifact_id, link_kind) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_artifact_link_reply_encode(acknowledged, response_body, response_capacity,
                                                     response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char decision_point[AIMEE_DB2_BANDIT_PROMOTION_SET_DECISION_POINT_MAX + 1] = "";
         char arm_id[AIMEE_DB2_BANDIT_PROMOTION_SET_ARM_ID_MAX + 1] = "";
         char rollback_arm[AIMEE_DB2_BANDIT_PROMOTION_SET_ROLLBACK_ARM_MAX + 1] = "";
         if (aimee_db2_bandit_promotion_set_request_decode(
                 request_body, request_len, decision_point, sizeof(decision_point), arm_id,
                 sizeof(arm_id), rollback_arm, sizeof(rollback_arm)) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_PROMOTION_SET_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_promotion_set)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->bandit_promotion_set(decision_point, arm_id, rollback_arm) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_bandit_promotion_set_reply_encode(acknowledged, response_body,
                                                            response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char rule_text[AIMEE_DB2_COLLAB_RULE_PROPOSE_RULE_TEXT_MAX + 1] = "";
         char rule_reason[AIMEE_DB2_COLLAB_RULE_PROPOSE_RULE_REASON_MAX + 1] = "";
         char proposed_by[AIMEE_DB2_COLLAB_RULE_PROPOSE_PROPOSED_BY_MAX + 1] = "";
         if (aimee_db2_collab_rule_propose_request_decode(
                 request_body, request_len, rule_text, sizeof(rule_text), rule_reason,
                 sizeof(rule_reason), proposed_by, sizeof(proposed_by)) == 0)
         {
            if (response_capacity < AIMEE_DB2_COLLAB_RULE_PROPOSE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->collab_rule_propose)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t rule_id = 0u;
            {
               int created = backend->collab_rule_propose(rule_text, rule_reason, proposed_by);
               rule_id = created < 0 ? 0u : (uint32_t)created;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_collab_rule_propose_reply_encode(rule_id, response_body,
                                                           response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char directive_type[AIMEE_DB2_RULES_DELETE_BY_DIRECTIVE_TYPE_DIRECTIVE_TYPE_MAX + 1] = "";
         if (aimee_db2_rules_delete_by_directive_type_request_decode(
                 request_body, request_len, directive_type, sizeof(directive_type)) == 0)
         {
            if (response_capacity < AIMEE_DB2_RULES_DELETE_BY_DIRECTIVE_TYPE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->rules_delete_by_directive_type)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t deleted = 0u;
            {
               int removed = backend->rules_delete_by_directive_type(directive_type);
               deleted = removed < 0 ? 0u : (uint32_t)removed;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_rules_delete_by_directive_type_reply_encode(
                    deleted, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char artifact_id[AIMEE_DB2_ARTIFACT_FLAG_REVIEW_ARTIFACT_ID_MAX + 1] = "";
         char flag_reason[AIMEE_DB2_ARTIFACT_FLAG_REVIEW_FLAG_REASON_MAX + 1] = "";
         if (aimee_db2_artifact_flag_review_request_decode(request_body, request_len, artifact_id,
                                                           sizeof(artifact_id), flag_reason,
                                                           sizeof(flag_reason)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ARTIFACT_FLAG_REVIEW_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->artifact_flag_review)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->artifact_flag_review(artifact_id, flag_reason) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_artifact_flag_review_reply_encode(acknowledged, response_body,
                                                            response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char verdict_tag[AIMEE_DB2_VERDICT_SUPPRESSED_VERDICT_TAG_MAX + 1] = "";
         char verdict_scope[AIMEE_DB2_VERDICT_SUPPRESSED_VERDICT_SCOPE_MAX + 1] = "";
         if (aimee_db2_verdict_suppressed_request_decode(request_body, request_len, verdict_tag,
                                                         sizeof(verdict_tag), verdict_scope,
                                                         sizeof(verdict_scope)) == 0)
         {
            if (response_capacity < AIMEE_DB2_VERDICT_SUPPRESSED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->verdict_suppressed)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t suppressed = 0u;
            suppressed = backend->verdict_suppressed(verdict_tag, verdict_scope) > 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_verdict_suppressed_reply_encode(suppressed, response_body,
                                                          response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_CURATOR_INVALIDATE_DOC_PROJECT_MAX + 1] = "";
         char file_path[AIMEE_DB2_CURATOR_INVALIDATE_DOC_FILE_PATH_MAX + 1] = "";
         if (aimee_db2_curator_invalidate_doc_request_decode(request_body, request_len, project,
                                                             sizeof(project), file_path,
                                                             sizeof(file_path)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CURATOR_INVALIDATE_DOC_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->curator_invalidate_doc)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t invalidated = 0u;
            {
               int staled = backend->curator_invalidate_doc(project, file_path);
               invalidated = staled < 0 ? 0u : (uint32_t)staled;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_curator_invalidate_doc_reply_encode(invalidated, response_body,
                                                              response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {

         if (aimee_db2_bandit_decision_points_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_DECISION_POINTS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_decision_points)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char decision_points[AIMEE_DB2_BANDIT_DECISION_POINTS_DECISION_POINTS_MAX + 1] = "";
            (void)backend->bandit_decision_points(decision_points, sizeof(decision_points));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_bandit_decision_points_reply_encode(decision_points, response_body,
                                                              response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char bandit_decision_id[AIMEE_DB2_BANDIT_DECISION_CLOSE_BANDIT_DECISION_ID_MAX + 1] = "";
         double reward = 0.0;
         if (aimee_db2_bandit_decision_close_request_decode(
                 request_body, request_len, bandit_decision_id, sizeof(bandit_decision_id),
                 &reward) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_DECISION_CLOSE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_decision_close)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->bandit_decision_close(bandit_decision_id, reward) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_bandit_decision_close_reply_encode(acknowledged, response_body,
                                                             response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         if (aimee_db2_rules_list_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_RULES_LIST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->rules_list)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_rules_list_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_RULES_LIST_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               rule_t *found = malloc(sizeof(*found) * AIMEE_DB2_RULES_LIST_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->rules_list(found, AIMEE_DB2_RULES_LIST_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].rule_id = found[index].id < 0 ? 0u : (uint32_t)found[index].id;
                  rows[index].rule_weight =
                      found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
                  snprintf(rows[index].polarity, sizeof(rows[index].polarity), "%s",
                           found[index].polarity);
                  snprintf(rows[index].rule_title, sizeof(rows[index].rule_title), "%s",
                           found[index].title);
                  snprintf(rows[index].rule_description, sizeof(rows[index].rule_description), "%s",
                           found[index].description);
                  snprintf(rows[index].domain, sizeof(rows[index].domain), "%s",
                           found[index].domain);
                  snprintf(rows[index].rule_created_at, sizeof(rows[index].rule_created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].rule_updated_at, sizeof(rows[index].rule_updated_at), "%s",
                           found[index].updated_at);
                  snprintf(rows[index].rule_directive_type, sizeof(rows[index].rule_directive_type),
                           "%s", found[index].directive_type);
                  snprintf(rows[index].expires_at, sizeof(rows[index].expires_at), "%s",
                           found[index].expires_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_rules_list_reply_encode(rows, count, response_body, response_capacity,
                                                  response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t min_weight = 0u;
         if (aimee_db2_rules_list_by_tier_request_decode(request_body, request_len, &min_weight) ==
             0)
         {
            if (response_capacity < AIMEE_DB2_RULES_LIST_BY_TIER_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->rules_list_by_tier)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_rules_list_by_tier_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_RULES_LIST_BY_TIER_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               rule_t *found = malloc(sizeof(*found) * AIMEE_DB2_RULES_LIST_BY_TIER_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->rules_list_by_tier((int)min_weight, found,
                                                         AIMEE_DB2_RULES_LIST_BY_TIER_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].rule_id = found[index].id < 0 ? 0u : (uint32_t)found[index].id;
                  rows[index].rule_weight =
                      found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
                  snprintf(rows[index].polarity, sizeof(rows[index].polarity), "%s",
                           found[index].polarity);
                  snprintf(rows[index].rule_title, sizeof(rows[index].rule_title), "%s",
                           found[index].title);
                  snprintf(rows[index].rule_description, sizeof(rows[index].rule_description), "%s",
                           found[index].description);
                  snprintf(rows[index].domain, sizeof(rows[index].domain), "%s",
                           found[index].domain);
                  snprintf(rows[index].rule_created_at, sizeof(rows[index].rule_created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].rule_updated_at, sizeof(rows[index].rule_updated_at), "%s",
                           found[index].updated_at);
                  snprintf(rows[index].rule_directive_type, sizeof(rows[index].rule_directive_type),
                           "%s", found[index].directive_type);
                  snprintf(rows[index].expires_at, sizeof(rows[index].expires_at), "%s",
                           found[index].expires_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_rules_list_by_tier_reply_encode(rows, count, response_body,
                                                          response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         if (aimee_db2_rules_list_hard_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_RULES_LIST_HARD_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->rules_list_hard)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_rules_list_hard_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_RULES_LIST_HARD_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               rule_t *found = malloc(sizeof(*found) * AIMEE_DB2_RULES_LIST_HARD_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->rules_list_hard(found, AIMEE_DB2_RULES_LIST_HARD_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].rule_id = found[index].id < 0 ? 0u : (uint32_t)found[index].id;
                  rows[index].rule_weight =
                      found[index].weight < 0 ? 0u : (uint32_t)found[index].weight;
                  snprintf(rows[index].polarity, sizeof(rows[index].polarity), "%s",
                           found[index].polarity);
                  snprintf(rows[index].rule_title, sizeof(rows[index].rule_title), "%s",
                           found[index].title);
                  snprintf(rows[index].rule_description, sizeof(rows[index].rule_description), "%s",
                           found[index].description);
                  snprintf(rows[index].domain, sizeof(rows[index].domain), "%s",
                           found[index].domain);
                  snprintf(rows[index].rule_created_at, sizeof(rows[index].rule_created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].rule_updated_at, sizeof(rows[index].rule_updated_at), "%s",
                           found[index].updated_at);
                  snprintf(rows[index].rule_directive_type, sizeof(rows[index].rule_directive_type),
                           "%s", found[index].directive_type);
                  snprintf(rows[index].expires_at, sizeof(rows[index].expires_at), "%s",
                           found[index].expires_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_rules_list_hard_reply_encode(rows, count, response_body,
                                                       response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         if (aimee_db2_anti_pattern_list_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_ANTI_PATTERN_LIST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->anti_pattern_list)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_anti_pattern_list_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ANTI_PATTERN_LIST_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               anti_pattern_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ANTI_PATTERN_LIST_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written =
                   backend->anti_pattern_list(found, AIMEE_DB2_ANTI_PATTERN_LIST_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].anti_pattern_id =
                      found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].hit_count =
                      found[index].hit_count < 0 ? 0u : (uint32_t)found[index].hit_count;
                  rows[index].confidence = found[index].confidence;
                  snprintf(rows[index].pattern, sizeof(rows[index].pattern), "%s",
                           found[index].pattern);
                  snprintf(rows[index].pattern_description, sizeof(rows[index].pattern_description),
                           "%s", found[index].description);
                  snprintf(rows[index].pattern_source, sizeof(rows[index].pattern_source), "%s",
                           found[index].source);
                  snprintf(rows[index].source_ref, sizeof(rows[index].source_ref), "%s",
                           found[index].source_ref);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_anti_pattern_list_reply_encode(rows, count, response_body,
                                                         response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t hit_threshold = 0u;
         if (aimee_db2_anti_pattern_list_hot_request_decode(request_body, request_len,
                                                            &hit_threshold) == 0)
         {
            if (response_capacity < AIMEE_DB2_ANTI_PATTERN_LIST_HOT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->anti_pattern_list_hot)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_anti_pattern_list_hot_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ANTI_PATTERN_LIST_HOT_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               anti_pattern_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ANTI_PATTERN_LIST_HOT_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->anti_pattern_list_hot(
                   (int)hit_threshold, found, AIMEE_DB2_ANTI_PATTERN_LIST_HOT_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].anti_pattern_id =
                      found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].hit_count =
                      found[index].hit_count < 0 ? 0u : (uint32_t)found[index].hit_count;
                  rows[index].confidence = found[index].confidence;
                  snprintf(rows[index].pattern, sizeof(rows[index].pattern), "%s",
                           found[index].pattern);
                  snprintf(rows[index].pattern_description, sizeof(rows[index].pattern_description),
                           "%s", found[index].description);
                  snprintf(rows[index].pattern_source, sizeof(rows[index].pattern_source), "%s",
                           found[index].source);
                  snprintf(rows[index].source_ref, sizeof(rows[index].source_ref), "%s",
                           found[index].source_ref);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_anti_pattern_list_hot_reply_encode(rows, count, response_body,
                                                             response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char file_path[AIMEE_DB2_ANTI_PATTERN_CHECK_FILE_PATH_MAX + 1] = "";
         char command[AIMEE_DB2_ANTI_PATTERN_CHECK_COMMAND_MAX + 1] = "";
         if (aimee_db2_anti_pattern_check_request_decode(request_body, request_len, file_path,
                                                         sizeof(file_path), command,
                                                         sizeof(command)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ANTI_PATTERN_CHECK_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->anti_pattern_check)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_anti_pattern_check_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_ANTI_PATTERN_CHECK_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               anti_pattern_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_ANTI_PATTERN_CHECK_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->anti_pattern_check(file_path, command, found,
                                                         AIMEE_DB2_ANTI_PATTERN_CHECK_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].anti_pattern_id =
                      found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].hit_count =
                      found[index].hit_count < 0 ? 0u : (uint32_t)found[index].hit_count;
                  rows[index].confidence = found[index].confidence;
                  snprintf(rows[index].pattern, sizeof(rows[index].pattern), "%s",
                           found[index].pattern);
                  snprintf(rows[index].pattern_description, sizeof(rows[index].pattern_description),
                           "%s", found[index].description);
                  snprintf(rows[index].pattern_source, sizeof(rows[index].pattern_source), "%s",
                           found[index].source);
                  snprintf(rows[index].source_ref, sizeof(rows[index].source_ref), "%s",
                           found[index].source_ref);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_anti_pattern_check_reply_encode(rows, count, response_body,
                                                          response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char bandit_decision_id[AIMEE_DB2_BANDIT_DECISION_INSERT_BANDIT_DECISION_ID_MAX + 1] = "";
         char decision_point[AIMEE_DB2_BANDIT_DECISION_INSERT_DECISION_POINT_MAX + 1] = "";
         char arm_id[AIMEE_DB2_BANDIT_DECISION_INSERT_ARM_ID_MAX + 1] = "";
         char context_hash[AIMEE_DB2_BANDIT_DECISION_INSERT_CONTEXT_HASH_MAX + 1] = "";
         double propensity = 0.0;
         uint32_t is_exploration = 0u;
         if (aimee_db2_bandit_decision_insert_request_decode(
                 request_body, request_len, bandit_decision_id, sizeof(bandit_decision_id),
                 decision_point, sizeof(decision_point), arm_id, sizeof(arm_id), context_hash,
                 sizeof(context_hash), &propensity, &is_exploration) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_DECISION_INSERT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_decision_insert)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->bandit_decision_insert(bandit_decision_id, decision_point, arm_id,
                                                context_hash, propensity, (int)is_exploration) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_bandit_decision_insert_reply_encode(acknowledged, response_body,
                                                              response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char artifact_id[AIMEE_DB2_ARTIFACT_WRITE_ARTIFACT_ID_MAX + 1] = "";
         char artifact_kind[AIMEE_DB2_ARTIFACT_WRITE_ARTIFACT_KIND_MAX + 1] = "";
         char artifact_state[AIMEE_DB2_ARTIFACT_WRITE_ARTIFACT_STATE_MAX + 1] = "";
         char scope_kind[AIMEE_DB2_ARTIFACT_WRITE_SCOPE_KIND_MAX + 1] = "";
         char scope_id[AIMEE_DB2_ARTIFACT_WRITE_SCOPE_ID_MAX + 1] = "";
         char operator_id[AIMEE_DB2_ARTIFACT_WRITE_OPERATOR_ID_MAX + 1] = "";
         double artifact_confidence = 0.0;
         char payload_json[AIMEE_DB2_ARTIFACT_WRITE_PAYLOAD_JSON_MAX + 1] = "";
         if (aimee_db2_artifact_write_request_decode(
                 request_body, request_len, artifact_id, sizeof(artifact_id), artifact_kind,
                 sizeof(artifact_kind), artifact_state, sizeof(artifact_state), scope_kind,
                 sizeof(scope_kind), scope_id, sizeof(scope_id), operator_id, sizeof(operator_id),
                 &artifact_confidence, payload_json, sizeof(payload_json)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ARTIFACT_WRITE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->artifact_write)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->artifact_write(artifact_id, artifact_kind, artifact_state,
                                                   scope_kind, scope_id, operator_id,
                                                   artifact_confidence, payload_json) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_artifact_write_reply_encode(acknowledged, response_body,
                                                      response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char artifact_id[AIMEE_DB2_ARTIFACT_WRITE_EX_ARTIFACT_ID_MAX + 1] = "";
         char artifact_kind[AIMEE_DB2_ARTIFACT_WRITE_EX_ARTIFACT_KIND_MAX + 1] = "";
         char artifact_state[AIMEE_DB2_ARTIFACT_WRITE_EX_ARTIFACT_STATE_MAX + 1] = "";
         char scope_kind[AIMEE_DB2_ARTIFACT_WRITE_EX_SCOPE_KIND_MAX + 1] = "";
         char scope_id[AIMEE_DB2_ARTIFACT_WRITE_EX_SCOPE_ID_MAX + 1] = "";
         char operator_id[AIMEE_DB2_ARTIFACT_WRITE_EX_OPERATOR_ID_MAX + 1] = "";
         double artifact_confidence = 0.0;
         uint32_t attempt_count = 0u;
         char payload_json[AIMEE_DB2_ARTIFACT_WRITE_EX_PAYLOAD_JSON_MAX + 1] = "";
         if (aimee_db2_artifact_write_ex_request_decode(
                 request_body, request_len, artifact_id, sizeof(artifact_id), artifact_kind,
                 sizeof(artifact_kind), artifact_state, sizeof(artifact_state), scope_kind,
                 sizeof(scope_kind), scope_id, sizeof(scope_id), operator_id, sizeof(operator_id),
                 &artifact_confidence, &attempt_count, payload_json, sizeof(payload_json)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ARTIFACT_WRITE_EX_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->artifact_write_ex)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->artifact_write_ex(artifact_id, artifact_kind, artifact_state, scope_kind,
                                           scope_id, operator_id, artifact_confidence,
                                           (int)attempt_count, payload_json) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_artifact_write_ex_reply_encode(acknowledged, response_body,
                                                         response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char artifact_id[AIMEE_DB2_ARTIFACT_TARGET_SURFACE_ARTIFACT_ID_MAX + 1] = "";
         if (aimee_db2_artifact_target_surface_request_decode(
                 request_body, request_len, artifact_id, sizeof(artifact_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ARTIFACT_TARGET_SURFACE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->artifact_target_surface)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char target_surface[AIMEE_DB2_ARTIFACT_TARGET_SURFACE_TARGET_SURFACE_MAX + 1] = "";
            (void)backend->artifact_target_surface(artifact_id, target_surface,
                                                   (int)sizeof(target_surface));
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_artifact_target_surface_reply_encode(
                    target_surface, response_body, response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char agent_name[AIMEE_DB2_AGENT_OUTCOME_RECORD_AGENT_NAME_MAX + 1] = "";
         char agent_role[AIMEE_DB2_AGENT_OUTCOME_RECORD_AGENT_ROLE_MAX + 1] = "";
         char outcome_kind[AIMEE_DB2_AGENT_OUTCOME_RECORD_OUTCOME_KIND_MAX + 1] = "";
         char outcome_reason[AIMEE_DB2_AGENT_OUTCOME_RECORD_OUTCOME_REASON_MAX + 1] = "";
         uint32_t turns_used = 0u;
         uint32_t tools_called = 0u;
         uint64_t tokens_used = 0u;
         char tool_error_pattern[AIMEE_DB2_AGENT_OUTCOME_RECORD_TOOL_ERROR_PATTERN_MAX + 1] = "";
         if (aimee_db2_agent_outcome_record_request_decode(
                 request_body, request_len, agent_name, sizeof(agent_name), agent_role,
                 sizeof(agent_role), outcome_kind, sizeof(outcome_kind), outcome_reason,
                 sizeof(outcome_reason), &turns_used, &tools_called, &tokens_used,
                 tool_error_pattern, sizeof(tool_error_pattern)) == 0)
         {
            if (response_capacity < AIMEE_DB2_AGENT_OUTCOME_RECORD_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->agent_outcome_record)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->agent_outcome_record(agent_name, agent_role, outcome_kind, outcome_reason,
                                              (int)turns_used, (int)tools_called,
                                              (int64_t)tokens_used, tool_error_pattern) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_agent_outcome_record_reply_encode(acknowledged, response_body,
                                                            response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char artifact_id[AIMEE_DB2_ARTIFACT_REJECT_ARTIFACT_ID_MAX + 1] = "";
         char verdict_tag[AIMEE_DB2_ARTIFACT_REJECT_VERDICT_TAG_MAX + 1] = "";
         char verdict_scope[AIMEE_DB2_ARTIFACT_REJECT_VERDICT_SCOPE_MAX + 1] = "";
         char counter_example[AIMEE_DB2_ARTIFACT_REJECT_COUNTER_EXAMPLE_MAX + 1] = "";
         char before_json[AIMEE_DB2_ARTIFACT_REJECT_BEFORE_JSON_MAX + 1] = "";
         if (aimee_db2_artifact_reject_request_decode(
                 request_body, request_len, artifact_id, sizeof(artifact_id), verdict_tag,
                 sizeof(verdict_tag), verdict_scope, sizeof(verdict_scope), counter_example,
                 sizeof(counter_example), before_json, sizeof(before_json)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ARTIFACT_REJECT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->artifact_reject)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->artifact_reject(artifact_id, verdict_tag, verdict_scope,
                                                    counter_example, before_json) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_artifact_reject_reply_encode(acknowledged, response_body,
                                                       response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char audit_id[AIMEE_DB2_AUDIT_EVENT_WRITE_AUDIT_ID_MAX + 1] = "";
         char source_artifact_id[AIMEE_DB2_AUDIT_EVENT_WRITE_SOURCE_ARTIFACT_ID_MAX + 1] = "";
         char audit_target_surface[AIMEE_DB2_AUDIT_EVENT_WRITE_AUDIT_TARGET_SURFACE_MAX + 1] = "";
         char audit_target_id[AIMEE_DB2_AUDIT_EVENT_WRITE_AUDIT_TARGET_ID_MAX + 1] = "";
         char audit_operator_id[AIMEE_DB2_AUDIT_EVENT_WRITE_AUDIT_OPERATOR_ID_MAX + 1] = "";
         char audit_scope_kind[AIMEE_DB2_AUDIT_EVENT_WRITE_AUDIT_SCOPE_KIND_MAX + 1] = "";
         char audit_scope_id[AIMEE_DB2_AUDIT_EVENT_WRITE_AUDIT_SCOPE_ID_MAX + 1] = "";
         double applied_confidence = 0.0;
         uint32_t flagged_for_review = 0u;
         char before_snapshot[AIMEE_DB2_AUDIT_EVENT_WRITE_BEFORE_SNAPSHOT_MAX + 1] = "";
         char after_snapshot[AIMEE_DB2_AUDIT_EVENT_WRITE_AFTER_SNAPSHOT_MAX + 1] = "";
         if (aimee_db2_audit_event_write_request_decode(
                 request_body, request_len, audit_id, sizeof(audit_id), source_artifact_id,
                 sizeof(source_artifact_id), audit_target_surface, sizeof(audit_target_surface),
                 audit_target_id, sizeof(audit_target_id), audit_operator_id,
                 sizeof(audit_operator_id), audit_scope_kind, sizeof(audit_scope_kind),
                 audit_scope_id, sizeof(audit_scope_id), &applied_confidence, &flagged_for_review,
                 before_snapshot, sizeof(before_snapshot), after_snapshot,
                 sizeof(after_snapshot)) == 0)
         {
            if (response_capacity < AIMEE_DB2_AUDIT_EVENT_WRITE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->audit_event_write)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->audit_event_write(
                    audit_id, source_artifact_id, audit_target_surface, audit_target_id,
                    audit_operator_id, audit_scope_kind, audit_scope_id, applied_confidence,
                    (int)flagged_for_review, before_snapshot, after_snapshot) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_audit_event_write_reply_encode(acknowledged, response_body,
                                                         response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char artifact_id[AIMEE_DB2_AUDIT_LATEST_BEFORE_ARTIFACT_ID_MAX + 1] = "";
         if (aimee_db2_audit_latest_before_request_decode(request_body, request_len, artifact_id,
                                                          sizeof(artifact_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_AUDIT_LATEST_BEFORE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->audit_latest_before)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char before_snapshot[AIMEE_DB2_AUDIT_LATEST_BEFORE_BEFORE_SNAPSHOT_MAX + 1] = "";
            (void)backend->audit_latest_before(artifact_id, before_snapshot,
                                               (int)sizeof(before_snapshot));
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_audit_latest_before_reply_encode(before_snapshot, response_body,
                                                           response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char decision_point[AIMEE_DB2_BANDIT_ARM_STATS_UPDATE_DECISION_POINT_MAX + 1] = "";
         char arm_id[AIMEE_DB2_BANDIT_ARM_STATS_UPDATE_ARM_ID_MAX + 1] = "";
         double reward_delta = 0.0;
         double posterior_alpha = 0.0;
         double posterior_beta = 0.0;
         if (aimee_db2_bandit_arm_stats_update_request_decode(
                 request_body, request_len, decision_point, sizeof(decision_point), arm_id,
                 sizeof(arm_id), &reward_delta, &posterior_alpha, &posterior_beta) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_ARM_STATS_UPDATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_arm_stats_update)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->bandit_arm_stats_update(decision_point, arm_id, reward_delta,
                                                            posterior_alpha, posterior_beta) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_bandit_arm_stats_update_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char memory_class[AIMEE_DB2_DEMOTION_PROFILE_READ_MEMORY_CLASS_MAX + 1] = "";
         char profile_scope_kind[AIMEE_DB2_DEMOTION_PROFILE_READ_PROFILE_SCOPE_KIND_MAX + 1] = "";
         char profile_scope_id[AIMEE_DB2_DEMOTION_PROFILE_READ_PROFILE_SCOPE_ID_MAX + 1] = "";
         if (aimee_db2_demotion_profile_read_request_decode(
                 request_body, request_len, memory_class, sizeof(memory_class), profile_scope_kind,
                 sizeof(profile_scope_kind), profile_scope_id, sizeof(profile_scope_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DEMOTION_PROFILE_READ_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->demotion_profile_read)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char profile_json[AIMEE_DB2_DEMOTION_PROFILE_READ_PROFILE_JSON_MAX + 1] = "";
            (void)backend->demotion_profile_read(memory_class, profile_scope_kind, profile_scope_id,
                                                 profile_json, sizeof(profile_json));
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_demotion_profile_read_reply_encode(profile_json, response_body,
                                                             response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char memory_class[AIMEE_DB2_DEMOTION_PROFILE_WRITE_MEMORY_CLASS_MAX + 1] = "";
         char profile_scope_kind[AIMEE_DB2_DEMOTION_PROFILE_WRITE_PROFILE_SCOPE_KIND_MAX + 1] = "";
         char profile_scope_id[AIMEE_DB2_DEMOTION_PROFILE_WRITE_PROFILE_SCOPE_ID_MAX + 1] = "";
         char payload_json[AIMEE_DB2_DEMOTION_PROFILE_WRITE_PAYLOAD_JSON_MAX + 1] = "";
         if (aimee_db2_demotion_profile_write_request_decode(
                 request_body, request_len, memory_class, sizeof(memory_class), profile_scope_kind,
                 sizeof(profile_scope_kind), profile_scope_id, sizeof(profile_scope_id),
                 payload_json, sizeof(payload_json)) == 0)
         {
            if (response_capacity < AIMEE_DB2_DEMOTION_PROFILE_WRITE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->demotion_profile_write)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char profile_id[AIMEE_DB2_DEMOTION_PROFILE_WRITE_PROFILE_ID_MAX + 1] = "";
            (void)backend->demotion_profile_write(memory_class, profile_scope_kind,
                                                  profile_scope_id, payload_json, profile_id,
                                                  (int)sizeof(profile_id));
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_demotion_profile_write_reply_encode(profile_id, response_body,
                                                              response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char retrieval_event_id[AIMEE_DB2_RETRIEVAL_ATTRIBUTION_WRITE_RETRIEVAL_EVENT_ID_MAX + 1] =
             "";
         uint64_t surfaced_row_id = 0u;
         char attribution_verdict[AIMEE_DB2_RETRIEVAL_ATTRIBUTION_WRITE_ATTRIBUTION_VERDICT_MAX +
                                  1] = "";
         double attribution_weight = 0.0;
         if (aimee_db2_retrieval_attribution_write_request_decode(
                 request_body, request_len, retrieval_event_id, sizeof(retrieval_event_id),
                 &surfaced_row_id, attribution_verdict, sizeof(attribution_verdict),
                 &attribution_weight) == 0)
         {
            if (response_capacity < AIMEE_DB2_RETRIEVAL_ATTRIBUTION_WRITE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->retrieval_attribution_write)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->retrieval_attribution_write(retrieval_event_id, (int64_t)surfaced_row_id,
                                                     attribution_verdict, attribution_weight) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_retrieval_attribution_write_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char turn_id[AIMEE_DB2_RETRIEVAL_EVENT_BY_TURN_TURN_ID_MAX + 1] = "";
         if (aimee_db2_retrieval_event_by_turn_request_decode(request_body, request_len, turn_id,
                                                              sizeof(turn_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_RETRIEVAL_EVENT_BY_TURN_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->retrieval_event_by_turn)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char retrieval_event_id[AIMEE_DB2_RETRIEVAL_EVENT_BY_TURN_RETRIEVAL_EVENT_ID_MAX + 1] =
                "";
            char retrieval_payload[AIMEE_DB2_RETRIEVAL_EVENT_BY_TURN_RETRIEVAL_PAYLOAD_MAX + 1] =
                "";
            (void)backend->retrieval_event_by_turn(
                turn_id, retrieval_event_id, (int)sizeof(retrieval_event_id), retrieval_payload,
                (int)sizeof(retrieval_payload));
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_retrieval_event_by_turn_reply_encode(
                    retrieval_event_id, retrieval_payload, response_body, response_capacity,
                    response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char subject_id[AIMEE_DB2_FEATURE_ROW_UPSERT_SUBJECT_ID_MAX + 1] = "";
         char feature_subject_kind[AIMEE_DB2_FEATURE_ROW_UPSERT_FEATURE_SUBJECT_KIND_MAX + 1] = "";
         char feature_scope_kind[AIMEE_DB2_FEATURE_ROW_UPSERT_FEATURE_SCOPE_KIND_MAX + 1] = "";
         char feature_scope_id[AIMEE_DB2_FEATURE_ROW_UPSERT_FEATURE_SCOPE_ID_MAX + 1] = "";
         char feature_set_version[AIMEE_DB2_FEATURE_ROW_UPSERT_FEATURE_SET_VERSION_MAX + 1] = "";
         char features_json[AIMEE_DB2_FEATURE_ROW_UPSERT_FEATURES_JSON_MAX + 1] = "";
         char computed_at[AIMEE_DB2_FEATURE_ROW_UPSERT_COMPUTED_AT_MAX + 1] = "";
         if (aimee_db2_feature_row_upsert_request_decode(
                 request_body, request_len, subject_id, sizeof(subject_id), feature_subject_kind,
                 sizeof(feature_subject_kind), feature_scope_kind, sizeof(feature_scope_kind),
                 feature_scope_id, sizeof(feature_scope_id), feature_set_version,
                 sizeof(feature_set_version), features_json, sizeof(features_json), computed_at,
                 sizeof(computed_at)) == 0)
         {
            if (response_capacity < AIMEE_DB2_FEATURE_ROW_UPSERT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->feature_row_upsert)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->feature_row_upsert(subject_id, feature_subject_kind, feature_scope_kind,
                                            feature_scope_id, feature_set_version, features_json,
                                            computed_at) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_feature_row_upsert_reply_encode(acknowledged, response_body,
                                                          response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char subject_id[AIMEE_DB2_FEATURE_ROW_READ_SUBJECT_ID_MAX + 1] = "";
         char feature_subject_kind[AIMEE_DB2_FEATURE_ROW_READ_FEATURE_SUBJECT_KIND_MAX + 1] = "";
         char feature_set_version[AIMEE_DB2_FEATURE_ROW_READ_FEATURE_SET_VERSION_MAX + 1] = "";
         if (aimee_db2_feature_row_read_request_decode(
                 request_body, request_len, subject_id, sizeof(subject_id), feature_subject_kind,
                 sizeof(feature_subject_kind), feature_set_version,
                 sizeof(feature_set_version)) == 0)
         {
            if (response_capacity < AIMEE_DB2_FEATURE_ROW_READ_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->feature_row_read)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char features_json[AIMEE_DB2_FEATURE_ROW_READ_FEATURES_JSON_MAX + 1] = "";
            (void)backend->feature_row_read(subject_id, feature_subject_kind, feature_set_version,
                                            features_json, sizeof(features_json));
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_feature_row_read_reply_encode(features_json, response_body,
                                                        response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char decision_point[AIMEE_DB2_BANDIT_EXPLORE_STATS_DECISION_POINT_MAX + 1] = "";
         uint32_t window_seconds = 0u;
         if (aimee_db2_bandit_explore_stats_request_decode(request_body, request_len,
                                                           decision_point, sizeof(decision_point),
                                                           &window_seconds) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_EXPLORE_STATS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_explore_stats)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t n_explore = 0u;
            uint64_t n_total = 0u;
            {
               long long explored = 0;
               long long total = 0;
               if (backend->bandit_explore_stats(decision_point, (int)window_seconds, &explored,
                                                 &total) == 0)
               {
                  n_explore = explored > 0 ? (uint64_t)explored : 0u;
                  n_total = total > 0 ? (uint64_t)total : 0u;
               }
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_bandit_explore_stats_reply_encode(n_explore, n_total, response_body,
                                                            response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char decision_point[AIMEE_DB2_BANDIT_ARM_STATS_READ_DECISION_POINT_MAX + 1] = "";
         char arm_id[AIMEE_DB2_BANDIT_ARM_STATS_READ_ARM_ID_MAX + 1] = "";
         if (aimee_db2_bandit_arm_stats_read_request_decode(request_body, request_len,
                                                            decision_point, sizeof(decision_point),
                                                            arm_id, sizeof(arm_id)) == 0)
         {
            if (response_capacity < AIMEE_DB2_BANDIT_ARM_STATS_READ_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->bandit_arm_stats_read)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t arm_n_decisions = 0u;
            uint64_t arm_n_rewards = 0u;
            double sum_reward = 0.0;
            double posterior_alpha = 0.0;
            double posterior_beta = 0.0;
            {
               db2_bandit_arm_stats_t stats;
               memset(&stats, 0, sizeof(stats));
               if (backend->bandit_arm_stats_read(decision_point, arm_id, &stats) == 0)
               {
                  arm_n_decisions = stats.n_decisions > 0 ? (uint64_t)stats.n_decisions : 0u;
                  arm_n_rewards = stats.n_rewards > 0 ? (uint64_t)stats.n_rewards : 0u;
                  sum_reward = stats.sum_reward;
                  posterior_alpha = stats.posterior_alpha;
                  posterior_beta = stats.posterior_beta;
               }
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_bandit_arm_stats_read_reply_encode(
                    arm_n_decisions, arm_n_rewards, sum_reward, posterior_alpha, posterior_beta,
                    response_body, response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
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
      {
         /* Every db2-envelope-string-u32-v1 operation this family owns. */
         const db2_string_count_binding_t bindings[] = {
             {aimee_db2_async_pending_count_request_decode,
              aimee_db2_async_pending_count_reply_encode,
              backend ? backend->async_pending_count : NULL, AIMEE_DB2_ASYNC_PENDING_COUNT_MAX},
             {aimee_db2_mining_job_try_lock_request_decode,
              aimee_db2_mining_job_try_lock_reply_encode,
              backend ? backend->mining_job_try_lock : NULL, AIMEE_DB2_MINING_JOB_TRY_LOCK_MAX},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_count(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         /* Every db2-envelope-string-ack-v1 operation this family owns. */
         const db2_string_ack_binding_t bindings[] = {
             {aimee_db2_runtime_state_touch_request_decode,
              aimee_db2_runtime_state_touch_reply_encode,
              backend ? backend->runtime_state_touch : NULL},
             {aimee_db2_synth_enqueue_request_decode, aimee_db2_synth_enqueue_reply_encode,
              backend ? backend->synth_enqueue : NULL},
             {aimee_db2_synth_mark_done_request_decode, aimee_db2_synth_mark_done_reply_encode,
              backend ? backend->synth_mark_done : NULL},
             {aimee_db2_reembed_mark_finished_request_decode,
              aimee_db2_reembed_mark_finished_reply_encode,
              backend ? backend->reembed_mark_finished : NULL},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_ack(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         /* Every db2-envelope-string-pair-ack-v1 operation this family owns. */
         const db2_string_pair_ack_binding_t bindings[] = {
             {aimee_db2_synth_mark_failed_request_decode, aimee_db2_synth_mark_failed_reply_encode,
              backend ? backend->synth_mark_failed : NULL},
             {aimee_db2_runtime_state_set_request_decode, aimee_db2_runtime_state_set_reply_encode,
              backend ? backend->runtime_state_set : NULL},
             {aimee_db2_set_active_embedder_version_request_decode,
              aimee_db2_set_active_embedder_version_reply_encode,
              backend ? backend->set_active_embedder_version : NULL},
         };
         int handled = 0;
         aimee_module_status_t status = db2_dispatch_string_pair_ack(
             bindings, sizeof(bindings) / sizeof(bindings[0]), request_body, request_len,
             response_body, response_capacity, response_len, invocation, &handled);
         if (handled)
            return status;
      }
      {
         char state_key[AIMEE_DB2_RUNTIME_STATE_GET_STATE_KEY_MAX + 1];
         if (aimee_db2_runtime_state_get_request_decode(request_body, request_len, state_key,
                                                        sizeof(state_key)) == 0)
         {
            if (response_capacity < AIMEE_DB2_RUNTIME_STATE_GET_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->runtime_state_get)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char state_value[AIMEE_DB2_RUNTIME_STATE_GET_STATE_VALUE_MAX + 1] = "";
            /* A key that is not set reads back empty rather than failing, which
             * is what the operation promises: it has one result. */
            backend->runtime_state_get(state_key, state_value, sizeof(state_value));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_runtime_state_get_reply_encode(state_value, response_body,
                                                         response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t ingest_job_id = 0u;
         char error_message[AIMEE_DB2_INGEST_QUEUE_FAIL_ERROR_MESSAGE_MAX + 1] = "";
         if (aimee_db2_ingest_queue_fail_request_decode(request_body, request_len, &ingest_job_id,
                                                        error_message, sizeof(error_message)) == 0)
         {
            if (response_capacity < AIMEE_DB2_INGEST_QUEUE_FAIL_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->ingest_queue_fail)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->ingest_queue_fail((int64_t)ingest_job_id, error_message) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_ingest_queue_fail_reply_encode(acknowledged, response_body,
                                                         response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t max_attempts = 0u;
         if (aimee_db2_reset_stuck_vector_ops_request_decode(request_body, request_len,
                                                             &max_attempts) == 0)
         {
            if (response_capacity < AIMEE_DB2_RESET_STUCK_VECTOR_OPS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->reset_stuck_vector_ops)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t reset_rows = 0u;
            {
               int reset = backend->reset_stuck_vector_ops((int)max_attempts);
               reset_rows = reset < 0 ? 0u : (uint32_t)reset;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_reset_stuck_vector_ops_reply_encode(reset_rows, response_body,
                                                              response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t directive_id = 0u;
         uint64_t resolution_memory_id = 0u;
         if (aimee_db2_directive_resolve_request_decode(request_body, request_len, &directive_id,
                                                        &resolution_memory_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_DIRECTIVE_RESOLVE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->directive_resolve)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->directive_resolve((int64_t)directive_id,
                                                      (int64_t)resolution_memory_id) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_directive_resolve_reply_encode(acknowledged, response_body,
                                                         response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_CSS_MIGRATION_ENUMERATE_PROJECT_MAX + 1] = "";
         if (aimee_db2_css_migration_enumerate_request_decode(request_body, request_len, project,
                                                              sizeof(project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CSS_MIGRATION_ENUMERATE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->css_migration_enumerate)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t enumerated = 0u;
            {
               int seen = backend->css_migration_enumerate(project);
               enumerated = seen < 0 ? 0u : (uint32_t)seen;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_css_migration_enumerate_reply_encode(
                    enumerated, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_CSS_MIGRATION_ASSERT_CONVENTIONS_PROJECT_MAX + 1] = "";
         char now_iso[AIMEE_DB2_CSS_MIGRATION_ASSERT_CONVENTIONS_NOW_ISO_MAX + 1] = "";
         if (aimee_db2_css_migration_assert_conventions_request_decode(
                 request_body, request_len, project, sizeof(project), now_iso, sizeof(now_iso)) ==
             0)
         {
            if (response_capacity < AIMEE_DB2_CSS_MIGRATION_ASSERT_CONVENTIONS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->css_migration_assert_conventions)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t asserted = 0u;
            {
               int facts = backend->css_migration_assert_conventions(project, now_iso);
               asserted = facts < 0 ? 0u : (uint32_t)facts;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_css_migration_assert_conventions_reply_encode(
                    asserted, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char exemplar_project[AIMEE_DB2_CSS_MIGRATION_RULES_DOC_EXEMPLAR_PROJECT_MAX + 1] = "";
         if (aimee_db2_css_migration_rules_doc_request_decode(
                 request_body, request_len, exemplar_project, sizeof(exemplar_project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CSS_MIGRATION_RULES_DOC_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->css_migration_rules_doc)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char rules_doc[AIMEE_DB2_CSS_MIGRATION_RULES_DOC_RULES_DOC_MAX + 1] = "";
            (void)backend->css_migration_rules_doc(exemplar_project, rules_doc, sizeof(rules_doc));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_css_migration_rules_doc_reply_encode(
                    rules_doc, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t max_attempts = 0u;
         uint32_t limit = 0u;
         if (aimee_db2_retryable_index_failures_request_decode(request_body, request_len,
                                                               &max_attempts, &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_RETRYABLE_INDEX_FAILURES_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->retryable_index_failures)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_retryable_index_failures_row_t
                rows[AIMEE_DB2_RETRYABLE_INDEX_FAILURES_MAX_ROWS];
            uint32_t count = 0u;
            {
               int64_t ids[AIMEE_DB2_RETRYABLE_INDEX_FAILURES_MAX_ROWS];
               int found = backend->retryable_index_failures(
                   (int)max_attempts, (int)limit, ids, AIMEE_DB2_RETRYABLE_INDEX_FAILURES_MAX_ROWS);
               for (int index = 0; index < found; index++)
                  rows[index].memory_id = (uint64_t)ids[index];
               count = found < 0 ? 0u : (uint32_t)found;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_retryable_index_failures_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {

         if (aimee_db2_active_embedder_version_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_ACTIVE_EMBEDDER_VERSION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->active_embedder_version)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            char embedder_version[AIMEE_DB2_ACTIVE_EMBEDDER_VERSION_EMBEDDER_VERSION_MAX + 1] = "";
            (void)backend->active_embedder_version(embedder_version, sizeof(embedder_version));
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_active_embedder_version_reply_encode(
                    embedder_version, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {

         if (aimee_db2_corpus_pipeline_stage_counts_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_CORPUS_PIPELINE_STAGE_COUNTS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->corpus_pipeline_stage_counts)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_corpus_pipeline_stage_counts_row_t
                rows[AIMEE_DB2_CORPUS_PIPELINE_STAGE_COUNTS_MAX_ROWS];
            uint32_t count = 0u;
            {
               db2_corpus_pipeline_stage_count_t
                   found[AIMEE_DB2_CORPUS_PIPELINE_STAGE_COUNTS_MAX_ROWS];
               int written = backend->corpus_pipeline_stage_counts(
                   found, AIMEE_DB2_CORPUS_PIPELINE_STAGE_COUNTS_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  snprintf(rows[index].stage, sizeof(rows[index].stage), "%s", found[index].stage);
                  snprintf(rows[index].stage_status, sizeof(rows[index].stage_status), "%s",
                           found[index].stage_status);
                  rows[index].job_count =
                      found[index].count < 0 ? 0u : (uint32_t)found[index].count;
               }
               count = written < 0 ? 0u : (uint32_t)written;
            }
            if (aimee_module_invocation_cancelled(invocation))
               return AIMEE_MODULE_STATUS_CANCELLED;
            if (aimee_db2_corpus_pipeline_stage_counts_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
               return AIMEE_MODULE_STATUS_INTERNAL;
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char state_filter[AIMEE_DB2_DIRECTIVE_LIST_STATE_FILTER_MAX + 1] = "";
         char cause_filter[AIMEE_DB2_DIRECTIVE_LIST_CAUSE_FILTER_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_directive_list_request_decode(request_body, request_len, state_filter,
                                                     sizeof(state_filter), cause_filter,
                                                     sizeof(cause_filter), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_DIRECTIVE_LIST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->directive_list)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_directive_list_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_DIRECTIVE_LIST_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_directive_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_DIRECTIVE_LIST_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->directive_list(state_filter, cause_filter, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].directive_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].memory_a_id =
                      found[index].memory_a_id < 0 ? 0u : (uint64_t)found[index].memory_a_id;
                  rows[index].memory_b_id =
                      found[index].memory_b_id < 0 ? 0u : (uint64_t)found[index].memory_b_id;
                  rows[index].resolution_memory_id =
                      found[index].resolution_memory_id < 0
                          ? 0u
                          : (uint64_t)found[index].resolution_memory_id;
                  rows[index].priority =
                      found[index].priority < 0 ? 0u : (uint32_t)found[index].priority;
                  rows[index].surfaced_count =
                      found[index].surfaced_count < 0 ? 0u : (uint32_t)found[index].surfaced_count;
                  snprintf(rows[index].question, sizeof(rows[index].question), "%s",
                           found[index].question);
                  snprintf(rows[index].topic, sizeof(rows[index].topic), "%s", found[index].topic);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].cause, sizeof(rows[index].cause), "%s", found[index].cause);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].evidence, sizeof(rows[index].evidence), "%s",
                           found[index].evidence);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_surfaced_at, sizeof(rows[index].last_surfaced_at), "%s",
                           found[index].last_surfaced_at);
                  snprintf(rows[index].resolved_at, sizeof(rows[index].resolved_at), "%s",
                           found[index].resolved_at);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_directive_list_reply_encode(rows, count, response_body, response_capacity,
                                                      response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char entity_lowered[AIMEE_DB2_DIRECTIVE_BY_ENTITY_ENTITY_LOWERED_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_directive_by_entity_request_decode(request_body, request_len, entity_lowered,
                                                          sizeof(entity_lowered), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_DIRECTIVE_BY_ENTITY_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->directive_by_entity)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_directive_by_entity_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_DIRECTIVE_BY_ENTITY_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_directive_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_DIRECTIVE_BY_ENTITY_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->directive_by_entity(entity_lowered, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].directive_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].memory_a_id =
                      found[index].memory_a_id < 0 ? 0u : (uint64_t)found[index].memory_a_id;
                  rows[index].memory_b_id =
                      found[index].memory_b_id < 0 ? 0u : (uint64_t)found[index].memory_b_id;
                  rows[index].resolution_memory_id =
                      found[index].resolution_memory_id < 0
                          ? 0u
                          : (uint64_t)found[index].resolution_memory_id;
                  rows[index].priority =
                      found[index].priority < 0 ? 0u : (uint32_t)found[index].priority;
                  rows[index].surfaced_count =
                      found[index].surfaced_count < 0 ? 0u : (uint32_t)found[index].surfaced_count;
                  snprintf(rows[index].question, sizeof(rows[index].question), "%s",
                           found[index].question);
                  snprintf(rows[index].topic, sizeof(rows[index].topic), "%s", found[index].topic);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].cause, sizeof(rows[index].cause), "%s", found[index].cause);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].evidence, sizeof(rows[index].evidence), "%s",
                           found[index].evidence);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_surfaced_at, sizeof(rows[index].last_surfaced_at), "%s",
                           found[index].last_surfaced_at);
                  snprintf(rows[index].resolved_at, sizeof(rows[index].resolved_at), "%s",
                           found[index].resolved_at);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_directive_by_entity_reply_encode(rows, count, response_body,
                                                           response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char file_anchor[AIMEE_DB2_DIRECTIVE_BY_FILE_FILE_ANCHOR_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_directive_by_file_request_decode(request_body, request_len, file_anchor,
                                                        sizeof(file_anchor), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_DIRECTIVE_BY_FILE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->directive_by_file)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_directive_by_file_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_DIRECTIVE_BY_FILE_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_directive_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_DIRECTIVE_BY_FILE_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->directive_by_file(file_anchor, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].directive_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].memory_a_id =
                      found[index].memory_a_id < 0 ? 0u : (uint64_t)found[index].memory_a_id;
                  rows[index].memory_b_id =
                      found[index].memory_b_id < 0 ? 0u : (uint64_t)found[index].memory_b_id;
                  rows[index].resolution_memory_id =
                      found[index].resolution_memory_id < 0
                          ? 0u
                          : (uint64_t)found[index].resolution_memory_id;
                  rows[index].priority =
                      found[index].priority < 0 ? 0u : (uint32_t)found[index].priority;
                  rows[index].surfaced_count =
                      found[index].surfaced_count < 0 ? 0u : (uint32_t)found[index].surfaced_count;
                  snprintf(rows[index].question, sizeof(rows[index].question), "%s",
                           found[index].question);
                  snprintf(rows[index].topic, sizeof(rows[index].topic), "%s", found[index].topic);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].cause, sizeof(rows[index].cause), "%s", found[index].cause);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].evidence, sizeof(rows[index].evidence), "%s",
                           found[index].evidence);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_surfaced_at, sizeof(rows[index].last_surfaced_at), "%s",
                           found[index].last_surfaced_at);
                  snprintf(rows[index].resolved_at, sizeof(rows[index].resolved_at), "%s",
                           found[index].resolved_at);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_directive_by_file_reply_encode(rows, count, response_body,
                                                         response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char match_clause[AIMEE_DB2_DIRECTIVE_BY_LEXICAL_MATCH_CLAUSE_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_directive_by_lexical_request_decode(request_body, request_len, match_clause,
                                                           sizeof(match_clause), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_DIRECTIVE_BY_LEXICAL_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->directive_by_lexical)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_directive_by_lexical_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_DIRECTIVE_BY_LEXICAL_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_directive_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_DIRECTIVE_BY_LEXICAL_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->directive_by_lexical(match_clause, found, (int)limit);
               for (int index = 0; index < written; index++)
               {
                  rows[index].directive_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].memory_a_id =
                      found[index].memory_a_id < 0 ? 0u : (uint64_t)found[index].memory_a_id;
                  rows[index].memory_b_id =
                      found[index].memory_b_id < 0 ? 0u : (uint64_t)found[index].memory_b_id;
                  rows[index].resolution_memory_id =
                      found[index].resolution_memory_id < 0
                          ? 0u
                          : (uint64_t)found[index].resolution_memory_id;
                  rows[index].priority =
                      found[index].priority < 0 ? 0u : (uint32_t)found[index].priority;
                  rows[index].surfaced_count =
                      found[index].surfaced_count < 0 ? 0u : (uint32_t)found[index].surfaced_count;
                  snprintf(rows[index].question, sizeof(rows[index].question), "%s",
                           found[index].question);
                  snprintf(rows[index].topic, sizeof(rows[index].topic), "%s", found[index].topic);
                  snprintf(rows[index].anchor_entity, sizeof(rows[index].anchor_entity), "%s",
                           found[index].anchor_entity);
                  snprintf(rows[index].anchor_file, sizeof(rows[index].anchor_file), "%s",
                           found[index].anchor_file);
                  snprintf(rows[index].cause, sizeof(rows[index].cause), "%s", found[index].cause);
                  snprintf(rows[index].state, sizeof(rows[index].state), "%s", found[index].state);
                  snprintf(rows[index].evidence, sizeof(rows[index].evidence), "%s",
                           found[index].evidence);
                  snprintf(rows[index].source_session, sizeof(rows[index].source_session), "%s",
                           found[index].source_session);
                  snprintf(rows[index].last_surfaced_at, sizeof(rows[index].last_surfaced_at), "%s",
                           found[index].last_surfaced_at);
                  snprintf(rows[index].resolved_at, sizeof(rows[index].resolved_at), "%s",
                           found[index].resolved_at);
                  snprintf(rows[index].valid_until, sizeof(rows[index].valid_until), "%s",
                           found[index].valid_until);
                  snprintf(rows[index].created_at, sizeof(rows[index].created_at), "%s",
                           found[index].created_at);
                  snprintf(rows[index].updated_at, sizeof(rows[index].updated_at), "%s",
                           found[index].updated_at);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_directive_by_lexical_reply_encode(rows, count, response_body,
                                                            response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         if (aimee_db2_memory_lint_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_MEMORY_LINT_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->memory_lint)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_memory_lint_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_MEMORY_LINT_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               memory_lint_issue_t *found = malloc(sizeof(*found) * AIMEE_DB2_MEMORY_LINT_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->memory_lint(found, AIMEE_DB2_MEMORY_LINT_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].lint_memory_id =
                      found[index].memory_id < 0 ? 0u : (uint64_t)found[index].memory_id;
                  snprintf(rows[index].issue_type, sizeof(rows[index].issue_type), "%s",
                           found[index].type);
                  snprintf(rows[index].memory_key, sizeof(rows[index].memory_key), "%s",
                           found[index].key);
                  snprintf(rows[index].issue_message, sizeof(rows[index].issue_message), "%s",
                           found[index].message);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_memory_lint_reply_encode(rows, count, response_body, response_capacity,
                                                   response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char outcome_filter[AIMEE_DB2_DECISION_LOG_LIST_OUTCOME_FILTER_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_decision_log_list_request_decode(request_body, request_len, outcome_filter,
                                                        sizeof(outcome_filter), &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_DECISION_LOG_LIST_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->decision_log_list)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_decision_log_list_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_DECISION_LOG_LIST_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_decision_log_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_DECISION_LOG_LIST_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->decision_log_list(outcome_filter, (int)limit, found,
                                                        AIMEE_DB2_DECISION_LOG_LIST_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].decision_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].decision_task_id =
                      found[index].task_id < 0 ? 0u : (uint64_t)found[index].task_id;
                  rows[index].supersedes_id =
                      found[index].supersedes_id < 0 ? 0u : (uint64_t)found[index].supersedes_id;
                  rows[index].linked_policy_id = found[index].linked_policy_id < 0
                                                     ? 0u
                                                     : (uint64_t)found[index].linked_policy_id;
                  snprintf(rows[index].options, sizeof(rows[index].options), "%s",
                           found[index].options);
                  snprintf(rows[index].chosen, sizeof(rows[index].chosen), "%s",
                           found[index].chosen);
                  snprintf(rows[index].rationale, sizeof(rows[index].rationale), "%s",
                           found[index].rationale);
                  snprintf(rows[index].assumptions, sizeof(rows[index].assumptions), "%s",
                           found[index].assumptions);
                  snprintf(rows[index].outcome, sizeof(rows[index].outcome), "%s",
                           found[index].outcome);
                  snprintf(rows[index].decision_created_at, sizeof(rows[index].decision_created_at),
                           "%s", found[index].created_at);
                  snprintf(rows[index].decision_status, sizeof(rows[index].decision_status), "%s",
                           found[index].status);
                  snprintf(rows[index].revisit_when, sizeof(rows[index].revisit_when), "%s",
                           found[index].revisit_when);
                  snprintf(rows[index].decision_subject, sizeof(rows[index].decision_subject), "%s",
                           found[index].subject);
                  snprintf(rows[index].decision_author, sizeof(rows[index].decision_author), "%s",
                           found[index].author);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_decision_log_list_reply_encode(rows, count, response_body,
                                                         response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char decision_subject_filter
             [AIMEE_DB2_DECISION_LOG_LIST_SCOPED_DECISION_SUBJECT_FILTER_MAX + 1] = "";
         char status_filter[AIMEE_DB2_DECISION_LOG_LIST_SCOPED_STATUS_FILTER_MAX + 1] = "";
         uint32_t limit = 0u;
         if (aimee_db2_decision_log_list_scoped_request_decode(
                 request_body, request_len, decision_subject_filter,
                 sizeof(decision_subject_filter), status_filter, sizeof(status_filter),
                 &limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_DECISION_LOG_LIST_SCOPED_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->decision_log_list_scoped)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            aimee_db2_decision_log_list_scoped_row_t *rows =
                malloc(sizeof(*rows) * AIMEE_DB2_DECISION_LOG_LIST_SCOPED_MAX_ROWS);
            uint32_t count = 0u;
            if (!rows)
               return AIMEE_MODULE_STATUS_INTERNAL;
            {
               db2_decision_log_row_t *found =
                   malloc(sizeof(*found) * AIMEE_DB2_DECISION_LOG_LIST_SCOPED_MAX_ROWS);
               if (!found)
               {
                  free(rows);
                  return AIMEE_MODULE_STATUS_INTERNAL;
               }
               int written = backend->decision_log_list_scoped(
                   decision_subject_filter, status_filter, (int)limit, found,
                   AIMEE_DB2_DECISION_LOG_LIST_SCOPED_MAX_ROWS);
               for (int index = 0; index < written; index++)
               {
                  rows[index].decision_id = found[index].id < 0 ? 0u : (uint64_t)found[index].id;
                  rows[index].decision_task_id =
                      found[index].task_id < 0 ? 0u : (uint64_t)found[index].task_id;
                  rows[index].supersedes_id =
                      found[index].supersedes_id < 0 ? 0u : (uint64_t)found[index].supersedes_id;
                  rows[index].linked_policy_id = found[index].linked_policy_id < 0
                                                     ? 0u
                                                     : (uint64_t)found[index].linked_policy_id;
                  snprintf(rows[index].options, sizeof(rows[index].options), "%s",
                           found[index].options);
                  snprintf(rows[index].chosen, sizeof(rows[index].chosen), "%s",
                           found[index].chosen);
                  snprintf(rows[index].rationale, sizeof(rows[index].rationale), "%s",
                           found[index].rationale);
                  snprintf(rows[index].assumptions, sizeof(rows[index].assumptions), "%s",
                           found[index].assumptions);
                  snprintf(rows[index].outcome, sizeof(rows[index].outcome), "%s",
                           found[index].outcome);
                  snprintf(rows[index].decision_created_at, sizeof(rows[index].decision_created_at),
                           "%s", found[index].created_at);
                  snprintf(rows[index].decision_status, sizeof(rows[index].decision_status), "%s",
                           found[index].status);
                  snprintf(rows[index].revisit_when, sizeof(rows[index].revisit_when), "%s",
                           found[index].revisit_when);
                  snprintf(rows[index].decision_subject, sizeof(rows[index].decision_subject), "%s",
                           found[index].subject);
                  snprintf(rows[index].decision_author, sizeof(rows[index].decision_author), "%s",
                           found[index].author);
               }
               count = written < 0 ? 0u : (uint32_t)written;
               free(found);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               free(rows);
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_decision_log_list_scoped_reply_encode(
                    rows, count, response_body, response_capacity, response_len) != 0)
            {
               free(rows);
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            free(rows);
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t directive_id = 0u;
         uint64_t resolution_memory_id = 0u;
         char resolution_note[AIMEE_DB2_KB_DIRECTIVE_RESOLVE_RESOLUTION_NOTE_MAX + 1] = "";
         if (aimee_db2_kb_directive_resolve_request_decode(request_body, request_len, &directive_id,
                                                           &resolution_memory_id, resolution_note,
                                                           sizeof(resolution_note)) == 0)
         {
            if (response_capacity < AIMEE_DB2_KB_DIRECTIVE_RESOLVE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->kb_directive_resolve)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->kb_directive_resolve((int64_t)directive_id, (int64_t)resolution_memory_id,
                                              resolution_note) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_kb_directive_resolve_reply_encode(acknowledged, response_body,
                                                            response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char decision_subject[AIMEE_DB2_DECISION_LOG_ACTIVE_ID_DECISION_SUBJECT_MAX + 1] = "";
         uint64_t linked_policy = 0u;
         if (aimee_db2_decision_log_active_id_request_decode(
                 request_body, request_len, decision_subject, sizeof(decision_subject),
                 &linked_policy) == 0)
         {
            if (response_capacity < AIMEE_DB2_DECISION_LOG_ACTIVE_ID_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->decision_log_active_id)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint64_t decision_id = 0u;
            {
               int64_t active =
                   backend->decision_log_active_id(decision_subject, (int64_t)linked_policy);
               decision_id = active < 0 ? 0u : (uint64_t)active;
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_decision_log_active_id_reply_encode(decision_id, response_body,
                                                              response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char project[AIMEE_DB2_CSS_RENDER_SNAPSHOT_STORE_PROJECT_MAX + 1] = "";
         char unit_path[AIMEE_DB2_CSS_RENDER_SNAPSHOT_STORE_UNIT_PATH_MAX + 1] = "";
         char render_phase[AIMEE_DB2_CSS_RENDER_SNAPSHOT_STORE_RENDER_PHASE_MAX + 1] = "";
         char snapshot_json[AIMEE_DB2_CSS_RENDER_SNAPSHOT_STORE_SNAPSHOT_JSON_MAX + 1] = "";
         char captured_at[AIMEE_DB2_CSS_RENDER_SNAPSHOT_STORE_CAPTURED_AT_MAX + 1] = "";
         if (aimee_db2_css_render_snapshot_store_request_decode(
                 request_body, request_len, project, sizeof(project), unit_path, sizeof(unit_path),
                 render_phase, sizeof(render_phase), snapshot_json, sizeof(snapshot_json),
                 captured_at, sizeof(captured_at)) == 0)
         {
            if (response_capacity < AIMEE_DB2_CSS_RENDER_SNAPSHOT_STORE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->css_render_snapshot_store)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged = backend->css_render_snapshot_store(project, unit_path, render_phase,
                                                              snapshot_json, captured_at) == 0
                               ? 1u
                               : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_css_render_snapshot_store_reply_encode(
                    acknowledged, response_body, response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint64_t memory_a_id = 0u;
         uint64_t memory_b_id = 0u;
         uint64_t resolution_memory_id = 0u;
         if (aimee_db2_resolve_contradiction_request_decode(
                 request_body, request_len, &memory_a_id, &memory_b_id, &resolution_memory_id) == 0)
         {
            if (response_capacity < AIMEE_DB2_RESOLVE_CONTRADICTION_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->resolve_contradiction)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->resolve_contradiction((int64_t)memory_a_id, (int64_t)memory_b_id,
                                               (int64_t)resolution_memory_id) == 0
                    ? 1u
                    : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_resolve_contradiction_reply_encode(acknowledged, response_body,
                                                             response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         char job_kind[AIMEE_DB2_ASYNC_ENQUEUE_JOB_KIND_MAX + 1] = "";
         uint64_t document_id = 0u;
         char job_project[AIMEE_DB2_ASYNC_ENQUEUE_JOB_PROJECT_MAX + 1] = "";
         if (aimee_db2_async_enqueue_request_decode(request_body, request_len, job_kind,
                                                    sizeof(job_kind), &document_id, job_project,
                                                    sizeof(job_project)) == 0)
         {
            if (response_capacity < AIMEE_DB2_ASYNC_ENQUEUE_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->async_enqueue)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t acknowledged = 0u;
            acknowledged =
                backend->async_enqueue(job_kind, (int64_t)document_id, job_project) == 0 ? 1u : 0u;
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_async_enqueue_reply_encode(acknowledged, response_body, response_capacity,
                                                     response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         if (aimee_db2_corpus_pipeline_status_request_decode(request_body, request_len) == 0)
         {
            if (response_capacity < AIMEE_DB2_CORPUS_PIPELINE_STATUS_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->corpus_pipeline_status)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t corpus_total = 0u;
            uint32_t corpus_pending = 0u;
            uint32_t corpus_running = 0u;
            uint32_t corpus_failed = 0u;
            uint32_t corpus_complete = 0u;
            uint32_t corpus_processed = 0u;
            uint32_t corpus_skipped = 0u;
            db2_corpus_pipeline_stats_t stats;
            memset(&stats, 0, sizeof(stats));
            if (backend->corpus_pipeline_status(&stats) == 0)
            {
               corpus_total = (uint32_t)(stats.total > 0 ? stats.total : 0);
               corpus_pending = (uint32_t)(stats.pending > 0 ? stats.pending : 0);
               corpus_running = (uint32_t)(stats.running > 0 ? stats.running : 0);
               corpus_failed = (uint32_t)(stats.failed > 0 ? stats.failed : 0);
               corpus_complete = (uint32_t)(stats.complete > 0 ? stats.complete : 0);
               corpus_processed = (uint32_t)(stats.processed > 0 ? stats.processed : 0);
               corpus_skipped = (uint32_t)(stats.skipped > 0 ? stats.skipped : 0);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_corpus_pipeline_status_reply_encode(
                    corpus_total, corpus_pending, corpus_running, corpus_failed, corpus_complete,
                    corpus_processed, corpus_skipped, response_body, response_capacity,
                    response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
      {
         uint32_t drain_limit = 0u;
         if (aimee_db2_corpus_pipeline_drain_request_decode(request_body, request_len,
                                                            &drain_limit) == 0)
         {
            if (response_capacity < AIMEE_DB2_CORPUS_PIPELINE_DRAIN_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->corpus_pipeline_drain)
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
            uint32_t drained = 0u;
            uint32_t corpus_total = 0u;
            uint32_t corpus_pending = 0u;
            uint32_t corpus_running = 0u;
            uint32_t corpus_failed = 0u;
            uint32_t corpus_complete = 0u;
            uint32_t corpus_processed = 0u;
            uint32_t corpus_skipped = 0u;
            db2_corpus_pipeline_stats_t stats;
            memset(&stats, 0, sizeof(stats));
            if (backend->corpus_pipeline_drain((int)drain_limit, &stats) == 0)
            {
               drained = 1u;
               corpus_total = (uint32_t)(stats.total > 0 ? stats.total : 0);
               corpus_pending = (uint32_t)(stats.pending > 0 ? stats.pending : 0);
               corpus_running = (uint32_t)(stats.running > 0 ? stats.running : 0);
               corpus_failed = (uint32_t)(stats.failed > 0 ? stats.failed : 0);
               corpus_complete = (uint32_t)(stats.complete > 0 ? stats.complete : 0);
               corpus_processed = (uint32_t)(stats.processed > 0 ? stats.processed : 0);
               corpus_skipped = (uint32_t)(stats.skipped > 0 ? stats.skipped : 0);
            }
            if (aimee_module_invocation_cancelled(invocation))
            {
               return AIMEE_MODULE_STATUS_CANCELLED;
            }
            if (aimee_db2_corpus_pipeline_drain_reply_encode(
                    drained, corpus_total, corpus_pending, corpus_running, corpus_failed,
                    corpus_complete, corpus_processed, corpus_skipped, response_body,
                    response_capacity, response_len) != 0)
            {
               return AIMEE_MODULE_STATUS_INTERNAL;
            }
            return AIMEE_MODULE_STATUS_OK;
         }
      }
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
