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
static int prune_orphaned_l0_value;
static int prune_orphaned_l0_calls;
static int lifecycle_sweep_value;
static int lifecycle_sweep_calls;
static int demote_id_value;
static int demote_id_calls;
static int64_t demote_id_last;
static int workspace_tag_value;
static int workspace_tag_calls;
static int delete_row_value;
static int delete_row_calls;
static int64_t delete_row_last;
static int touch_value;
static int touch_calls;
static int64_t touch_last;
static int link_delete_value;
static int link_delete_calls;
static int valid_at_value;
static int valid_at_calls;
static char valid_at_last[64];
static int scope_type_value;
static int scope_type_calls;
static int reject_value;
static int reject_calls;
static int64_t reject_last;
static int update_content_value;
static int update_content_calls;
static int decay_confidence_calls;
static int64_t decay_confidence_last;
static int workspace_tag_insert_calls;
static char workspace_tag_insert_last[512];
static int cognified_kind_calls;
static char cognified_kind_last[32];
static int source_session_calls;
static char source_session_last[160];
static int negation_tokens_calls;
static char negation_tokens_last[2048];
static int get_content_hit;
static int get_content_calls;
static int get_source_session_rc;
static int get_source_session_calls;
static int temporal_ref_hit;
static int temporal_ref_calls;
static int corpus_stat_rc;
static int corpus_stat_count;
static int corpus_stat_calls;
static int edge_prune_value;
static int edge_prune_calls;
static int edge_normalize_value;
static int edge_normalize_calls;
static int project_count_value;
static int project_count_calls;
static int purge_pollution_value;
static int purge_pollution_calls;
static int requeue_drifted_value;
static int requeue_drifted_calls;
static int rebuild_routes_value;
static int rebuild_routes_calls;
static int rebuild_identities_value;
static int rebuild_identities_calls;
static int rebuild_build_deps_value;
static int rebuild_build_deps_calls;
static int64_t drift_candidates_value;
static int drift_candidates_calls;
static int rules_decay_value;
static int rules_decay_calls;
static int curiosity_rescore_value;
static int curiosity_rescore_calls;
static int mining_seed_value;
static int mining_seed_calls;
static int proposals_archive_calls;
static int64_t trace_watermark_value;
static int trace_watermark_calls;
static int rel_types_seed_value;
static int rel_types_seed_calls;
static int lock_acquire_value;
static int lock_acquire_calls;
static int lock_release_calls;
static int64_t release_active_value;
static int release_active_calls;
static int prospective_sweep_value;
static int prospective_sweep_calls;
static int directive_sweep_value;
static int directive_sweep_calls;
static int directive_suppress_value;
static int directive_suppress_calls;
static int64_t directive_suppress_id;
static int directive_surface_value;
static int directive_surface_calls;
static int anti_pattern_bump_value;
static int anti_pattern_bump_calls;
static int64_t anti_pattern_bump_seen;
static int anti_pattern_delete_value;
static int anti_pattern_delete_calls;
static int64_t anti_pattern_delete_seen;
static int doc_delete_value;
static int doc_delete_calls;
static int64_t doc_delete_seen;
static int task_delete_value;
static int task_delete_calls;
static int64_t task_delete_seen;
static int file_index_delete_project_value;
static int file_index_delete_project_calls;
static char file_index_delete_project_seen[128];
static int clear_project_value;
static int clear_project_calls;
static char clear_project_seen[128];
static int clear_current_project_value;
static int clear_current_project_calls;
static char clear_current_project_seen[128];
static int64_t directive_surface_id;
static int mark_revisit_value;
static int mark_revisit_calls;
static int queue_reset_value;
static int queue_reset_calls;
static int evidence_reembed_value;
static int evidence_reembed_calls;
static int curator_reembed_value;
static int curator_reembed_calls;
static int synth_reenqueue_value;
static int synth_reenqueue_calls;
static int extract_reenqueue_value;
static int extract_reenqueue_calls;
static char corpus_stat_stamp[64];
static char temporal_ref_value[160];
static char get_source_session_value[160];
static char get_content_value[2048];
static char update_content_last[2048];
static char scope_type_last[64];
static int64_t link_delete_last;
static int64_t workspace_tag_last;
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
static int prune_health_value;
static int prune_health_days;
static int prune_contradictions_value;
static int prune_contradictions_days;
static int health_counters_result;
static int health_counters_calls;
static int health_counters_use_count;
static double health_counters_confidence;
static uint32_t health_counters_cycles;
static int stats_counts_result;
static int stats_counts_calls;
static uint32_t stats_counts_last_kind;
static int expire_l0_value;
static int expire_kind_count;
static int expire_days_value;
static int expire_stale_value;
static int expire_l0_provenance_calls;
static int expire_stale_provenance_calls;
static char expire_last_window[16];
static int demote_policy_result;
static int demote_days_value;
static int demote_kind_value;
static int demote_cascade_value;
static int demote_cascade_calls;
static char demote_kind_stamp[32];
static char demote_cascade_stamp[32];
static int promote_stable_value;
static int promote_stable_calls;
static char promote_stable_stamp[32];
static int reclassify_value;
static int reclassify_calls;
static int reclassify_last_gate;
static int approval_result;
static int approval_calls;
static int64_t approval_last_id;
static char approval_last_approver[64];
static char approval_last_note[512];
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
static int transport_expect_health_retention;
static int transport_expect_health_counters;
static int transport_expect_stats_counts;
static int transport_expect_expire;
static int transport_expect_demote;
static int transport_expect_promote_stable;
static int transport_expect_reclassify;
static int transport_expect_approval;
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

int db2_memory_prune_orphaned_l0(void)
{
   prune_orphaned_l0_calls++;
   return prune_orphaned_l0_value;
}

static int prune_orphaned_l0(void)
{
   prune_orphaned_l0_calls++;
   return prune_orphaned_l0_value;
}

int db2_memory_lifecycle_sweep_expired(void)
{
   lifecycle_sweep_calls++;
   return lifecycle_sweep_value;
}

static int lifecycle_sweep_expired(void)
{
   lifecycle_sweep_calls++;
   return lifecycle_sweep_value;
}

int db2_memory_promotion_demote_id(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

static int demote_id(int64_t memory_id)
{
   demote_id_calls++;
   demote_id_last = memory_id;
   return demote_id_value;
}

int db2_memory_has_any_workspace_tag(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

static int has_workspace_tag(int64_t memory_id)
{
   workspace_tag_calls++;
   workspace_tag_last = memory_id;
   return workspace_tag_value;
}

int db2_memory_delete_row(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

static int delete_row(int64_t memory_id)
{
   delete_row_calls++;
   delete_row_last = memory_id;
   return delete_row_value;
}

int db2_memory_touch(int64_t memory_id)
{
   (void)memory_id;
   return -1;
}

static int touch(int64_t memory_id)
{
   touch_calls++;
   touch_last = memory_id;
   return touch_value;
}

int db2_memory_link_delete(int64_t link_id)
{
   (void)link_id;
   return -1;
}

static int link_delete(int64_t link_id)
{
   link_delete_calls++;
   link_delete_last = link_id;
   return link_delete_value;
}

int db2_memory_valid_at(int64_t memory_id, const char *as_of)
{
   (void)memory_id;
   (void)as_of;
   return -1;
}

static int valid_at(int64_t memory_id, const char *as_of)
{
   (void)memory_id;
   valid_at_calls++;
   snprintf(valid_at_last, sizeof(valid_at_last), "%s", as_of);
   return valid_at_value;
}

int db2_memory_has_scope_type(int64_t memory_id, const char *scope_type)
{
   (void)memory_id;
   (void)scope_type;
   return 0;
}

static int has_scope_type(int64_t memory_id, const char *scope_type)
{
   (void)memory_id;
   scope_type_calls++;
   snprintf(scope_type_last, sizeof(scope_type_last), "%s", scope_type);
   return scope_type_value;
}

int db2_memory_reject(int64_t memory_id, const char *reason)
{
   (void)memory_id;
   (void)reason;
   return -1;
}

static int reject(int64_t memory_id)
{
   reject_calls++;
   reject_last = memory_id;
   return reject_value;
}

int db2_memory_update_content(int64_t memory_id, const char *content)
{
   (void)memory_id;
   (void)content;
   return 0;
}

static int update_content(int64_t memory_id, const char *content)
{
   (void)memory_id;
   update_content_calls++;
   snprintf(update_content_last, sizeof(update_content_last), "%s", content);
   return update_content_value;
}

void db2_memory_decay_confidence(int64_t memory_id)
{
   (void)memory_id;
}

static void decay_confidence(int64_t memory_id)
{
   decay_confidence_calls++;
   decay_confidence_last = memory_id;
}

void db2_memory_workspace_tag_insert(int64_t memory_id, const char *workspace)
{
   (void)memory_id;
   (void)workspace;
}

static void workspace_tag_insert(int64_t memory_id, const char *workspace)
{
   (void)memory_id;
   workspace_tag_insert_calls++;
   snprintf(workspace_tag_insert_last, sizeof(workspace_tag_insert_last), "%s", workspace);
}

void db2_memory_set_cognified_kind(int64_t memory_id, const char *kind)
{
   (void)memory_id;
   (void)kind;
}

static void set_cognified_kind(int64_t memory_id, const char *kind)
{
   (void)memory_id;
   cognified_kind_calls++;
   snprintf(cognified_kind_last, sizeof(cognified_kind_last), "%s", kind);
}

void db2_memory_set_source_session(int64_t memory_id, const char *session_id)
{
   (void)memory_id;
   (void)session_id;
}

static void set_source_session(int64_t memory_id, const char *session_id)
{
   (void)memory_id;
   source_session_calls++;
   snprintf(source_session_last, sizeof(source_session_last), "%s", session_id);
}

void db2_memory_negation_tokens_update(int64_t memory_id, const char *new_tokens)
{
   (void)memory_id;
   (void)new_tokens;
}

static void negation_tokens_update(int64_t memory_id, const char *tokens)
{
   (void)memory_id;
   negation_tokens_calls++;
   snprintf(negation_tokens_last, sizeof(negation_tokens_last), "%s", tokens);
}

int db2_memory_get_content(int64_t memory_id, char *out, int out_len)
{
   (void)memory_id;
   if (out && out_len > 0)
      out[0] = '\0';
   return 0;
}

static int get_content(int64_t memory_id, char *out, int out_len)
{
   (void)memory_id;
   get_content_calls++;
   if (!get_content_hit)
      return 0;
   snprintf(out, (size_t)out_len, "%s", get_content_value);
   return 1;
}

int db2_memory_get_source_session(int64_t memory_id, char *out, int out_len)
{
   (void)memory_id;
   if (out && out_len > 0)
      out[0] = '\0';
   return -1;
}

static int get_source_session(int64_t memory_id, char *out, int out_len)
{
   (void)memory_id;
   get_source_session_calls++;
   if (get_source_session_rc != 0)
      return get_source_session_rc;
   snprintf(out, (size_t)out_len, "%s", get_source_session_value);
   return 0;
}

int db2_memory_pick_first_temporal_ref(int64_t memory_id, char *out, int out_len)
{
   (void)memory_id;
   if (out && out_len > 0)
      out[0] = '\0';
   return 0;
}

static int pick_first_temporal_ref(int64_t memory_id, char *out, int out_len)
{
   (void)memory_id;
   temporal_ref_calls++;
   if (!temporal_ref_hit)
      return 0;
   snprintf(out, (size_t)out_len, "%s", temporal_ref_value);
   return 1;
}

int db2_memory_count_and_max_updated(int *out_count, char *out_ts, int out_ts_len)
{
   if (out_count)
      *out_count = 0;
   if (out_ts && out_ts_len > 0)
      out_ts[0] = '\0';
   return 0;
}

static int count_and_max_updated(int *out_count, char *out_ts, int out_ts_len)
{
   corpus_stat_calls++;
   if (!corpus_stat_rc)
      return 0;
   *out_count = corpus_stat_count;
   snprintf(out_ts, (size_t)out_ts_len, "%s", corpus_stat_stamp);
   return 1;
}

int db2_entity_edge_prune_orphans(void)
{
   return 0;
}

static int entity_edge_prune_orphans(void)
{
   edge_prune_calls++;
   return edge_prune_value;
}

int db2_entity_edge_normalize_weights(void)
{
   return 0;
}

static int entity_edge_normalize_weights(void)
{
   edge_normalize_calls++;
   return edge_normalize_value;
}

int db2_code_index_project_count(void)
{
   return 0;
}

static int project_count(void)
{
   project_count_calls++;
   return project_count_value;
}

int db2_code_index_purge_hidden_pollution(void)
{
   return 0;
}

static int purge_hidden_pollution(void)
{
   purge_pollution_calls++;
   return purge_pollution_value;
}

int db2_code_index_requeue_drifted(void)
{
   return 0;
}

static int requeue_drifted(void)
{
   requeue_drifted_calls++;
   return requeue_drifted_value;
}

int db2_cross_repo_rebuild_routes(void)
{
   return 0;
}

static int cross_repo_rebuild_routes(void)
{
   rebuild_routes_calls++;
   return rebuild_routes_value;
}

int db2_cross_repo_rebuild_identities(void)
{
   return 0;
}

static int cross_repo_rebuild_identities(void)
{
   rebuild_identities_calls++;
   return rebuild_identities_value;
}

int db2_cross_repo_rebuild_build_deps(void)
{
   return 0;
}

static int cross_repo_rebuild_build_deps(void)
{
   rebuild_build_deps_calls++;
   return rebuild_build_deps_value;
}

int64_t db2_code_index_drift_candidates(void)
{
   return 0;
}

static int64_t drift_candidates(void)
{
   drift_candidates_calls++;
   return drift_candidates_value;
}

int db2_rules_decay(void)
{
   return 0;
}

static int rules_decay(void)
{
   rules_decay_calls++;
   return rules_decay_value;
}

int db2_curiosity_rescore_all(void)
{
   return 0;
}

static int curiosity_rescore_all(void)
{
   curiosity_rescore_calls++;
   return curiosity_rescore_value;
}

int db2_mining_seed_job_defaults(void)
{
   return 0;
}

static int mining_seed_job_defaults(void)
{
   mining_seed_calls++;
   return mining_seed_value;
}

void db2_learning_proposals_archive_expired(void)
{
}

static void proposals_archive_expired(void)
{
   proposals_archive_calls++;
}

int64_t db2_trace_mining_last_id(void)
{
   return 0;
}

static int64_t trace_mining_last_id(void)
{
   trace_watermark_calls++;
   return trace_watermark_value;
}

int db2_rel_types_ensure_seed(void)
{
   return 0;
}

static int rel_types_ensure_seed(void)
{
   rel_types_seed_calls++;
   return rel_types_seed_value;
}

int db2_kb_runtime_state_vector_rebuild_lock_try_acquire(void)
{
   return 0;
}

void db2_kb_runtime_state_vector_rebuild_lock_release(void)
{
}

static int vector_rebuild_lock_try_acquire(void)
{
   lock_acquire_calls++;
   return lock_acquire_value;
}

static void vector_rebuild_lock_release(void)
{
   lock_release_calls++;
}

int64_t db2_kb_release_get_active(void)
{
   return 0;
}

static int64_t release_get_active(void)
{
   release_active_calls++;
   return release_active_value;
}

int db2_prospective_sweep_expired(void)
{
   return 0;
}

static int prospective_sweep_expired(void)
{
   prospective_sweep_calls++;
   return prospective_sweep_value;
}

int db2_directive_sweep_expired(void)
{
   return 0;
}

int db2_kb_service_directive_sweep_expired(void)
{
   return 0;
}

int db2_directive_suppress(int64_t directive_id)
{
   (void)directive_id;
   return 0;
}

int db2_directive_record_surface(int64_t directive_id)
{
   (void)directive_id;
   return 0;
}

static int directive_suppress(int64_t directive_id)
{
   directive_suppress_calls++;
   directive_suppress_id = directive_id;
   return directive_suppress_value;
}

static int directive_record_surface(int64_t directive_id)
{
   directive_surface_calls++;
   directive_surface_id = directive_id;
   return directive_surface_value;
}

int db2_anti_pattern_bump(int64_t anti_pattern_id)
{
   (void)anti_pattern_id;
   return 0;
}

static int anti_pattern_bump(int64_t anti_pattern_id)
{
   anti_pattern_bump_calls++;
   anti_pattern_bump_seen = anti_pattern_id;
   return anti_pattern_bump_value;
}

int db2_anti_pattern_delete(int64_t anti_pattern_id)
{
   (void)anti_pattern_id;
   return 0;
}

static int anti_pattern_delete(int64_t anti_pattern_id)
{
   anti_pattern_delete_calls++;
   anti_pattern_delete_seen = anti_pattern_id;
   return anti_pattern_delete_value;
}

int db2_kb_doc_delete(int64_t doc_id)
{
   (void)doc_id;
   return 0;
}

static int doc_delete(int64_t doc_id)
{
   doc_delete_calls++;
   doc_delete_seen = doc_id;
   return doc_delete_value;
}

int db2_task_delete(int64_t task_id)
{
   (void)task_id;
   return 0;
}

static int task_delete(int64_t task_id)
{
   task_delete_calls++;
   task_delete_seen = task_id;
   return task_delete_value;
}

int db2_kb_file_index_delete_project(const char *project)
{
   (void)project;
   return 0;
}

static int file_index_delete_project(const char *project)
{
   file_index_delete_project_calls++;
   snprintf(file_index_delete_project_seen, sizeof(file_index_delete_project_seen), "%s",
            project ? project : "");
   return file_index_delete_project_value;
}

int db2_kb_service_clear_project(const char *project)
{
   (void)project;
   return 0;
}

static int clear_project(const char *project)
{
   clear_project_calls++;
   snprintf(clear_project_seen, sizeof(clear_project_seen), "%s", project ? project : "");
   return clear_project_value;
}

int db2_kb_service_clear_current_project(const char *project)
{
   (void)project;
   return 0;
}

static int clear_current_project(const char *project)
{
   clear_current_project_calls++;
   snprintf(clear_current_project_seen, sizeof(clear_current_project_seen), "%s",
            project ? project : "");
   return clear_current_project_value;
}

static int directive_sweep_expired(void)
{
   directive_sweep_calls++;
   return directive_sweep_value;
}

int db2_decision_log_mark_revisit_due(void)
{
   return 0;
}

static int mark_revisit_due(void)
{
   mark_revisit_calls++;
   return mark_revisit_value;
}

int db2_kb_ingest_queue_reset_running(void)
{
   return 0;
}

static int ingest_queue_reset_running(void)
{
   queue_reset_calls++;
   return queue_reset_value;
}

int db2_evidence_reembed_all(void)
{
   return 0;
}

static int evidence_reembed_all(void)
{
   evidence_reembed_calls++;
   return evidence_reembed_value;
}

int db2_curator_reembed_all(void)
{
   return 0;
}

static int curator_reembed_all(void)
{
   curator_reembed_calls++;
   return curator_reembed_value;
}

int db2_synth_reenqueue_all(void)
{
   return 0;
}

static int synth_reenqueue_all(void)
{
   synth_reenqueue_calls++;
   return synth_reenqueue_value;
}

int db2_curator_reenqueue_extract_all(void)
{
   return 0;
}

static int curator_reenqueue_extract_all(void)
{
   extract_reenqueue_calls++;
   return extract_reenqueue_value;
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

static int prune_health_impl(int days)
{
   prune_health_days = days;
   return prune_health_value;
}

int db2_memory_health_prune_old(int days)
{
   return prune_health_impl(days);
}

static int prune_health(int days)
{
   return prune_health_impl(days);
}

static int prune_contradictions_impl(int days)
{
   prune_contradictions_days = days;
   return prune_contradictions_value;
}

int db2_memory_health_prune_old_contradictions(int days)
{
   return prune_contradictions_impl(days);
}

static int prune_contradictions(int days)
{
   return prune_contradictions_impl(days);
}

/* The counter struct is DB2-private, so the production symbol takes void * here
 * the way db2_dim_change_reset does; these tests drive their own backend. */
int db2_memory_health_query_counters(int promote_use_count, double promote_confidence,
                                     db2_memory_health_query_counters_t *out)
{
   (void)promote_use_count;
   (void)promote_confidence;
   (void)out;
   return -1;
}

int db2_memory_find_facts_like(const char *term, int limit, memory_t *out, int max)
{
   (void)term;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

/* Three arguments, not four: this one binds the caller's buffer size as its
 * own LIMIT rather than taking a separate one. */
int db2_memory_list_session_scope_priority_like(const char *pattern, memory_t *out, int max)
{
   (void)pattern;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_negation_fts_search(const char *term, int limit, memory_t *out, int max)
{
   (void)term;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_session_neighbors_before(const char *session_id, int64_t anchor_id, int limit,
                                        memory_t *out, int max)
{
   (void)session_id;
   (void)anchor_id;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_session_neighbors_after(const char *session_id, int64_t anchor_id, int limit,
                                       memory_t *out, int max)
{
   (void)session_id;
   (void)anchor_id;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_get(int64_t memory_id, memory_t *out)
{
   (void)memory_id;
   (void)out;
   return -1;
}

int db2_memory_get_by_unit_id(int64_t unit_id, memory_t *out)
{
   (void)unit_id;
   (void)out;
   return -1;
}

/* Three arguments: the keyword search and the history both bind the caller's
 * buffer size rather than taking a separate limit. */
int db2_memory_search_facts_patterns_by_keyword(const char *keyword, memory_t *out, int max)
{
   (void)keyword;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_fact_history(const char *normalized_key, memory_t *out, int max)
{
   (void)normalized_key;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_list(const char *tier, const char *kind, int hide_archived, int limit, memory_t *out,
                    int max)
{
   (void)tier;
   (void)kind;
   (void)hide_archived;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_aggregate(const char *entity_seed, const char *keyword, memory_t *out, int max,
                         int *truncated_out)
{
   (void)entity_seed;
   (void)keyword;
   (void)out;
   (void)max;
   if (truncated_out)
      *truncated_out = 0;
   return 0;
}

int db2_memory_load_eval_corpus(memory_t *out, int max, char *label_out, size_t label_len)
{
   (void)out;
   (void)max;
   if (label_out && label_len)
      label_out[0] = '\0';
   return 0;
}

int db2_entity_count_observations(const char *entity_id)
{
   (void)entity_id;
   return 0;
}

int db2_fidelity_attribution_count_by_turn(const char *turn_id)
{
   (void)turn_id;
   return 0;
}

int db2_kb_blob_ref_referenced(const char *blob_ref)
{
   (void)blob_ref;
   return 0;
}

int db2_kb_async_count_kind_pending(const char *kind)
{
   (void)kind;
   return 0;
}

int db2_artifact_stamp_reflected(const char *artifact_id)
{
   (void)artifact_id;
   return 0;
}

int db2_failed_query_bump(const char *query_norm)
{
   (void)query_norm;
   return 0;
}

int db2_kb_purge_fence_active(const char *project)
{
   (void)project;
   return 0;
}

int db2_kb_runtime_state_set_now(const char *state_key)
{
   (void)state_key;
   return 0;
}

int db2_synth_enqueue(const char *artifact_id)
{
   (void)artifact_id;
   return 0;
}

int db2_synth_mark_done(const char *artifact_id)
{
   (void)artifact_id;
   return 0;
}

int db2_kb_service_mark_reembed_finished(const char *finished_at)
{
   (void)finished_at;
   return 0;
}

int db2_mining_job_try_lock(const char *job_id)
{
   (void)job_id;
   return 0;
}

int db2_artifact_set_state(const char *state, const char *artifact_id)
{
   (void)state;
   (void)artifact_id;
   return 0;
}

int db2_artifact_register_exemplar(const char *artifact_id, const char *collection)
{
   (void)artifact_id;
   (void)collection;
   return 0;
}

int db2_evidence_enqueue(const char *artifact_id, const char *collection)
{
   (void)artifact_id;
   (void)collection;
   return 0;
}

int db2_evidence_mark_failed(const char *artifact_id, const char *last_error)
{
   (void)artifact_id;
   (void)last_error;
   return 0;
}

int db2_synth_mark_failed(const char *artifact_id, const char *last_error)
{
   (void)artifact_id;
   (void)last_error;
   return 0;
}

int db2_kb_runtime_state_set(const char *state_key, const char *state_value)
{
   (void)state_key;
   (void)state_value;
   return 0;
}

int db2_kb_service_set_active_embedder_version(const char *version, const char *updated_at)
{
   (void)version;
   (void)updated_at;
   return 0;
}

int db2_entity_profile_is_fresh(const char *entity_id, const char *window)
{
   (void)entity_id;
   (void)window;
   return 0;
}

int db2_kb_doc_exists_by_hash_scope(const char *content_hash, const char *scope)
{
   (void)content_hash;
   (void)scope;
   return 0;
}

int db2_kb_pdf_quarantine_confirm(const char *project, const char *file_path)
{
   (void)project;
   (void)file_path;
   return 0;
}

int db2_kb_pdf_quarantine_reject(const char *project, const char *file_path)
{
   (void)project;
   (void)file_path;
   return 0;
}

int db2_enrollment_is_active_by_key(const char *cert_issuer, const char *cert_serial_norm)
{
   (void)cert_issuer;
   (void)cert_serial_norm;
   return 0;
}

int db2_kb_runtime_state_get(const char *key, char *out, size_t out_len)
{
   (void)key;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

int db2_bandit_arms_list(const char *decision_point, char *buf, size_t len)
{
   (void)decision_point;
   if (buf && len)
      buf[0] = '\0';
   return 0;
}

int db2_bandit_promotion_get(const char *decision_point, char *arm_out, size_t arm_out_len)
{
   (void)decision_point;
   if (arm_out && arm_out_len)
      arm_out[0] = '\0';
   return 0;
}

int db2_code_projection_project_fingerprint(const char *project, char *out, size_t out_len)
{
   (void)project;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

int db2_code_projection_visible_source_hash(const char *project, char *out, size_t out_len)
{
   (void)project;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

int db2_entity_profile_get_card(const char *entity_id, char *out_json, size_t out_len)
{
   (void)entity_id;
   if (out_json && out_len)
      out_json[0] = '\0';
   return 0;
}

int db2_ontology_eval_status(const char *rel_type, char *out, size_t out_len)
{
   (void)rel_type;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

int db2_decision_log_set_outcome(int64_t id, const char *outcome)
{
   (void)id;
   (void)outcome;
   return 0;
}

int db2_decision_log_set_status(int64_t id, const char *status)
{
   (void)id;
   (void)status;
   return 0;
}

int db2_decision_log_set_revisit(int64_t id, const char *revisit_when)
{
   (void)id;
   (void)revisit_when;
   return 0;
}

int db2_prospective_set_state(int64_t id, const char *new_state)
{
   (void)id;
   (void)new_state;
   return 0;
}

int db2_task_update_state(int64_t id, const char *state)
{
   (void)id;
   (void)state;
   return 0;
}

int db2_kb_ingest_queue_fail(int64_t job_id, const char *error_message)
{
   (void)job_id;
   (void)error_message;
   return 0;
}

int db2_code_projection_generation_abort(int64_t gen_id, const char *error_msg)
{
   (void)gen_id;
   (void)error_msg;
   return 0;
}

int db2_code_projection_generation_set_source_hash(int64_t gen_id, const char *source_hash)
{
   (void)gen_id;
   (void)source_hash;
   return 0;
}

int db2_code_projection_generation_publish(int64_t gen_id, const char *project)
{
   (void)gen_id;
   (void)project;
   return 0;
}

int db2_code_index_purge_files_matching(int64_t project_id, const char *path_glob)
{
   (void)project_id;
   (void)path_glob;
   return 0;
}

int db2_collab_rules_approve(int rule_id)
{
   (void)rule_id;
   return 0;
}

int db2_collab_rules_reject(int rule_id)
{
   (void)rule_id;
   return 0;
}

int db2_collab_rules_retire(int rule_id)
{
   (void)rule_id;
   return 0;
}

int db2_learning_proposal_bump_corroboration(int id)
{
   (void)id;
   return 0;
}

int db2_learning_proposal_mark_committed(int id)
{
   (void)id;
   return 0;
}

int db2_rules_delete(int id)
{
   (void)id;
   return 0;
}

int db2_calibration_surfaces_with_data(int min_rows)
{
   (void)min_rows;
   return 0;
}

int db2_kb_service_reset_stuck_vector_ops(int max_attempts)
{
   (void)max_attempts;
   return 0;
}

int db2_memory_dedupe_by_key(int dry_run)
{
   (void)dry_run;
   return 0;
}

int db2_directive_resolve(int64_t id, int64_t resolution_memory_id)
{
   (void)id;
   (void)resolution_memory_id;
   return 0;
}

int db2_kb_release_add_doc(int64_t release_id, int64_t doc_id)
{
   (void)release_id;
   (void)doc_id;
   return 0;
}

int db2_memory_scene_member_exists(int64_t memory_id, int64_t scene_id)
{
   (void)memory_id;
   (void)scene_id;
   return 0;
}

int db2_memory_unit_edge_exists(int64_t unit_id_a, int64_t unit_id_b)
{
   (void)unit_id_a;
   (void)unit_id_b;
   return 0;
}

int db2_artifact_cite(const char *artifact_id, const char *source_kind, const char *source_id)
{
   (void)artifact_id;
   (void)source_kind;
   (void)source_id;
   return 0;
}

int db2_artifact_link(const char *from_id, const char *to_id, const char *link_kind)
{
   (void)from_id;
   (void)to_id;
   (void)link_kind;
   return 0;
}

int db2_bandit_promotion_set(const char *decision_point, const char *arm_id,
                             const char *rollback_arm)
{
   (void)decision_point;
   (void)arm_id;
   (void)rollback_arm;
   return 0;
}

int db2_collab_rules_propose(const char *text, const char *reason, const char *proposed_by)
{
   (void)text;
   (void)reason;
   (void)proposed_by;
   return 0;
}

int db2_kb_file_index_delete_current_project(const char *project)
{
   (void)project;
   return 0;
}

int db2_code_index_project_delete(const char *name)
{
   (void)name;
   return 0;
}

int db2_sketch_minhash_signature_delete_project(const char *project)
{
   (void)project;
   return 0;
}

int db2_css_migration_enumerate(const char *project)
{
   (void)project;
   return 0;
}

int db2_ontology_approve(const char *rel_type)
{
   (void)rel_type;
   return 0;
}

int db2_ontology_reject(const char *rel_type)
{
   (void)rel_type;
   return 0;
}

int db2_rules_delete_by_directive_type(const char *directive_type)
{
   (void)directive_type;
   return 0;
}

int db2_artifact_flag_review(const char *id, const char *reason)
{
   (void)id;
   (void)reason;
   return 0;
}

int db2_artifact_verdict_suppressed(const char *verdict_tag, const char *verdict_scope)
{
   (void)verdict_tag;
   (void)verdict_scope;
   return 0;
}

int db2_css_migration_assert_conventions(const char *project, const char *now_iso)
{
   (void)project;
   (void)now_iso;
   return 0;
}

int db2_curator_invalidate_doc(const char *project, const char *file_path)
{
   (void)project;
   (void)file_path;
   return 0;
}

int db2_kb_doc_assets_delete_for_doc(const char *project, const char *document_key)
{
   (void)project;
   (void)document_key;
   return 0;
}

int db2_ontology_map(const char *novel, const char *target)
{
   (void)novel;
   (void)target;
   return 0;
}

int db2_sketch_minhash_signature_delete(const char *project, const char *file_path)
{
   (void)project;
   (void)file_path;
   return 0;
}

int db2_code_index_project_current_generation(const char *name, int64_t *generation_out)
{
   (void)name;
   if (generation_out)
      *generation_out = 0;
   return -1;
}

int64_t db2_code_projection_generation_create(const char *project)
{
   (void)project;
   return 0;
}

int64_t db2_code_projection_visible_id(const char *project)
{
   (void)project;
   return 0;
}

int64_t db2_kb_release_create(const char *name)
{
   (void)name;
   return 0;
}

int db2_css_migration_rules_doc(const char *exemplar_project, char *buf, size_t cap)
{
   (void)exemplar_project;
   if (buf && cap)
      buf[0] = '\0';
   return 0;
}

int db2_code_index_unique_file_basename(const char *project, const char *basename, char *out,
                                        size_t out_cap)
{
   (void)project;
   (void)basename;
   if (out && out_cap)
      out[0] = '\0';
   return 0;
}

int db2_kb_purge_fence_heartbeat(const char *project, const char *generation, const char *purge_id)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   return 0;
}

int db2_kb_purge_fence_clear(const char *project, const char *generation, const char *purge_id)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   return 0;
}

int db2_kb_documents_get_stored_hash(const char *project, const char *file_path, char *out,
                                     size_t out_len)
{
   (void)project;
   (void)file_path;
   if (out && out_len)
      out[0] = '\0';
   return -1;
}

int db2_kb_documents_hash_exists(const char *project, const char *file_hash, char *sample_path,
                                 size_t sample_path_len)
{
   (void)project;
   (void)file_hash;
   if (sample_path && sample_path_len)
      sample_path[0] = '\0';
   return 0;
}

int db2_kb_pdf_tsr_state(const char *project, const char *document_key, char *out, size_t out_len)
{
   (void)project;
   (void)document_key;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

int db2_memory_promotion_match_error_keys(const char *error_lowered, int64_t *ids_out, int max)
{
   (void)error_lowered;
   (void)ids_out;
   (void)max;
   return 0;
}

int db2_kb_documents_list_chunk_ids_for_file(const char *project, const char *file_path,
                                             int64_t *out, int max)
{
   (void)project;
   (void)file_path;
   (void)out;
   (void)max;
   return 0;
}

int db2_kb_service_list_memory_ids_by_updated(int limit, int64_t *ids, int max_ids)
{
   (void)limit;
   (void)ids;
   (void)max_ids;
   return 0;
}

int db2_memory_unit_list_ids(int64_t memory_id, int64_t *out, int max)
{
   (void)memory_id;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_list_retryable_index_failures(int max_attempts, int limit, int64_t *out, int max)
{
   (void)max_attempts;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_entity_edge_neighbors(const char *entity, db2_entity_neighbor_t *out, int max,
                              int limit_sql)
{
   (void)entity;
   (void)out;
   (void)max;
   (void)limit_sql;
   return 0;
}

int db2_entity_edge_neighbors_filtered(const char *entity, const char *rel_a, const char *rel_b,
                                       int order_by_weight, db2_entity_neighbor_t *out, int max,
                                       int limit_sql)
{
   (void)entity;
   (void)rel_a;
   (void)rel_b;
   (void)order_by_weight;
   (void)out;
   (void)max;
   (void)limit_sql;
   return 0;
}

int db2_entity_edge_outbound_neighbors(const char *source, db2_entity_neighbor_t *out, int max,
                                       int limit_sql)
{
   (void)source;
   (void)out;
   (void)max;
   (void)limit_sql;
   return 0;
}

int db2_entity_edge_top_partners_by_relation(const char *entity, const char *relation,
                                             db2_entity_neighbor_t *out, int max)
{
   (void)entity;
   (void)relation;
   (void)out;
   (void)max;
   return 0;
}

int db2_entity_edge_top_targets_by_relation(const char *source, const char *relation,
                                            db2_entity_neighbor_t *out, int max)
{
   (void)source;
   (void)relation;
   (void)out;
   (void)max;
   return 0;
}

int db2_code_index_file_definitions(const char *project, const char *file_path, definition_t *out,
                                    int max)
{
   (void)project;
   (void)file_path;
   (void)out;
   (void)max;
   return 0;
}

int canonical_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max)
{
   return db2_code_index_file_definitions(project, file_path, out, max);
}

int db2_code_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                               int max, int enrich)
{
   (void)query;
   (void)project;
   (void)out;
   (void)max;
   (void)enrich;
   return 0;
}

int canonical_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max, int enrich)
{
   return db2_code_index_code_search(query, project, out, max, enrich);
}

int db2_code_index_code_search_excluding_project(const char *query, const char *excluded_project,
                                                 code_search_hit_t *out, int max, int enrich)
{
   (void)query;
   (void)excluded_project;
   (void)out;
   (void)max;
   (void)enrich;
   return 0;
}

int canonical_index_code_search_excluding_project(const char *query, const char *excluded_project,
                                                  code_search_hit_t *out, int max, int enrich)
{
   return db2_code_index_code_search_excluding_project(query, excluded_project, out, max, enrich);
}

int db2_code_index_project_last_scan(char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   return 0;
}

int db2_kb_service_get_active_embedder_version(char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   return -1;
}

int db2_bandit_decision_points_list(char *buf, size_t len)
{
   if (buf && len >= 3)
   {
      buf[0] = '[';
      buf[1] = ']';
      buf[2] = '\0';
   }
   return 0;
}

int db2_corpus_pipeline_stage_counts(db2_corpus_pipeline_stage_count_t *out, int max_out)
{
   (void)out;
   (void)max_out;
   return 0;
}

int db2_memory_briefing_list_active_entities(db2_memory_briefing_entity_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db2_entity_edge_walk_step_typed(const char *node, db2_entity_edge_typed_t *out, int max)
{
   (void)node;
   (void)out;
   (void)max;
   return 0;
}

int db2_code_projection_generations_list(const char *project, code_projection_generation_row_t *out,
                                         int max)
{
   (void)project;
   (void)out;
   (void)max;
   return 0;
}

int db2_entity_edge_bump_utility(const char *key, double delta)
{
   (void)key;
   (void)delta;
   return 0;
}

int db2_bandit_decision_close(const char *id, double reward)
{
   (void)id;
   (void)reward;
   return 0;
}

int db2_entity_edge_neighbors_weighted(const char *entity, db2_entity_edge_weighted_neighbor_t *out,
                                       int max, int limit_sql, int utility_scoring_enabled)
{
   (void)entity;
   (void)out;
   (void)max;
   (void)limit_sql;
   (void)utility_scoring_enabled;
   return 0;
}

int db2_prospective_list(const char *state, memory_prospective_t *out, int max)
{
   (void)state;
   (void)out;
   (void)max;
   return 0;
}

int db2_prospective_list_armed(memory_prospective_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db2_prospective_list_by_entity(const char *entity_lc, memory_prospective_t *out, int max)
{
   (void)entity_lc;
   (void)out;
   (void)max;
   return 0;
}

int db2_prospective_list_by_file(const char *file, memory_prospective_t *out, int max)
{
   (void)file;
   (void)out;
   (void)max;
   return 0;
}

int db2_prospective_list_by_trigger_terms(const char *turn_text, memory_prospective_t *out, int max)
{
   (void)turn_text;
   (void)out;
   (void)max;
   return 0;
}

int db2_directive_list(const char *state, const char *cause, memory_directive_t *out, int max)
{
   (void)state;
   (void)cause;
   (void)out;
   (void)max;
   return 0;
}

int db2_directive_match_by_entity(const char *entity_lc, memory_directive_t *out, int max)
{
   (void)entity_lc;
   (void)out;
   (void)max;
   return 0;
}

int db2_directive_match_by_file(const char *file, memory_directive_t *out, int max)
{
   (void)file;
   (void)out;
   (void)max;
   return 0;
}

int db2_directive_match_by_lexical(const char *match_clause, memory_directive_t *out, int max)
{
   (void)match_clause;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_relations_for_entity(const char *entity, int limit, memory_relation_t *out, int max)
{
   (void)entity;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_relations_search(const char *query, int limit, memory_relation_t *out, int max)
{
   (void)query;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_relations_search_as_of(const char *query, const char *as_of, int limit,
                                      memory_relation_t *out, int max)
{
   (void)query;
   (void)as_of;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_relations_supporting(const char *entity_token, int limit, memory_relation_t *out,
                                    int max)
{
   (void)entity_token;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_entity_edge_list_by_entity(const char *entity, edge_t *out, int max)
{
   (void)entity;
   (void)out;
   (void)max;
   return 0;
}

int db2_entity_edge_search_by_token(const char *token, edge_t *out, int max, int limit_sql)
{
   (void)token;
   (void)out;
   (void)max;
   (void)limit_sql;
   return 0;
}

int db2_entity_edge_top_distinct_triples(edge_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db2_code_projection_list_edges(const char *project, code_projection_edge_t *out, int max)
{
   (void)project;
   (void)out;
   (void)max;
   return 0;
}

int db2_code_projection_list_edges_for_gen(int64_t gen_id, code_projection_edge_t *out, int max)
{
   (void)gen_id;
   (void)out;
   (void)max;
   return 0;
}

int db2_task_get_edges(int64_t task_id, task_edge_t *out, int max)
{
   (void)task_id;
   (void)out;
   (void)max;
   return 0;
}

int db2_code_index_term_find(const char *identifier, term_hit_t *out, int max)
{
   (void)identifier;
   (void)out;
   (void)max;
   return 0;
}

int canonical_index_find_project(const char *project, const char *identifier, term_hit_t *out,
                                 int max)
{
   (void)project;
   (void)identifier;
   (void)out;
   (void)max;
   return 0;
}

int canonical_index_find(const char *identifier, term_hit_t *out, int max)
{
   return canonical_index_find_project(NULL, identifier, out, max);
}

int canonical_index_find_excluding_project(const char *excluded_project, const char *identifier,
                                           term_hit_t *out, int max)
{
   (void)excluded_project;
   (void)identifier;
   (void)out;
   (void)max;
   return 0;
}

int db2_code_index_callers_find(const char *project, const char *symbol, caller_hit_t *out, int max)
{
   (void)project;
   (void)symbol;
   (void)out;
   (void)max;
   return 0;
}

int canonical_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max)
{
   (void)project;
   (void)symbol;
   (void)out;
   (void)max;
   return 0;
}

int canonical_index_find_callers_excluding_project(const char *excluded_project, const char *symbol,
                                                   caller_hit_t *out, int max)
{
   (void)excluded_project;
   (void)symbol;
   (void)out;
   (void)max;
   return 0;
}

int db2_rules_list(rule_t *out, int max_rules)
{
   (void)out;
   (void)max_rules;
   return 0;
}

int db2_rules_list_by_tier(int min_weight, rule_t *out, int max_rules)
{
   (void)min_weight;
   (void)out;
   (void)max_rules;
   return 0;
}

int db2_rules_list_hard(rule_t *out, int max_rules)
{
   (void)out;
   (void)max_rules;
   return 0;
}

int db2_anti_pattern_list(anti_pattern_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db2_anti_pattern_list_hot(int threshold, anti_pattern_t *out, int max)
{
   (void)threshold;
   (void)out;
   (void)max;
   return 0;
}

int db2_anti_pattern_check(const char *file_path, const char *command, anti_pattern_t *out, int max)
{
   (void)file_path;
   (void)command;
   (void)out;
   (void)max;
   return 0;
}

int db2_task_list(const char *state, const char *session_id, int limit, aimee_task_t *out, int max)
{
   (void)state;
   (void)session_id;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_task_get_subtasks(int64_t parent_id, aimee_task_t *out, int max)
{
   (void)parent_id;
   (void)out;
   (void)max;
   return 0;
}

int db2_typed_fact_recall(const char *subject, const char *relation_filter, typed_fact_t *out,
                          int max)
{
   (void)subject;
   (void)relation_filter;
   (void)out;
   (void)max;
   return 0;
}

int memory_lint_run(memory_lint_issue_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db2_decision_log_list(const char *outcome, int limit, db2_decision_log_row_t *out, int max)
{
   (void)outcome;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_decision_log_list_scoped(const char *subject, const char *status, int limit,
                                 db2_decision_log_row_t *out, int max)
{
   (void)subject;
   (void)status;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_list_global_constraints(db2_memory_kv_row_t *rows, int max)
{
   (void)rows;
   (void)max;
   return 0;
}

int db2_memory_list_kv_section(db2_memory_section_t section, db2_memory_kv_row_t *rows, int max)
{
   (void)section;
   (void)rows;
   (void)max;
   return 0;
}

int db2_memory_list_by_key(const char *key, db2_memory_id_content_row_t *out, int max)
{
   (void)key;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_session_id_content_list(const char *session_id, int limit,
                                       db2_memory_id_content_row_t *out, int max)
{
   (void)session_id;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_list_candidates(db2_memory_cand_filter_t filter, db2_memory_cand_row_t *rows,
                               int max)
{
   (void)filter;
   (void)rows;
   (void)max;
   return 0;
}

int db2_memory_list_recall_section(db2_memory_recall_section_t section, db2_memory_cand_row_t *rows,
                                   int max)
{
   (void)section;
   (void)rows;
   (void)max;
   return 0;
}

int db2_memory_l2_cross_key_pairs(int max_pairs, db2_memory_pair_row_t *out, int max)
{
   (void)max_pairs;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_l2_fact_vs_decision_pairs(int max_pairs, db2_memory_pair_row_t *out, int max)
{
   (void)max_pairs;
   (void)out;
   (void)max;
   return 0;
}

int db2_kb_service_directive_resolve(int64_t id, int64_t resolution_memory_id, const char *note)
{
   (void)id;
   (void)resolution_memory_id;
   (void)note;
   return 0;
}

int db2_memory_link_create(int64_t source_id, int64_t target_id, const char *relation)
{
   (void)source_id;
   (void)target_id;
   (void)relation;
   return 0;
}

int db2_task_add_edge(int64_t source, int64_t target, const char *relation)
{
   (void)source;
   (void)target;
   (void)relation;
   return 0;
}

int64_t db2_decision_log_active_id(const char *subject, int64_t linked_policy_id)
{
   (void)subject;
   (void)linked_policy_id;
   return 0;
}

int db2_entity_node_get(const char *node_key, db2_entity_node_t *out)
{
   (void)node_key;
   (void)out;
   return -1;
}

int db2_entity_node_alias_upsert(const char *alias, const char *node_key, const char *alias_kind,
                                 const char *project, int64_t generation_id)
{
   (void)alias;
   (void)node_key;
   (void)alias_kind;
   (void)project;
   (void)generation_id;
   return 0;
}

int db2_entity_edge_upsert(const char *source, const char *relation, const char *target,
                           int64_t window_id, int relation_id, int subject_kind, int object_kind,
                           int *out_added)
{
   (void)source;
   (void)relation;
   (void)target;
   (void)window_id;
   (void)relation_id;
   (void)subject_kind;
   (void)object_kind;
   if (out_added)
      *out_added = 0;
   return 0;
}

int db2_bandit_decision_insert(const char *id, const char *decision_point, const char *arm_id,
                               const char *context_hash, double propensity, int is_exploration)
{
   (void)id;
   (void)decision_point;
   (void)arm_id;
   (void)context_hash;
   (void)propensity;
   (void)is_exploration;
   return 0;
}

int db2_artifact_write(const char *id, const char *kind, const char *state, const char *scope_kind,
                       const char *scope_id, const char *operator_id, double confidence,
                       const char *payload_json)
{
   (void)id;
   (void)kind;
   (void)state;
   (void)scope_kind;
   (void)scope_id;
   (void)operator_id;
   (void)confidence;
   (void)payload_json;
   return 0;
}

int db2_artifact_write_ex(const char *id, const char *kind, const char *state,
                          const char *scope_kind, const char *scope_id, const char *operator_id,
                          double confidence, int attempt_count, const char *payload_json)
{
   (void)id;
   (void)kind;
   (void)state;
   (void)scope_kind;
   (void)scope_id;
   (void)operator_id;
   (void)confidence;
   (void)attempt_count;
   (void)payload_json;
   return 0;
}

int db2_artifact_target_surface(const char *id, char *out, int out_len)
{
   (void)id;
   if (out && out_len > 0)
      out[0] = '\0';
   return -1;
}

int db2_agent_outcome_record(const char *agent_name, const char *role, const char *outcome_kind,
                             const char *reason, int turns_used, int tools_called,
                             int64_t tokens_used, const char *tool_error_pattern)
{
   (void)agent_name;
   (void)role;
   (void)outcome_kind;
   (void)reason;
   (void)turns_used;
   (void)tools_called;
   (void)tokens_used;
   (void)tool_error_pattern;
   return 0;
}

int db2_artifact_reject(const char *id, const char *verdict_tag, const char *verdict_scope,
                        const char *counter_example, const char *before_json)
{
   (void)id;
   (void)verdict_tag;
   (void)verdict_scope;
   (void)counter_example;
   (void)before_json;
   return 0;
}

int db2_audit_event_write(const char *id, const char *source_artifact_id,
                          const char *target_surface, const char *target_id,
                          const char *operator_id, const char *scope_kind, const char *scope_id,
                          double applied_confidence, int flagged_for_review,
                          const char *before_json, const char *after_json)
{
   (void)id;
   (void)source_artifact_id;
   (void)target_surface;
   (void)target_id;
   (void)operator_id;
   (void)scope_kind;
   (void)scope_id;
   (void)applied_confidence;
   (void)flagged_for_review;
   (void)before_json;
   (void)after_json;
   return 0;
}

int db2_audit_read_latest_before(const char *artifact_id, char *out, int out_len)
{
   (void)artifact_id;
   if (out && out_len > 0)
      out[0] = '\0';
   return -1;
}

int db2_bandit_arm_stats_update(const char *decision_point, const char *arm_id, double reward_delta,
                                double posterior_alpha, double posterior_beta)
{
   (void)decision_point;
   (void)arm_id;
   (void)reward_delta;
   (void)posterior_alpha;
   (void)posterior_beta;
   return 0;
}

int db2_code_file_hash(const char *project, const char *file_path, char *out, int out_len)
{
   (void)project;
   (void)file_path;
   if (out && out_len > 0)
      out[0] = '\0';
   return -1;
}

int db2_code_index_file_modified_since(int64_t project_id, const char *rel_path, time_t mtime)
{
   (void)project_id;
   (void)rel_path;
   (void)mtime;
   return 1;
}

int64_t db2_code_index_file_upsert(int64_t project_id, const char *rel_path, const char *scanned_at)
{
   (void)project_id;
   (void)rel_path;
   (void)scanned_at;
   return 0;
}

void db2_code_index_op_record(int64_t point_id, const char *project, const char *node_key,
                              const char *file_path, int ok, const char *error_msg)
{
   (void)point_id;
   (void)project;
   (void)node_key;
   (void)file_path;
   (void)ok;
   (void)error_msg;
}

int64_t db2_code_index_project_upsert(const char *name, const char *root)
{
   (void)name;
   (void)root;
   return 0;
}

int db2_demotion_profile_read(const char *memory_class, const char *scope_kind,
                              const char *scope_id, char *buf, size_t len)
{
   (void)memory_class;
   (void)scope_kind;
   (void)scope_id;
   if (buf && len)
      buf[0] = '\0';
   return -1;
}

int db2_demotion_profile_write(const char *memory_class, const char *scope_kind,
                               const char *scope_id, const char *payload_json, char *id_out,
                               int id_out_len)
{
   (void)memory_class;
   (void)scope_kind;
   (void)scope_id;
   (void)payload_json;
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   return -1;
}

int db2_demotion_retrieval_attribution_write(const char *retrieval_event_id,
                                             int64_t surfaced_row_id, const char *verdict,
                                             double weight)
{
   (void)retrieval_event_id;
   (void)surfaced_row_id;
   (void)verdict;
   (void)weight;
   return 0;
}

int db2_css_render_snapshot_store(const char *project, const char *unit_path, const char *phase,
                                  const char *snapshot_json, const char *now_iso)
{
   (void)project;
   (void)unit_path;
   (void)phase;
   (void)snapshot_json;
   (void)now_iso;
   return 0;
}

int db2_entity_node_upsert(const char *node_key, int node_kind, const char *project,
                           const char *display_name, const char *full_key, const char *file_path,
                           const char *symbol, const char *node_origin, int64_t generation_id)
{
   (void)node_key;
   (void)node_kind;
   (void)project;
   (void)display_name;
   (void)full_key;
   (void)file_path;
   (void)symbol;
   (void)node_origin;
   (void)generation_id;
   return 0;
}

int db2_entity_profile_upsert(const char *entity_id, const char *canonical_name,
                              int observation_count, const char *card_json)
{
   (void)entity_id;
   (void)canonical_name;
   (void)observation_count;
   (void)card_json;
   return 0;
}

int db2_directive_resolve_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                        int64_t resolution_memory_id)
{
   (void)memory_a_id;
   (void)memory_b_id;
   (void)resolution_memory_id;
   return 0;
}

void db2_enrollment_touch_last_seen(const char *fingerprint, const char *scope)
{
   (void)fingerprint;
   (void)scope;
}

int db2_demotion_retrieval_event_by_turn(const char *turn_id, char *id_out, int id_out_len,
                                         char *payload_out, int payload_out_len)
{
   (void)turn_id;
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   if (payload_out && payload_out_len > 0)
      payload_out[0] = '\0';
   return -1;
}

int db2_kb_audit_append(const char *actor_role, const char *actor_principal, const char *action,
                        const char *subject, const char *verdict, const char *detail)
{
   (void)actor_role;
   (void)actor_principal;
   (void)action;
   (void)subject;
   (void)verdict;
   (void)detail;
   return 0;
}

int db2_feature_row_upsert(const char *subject_id, const char *subject_kind, const char *scope_kind,
                           const char *scope_id, const char *feature_set_version,
                           const char *features_json, const char *computed_at)
{
   (void)subject_id;
   (void)subject_kind;
   (void)scope_kind;
   (void)scope_id;
   (void)feature_set_version;
   (void)features_json;
   (void)computed_at;
   return 0;
}

int db2_feature_row_read(const char *subject_id, const char *subject_kind,
                         const char *feature_set_version, char *buf, size_t len)
{
   (void)subject_id;
   (void)subject_kind;
   (void)feature_set_version;
   if (buf && len)
      buf[0] = '\0';
   return -1;
}

int db2_kb_async_enqueue(const char *kind, int64_t document_id, const char *project)
{
   (void)kind;
   (void)document_id;
   (void)project;
   return 0;
}

int db2_console_oidc_get(db2_console_oidc_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_console_oidc_put(const db2_console_oidc_t *in)
{
   (void)in;
   return -1;
}

int db2_corpus_pipeline_status(db2_corpus_pipeline_stats_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_corpus_pipeline_drain(int limit, db2_corpus_pipeline_stats_t *out)
{
   (void)limit;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_cross_repo_set_trust(const char *project, const char *new_trust, const char *actor,
                             const char *request_id, char *prior_out, size_t prior_cap,
                             int *changed_out)
{
   (void)project;
   (void)new_trust;
   (void)actor;
   (void)request_id;
   if (prior_out && prior_cap)
      prior_out[0] = '\0';
   if (changed_out)
      *changed_out = 0;
   return -1;
}

int db2_bandit_explore_stats(const char *decision_point, int window_seconds,
                             long long *n_explore_out, long long *n_total_out)
{
   (void)decision_point;
   (void)window_seconds;
   if (n_explore_out)
      *n_explore_out = 0;
   if (n_total_out)
      *n_total_out = 0;
   return -1;
}

int db2_bandit_arm_stats_read(const char *decision_point, const char *arm_id,
                              db2_bandit_arm_stats_t *out)
{
   (void)decision_point;
   (void)arm_id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int canonical_index_project_stats(const char *project, int *files_out, int *defs_out)
{
   (void)project;
   if (files_out)
      *files_out = 0;
   if (defs_out)
      *defs_out = 0;
   return -1;
}

int db2_cross_repo_recompute_blocked_symbols(int k, int m, int len_min)
{
   (void)k;
   (void)m;
   (void)len_min;
   return -1;
}

int db2_artifact_write_evidence(const char *kind, const char *scope_kind, const char *scope_id,
                                const char *operator_id, const char *content_hash,
                                const char *payload_json, char *id_out, int id_out_len)
{
   (void)kind;
   (void)scope_kind;
   (void)scope_id;
   (void)operator_id;
   (void)content_hash;
   (void)payload_json;
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   return -1;
}

int db2_calibration_profile_write(const char *target_surface, const char *kind,
                                  const char *scope_kind, const char *scope_id,
                                  const char *feature_set_version, const char *payload_json,
                                  char *id_out, int id_out_len)
{
   (void)target_surface;
   (void)kind;
   (void)scope_kind;
   (void)scope_id;
   (void)feature_set_version;
   (void)payload_json;
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   return -1;
}

int db2_code_projection_generation_meta(int64_t gen_id, code_projection_generation_meta_t *out)
{
   (void)gen_id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int64_t db2_code_projection_sync_project(const char *project, int64_t gen_id)
{
   (void)project;
   (void)gen_id;
   return -1;
}

double db2_demotion_score(int64_t row_id, int window_size, double half_life_days, int n_min)
{
   (void)row_id;
   (void)window_size;
   (void)half_life_days;
   (void)n_min;
   return 0.0 / 0.0;
}

int db2_decision_log_get(int64_t id, db2_decision_log_row_t *out)
{
   (void)id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_fidelity_report_by_turn(const char *turn_id, char *status_out, int status_out_len,
                                int *supported_out, int *unsupported_out, int *abstained_out)
{
   (void)turn_id;
   if (status_out && status_out_len > 0)
      status_out[0] = '\0';
   if (supported_out)
      *supported_out = 0;
   if (unsupported_out)
      *unsupported_out = 0;
   if (abstained_out)
      *abstained_out = 0;
   return -1;
}

int db2_directive_counts_by_state(int64_t *open, int64_t *suppressed, int64_t *resolved,
                                  int64_t *expired)
{
   if (open)
      *open = 0;
   if (suppressed)
      *suppressed = 0;
   if (resolved)
      *resolved = 0;
   if (expired)
      *expired = 0;
   return -1;
}

int db2_kb_doc_read(int64_t id, db2_kb_doc_t *out)
{
   (void)id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_kb_doc_set_state(int64_t id, const char *state, int clear_review_needed,
                         const char *review_reason)
{
   (void)id;
   (void)state;
   (void)clear_review_needed;
   (void)review_reason;
   return -1;
}

int db2_enrollment_authority_resolve(const char *fingerprint, const char *cert_issuer,
                                     const char *cert_serial_norm, char out_authority[33])
{
   (void)fingerprint;
   (void)cert_issuer;
   (void)cert_serial_norm;
   if (out_authority)
      out_authority[0] = '\0';
   return -1;
}

int db2_feedback_record(const char *polarity, const char *title, const char *description,
                        int weight_override, int *reinforced)
{
   (void)polarity;
   (void)title;
   (void)description;
   (void)weight_override;
   if (reinforced)
      *reinforced = 0;
   return -1;
}

int db2_kb_file_index_get(const char *project, const char *file_path, char *hash_out,
                          size_t hash_cap, char *ingested_at_out, size_t ingested_at_cap)
{
   (void)project;
   (void)file_path;
   if (hash_out && hash_cap)
      hash_out[0] = '\0';
   if (ingested_at_out && ingested_at_cap)
      ingested_at_out[0] = '\0';
   return 0;
}

int db2_kb_ingest_queue_complete(int64_t job_id, int files_indexed, int chunks_added,
                                 int embeddings_added)
{
   (void)job_id;
   (void)files_indexed;
   (void)chunks_added;
   (void)embeddings_added;
   return -1;
}

int db2_kb_service_count_embeddings_for_version(const char *version)
{
   (void)version;
   return -1;
}

int db2_learning_proposals_settled_counts(int window_days, int64_t *committed, int64_t *terminal)
{
   (void)window_days;
   if (committed)
      *committed = 0;
   if (terminal)
      *terminal = 0;
   return -1;
}

int db2_kb_release_read(int64_t id, db2_kb_release_t *out)
{
   (void)id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_kb_release_promote(int64_t id)
{
   (void)id;
   return -1;
}

int db2_kb_release_rollback(int64_t target_id)
{
   (void)target_id;
   return -1;
}

int db2_learning_proposal_archive(int id, const char *reason)
{
   (void)id;
   (void)reason;
   return -1;
}

int db2_memory_lifecycle_get_state(int64_t memory_id, char *out, size_t out_len)
{
   (void)memory_id;
   if (out && out_len)
      out[0] = '\0';
   return -1;
}

int db2_memory_lifecycle_counts(db2_memory_lifecycle_counts_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_memory_lifecycle_mark_pending(int64_t memory_id, int ttl_days)
{
   (void)memory_id;
   (void)ttl_days;
   return -1;
}

int db2_memory_lifecycle_update_state(int64_t memory_id, const char *new_state,
                                      const char *archive_reason)
{
   (void)memory_id;
   (void)new_state;
   (void)archive_reason;
   return -1;
}

double db2_memory_get_salience(int64_t memory_id, double default_value)
{
   (void)memory_id;
   return default_value;
}

double db2_memory_get_surprise(int64_t memory_id, double default_value)
{
   (void)memory_id;
   return default_value;
}

int db2_memory_get_confidence_by_key(const char *key, double *confidence_out)
{
   (void)key;
   if (confidence_out)
      *confidence_out = 0.0;
   return 0;
}

int db2_memory_get_evidence_fields(int64_t memory_id, double *evidence, int *observations)
{
   (void)memory_id;
   if (evidence)
      *evidence = 0.0;
   if (observations)
      *observations = 0;
   return 0;
}

int db2_memory_get_state_fields(int64_t memory_id, int *has_valid_until, int *observations,
                                int *use_count)
{
   (void)memory_id;
   if (has_valid_until)
      *has_valid_until = 0;
   if (observations)
      *observations = 0;
   if (use_count)
      *use_count = 0;
   return 0;
}

int db2_memory_last_retro_scan(char *out, int out_len)
{
   if (out && out_len > 0)
      out[0] = '\0';
   return 0;
}

int db2_memory_find_conflicting_l2(const char *key, const char *content,
                                   double *existing_confidence_out)
{
   (void)key;
   (void)content;
   if (existing_confidence_out)
      *existing_confidence_out = 0.0;
   return 0;
}

int db2_mining_job_get(const char *id, db2_mining_job_row_t *out)
{
   (void)id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_mining_job_complete(const char *id, int64_t hwm, const char *error)
{
   (void)id;
   (void)hwm;
   (void)error;
   return -1;
}

void db2_prospective_count_by_state(int *armed_out, int *triggered_out, int *completed_out,
                                    int *expired_out)
{
   if (armed_out)
      *armed_out = 0;
   if (triggered_out)
      *triggered_out = 0;
   if (completed_out)
      *completed_out = 0;
   if (expired_out)
      *expired_out = 0;
}

long db2_ontology_eval_count(const char *rel_type)
{
   (void)rel_type;
   return -1;
}

int db2_memory_provenance_by_id(int64_t memory_id, char *kind_out, int kind_len, char *source_out,
                                int source_len, char *version_out, int version_len)
{
   (void)memory_id;
   if (kind_out && kind_len > 0)
      kind_out[0] = '\0';
   if (source_out && source_len > 0)
      source_out[0] = '\0';
   if (version_out && version_len > 0)
      version_out[0] = '\0';
   return -1;
}

int db2_memory_set_artifact(int64_t memory_id, const char *artifact_type, const char *artifact_ref,
                            const char *artifact_hash)
{
   (void)memory_id;
   (void)artifact_type;
   (void)artifact_ref;
   (void)artifact_hash;
   return 0;
}

int db2_memory_unit_active_meta(int64_t unit_id, double *weight_out, char *unit_type_out,
                                int unit_type_len, char *unit_kind_out, int unit_kind_len)
{
   (void)unit_id;
   if (weight_out)
      *weight_out = 0.0;
   if (unit_type_out && unit_type_len > 0)
      unit_type_out[0] = '\0';
   if (unit_kind_out && unit_kind_len > 0)
      unit_kind_out[0] = '\0';
   return 0;
}

int db2_anti_pattern_exists_exact(const char *pattern)
{
   (void)pattern;
   return 0;
}

int db2_anti_pattern_exists_by_source_ref(const char *source_ref)
{
   (void)source_ref;
   return 0;
}

int db2_artifact_citation_count(const char *artifact_id)
{
   (void)artifact_id;
   return 0;
}

int db2_learning_commits_in_last_7_days(const char *sink)
{
   (void)sink;
   return 0;
}

int db2_kb_service_memory_record_exists(int64_t record_id)
{
   (void)record_id;
   return 0;
}

int db2_kb_service_kb_document_exists(int64_t document_id)
{
   (void)document_id;
   return 0;
}

int db2_trace_mining_record(int64_t last_trace_id)
{
   (void)last_trace_id;
   return 0;
}

int db2_memory_collect_alias_matches(const char *term, int limit, memory_t *out, int max)
{
   (void)term;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_collect_entity_matches(const char *term, int limit, memory_t *out, int max)
{
   (void)term;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_collect_event_frame_matches(const char *term, int limit, memory_t *out, int max)
{
   (void)term;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_collect_relation_token_matches(const char *term, int limit, memory_t *out, int max)
{
   (void)term;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_collect_summary_matches(const char *term, int limit, memory_t *out, int max)
{
   (void)term;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_collect_temporal_matches(const char *term, int limit, memory_t *out, int max)
{
   (void)term;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

/* memory_t and the scope context are host types too. The adapter's scoped
 * identifier readers reach these; the tests below drive the backend directly. */
int db2_memory_top_l2_facts(memory_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_list_session_scope_priority(memory_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

void db2_memory_scope_context_set(const char *workspace, const char *project, int include_all)
{
   (void)workspace;
   (void)project;
   (void)include_all;
}

void db2_memory_scope_context_clear(void)
{
}

void db2_memory_scope_context_get(db2_memory_scope_context_t *out)
{
   (void)out;
}

/* memory_stats_t is a host type, so the production symbol takes void * here the
 * way db2_dim_change_reset does; these tests drive their own backend. */
int db2_memory_stats_counts(memory_stats_t *out)
{
   (void)out;
   return -1;
}

int db2_memory_promotion_delete_l0_provenance(void)
{
   return 0;
}

int db2_memory_promotion_delete_l0(void)
{
   return 0;
}

int db2_memory_promotion_list_kinds_in_tier(const char *tier, db2_memory_promotion_kind_t *out,
                                            int max)
{
   (void)tier;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_promotion_delete_stale_l1_provenance(const char *kind, const char *days_neg)
{
   (void)kind;
   (void)days_neg;
   return 0;
}

int db2_memory_promotion_delete_stale_l1(const char *kind, const char *days_neg)
{
   (void)kind;
   (void)days_neg;
   return 0;
}

int db2_kind_lifecycle_load(const char *kind, kind_lifecycle_t *out)
{
   (void)kind;
   (void)out;
   return -1;
}

static int delete_l0_provenance(void)
{
   expire_l0_provenance_calls++;
   return 0;
}

static int delete_l0(void)
{
   return expire_l0_value;
}

/* Both tier-cycle operations enumerate kinds through this one entry: expire
 * walks L1, demote walks L2. */
static int list_kinds_in_tier(const char *tier, char (*kinds)[16], int max)
{
   if (strcmp(tier, AIMEE_DB2_EXPIRE_STALE_TIER) != 0 && strcmp(tier, AIMEE_DB2_DEMOTE_TIER) != 0)
      return -1;
   if (expire_kind_count < 0 || expire_kind_count > max)
      return expire_kind_count;
   for (int index = 0; index < expire_kind_count; index++)
      snprintf(kinds[index], sizeof(kinds[index]), "kind%d", index);
   return expire_kind_count;
}

static int kind_expire_days(const char *kind)
{
   (void)kind;
   return expire_days_value;
}

static int delete_stale_l1_provenance(const char *kind, const char *days_neg)
{
   (void)kind;
   expire_stale_provenance_calls++;
   snprintf(expire_last_window, sizeof(expire_last_window), "%s", days_neg);
   return 0;
}

static int delete_stale_l1(const char *kind, const char *days_neg)
{
   (void)kind;
   (void)days_neg;
   return expire_stale_value;
}

int db2_memory_promotion_demote_kind(const char *ts, const char *kind, double confidence,
                                     const char *days_neg)
{
   (void)ts;
   (void)kind;
   (void)confidence;
   (void)days_neg;
   return 0;
}

int db2_memory_promotion_demote_cascade(const char *ts)
{
   (void)ts;
   return 0;
}

static void contract_now_utc(char *buf, size_t len)
{
   snprintf(buf, len, "%s", "2026-08-18T09:00:00Z");
}

static int kind_demote_policy(const char *kind, double *confidence, int *days)
{
   (void)kind;
   *confidence = 0.6;
   *days = demote_days_value;
   return demote_policy_result;
}

static int demote_kind(const char *ts, const char *kind, double confidence, const char *days_neg)
{
   (void)kind;
   (void)confidence;
   snprintf(demote_kind_stamp, sizeof(demote_kind_stamp), "%s", ts);
   snprintf(expire_last_window, sizeof(expire_last_window), "%s", days_neg);
   return demote_kind_value;
}

static int demote_cascade(const char *ts)
{
   demote_cascade_calls++;
   snprintf(demote_cascade_stamp, sizeof(demote_cascade_stamp), "%s", ts);
   return demote_cascade_value;
}

int db2_memory_promotion_promote_stable_l2_to_l3(const char *ts)
{
   (void)ts;
   return 0;
}

int db2_memory_promotion_reclassify_directives(int require_approval)
{
   (void)require_approval;
   return 0;
}

int db2_memory_promotion_record_l4_approval(int64_t memory_id, const char *approver,
                                            const char *note)
{
   (void)memory_id;
   (void)approver;
   (void)note;
   return -1;
}

static int record_l4_approval(int64_t memory_id, const char *approver, const char *note)
{
   approval_calls++;
   approval_last_id = memory_id;
   snprintf(approval_last_approver, sizeof(approval_last_approver), "%s", approver);
   snprintf(approval_last_note, sizeof(approval_last_note), "%s", note);
   return approval_result;
}

static int reclassify_directives(int require_approval)
{
   reclassify_calls++;
   reclassify_last_gate = require_approval;
   return reclassify_value;
}

static int promote_stable(const char *ts)
{
   promote_stable_calls++;
   snprintf(promote_stable_stamp, sizeof(promote_stable_stamp), "%s", ts);
   return promote_stable_value;
}

static int stats_counts(aimee_db2_memory_stats_t *stats)
{
   stats_counts_calls++;
   if (stats_counts_result != 0)
      return stats_counts_result;
   *stats = (aimee_db2_memory_stats_t){
       .tier_counts = {3, 12, 30, 8, 2, 1},
       .kind_counts = {14, 5, 6, 9, 4, 3, 2, 1, 7, stats_counts_last_kind},
       .total = 56,
       .conflicts = 4,
   };
   return 0;
}

static int health_counters(int promote_use_count, double promote_confidence,
                           aimee_db2_health_counters_t *counters)
{
   health_counters_calls++;
   health_counters_use_count = promote_use_count;
   health_counters_confidence = promote_confidence;
   if (health_counters_result != 0)
      return health_counters_result;
   *counters = (aimee_db2_health_counters_t){
       .cycles = health_counters_cycles,
       .total_contradictions = 13,
       .total_promotions = 5,
       .total_demotions = 2,
       .total_expirations = 4,
       .new_memories = 21,
       .l1_eligible = 9,
       .l2_total = 30,
       .l2_stale_30_days = 6,
   };
   return 0;
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

int db2_dim_change_reset(int target_dim, int force, int dry_run, db2_reembed_plan_t *out)
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
   prune_orphaned_l0_value = 3;
   prune_orphaned_l0_calls = 0;
   lifecycle_sweep_value = 4;
   lifecycle_sweep_calls = 0;
   demote_id_value = 1;
   demote_id_calls = 0;
   demote_id_last = 0;
   workspace_tag_value = 1;
   workspace_tag_calls = 0;
   workspace_tag_last = 0;
   delete_row_value = 1;
   delete_row_calls = 0;
   delete_row_last = 0;
   touch_value = 0;
   touch_calls = 0;
   touch_last = 0;
   link_delete_value = 0;
   link_delete_calls = 0;
   link_delete_last = 0;
   valid_at_value = 1;
   valid_at_calls = 0;
   valid_at_last[0] = '\0';
   scope_type_value = 1;
   scope_type_calls = 0;
   scope_type_last[0] = '\0';
   reject_value = 0;
   reject_calls = 0;
   reject_last = 0;
   update_content_value = 1;
   update_content_calls = 0;
   decay_confidence_calls = 0;
   decay_confidence_last = 0;
   workspace_tag_insert_calls = 0;
   workspace_tag_insert_last[0] = '\0';
   cognified_kind_calls = 0;
   cognified_kind_last[0] = '\0';
   source_session_calls = 0;
   source_session_last[0] = '\0';
   negation_tokens_calls = 0;
   negation_tokens_last[0] = '\0';
   get_content_hit = 1;
   get_content_calls = 0;
   get_source_session_rc = 0;
   get_source_session_calls = 0;
   temporal_ref_hit = 1;
   temporal_ref_calls = 0;
   corpus_stat_rc = 1;
   corpus_stat_count = 7;
   corpus_stat_calls = 0;
   edge_prune_value = 2;
   edge_prune_calls = 0;
   edge_normalize_value = 3;
   edge_normalize_calls = 0;
   project_count_value = 4;
   project_count_calls = 0;
   purge_pollution_value = 5;
   purge_pollution_calls = 0;
   requeue_drifted_value = 6;
   requeue_drifted_calls = 0;
   rebuild_routes_value = 15;
   rebuild_routes_calls = 0;
   rebuild_identities_value = 16;
   rebuild_identities_calls = 0;
   rebuild_build_deps_value = 17;
   rebuild_build_deps_calls = 0;
   drift_candidates_value = 20;
   drift_candidates_calls = 0;
   rules_decay_value = 18;
   rules_decay_calls = 0;
   curiosity_rescore_value = 19;
   curiosity_rescore_calls = 0;
   mining_seed_value = 0;
   mining_seed_calls = 0;
   proposals_archive_calls = 0;
   trace_watermark_value = 22;
   trace_watermark_calls = 0;
   rel_types_seed_value = 0;
   rel_types_seed_calls = 0;
   lock_acquire_value = 1;
   lock_acquire_calls = 0;
   lock_release_calls = 0;
   release_active_value = 21;
   release_active_calls = 0;
   prospective_sweep_value = 7;
   prospective_sweep_calls = 0;
   directive_sweep_value = 8;
   directive_sweep_calls = 0;
   directive_suppress_value = 0;
   directive_suppress_calls = 0;
   directive_suppress_id = 0;
   directive_surface_value = 0;
   directive_surface_calls = 0;
   anti_pattern_bump_value = 0;
   anti_pattern_bump_calls = 0;
   anti_pattern_bump_seen = 0;
   anti_pattern_delete_value = 0;
   anti_pattern_delete_calls = 0;
   anti_pattern_delete_seen = 0;
   doc_delete_value = 0;
   doc_delete_calls = 0;
   doc_delete_seen = 0;
   task_delete_value = 0;
   task_delete_calls = 0;
   task_delete_seen = 0;
   file_index_delete_project_value = 51;
   file_index_delete_project_calls = 0;
   file_index_delete_project_seen[0] = '\0';
   clear_project_value = 52;
   clear_project_calls = 0;
   clear_project_seen[0] = '\0';
   clear_current_project_value = 53;
   clear_current_project_calls = 0;
   clear_current_project_seen[0] = '\0';
   directive_surface_id = 0;
   mark_revisit_value = 9;
   mark_revisit_calls = 0;
   queue_reset_value = 10;
   queue_reset_calls = 0;
   evidence_reembed_value = 11;
   evidence_reembed_calls = 0;
   curator_reembed_value = 12;
   curator_reembed_calls = 0;
   synth_reenqueue_value = 13;
   synth_reenqueue_calls = 0;
   extract_reenqueue_value = 14;
   extract_reenqueue_calls = 0;
   snprintf(corpus_stat_stamp, sizeof(corpus_stat_stamp), "%s", "2026-08-19 09:00:00");
   snprintf(temporal_ref_value, sizeof(temporal_ref_value), "%s", "2026-08-19");
   snprintf(get_source_session_value, sizeof(get_source_session_value), "%s", "sess-1");
   snprintf(get_content_value, sizeof(get_content_value), "%s", "stored text");
   update_content_last[0] = '\0';
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
   prune_health_value = 11;
   prune_health_days = 0;
   prune_contradictions_value = 3;
   prune_contradictions_days = 0;
   health_counters_result = 0;
   health_counters_calls = 0;
   health_counters_use_count = 0;
   health_counters_confidence = 0.0;
   health_counters_cycles = 7;
   stats_counts_result = 0;
   stats_counts_calls = 0;
   stats_counts_last_kind = 5;
   expire_l0_value = 9;
   expire_kind_count = 2;
   expire_days_value = 7;
   expire_stale_value = 4;
   expire_l0_provenance_calls = 0;
   expire_stale_provenance_calls = 0;
   expire_last_window[0] = '\0';
   demote_policy_result = 0;
   demote_days_value = 14;
   demote_kind_value = 3;
   demote_cascade_value = 2;
   demote_cascade_calls = 0;
   demote_kind_stamp[0] = '\0';
   demote_cascade_stamp[0] = '\0';
   promote_stable_value = 4;
   promote_stable_calls = 0;
   promote_stable_stamp[0] = '\0';
   reclassify_value = 3;
   reclassify_calls = 0;
   reclassify_last_gate = -1;
   approval_result = 0;
   approval_calls = 0;
   approval_last_id = 0;
   approval_last_approver[0] = '\0';
   approval_last_note[0] = '\0';
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
   transport_expect_health_retention = 0;
   transport_expect_health_counters = 0;
   transport_expect_stats_counts = 0;
   transport_expect_expire = 0;
   transport_expect_demote = 0;
   transport_expect_promote_stable = 0;
   transport_expect_reclassify = 0;
   transport_expect_approval = 0;
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
       transport_expect_approval                  ? AIMEE_DB2_EVENT_RECORD_L4_APPROVAL
       : transport_expect_reclassify              ? AIMEE_DB2_EVENT_RECLASSIFY_DIRECTIVES
       : transport_expect_promote_stable          ? AIMEE_DB2_EVENT_PROMOTE_STABLE
       : transport_expect_demote                  ? AIMEE_DB2_EVENT_DEMOTE
       : transport_expect_expire                  ? AIMEE_DB2_EVENT_EXPIRE
       : transport_expect_stats_counts            ? AIMEE_DB2_EVENT_STATS_COUNTS
       : transport_expect_health_counters         ? AIMEE_DB2_EVENT_HEALTH_COUNTERS
       : transport_expect_health_retention        ? AIMEE_DB2_EVENT_HEALTH_RETENTION
       : transport_expect_health_record           ? AIMEE_DB2_EVENT_HEALTH_RECORD
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
       transport_expect_approval                  ? AIMEE_DB2_STAGE_RECORD_L4_APPROVAL
       : transport_expect_reclassify              ? AIMEE_DB2_STAGE_RECLASSIFY_DIRECTIVES
       : transport_expect_promote_stable          ? AIMEE_DB2_STAGE_PROMOTE_STABLE
       : transport_expect_demote                  ? AIMEE_DB2_STAGE_DEMOTE
       : transport_expect_expire                  ? AIMEE_DB2_STAGE_EXPIRE
       : transport_expect_stats_counts            ? AIMEE_DB2_STAGE_STATS_COUNTS
       : transport_expect_health_counters         ? AIMEE_DB2_STAGE_HEALTH_COUNTERS
       : transport_expect_health_retention        ? AIMEE_DB2_STAGE_HEALTH_RETENTION
       : transport_expect_health_record           ? AIMEE_DB2_STAGE_HEALTH_RECORD
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
   if (transport_expect_approval)
   {
      uint64_t id = 0;
      char who[64] = "", what[512] = "";
      assert(aimee_db2_record_l4_approval_request_decode(request_body, request_len, &id, who,
                                                         sizeof(who), what, sizeof(what)) == 0);
      assert(id == 42u && strcmp(who, "operator") == 0 && strcmp(what, "reviewed") == 0);
   }
   else if (transport_expect_reclassify)
   {
      uint32_t gate = 99;
      assert(aimee_db2_reclassify_directives_request_decode(request_body, request_len, &gate) == 0);
      assert(gate == 1u);
   }
   else if (transport_expect_promote_stable)
      assert(aimee_db2_promote_stable_request_decode(request_body, request_len) == 0);
   else if (transport_expect_demote)
      assert(aimee_db2_demote_request_decode(request_body, request_len) == 0);
   else if (transport_expect_expire)
      assert(aimee_db2_expire_request_decode(request_body, request_len) == 0);
   else if (transport_expect_stats_counts)
      assert(aimee_db2_stats_counts_request_decode(request_body, request_len) == 0);
   else if (transport_expect_health_counters)
      assert(aimee_db2_health_counters_request_decode(request_body, request_len) == 0);
   else if (transport_expect_health_retention)
      assert(aimee_db2_health_retention_request_decode(request_body, request_len) == 0);
   else if (transport_expect_health_record)
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

static void test_health_retention_wire(void)
{
   uint8_t request[AIMEE_DB2_HEALTH_RETENTION_REQUEST_LEN] = {0};
   assert(aimee_db2_health_retention_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_health_retention_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_health_retention_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_HEALTH_RETENTION_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, snapshots = 99, contradictions = 99;
   assert(aimee_db2_health_retention_reply_encode(11u, 3u, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_health_retention_reply_decode(reply, reply_len, &snapshots, &contradictions) ==
          0);
   assert(snapshots == 11 && contradictions == 3);

   /* Both halves are reported, and both are bounded independently. */
   assert(aimee_db2_health_retention_reply_encode(AIMEE_DB2_HEALTH_RETENTION_MAX + 1u, 3u, reply,
                                                  sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_health_retention_reply_encode(11u, AIMEE_DB2_HEALTH_RETENTION_MAX + 1u, reply,
                                                  sizeof(reply), &reply_len) == -1);
   assert(reply_len == 0);
   assert(aimee_db2_health_retention_reply_encode(11u, 3u, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u, 0x80000000u);
   assert(aimee_db2_health_retention_reply_decode(reply, reply_len, &snapshots, &contradictions) ==
          -1);
   assert(snapshots == 0 && contradictions == 0);
}

static void test_health_counters_wire(void)
{
   uint64_t confidence_bits = 0;
   double confidence = AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_CONFIDENCE;
   memcpy(&confidence_bits, &confidence, sizeof(confidence_bits));
   assert(confidence_bits == 0x3feccccccccccccdULL);
   assert(AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_USE_COUNT == 3);

   uint8_t request[AIMEE_DB2_HEALTH_COUNTERS_REQUEST_LEN] = {0};
   assert(aimee_db2_health_counters_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_health_counters_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_health_counters_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   const aimee_db2_health_counters_t counters = {
       .cycles = 7,
       .total_contradictions = 13,
       .total_promotions = 5,
       .total_demotions = 2,
       .total_expirations = 4,
       .new_memories = 21,
       .l1_eligible = 9,
       .l2_total = 30,
       .l2_stale_30_days = 6,
   };
   aimee_db2_health_counters_t decoded = {0};
   assert(aimee_db2_health_counters_reply_encode(&counters, reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN);
   assert(aimee_db2_health_counters_reply_decode(reply, reply_len, &decoded) == 0);
   assert(memcmp(&decoded, &counters, sizeof(decoded)) == 0);

   /* Every counter is bounded, including the last field on the wire. */
   aimee_db2_health_counters_t overflow = counters;
   overflow.cycles = AIMEE_DB2_HEALTH_COUNTERS_MAX + 1u;
   assert(aimee_db2_health_counters_reply_encode(&overflow, reply, sizeof(reply), &reply_len) ==
          -1);
   overflow = counters;
   overflow.l2_stale_30_days = AIMEE_DB2_HEALTH_COUNTERS_MAX + 1u;
   assert(aimee_db2_health_counters_reply_encode(&overflow, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(reply_len == 0);
   assert(aimee_db2_health_counters_reply_encode(&counters, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN + 32u, 0x80000000u);
   assert(aimee_db2_health_counters_reply_decode(reply, reply_len, &decoded) == -1);
   assert(decoded.cycles == 0 && decoded.l2_stale_30_days == 0);
}

static void test_stats_counts_wire(void)
{
   uint8_t request[AIMEE_DB2_STATS_COUNTS_REQUEST_LEN] = {0};
   assert(aimee_db2_stats_counts_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_stats_counts_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_stats_counts_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   const aimee_db2_memory_stats_t stats = {
       .tier_counts = {3, 12, 30, 8, 2, 1},
       .kind_counts = {14, 5, 6, 9, 4, 3, 2, 1, 7, 5},
       .total = 56,
       .conflicts = 4,
   };
   aimee_db2_memory_stats_t decoded = {0};
   assert(aimee_db2_stats_counts_reply_encode(&stats, reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN);
   assert(aimee_db2_stats_counts_reply_decode(reply, reply_len, &decoded) == 0);
   assert(memcmp(&decoded, &stats, sizeof(decoded)) == 0);

   /* The last kind bucket is the one a short backend mapping would drop; it
    * must survive a round trip like any other. */
   assert(decoded.kind_counts[AIMEE_DB2_STATS_COUNTS_KINDS - 1u] == 5);

   /* Every bucket is bounded, at both ends of the payload. */
   aimee_db2_memory_stats_t overflow = stats;
   overflow.tier_counts[0] = AIMEE_DB2_STATS_COUNTS_MAX + 1u;
   assert(aimee_db2_stats_counts_reply_encode(&overflow, reply, sizeof(reply), &reply_len) == -1);
   overflow = stats;
   overflow.kind_counts[AIMEE_DB2_STATS_COUNTS_KINDS - 1u] = AIMEE_DB2_STATS_COUNTS_MAX + 1u;
   assert(aimee_db2_stats_counts_reply_encode(&overflow, reply, sizeof(reply), &reply_len) == -1);
   overflow = stats;
   overflow.conflicts = AIMEE_DB2_STATS_COUNTS_MAX + 1u;
   assert(aimee_db2_stats_counts_reply_encode(&overflow, reply, sizeof(reply), &reply_len) == -1);
   assert(reply_len == 0);

   assert(aimee_db2_stats_counts_reply_encode(&stats, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN +
                         4u * (AIMEE_DB2_STATS_COUNTS_TIERS + AIMEE_DB2_STATS_COUNTS_KINDS - 1u),
                     0x80000000u);
   assert(aimee_db2_stats_counts_reply_decode(reply, reply_len, &decoded) == -1);
   assert(decoded.total == 0 && decoded.kind_counts[0] == 0);
}

static void test_expire_wire(void)
{
   uint8_t request[AIMEE_DB2_EXPIRE_REQUEST_LEN] = {0};
   assert(aimee_db2_expire_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_expire_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_expire_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_EXPIRE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, level0 = 99, stale = 99;
   assert(aimee_db2_expire_reply_encode(9u, 17u, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_expire_reply_decode(reply, reply_len, &level0, &stale) == 0);
   assert(level0 == 9 && stale == 17);

   /* Both stages are reported, and each is bounded independently. */
   assert(aimee_db2_expire_reply_encode(AIMEE_DB2_EXPIRE_MAX + 1u, 17u, reply, sizeof(reply),
                                        &reply_len) == -1);
   assert(aimee_db2_expire_reply_encode(9u, AIMEE_DB2_EXPIRE_MAX + 1u, reply, sizeof(reply),
                                        &reply_len) == -1);
   assert(reply_len == 0);
   assert(aimee_db2_expire_reply_encode(9u, 17u, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN + 4u, 0x80000000u);
   assert(aimee_db2_expire_reply_decode(reply, reply_len, &level0, &stale) == -1);
   assert(level0 == 0 && stale == 0);
}

static void test_demote_wire(void)
{
   uint8_t request[AIMEE_DB2_DEMOTE_REQUEST_LEN] = {0};
   assert(aimee_db2_demote_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_demote_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_demote_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_DEMOTE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, demoted = 99, cascaded = 99;
   assert(aimee_db2_demote_reply_encode(6u, 2u, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_demote_reply_decode(reply, reply_len, &demoted, &cascaded) == 0);
   assert(demoted == 6 && cascaded == 2);

   /* An idle cycle is a legal reply. */
   assert(aimee_db2_demote_reply_encode(0u, 0u, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_demote_reply_decode(reply, reply_len, &demoted, &cascaded) == 0 &&
          demoted == 0 && cascaded == 0);

   /* A cascade without a demotion contradicts the invariant, on both sides. */
   assert(aimee_db2_demote_reply_encode(0u, 1u, reply, sizeof(reply), &reply_len) == -1);
   assert(reply_len == 0);
   assert(aimee_db2_demote_reply_encode(6u, 2u, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 0u);
   assert(aimee_db2_demote_reply_decode(reply, reply_len, &demoted, &cascaded) == -1);
   assert(demoted == 0 && cascaded == 0);

   /* Each count is bounded independently. */
   assert(aimee_db2_demote_reply_encode(AIMEE_DB2_DEMOTE_MAX + 1u, 2u, reply, sizeof(reply),
                                        &reply_len) == -1);
   assert(aimee_db2_demote_reply_encode(6u, AIMEE_DB2_DEMOTE_MAX + 1u, reply, sizeof(reply),
                                        &reply_len) == -1);
   assert(reply_len == 0);
}

static void test_promote_stable_wire(void)
{
   /* The stability policy is fixed, so the wire pins the confidence floor. */
   uint64_t confidence_bits = 0;
   double confidence = AIMEE_DB2_PROMOTE_STABLE_CONFIDENCE;
   memcpy(&confidence_bits, &confidence, sizeof(confidence_bits));
   assert(confidence_bits == 0x3fee666666666666ULL);
   assert(AIMEE_DB2_PROMOTE_STABLE_USE_COUNT == 5 && AIMEE_DB2_PROMOTE_STABLE_DAYS == 30);
   assert(strcmp(AIMEE_DB2_PROMOTE_STABLE_SOURCE_TIER, "L2") == 0);
   assert(strcmp(AIMEE_DB2_PROMOTE_STABLE_TARGET_TIER, "L3") == 0);

   uint8_t request[AIMEE_DB2_PROMOTE_STABLE_REQUEST_LEN] = {0};
   assert(aimee_db2_promote_stable_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_promote_stable_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_promote_stable_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_PROMOTE_STABLE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, promoted = 99;
   assert(aimee_db2_promote_stable_reply_encode(4u, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_promote_stable_reply_decode(reply, reply_len, &promoted) == 0 && promoted == 4);
   assert(aimee_db2_promote_stable_reply_encode(AIMEE_DB2_PROMOTE_STABLE_MAX + 1u, reply,
                                                sizeof(reply), &reply_len) == -1);
   assert(reply_len == 0);
}

static void test_reclassify_directives_wire(void)
{
   assert(strcmp(AIMEE_DB2_RECLASSIFY_DIRECTIVES_SOURCE_TIER, "L3") == 0);
   assert(strcmp(AIMEE_DB2_RECLASSIFY_DIRECTIVES_TARGET_TIER, "L4") == 0);
   assert(strcmp(AIMEE_DB2_RECLASSIFY_DIRECTIVES_GATED_KIND, "policy") == 0);

   uint8_t request[AIMEE_DB2_RECLASSIFY_DIRECTIVES_REQUEST_LEN] = {0};
   uint32_t gate = 99;
   /* Both gate settings are canonical requests. */
   assert(aimee_db2_reclassify_directives_request_encode(1u, request, sizeof(request)) == 0);
   assert(aimee_db2_reclassify_directives_request_decode(request, sizeof(request), &gate) == 0 &&
          gate == 1u);
   assert(aimee_db2_reclassify_directives_request_encode(0u, request, sizeof(request)) == 0);
   assert(aimee_db2_reclassify_directives_request_decode(request, sizeof(request), &gate) == 0 &&
          gate == 0u);

   /* The gate is a boolean, so anything wider is refused on both sides. */
   assert(aimee_db2_reclassify_directives_request_encode(
              AIMEE_DB2_RECLASSIFY_DIRECTIVES_GATE_MAX + 1u, request, sizeof(request)) == -1);
   assert(aimee_db2_reclassify_directives_request_encode(1u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + AIMEE_DB2_ENVELOPE_HEADER_LEN, 2u);
   assert(aimee_db2_reclassify_directives_request_decode(request, sizeof(request), &gate) == -1);
   assert(gate == 0u);
   assert(aimee_db2_reclassify_directives_request_encode(1u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12u, 1u);
   assert(aimee_db2_reclassify_directives_request_decode(request, sizeof(request), &gate) == -1);

   uint8_t reply[AIMEE_DB2_RECLASSIFY_DIRECTIVES_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, reclassified = 99;
   assert(aimee_db2_reclassify_directives_reply_encode(3u, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_reclassify_directives_reply_decode(reply, reply_len, &reclassified) == 0 &&
          reclassified == 3);
   assert(aimee_db2_reclassify_directives_reply_encode(AIMEE_DB2_RECLASSIFY_DIRECTIVES_MAX + 1u,
                                                       reply, sizeof(reply), &reply_len) == -1);
   assert(reply_len == 0);
}

static void test_prune_orphaned_l0_wire(void)
{
   /* The retention window and tier are compiled-in policy, so they must be
    * exactly what the reviewed catalog declares and must never be encoded. */
   assert(strcmp(AIMEE_DB2_PRUNE_ORPHANED_L0_TIER, "L0") == 0);
   assert(strcmp(AIMEE_DB2_PRUNE_ORPHANED_L0_MAX_AGE, "-7 days") == 0);

   uint8_t request[AIMEE_DB2_PRUNE_ORPHANED_L0_REQUEST_LEN] = {0};
   assert(aimee_db2_prune_orphaned_l0_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_prune_orphaned_l0_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_prune_orphaned_l0_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_PRUNE_ORPHANED_L0_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, deleted = 99;
   assert(aimee_db2_prune_orphaned_l0_reply_encode(3, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_prune_orphaned_l0_reply_decode(reply, reply_len, &deleted) == 0 &&
          deleted == 3);
   assert(aimee_db2_prune_orphaned_l0_reply_encode(AIMEE_DB2_PRUNE_ORPHANED_L0_COUNT_MAX + 1u,
                                                   reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_prune_orphaned_l0_reply_encode(3, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_prune_orphaned_l0_reply_encode(3, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_prune_orphaned_l0_reply_decode(reply, reply_len, &deleted) == -1 &&
          deleted == 0);
}

static void test_curator_reenqueue_extract_all_wire(void)
{
   uint8_t request[AIMEE_DB2_CURATOR_REENQUEUE_EXTRACT_ALL_REQUEST_LEN] = {0};
   assert(aimee_db2_curator_reenqueue_extract_all_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_curator_reenqueue_extract_all_request_decode(request, sizeof(request)) == 0);
   /* Eighth maintenance operation, so the seven before it must refuse it. */
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_mark_revisit_due_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_ingest_queue_reset_running_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_evidence_reembed_all_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_curator_reembed_all_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_synth_reenqueue_all_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_curator_reenqueue_extract_all_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_CURATOR_REENQUEUE_EXTRACT_ALL_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, jobs = 99;
   assert(aimee_db2_curator_reenqueue_extract_all_reply_encode(14, reply, sizeof(reply),
                                                               &reply_len) == 0);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_decode(reply, reply_len, &jobs) == 0 &&
          jobs == 14);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_encode(0, reply, sizeof(reply),
                                                               &reply_len) == 0);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_decode(reply, reply_len, &jobs) == 0 &&
          jobs == 0);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_encode(
              AIMEE_DB2_CURATOR_REENQUEUE_EXTRACT_ALL_MAX + 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_encode(14, reply, sizeof(reply) - 1,
                                                               &reply_len) == -1);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_encode(14, reply, sizeof(reply),
                                                               &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_decode(reply, reply_len, &jobs) == -1 &&
          jobs == 0);
}

static void test_synth_reenqueue_all_wire(void)
{
   uint8_t request[AIMEE_DB2_SYNTH_REENQUEUE_ALL_REQUEST_LEN] = {0};
   assert(aimee_db2_synth_reenqueue_all_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_synth_reenqueue_all_request_decode(request, sizeof(request)) == 0);
   /* Seventh maintenance operation, so the six before it must refuse it. */
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_mark_revisit_due_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_ingest_queue_reset_running_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_evidence_reembed_all_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_curator_reembed_all_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_synth_reenqueue_all_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_SYNTH_REENQUEUE_ALL_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, ops = 99;
   assert(aimee_db2_synth_reenqueue_all_reply_encode(13, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_synth_reenqueue_all_reply_decode(reply, reply_len, &ops) == 0 && ops == 13);
   assert(aimee_db2_synth_reenqueue_all_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_synth_reenqueue_all_reply_decode(reply, reply_len, &ops) == 0 && ops == 0);
   assert(aimee_db2_synth_reenqueue_all_reply_encode(AIMEE_DB2_SYNTH_REENQUEUE_ALL_MAX + 1u, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_synth_reenqueue_all_reply_encode(13, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_synth_reenqueue_all_reply_encode(13, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_synth_reenqueue_all_reply_decode(reply, reply_len, &ops) == -1 && ops == 0);
}

static void test_curator_reembed_all_wire(void)
{
   uint8_t request[AIMEE_DB2_CURATOR_REEMBED_ALL_REQUEST_LEN] = {0};
   assert(aimee_db2_curator_reembed_all_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_curator_reembed_all_request_decode(request, sizeof(request)) == 0);
   /* Sixth maintenance operation, so the five before it must refuse it. */
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_mark_revisit_due_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_ingest_queue_reset_running_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_evidence_reembed_all_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_curator_reembed_all_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_CURATOR_REEMBED_ALL_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, demoted = 99;
   assert(aimee_db2_curator_reembed_all_reply_encode(12, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_curator_reembed_all_reply_decode(reply, reply_len, &demoted) == 0 &&
          demoted == 12);
   assert(aimee_db2_curator_reembed_all_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_curator_reembed_all_reply_decode(reply, reply_len, &demoted) == 0 &&
          demoted == 0);
   assert(aimee_db2_curator_reembed_all_reply_encode(AIMEE_DB2_CURATOR_REEMBED_ALL_MAX + 1u, reply,
                                                     sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_curator_reembed_all_reply_encode(12, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_curator_reembed_all_reply_encode(12, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_curator_reembed_all_reply_decode(reply, reply_len, &demoted) == -1 &&
          demoted == 0);
}

static void test_evidence_reembed_all_wire(void)
{
   uint8_t request[AIMEE_DB2_EVIDENCE_REEMBED_ALL_REQUEST_LEN] = {0};
   assert(aimee_db2_evidence_reembed_all_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_evidence_reembed_all_request_decode(request, sizeof(request)) == 0);
   /* Fifth maintenance operation, so the four before it must refuse it. */
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_mark_revisit_due_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_ingest_queue_reset_running_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_evidence_reembed_all_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_EVIDENCE_REEMBED_ALL_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, rows = 99;
   assert(aimee_db2_evidence_reembed_all_reply_encode(11, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_evidence_reembed_all_reply_decode(reply, reply_len, &rows) == 0 && rows == 11);
   assert(aimee_db2_evidence_reembed_all_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_evidence_reembed_all_reply_decode(reply, reply_len, &rows) == 0 && rows == 0);
   assert(aimee_db2_evidence_reembed_all_reply_encode(AIMEE_DB2_EVIDENCE_REEMBED_ALL_MAX + 1u,
                                                      reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_evidence_reembed_all_reply_encode(11, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_evidence_reembed_all_reply_encode(11, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_evidence_reembed_all_reply_decode(reply, reply_len, &rows) == -1 && rows == 0);
}

static void test_ingest_queue_reset_running_wire(void)
{
   uint8_t request[AIMEE_DB2_INGEST_QUEUE_RESET_RUNNING_REQUEST_LEN] = {0};
   assert(aimee_db2_ingest_queue_reset_running_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_ingest_queue_reset_running_request_decode(request, sizeof(request)) == 0);
   /* Fourth maintenance operation, so the three before it must refuse it. */
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_mark_revisit_due_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_ingest_queue_reset_running_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_INGEST_QUEUE_RESET_RUNNING_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, reset = 99;
   assert(aimee_db2_ingest_queue_reset_running_reply_encode(10, reply, sizeof(reply), &reply_len) ==
          0);
   assert(aimee_db2_ingest_queue_reset_running_reply_decode(reply, reply_len, &reset) == 0 &&
          reset == 10);
   assert(aimee_db2_ingest_queue_reset_running_reply_encode(0, reply, sizeof(reply), &reply_len) ==
          0);
   assert(aimee_db2_ingest_queue_reset_running_reply_decode(reply, reply_len, &reset) == 0 &&
          reset == 0);
   assert(aimee_db2_ingest_queue_reset_running_reply_encode(
              AIMEE_DB2_INGEST_QUEUE_RESET_RUNNING_MAX + 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(aimee_db2_ingest_queue_reset_running_reply_encode(10, reply, sizeof(reply) - 1,
                                                            &reply_len) == -1);
   assert(aimee_db2_ingest_queue_reset_running_reply_encode(10, reply, sizeof(reply), &reply_len) ==
          0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_ingest_queue_reset_running_reply_decode(reply, reply_len, &reset) == -1 &&
          reset == 0);
}

static void test_mark_revisit_due_wire(void)
{
   uint8_t request[AIMEE_DB2_MARK_REVISIT_DUE_REQUEST_LEN] = {0};
   assert(aimee_db2_mark_revisit_due_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_mark_revisit_due_request_decode(request, sizeof(request)) == 0);
   /* Third maintenance operation, so the two before it must refuse it. */
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_mark_revisit_due_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_MARK_REVISIT_DUE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, marked = 99;
   assert(aimee_db2_mark_revisit_due_reply_encode(9, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_mark_revisit_due_reply_decode(reply, reply_len, &marked) == 0 && marked == 9);
   assert(aimee_db2_mark_revisit_due_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_mark_revisit_due_reply_decode(reply, reply_len, &marked) == 0 && marked == 0);
   assert(aimee_db2_mark_revisit_due_reply_encode(AIMEE_DB2_MARK_REVISIT_DUE_MAX + 1u, reply,
                                                  sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_mark_revisit_due_reply_encode(9, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_mark_revisit_due_reply_encode(9, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_mark_revisit_due_reply_decode(reply, reply_len, &marked) == -1 && marked == 0);
}

static void test_project_clear_operations_wire(void)
{
   uint8_t request[AIMEE_DB2_CLEAR_PROJECT_REQUEST_MAX] = {0};
   uint32_t request_len = 0;
   char project[128] = "";
   assert(aimee_db2_clear_project_request_encode("demo", request, sizeof(request), &request_len) ==
          0);
   assert(aimee_db2_clear_project_request_decode(request, request_len, project, sizeof(project)) ==
          0);
   assert(strcmp(project, "demo") == 0);
   /* An empty project name is refused rather than sent as a statement that
    * would match every row or none depending on the query. */
   assert(aimee_db2_clear_project_request_encode("", request, sizeof(request), &request_len) == -1);
   assert(aimee_db2_clear_project_request_encode("demo", request, sizeof(request), &request_len) ==
          0);
   /* Same family and the same payload shape as the current-generation clear:
    * confusing them would delete every generation instead of one. */
   assert(aimee_db2_clear_current_project_request_decode(request, request_len, project,
                                                         sizeof(project)) == -1);
   /* A buffer too small for the name plus its terminator is refused rather
    * than truncating a project name into a different project. */
   assert(aimee_db2_clear_project_request_decode(request, request_len, project, 4) == -1);

   uint8_t reply[AIMEE_DB2_CLEAR_PROJECT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, deleted = 99;
   assert(aimee_db2_clear_project_reply_encode(52, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_clear_project_reply_decode(reply, reply_len, &deleted) == 0 && deleted == 52);
   assert(aimee_db2_clear_project_reply_encode(AIMEE_DB2_CLEAR_PROJECT_MAX + 1u, reply,
                                               sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_clear_project_reply_encode(52, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_clear_project_reply_decode(reply, reply_len, &deleted) == -1 && deleted == 0);
}

static void test_by_id_operations_wire(void)
{
   /* Four operations across two families that all carry one identifier and
    * answer with an acknowledgement. What differs is what each does about a
    * row that is not there, and that is recorded in the catalog rather than
    * being visible on the wire -- these envelopes are indistinguishable. */
   uint8_t bump[AIMEE_DB2_ANTI_PATTERN_BUMP_REQUEST_LEN] = {0};
   uint8_t remove[AIMEE_DB2_ANTI_PATTERN_DELETE_REQUEST_LEN] = {0};
   uint64_t decoded = 0;
   assert(aimee_db2_anti_pattern_bump_request_encode(41, bump, sizeof(bump)) == 0);
   assert(aimee_db2_anti_pattern_delete_request_encode(41, remove, sizeof(remove)) == 0);
   assert(aimee_db2_anti_pattern_bump_request_decode(bump, sizeof(bump), &decoded) == 0 &&
          decoded == 41);
   /* Same family, same payload, adjacent numbers: bumping must never delete. */
   assert(aimee_db2_anti_pattern_bump_request_decode(remove, sizeof(remove), &decoded) == -1);
   assert(aimee_db2_anti_pattern_delete_request_decode(bump, sizeof(bump), &decoded) == -1);

   assert(aimee_db2_anti_pattern_bump_request_encode(0, bump, sizeof(bump)) == -1);
   assert(aimee_db2_anti_pattern_bump_request_encode(AIMEE_DB2_ANTI_PATTERN_BUMP_ID_MAX + 1ull,
                                                     bump, sizeof(bump)) == -1);

   uint8_t doc[AIMEE_DB2_DOC_DELETE_REQUEST_LEN] = {0};
   uint8_t task[AIMEE_DB2_TASK_DELETE_REQUEST_LEN] = {0};
   assert(aimee_db2_doc_delete_request_encode(43, doc, sizeof(doc)) == 0);
   assert(aimee_db2_task_delete_request_encode(44, task, sizeof(task)) == 0);
   assert(aimee_db2_doc_delete_request_decode(task, sizeof(task), &decoded) == -1);
   assert(aimee_db2_task_delete_request_decode(doc, sizeof(doc), &decoded) == -1);
   /* Different families, so these also differ from the learning pair. */
   assert(aimee_db2_anti_pattern_delete_request_decode(doc, sizeof(doc), &decoded) == -1);

   uint8_t reply[AIMEE_DB2_TASK_DELETE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   assert(aimee_db2_task_delete_reply_encode(reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_task_delete_reply_decode(reply, reply_len) == 0);
   assert(aimee_db2_doc_delete_reply_decode(reply, reply_len) == -1);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_task_delete_reply_decode(reply, reply_len) == -1);
}

static void test_directive_id_operations_wire(void)
{
   uint8_t suppress[AIMEE_DB2_DIRECTIVE_SUPPRESS_REQUEST_LEN] = {0};
   assert(aimee_db2_directive_suppress_request_encode(31, suppress, sizeof(suppress)) == 0);
   uint64_t decoded = 0;
   assert(aimee_db2_directive_suppress_request_decode(suppress, sizeof(suppress), &decoded) == 0 &&
          decoded == 31);
   /* Zero is not an identifier and the encoder refuses it rather than sending
    * a request the statement would match nothing for. */
   assert(aimee_db2_directive_suppress_request_encode(0, suppress, sizeof(suppress)) == -1);
   assert(aimee_db2_directive_suppress_request_encode(AIMEE_DB2_DIRECTIVE_SUPPRESS_ID_MAX + 1ull,
                                                      suppress, sizeof(suppress)) == -1);
   assert(aimee_db2_directive_suppress_request_encode(31, suppress, sizeof(suppress)) == 0);

   uint8_t surface[AIMEE_DB2_DIRECTIVE_RECORD_SURFACE_REQUEST_LEN] = {0};
   assert(aimee_db2_directive_record_surface_request_encode(32, surface, sizeof(surface)) == 0);
   /* Two operations on one stage carrying the same payload shape: each must
    * refuse the other, or a surfacing would suppress the directive instead. */
   assert(aimee_db2_directive_suppress_request_decode(surface, sizeof(surface), &decoded) == -1);
   assert(aimee_db2_directive_record_surface_request_decode(suppress, sizeof(suppress), &decoded) ==
          -1);

   uint8_t reply[AIMEE_DB2_DIRECTIVE_SUPPRESS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   assert(aimee_db2_directive_suppress_reply_encode(reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_directive_suppress_reply_decode(reply, reply_len) == 0);
   assert(aimee_db2_directive_record_surface_reply_decode(reply, reply_len) == -1);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_directive_suppress_reply_decode(reply, reply_len) == -1);
}

static void test_directive_sweep_expired_wire(void)
{
   uint8_t request[AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_REQUEST_LEN] = {0};
   assert(aimee_db2_directive_sweep_expired_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == 0);
   /* Second maintenance operation, so the first one must refuse it. It shares
    * operation number 2 with the index family's second operation, whose empty
    * request is byte-identical -- the stage separates them, not the envelope. */
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, directives = 99;
   assert(aimee_db2_directive_sweep_expired_reply_encode(8, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_directive_sweep_expired_reply_decode(reply, reply_len, &directives) == 0 &&
          directives == 8);
   assert(aimee_db2_directive_sweep_expired_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_directive_sweep_expired_reply_decode(reply, reply_len, &directives) == 0 &&
          directives == 0);
   assert(aimee_db2_directive_sweep_expired_reply_encode(AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_MAX + 1u,
                                                         reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_directive_sweep_expired_reply_encode(8, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_directive_sweep_expired_reply_encode(8, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_directive_sweep_expired_reply_decode(reply, reply_len, &directives) == -1 &&
          directives == 0);
}

static void test_prospective_sweep_expired_wire(void)
{
   uint8_t request[AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_REQUEST_LEN] = {0};
   assert(aimee_db2_prospective_sweep_expired_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == 0);
   /* First operation of the maintenance family. It carries operation number 1,
    * which every family's first operation carries, so the byte-identical
    * empty-payload envelope of index.entity_edge_prune_orphans decodes here
    * too. The envelope has no family field; the stage the invocation arrives
    * on is what separates them, and the handler test below pins that. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == 0);
   /* Later operations in either family are still refused. */
   assert(aimee_db2_requeue_drifted_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, expired = 99;
   assert(aimee_db2_prospective_sweep_expired_reply_encode(7, reply, sizeof(reply), &reply_len) ==
          0);
   assert(aimee_db2_prospective_sweep_expired_reply_decode(reply, reply_len, &expired) == 0 &&
          expired == 7);
   assert(aimee_db2_prospective_sweep_expired_reply_encode(0, reply, sizeof(reply), &reply_len) ==
          0);
   assert(aimee_db2_prospective_sweep_expired_reply_decode(reply, reply_len, &expired) == 0 &&
          expired == 0);
   assert(aimee_db2_prospective_sweep_expired_reply_encode(
              AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_MAX + 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(aimee_db2_prospective_sweep_expired_reply_encode(7, reply, sizeof(reply) - 1,
                                                           &reply_len) == -1);
   assert(aimee_db2_prospective_sweep_expired_reply_encode(7, reply, sizeof(reply), &reply_len) ==
          0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_prospective_sweep_expired_reply_decode(reply, reply_len, &expired) == -1 &&
          expired == 0);
}

static void test_release_get_active_wire(void)
{
   uint8_t request[AIMEE_DB2_RELEASE_GET_ACTIVE_REQUEST_LEN] = {0};
   assert(aimee_db2_release_get_active_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_release_get_active_request_decode(request, sizeof(request)) == 0);
   /* Third custody operation, so the lock pair must refuse it. */
   assert(aimee_db2_vector_rebuild_lock_try_acquire_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_vector_rebuild_lock_release_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_release_get_active_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_RELEASE_GET_ACTIVE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   uint64_t release_id = 99;
   assert(aimee_db2_release_get_active_reply_encode(21, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_release_get_active_reply_decode(reply, reply_len, &release_id) == 0 &&
          release_id == 21);
   assert(aimee_db2_release_get_active_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_release_get_active_reply_decode(reply, reply_len, &release_id) == 0 &&
          release_id == 0);
   assert(aimee_db2_release_get_active_reply_encode(AIMEE_DB2_RELEASE_GET_ACTIVE_MAX, reply,
                                                    sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_release_get_active_reply_encode(AIMEE_DB2_RELEASE_GET_ACTIVE_MAX + 1ull, reply,
                                                    sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_release_get_active_reply_encode(21, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_release_get_active_reply_encode(21, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_release_get_active_reply_decode(reply, reply_len, &release_id) == -1 &&
          release_id == 0);
}

static void test_vector_rebuild_lock_wire(void)
{
   uint8_t acquire[AIMEE_DB2_VECTOR_REBUILD_LOCK_TRY_ACQUIRE_REQUEST_LEN] = {0};
   assert(aimee_db2_vector_rebuild_lock_try_acquire_request_encode(acquire, sizeof(acquire)) == 0);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_request_decode(acquire, sizeof(acquire)) == 0);
   /* First operation of the custody family, so it shares its bytes with every
    * other family's first operation. */
   assert(aimee_db2_rules_decay_request_decode(acquire, sizeof(acquire)) == 0);
   assert(aimee_db2_rel_types_ensure_seed_request_decode(acquire, sizeof(acquire)) == 0);

   uint8_t release[AIMEE_DB2_VECTOR_REBUILD_LOCK_RELEASE_REQUEST_LEN] = {0};
   assert(aimee_db2_vector_rebuild_lock_release_request_encode(release, sizeof(release)) == 0);
   assert(aimee_db2_vector_rebuild_lock_release_request_decode(release, sizeof(release)) == 0);
   /* Acquire and release are operations 1 and 2 of the same family, so each
    * must refuse the other: a release must never be read as an acquire. */
   assert(aimee_db2_vector_rebuild_lock_try_acquire_request_decode(release, sizeof(release)) == -1);
   assert(aimee_db2_vector_rebuild_lock_release_request_decode(acquire, sizeof(acquire)) == -1);

   uint8_t reply[AIMEE_DB2_VECTOR_REBUILD_LOCK_TRY_ACQUIRE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, flag = 99;
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_encode(1, reply, sizeof(reply),
                                                                 &reply_len) == 0);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_decode(reply, reply_len, &flag) == 0 &&
          flag == 1);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_encode(0, reply, sizeof(reply),
                                                                 &reply_len) == 0);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_decode(reply, reply_len, &flag) == 0 &&
          flag == 0);
   /* The flag is zero or one and nothing else: a two here would be a decoder
    * inventing a third answer to a yes-or-no question. */
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_encode(2, reply, sizeof(reply),
                                                                 &reply_len) == -1);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_encode(1, reply, sizeof(reply),
                                                                 &reply_len) == 0);
   aimee_db2_put_u32(reply + AIMEE_DB2_ENVELOPE_HEADER_LEN, 2u);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_decode(reply, reply_len, &flag) == -1 &&
          flag == 0);
}

static void test_rel_types_ensure_seed_wire(void)
{
   uint8_t request[AIMEE_DB2_REL_TYPES_ENSURE_SEED_REQUEST_LEN] = {0};
   assert(aimee_db2_rel_types_ensure_seed_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_rel_types_ensure_seed_request_decode(request, sizeof(request)) == 0);
   /* First operation of the organization family, so it carries number 1 and
    * shares its bytes with the first operation of every other open family.
    * The stage separates them; the handler test pins that. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == 0);
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == 0);
   assert(aimee_db2_rules_decay_request_decode(request, sizeof(request)) == 0);
   /* An acknowledgement request is not distinguishable from a counted one at
    * this point either: only the reply shapes differ. */
   assert(aimee_db2_mining_seed_job_defaults_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_rel_types_ensure_seed_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_REL_TYPES_ENSURE_SEED_RESPONSE_LEN + 4] = {0};
   uint32_t reply_len = 99;
   assert(aimee_db2_rel_types_ensure_seed_reply_encode(reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_REL_TYPES_ENSURE_SEED_RESPONSE_LEN);
   assert(aimee_db2_rel_types_ensure_seed_reply_decode(reply, reply_len) == 0);
   assert(aimee_db2_rel_types_ensure_seed_reply_decode(reply, reply_len + 4) == -1);
   assert(aimee_db2_rel_types_ensure_seed_reply_decode(reply, reply_len - 1) == -1);
   assert(aimee_db2_rel_types_ensure_seed_reply_encode(
              reply, AIMEE_DB2_REL_TYPES_ENSURE_SEED_RESPONSE_LEN - 1, &reply_len) == -1);
   assert(aimee_db2_rel_types_ensure_seed_reply_encode(reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_rel_types_ensure_seed_reply_decode(reply, reply_len) == -1);
}

static void test_trace_mining_last_id_wire(void)
{
   uint8_t request[AIMEE_DB2_TRACE_MINING_LAST_ID_REQUEST_LEN] = {0};
   assert(aimee_db2_trace_mining_last_id_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_trace_mining_last_id_request_decode(request, sizeof(request)) == 0);
   /* Fifth learning operation, so the four before it must refuse it. */
   assert(aimee_db2_rules_decay_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_curiosity_rescore_all_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_mining_seed_job_defaults_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_proposals_archive_expired_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_trace_mining_last_id_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_TRACE_MINING_LAST_ID_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   uint64_t watermark = 99;
   assert(aimee_db2_trace_mining_last_id_reply_encode(22, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_trace_mining_last_id_reply_decode(reply, reply_len, &watermark) == 0 &&
          watermark == 22);
   assert(aimee_db2_trace_mining_last_id_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_trace_mining_last_id_reply_decode(reply, reply_len, &watermark) == 0 &&
          watermark == 0);
   assert(aimee_db2_trace_mining_last_id_reply_encode(AIMEE_DB2_TRACE_MINING_LAST_ID_MAX, reply,
                                                      sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_trace_mining_last_id_reply_encode(AIMEE_DB2_TRACE_MINING_LAST_ID_MAX + 1ull,
                                                      reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_trace_mining_last_id_reply_encode(22, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_trace_mining_last_id_reply_encode(22, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_trace_mining_last_id_reply_decode(reply, reply_len, &watermark) == -1 &&
          watermark == 0);
}

static void test_proposals_archive_expired_wire(void)
{
   uint8_t request[AIMEE_DB2_PROPOSALS_ARCHIVE_EXPIRED_REQUEST_LEN] = {0};
   assert(aimee_db2_proposals_archive_expired_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_proposals_archive_expired_request_decode(request, sizeof(request)) == 0);
   /* Fourth learning operation, so the three before it must refuse it. */
   assert(aimee_db2_rules_decay_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_curiosity_rescore_all_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_mining_seed_job_defaults_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_proposals_archive_expired_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_PROPOSALS_ARCHIVE_EXPIRED_RESPONSE_LEN + 4] = {0};
   uint32_t reply_len = 99;
   assert(aimee_db2_proposals_archive_expired_reply_encode(reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_PROPOSALS_ARCHIVE_EXPIRED_RESPONSE_LEN);
   assert(aimee_db2_proposals_archive_expired_reply_decode(reply, reply_len) == 0);
   assert(aimee_db2_proposals_archive_expired_reply_decode(reply, reply_len + 4) == -1);
   assert(aimee_db2_proposals_archive_expired_reply_decode(reply, reply_len - 1) == -1);
   assert(aimee_db2_proposals_archive_expired_reply_encode(
              reply, AIMEE_DB2_PROPOSALS_ARCHIVE_EXPIRED_RESPONSE_LEN - 1, &reply_len) == -1);
   assert(aimee_db2_proposals_archive_expired_reply_encode(reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_proposals_archive_expired_reply_decode(reply, reply_len) == -1);
}

static void test_mining_seed_job_defaults_wire(void)
{
   uint8_t request[AIMEE_DB2_MINING_SEED_JOB_DEFAULTS_REQUEST_LEN] = {0};
   assert(aimee_db2_mining_seed_job_defaults_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_mining_seed_job_defaults_request_decode(request, sizeof(request)) == 0);
   /* Third learning operation, so the two before it must refuse it. */
   assert(aimee_db2_rules_decay_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_curiosity_rescore_all_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_mining_seed_job_defaults_request_decode(request, sizeof(request)) == -1);

   /* The reply is the envelope and nothing after it. Length is therefore the
    * whole of the payload check: a reply that carries four bytes of anything
    * is not this operation's reply, even if the header still says ok. */
   uint8_t reply[AIMEE_DB2_MINING_SEED_JOB_DEFAULTS_RESPONSE_LEN + 4] = {0};
   uint32_t reply_len = 99;
   assert(aimee_db2_mining_seed_job_defaults_reply_encode(reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_MINING_SEED_JOB_DEFAULTS_RESPONSE_LEN);
   assert(aimee_db2_mining_seed_job_defaults_reply_decode(reply, reply_len) == 0);
   assert(aimee_db2_mining_seed_job_defaults_reply_decode(reply, reply_len + 4) == -1);
   assert(aimee_db2_mining_seed_job_defaults_reply_decode(reply, reply_len - 1) == -1);
   assert(aimee_db2_mining_seed_job_defaults_reply_encode(
              reply, AIMEE_DB2_MINING_SEED_JOB_DEFAULTS_RESPONSE_LEN - 1, &reply_len) == -1);
   assert(aimee_db2_mining_seed_job_defaults_reply_encode(reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_mining_seed_job_defaults_reply_decode(reply, reply_len) == -1);
}

static void test_curiosity_rescore_all_wire(void)
{
   uint8_t request[AIMEE_DB2_CURIOSITY_RESCORE_ALL_REQUEST_LEN] = {0};
   assert(aimee_db2_curiosity_rescore_all_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_curiosity_rescore_all_request_decode(request, sizeof(request)) == 0);
   /* Second learning operation, so the first must refuse it. Operation 2 of
    * the index and maintenance families produces the same bytes; the stage
    * separates them, not the envelope. */
   assert(aimee_db2_rules_decay_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(request, sizeof(request)) == 0);
   assert(aimee_db2_directive_sweep_expired_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_curiosity_rescore_all_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_CURIOSITY_RESCORE_ALL_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, rescored = 99;
   assert(aimee_db2_curiosity_rescore_all_reply_encode(19, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_curiosity_rescore_all_reply_decode(reply, reply_len, &rescored) == 0 &&
          rescored == 19);
   assert(aimee_db2_curiosity_rescore_all_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_curiosity_rescore_all_reply_decode(reply, reply_len, &rescored) == 0 &&
          rescored == 0);
   assert(aimee_db2_curiosity_rescore_all_reply_encode(AIMEE_DB2_CURIOSITY_RESCORE_ALL_MAX + 1u,
                                                       reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_curiosity_rescore_all_reply_encode(19, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_curiosity_rescore_all_reply_encode(19, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_curiosity_rescore_all_reply_decode(reply, reply_len, &rescored) == -1 &&
          rescored == 0);
}

static void test_rules_decay_wire(void)
{
   uint8_t request[AIMEE_DB2_RULES_DECAY_REQUEST_LEN] = {0};
   assert(aimee_db2_rules_decay_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_rules_decay_request_decode(request, sizeof(request)) == 0);
   /* First operation of the learning family. It carries operation number 1,
    * which every family's first operation carries, so the byte-identical
    * empty-payload envelopes of index.entity_edge_prune_orphans and
    * maintenance.prospective_sweep_expired decode here too. The stage the
    * invocation arrives on is what separates them; the handler test pins it. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == 0);
   assert(aimee_db2_prospective_sweep_expired_request_decode(request, sizeof(request)) == 0);
   /* Later operations in any of those families are still refused. */
   assert(aimee_db2_cross_repo_rebuild_build_deps_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_rules_decay_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_RULES_DECAY_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, touched = 99;
   assert(aimee_db2_rules_decay_reply_encode(18, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_rules_decay_reply_decode(reply, reply_len, &touched) == 0 && touched == 18);
   assert(aimee_db2_rules_decay_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_rules_decay_reply_decode(reply, reply_len, &touched) == 0 && touched == 0);
   assert(aimee_db2_rules_decay_reply_encode(AIMEE_DB2_RULES_DECAY_MAX + 1u, reply, sizeof(reply),
                                             &reply_len) == -1);
   assert(aimee_db2_rules_decay_reply_encode(18, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_rules_decay_reply_encode(18, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_rules_decay_reply_decode(reply, reply_len, &touched) == -1 && touched == 0);
}

static void test_drift_candidates_wire(void)
{
   uint8_t request[AIMEE_DB2_DRIFT_CANDIDATES_REQUEST_LEN] = {0};
   assert(aimee_db2_drift_candidates_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_drift_candidates_request_decode(request, sizeof(request)) == 0);
   /* Ninth index operation, so the earlier ones must refuse it -- including
    * requeue_drifted, whose predicate it shares. Sharing a predicate is not
    * sharing an operation number. */
   assert(aimee_db2_requeue_drifted_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_cross_repo_rebuild_build_deps_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_drift_candidates_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_DRIFT_CANDIDATES_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99;
   uint64_t drift = 99;
   assert(aimee_db2_drift_candidates_reply_encode(20, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_drift_candidates_reply_decode(reply, reply_len, &drift) == 0 && drift == 20);
   assert(aimee_db2_drift_candidates_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_drift_candidates_reply_decode(reply, reply_len, &drift) == 0 && drift == 0);
   /* Sixty-four bits wide, so the bound is the signed maximum rather than the
    * u32 ceiling every other count on this stage carries. */
   assert(aimee_db2_drift_candidates_reply_encode(AIMEE_DB2_DRIFT_CANDIDATES_MAX, reply,
                                                  sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_drift_candidates_reply_decode(reply, reply_len, &drift) == 0 &&
          drift == AIMEE_DB2_DRIFT_CANDIDATES_MAX);
   assert(aimee_db2_drift_candidates_reply_encode(AIMEE_DB2_DRIFT_CANDIDATES_MAX + 1ull, reply,
                                                  sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_drift_candidates_reply_encode(20, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_drift_candidates_reply_encode(20, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_drift_candidates_reply_decode(reply, reply_len, &drift) == -1 && drift == 0);
}

static void test_cross_repo_rebuild_build_deps_wire(void)
{
   uint8_t request[AIMEE_DB2_CROSS_REPO_REBUILD_BUILD_DEPS_REQUEST_LEN] = {0};
   assert(aimee_db2_cross_repo_rebuild_build_deps_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_cross_repo_rebuild_build_deps_request_decode(request, sizeof(request)) == 0);
   /* Eighth index operation, so the seven before it must refuse it. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_project_count_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_purge_hidden_pollution_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_requeue_drifted_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_cross_repo_rebuild_routes_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_cross_repo_rebuild_identities_request_decode(request, sizeof(request)) == -1);
   /* Operation 8 of the maintenance family produces the same bytes; the stage
    * separates them, not the envelope. */
   assert(aimee_db2_curator_reenqueue_extract_all_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_cross_repo_rebuild_build_deps_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_CROSS_REPO_REBUILD_BUILD_DEPS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, deps = 99;
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_encode(17, reply, sizeof(reply),
                                                               &reply_len) == 0);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_decode(reply, reply_len, &deps) == 0 &&
          deps == 17);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_encode(0, reply, sizeof(reply),
                                                               &reply_len) == 0);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_decode(reply, reply_len, &deps) == 0 &&
          deps == 0);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_encode(
              AIMEE_DB2_CROSS_REPO_REBUILD_BUILD_DEPS_MAX + 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_encode(17, reply, sizeof(reply) - 1,
                                                               &reply_len) == -1);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_encode(17, reply, sizeof(reply),
                                                               &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_decode(reply, reply_len, &deps) == -1 &&
          deps == 0);
}

static void test_cross_repo_rebuild_identities_wire(void)
{
   uint8_t request[AIMEE_DB2_CROSS_REPO_REBUILD_IDENTITIES_REQUEST_LEN] = {0};
   assert(aimee_db2_cross_repo_rebuild_identities_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_cross_repo_rebuild_identities_request_decode(request, sizeof(request)) == 0);
   /* Seventh index operation, so the six before it must refuse it. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_project_count_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_purge_hidden_pollution_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_requeue_drifted_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_cross_repo_rebuild_routes_request_decode(request, sizeof(request)) == -1);
   /* Operation 7 of the maintenance family produces the same bytes; the stage
    * separates them, not the envelope. */
   assert(aimee_db2_synth_reenqueue_all_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_cross_repo_rebuild_identities_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_CROSS_REPO_REBUILD_IDENTITIES_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, written = 99;
   assert(aimee_db2_cross_repo_rebuild_identities_reply_encode(16, reply, sizeof(reply),
                                                               &reply_len) == 0);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_decode(reply, reply_len, &written) == 0 &&
          written == 16);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_encode(0, reply, sizeof(reply),
                                                               &reply_len) == 0);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_decode(reply, reply_len, &written) == 0 &&
          written == 0);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_encode(
              AIMEE_DB2_CROSS_REPO_REBUILD_IDENTITIES_MAX + 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_encode(16, reply, sizeof(reply) - 1,
                                                               &reply_len) == -1);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_encode(16, reply, sizeof(reply),
                                                               &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_decode(reply, reply_len, &written) == -1 &&
          written == 0);
}

static void test_cross_repo_rebuild_routes_wire(void)
{
   uint8_t request[AIMEE_DB2_CROSS_REPO_REBUILD_ROUTES_REQUEST_LEN] = {0};
   assert(aimee_db2_cross_repo_rebuild_routes_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_cross_repo_rebuild_routes_request_decode(request, sizeof(request)) == 0);
   /* Sixth index operation, so the five before it must refuse it. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_project_count_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_purge_hidden_pollution_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_requeue_drifted_request_decode(request, sizeof(request)) == -1);
   /* Operation 6 of the maintenance family produces the same bytes; the stage
    * separates them, not the envelope. */
   assert(aimee_db2_curator_reembed_all_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_cross_repo_rebuild_routes_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_CROSS_REPO_REBUILD_ROUTES_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, routes = 99;
   assert(aimee_db2_cross_repo_rebuild_routes_reply_encode(15, reply, sizeof(reply), &reply_len) ==
          0);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_decode(reply, reply_len, &routes) == 0 &&
          routes == 15);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_encode(0, reply, sizeof(reply), &reply_len) ==
          0);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_decode(reply, reply_len, &routes) == 0 &&
          routes == 0);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_encode(
              AIMEE_DB2_CROSS_REPO_REBUILD_ROUTES_MAX + 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_encode(15, reply, sizeof(reply) - 1,
                                                           &reply_len) == -1);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_encode(15, reply, sizeof(reply), &reply_len) ==
          0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_decode(reply, reply_len, &routes) == -1 &&
          routes == 0);
}

static void test_requeue_drifted_wire(void)
{
   uint8_t request[AIMEE_DB2_REQUEUE_DRIFTED_REQUEST_LEN] = {0};
   assert(aimee_db2_requeue_drifted_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_requeue_drifted_request_decode(request, sizeof(request)) == 0);
   /* Fifth index operation: every earlier one in the family must refuse it,
    * because the operation number is all that tells them apart. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_project_count_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_purge_hidden_pollution_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_requeue_drifted_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_REQUEUE_DRIFTED_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, requeued = 99;
   assert(aimee_db2_requeue_drifted_reply_encode(6, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_requeue_drifted_reply_decode(reply, reply_len, &requeued) == 0 &&
          requeued == 6);
   assert(aimee_db2_requeue_drifted_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_requeue_drifted_reply_decode(reply, reply_len, &requeued) == 0 &&
          requeued == 0);
   assert(aimee_db2_requeue_drifted_reply_encode(AIMEE_DB2_REQUEUE_DRIFTED_MAX + 1u, reply,
                                                 sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_requeue_drifted_reply_encode(6, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_requeue_drifted_reply_encode(6, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_requeue_drifted_reply_decode(reply, reply_len, &requeued) == -1 &&
          requeued == 0);
}

static void test_purge_hidden_pollution_wire(void)
{
   uint8_t request[AIMEE_DB2_PURGE_HIDDEN_POLLUTION_REQUEST_LEN] = {0};
   assert(aimee_db2_purge_hidden_pollution_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_purge_hidden_pollution_request_decode(request, sizeof(request)) == 0);
   /* Fourth index operation: every earlier one in the family must refuse it,
    * because the operation number is all that tells them apart. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_project_count_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_purge_hidden_pollution_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_PURGE_HIDDEN_POLLUTION_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, purged = 99;
   assert(aimee_db2_purge_hidden_pollution_reply_encode(5, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_purge_hidden_pollution_reply_decode(reply, reply_len, &purged) == 0 &&
          purged == 5);
   assert(aimee_db2_purge_hidden_pollution_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_purge_hidden_pollution_reply_decode(reply, reply_len, &purged) == 0 &&
          purged == 0);
   assert(aimee_db2_purge_hidden_pollution_reply_encode(AIMEE_DB2_PURGE_HIDDEN_POLLUTION_MAX + 1u,
                                                        reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_purge_hidden_pollution_reply_encode(5, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_purge_hidden_pollution_reply_encode(5, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_purge_hidden_pollution_reply_decode(reply, reply_len, &purged) == -1 &&
          purged == 0);
}

static void test_project_count_wire(void)
{
   uint8_t request[AIMEE_DB2_PROJECT_COUNT_REQUEST_LEN] = {0};
   assert(aimee_db2_project_count_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_project_count_request_decode(request, sizeof(request)) == 0);
   /* Third index operation: distinct number, same family. The two before it
    * must reject this request and it must reject theirs. */
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == -1);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(request, sizeof(request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_project_count_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_PROJECT_COUNT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, projects = 99;
   assert(aimee_db2_project_count_reply_encode(4, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_project_count_reply_decode(reply, reply_len, &projects) == 0 && projects == 4);
   assert(aimee_db2_project_count_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_project_count_reply_decode(reply, reply_len, &projects) == 0 && projects == 0);
   assert(aimee_db2_project_count_reply_encode(AIMEE_DB2_PROJECT_COUNT_MAX + 1u, reply,
                                               sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_project_count_reply_encode(4, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_project_count_reply_encode(4, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_project_count_reply_decode(reply, reply_len, &projects) == -1 && projects == 0);
}

static void test_entity_edge_normalize_weights_wire(void)
{
   assert(AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_SCALE == 100u);
   /* Second index-family operation: a distinct operation number within the
    * family, and the same index event kind and stage as the first. */
   assert(AIMEE_DB2_OPERATION_ENTITY_EDGE_NORMALIZE_WEIGHTS !=
          AIMEE_DB2_OPERATION_ENTITY_EDGE_PRUNE_ORPHANS);
   assert(AIMEE_DB2_EVENT_ENTITY_EDGE_NORMALIZE_WEIGHTS == AIMEE_DB2_EVENT_INDEX);

   uint8_t request[AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_REQUEST_LEN] = {0};
   assert(aimee_db2_entity_edge_normalize_weights_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(request, sizeof(request)) == 0);
   /* The prune request is a different operation number, so this decoder must
    * reject it even though both arrive on the index stage. */
   uint8_t prune_request[AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_REQUEST_LEN] = {0};
   assert(aimee_db2_entity_edge_prune_orphans_request_encode(prune_request,
                                                             sizeof(prune_request)) == 0);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(prune_request,
                                                                 sizeof(prune_request)) == -1);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_entity_edge_normalize_weights_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, normalized = 99;
   assert(aimee_db2_entity_edge_normalize_weights_reply_encode(3, reply, sizeof(reply),
                                                               &reply_len) == 0);
   assert(aimee_db2_entity_edge_normalize_weights_reply_decode(reply, reply_len, &normalized) ==
              0 &&
          normalized == 3);
   /* A converged graph reports zero rewrites, which is a success. */
   assert(aimee_db2_entity_edge_normalize_weights_reply_encode(0, reply, sizeof(reply),
                                                               &reply_len) == 0);
   assert(aimee_db2_entity_edge_normalize_weights_reply_decode(reply, reply_len, &normalized) ==
              0 &&
          normalized == 0);
   assert(aimee_db2_entity_edge_normalize_weights_reply_encode(
              AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_COUNT_MAX + 1u, reply, sizeof(reply),
              &reply_len) == -1);
   assert(aimee_db2_entity_edge_normalize_weights_reply_encode(3, reply, sizeof(reply),
                                                               &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_entity_edge_normalize_weights_reply_decode(reply, reply_len, &normalized) ==
          -1);
}

static void test_entity_edge_prune_orphans_wire(void)
{
   uint8_t request[AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_REQUEST_LEN] = {0};
   assert(aimee_db2_entity_edge_prune_orphans_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_entity_edge_prune_orphans_request_decode(request, sizeof(request)) == -1);

   /* This is the first index-family operation, and its event kind and stage
    * must be the index ones rather than the memory ones every operation before
    * it uses. Operation numbers are unique only within a family, so the stage
    * is what keeps an index request out of the memory decoders. */
   assert(AIMEE_DB2_EVENT_ENTITY_EDGE_PRUNE_ORPHANS == AIMEE_DB2_EVENT_INDEX);
   assert(AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS == AIMEE_DB2_FAMILY_INDEX);
   assert(AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS != AIMEE_DB2_FAMILY_MEMORY);
   assert(AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS != AIMEE_DB2_FAMILY_LIFECYCLE);

   uint8_t reply[AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, pruned = 99;
   assert(aimee_db2_entity_edge_prune_orphans_reply_encode(2, reply, sizeof(reply), &reply_len) ==
          0);
   assert(aimee_db2_entity_edge_prune_orphans_reply_decode(reply, reply_len, &pruned) == 0 &&
          pruned == 2);
   assert(aimee_db2_entity_edge_prune_orphans_reply_encode(
              AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_COUNT_MAX + 1u, reply, sizeof(reply),
              &reply_len) == -1);
   assert(aimee_db2_entity_edge_prune_orphans_reply_encode(2, reply, sizeof(reply) - 1,
                                                           &reply_len) == -1);
   assert(aimee_db2_entity_edge_prune_orphans_reply_encode(2, reply, sizeof(reply), &reply_len) ==
          0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_entity_edge_prune_orphans_reply_decode(reply, reply_len, &pruned) == -1 &&
          pruned == 0);
}

static void test_count_and_max_updated_wire(void)
{
   uint8_t request[AIMEE_DB2_COUNT_AND_MAX_UPDATED_REQUEST_LEN] = {0};
   assert(aimee_db2_count_and_max_updated_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_count_and_max_updated_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_count_and_max_updated_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_COUNT_AND_MAX_UPDATED_RESPONSE_MAX_LEN];
   char stamp[AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX + 1];
   uint32_t reply_len = 99, result = 99, count = 99;
   assert(aimee_db2_count_and_max_updated_reply_encode(AIMEE_DB2_RESULT_OK, 7,
                                                       "2026-08-19 09:00:00", reply, sizeof(reply),
                                                       &reply_len) == 0);
   assert(aimee_db2_count_and_max_updated_reply_decode(reply, reply_len, &result, &count, stamp,
                                                       sizeof(stamp)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && count == 7 && strcmp(stamp, "2026-08-19 09:00:00") == 0);

   /* AN EMPTY CORPUS IS A REAL ANSWER. Zero rows means there is no latest
    * update, so the stamp is empty and the result is still ok. That must not
    * collapse into the invalid_state below, which means the aggregate never
    * ran at all. */
   assert(aimee_db2_count_and_max_updated_reply_encode(AIMEE_DB2_RESULT_OK, 0, "", reply,
                                                       sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_COUNT_AND_MAX_UPDATED_RESPONSE_MIN_LEN);
   assert(aimee_db2_count_and_max_updated_reply_decode(reply, reply_len, &result, &count, stamp,
                                                       sizeof(stamp)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && count == 0 && stamp[0] == '\0');

   assert(aimee_db2_count_and_max_updated_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 0, NULL,
                                                       reply, sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_COUNT_AND_MAX_UPDATED_ERROR_LEN);
   assert(aimee_db2_count_and_max_updated_reply_decode(reply, reply_len, &result, &count, stamp,
                                                       sizeof(stamp)) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE && count == 0 && stamp[0] == '\0');

   /* invalid_state carries neither number. */
   assert(aimee_db2_count_and_max_updated_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 1, NULL,
                                                       reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_count_and_max_updated_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 0, "", reply,
                                                       sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_count_and_max_updated_reply_encode(
              AIMEE_DB2_RESULT_OK, AIMEE_DB2_COUNT_AND_MAX_UPDATED_COUNT_MAX + 1u, "", reply,
              sizeof(reply), &reply_len) == -1);
}

static void test_pick_first_temporal_ref_wire(void)
{
   uint8_t request[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_pick_first_temporal_ref_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_pick_first_temporal_ref_request_decode(request, sizeof(request), &memory_id) ==
              0 &&
          memory_id == 42);
   assert(aimee_db2_pick_first_temporal_ref_request_encode(0u, request, sizeof(request)) == -1);
   /* The request is exactly a memory: the ranking that decides which reference
    * wins lives in the statement, so there is nothing here to steer it. */
   assert(sizeof(request) == AIMEE_DB2_ENVELOPE_HEADER_LEN + 8u);

   uint8_t reply[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_RESPONSE_MAX_LEN];
   char ref_key[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX + 1];
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_pick_first_temporal_ref_reply_encode(AIMEE_DB2_RESULT_OK, "2026-08-19", reply,
                                                         sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_pick_first_temporal_ref_reply_decode(reply, reply_len, &result, ref_key,
                                                         sizeof(ref_key)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && strcmp(ref_key, "2026-08-19") == 0);

   assert(aimee_db2_pick_first_temporal_ref_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, NULL, reply,
                                                         sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_ERROR_LEN);
   assert(aimee_db2_pick_first_temporal_ref_reply_decode(reply, reply_len, &result, ref_key,
                                                         sizeof(ref_key)) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND && ref_key[0] == '\0');

   /* No empty ok, for the same reason as get_source_session. */
   assert(aimee_db2_pick_first_temporal_ref_reply_encode(AIMEE_DB2_RESULT_OK, "", reply,
                                                         sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_pick_first_temporal_ref_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, "x", reply,
                                                         sizeof(reply), &reply_len) == -1);
}

static void test_get_source_session_wire(void)
{
   uint8_t request[AIMEE_DB2_GET_SOURCE_SESSION_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_get_source_session_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_get_source_session_request_decode(request, sizeof(request), &memory_id) == 0 &&
          memory_id == 42);
   assert(aimee_db2_get_source_session_request_encode(0u, request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_GET_SOURCE_SESSION_RESPONSE_MAX_LEN];
   char session_id[AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX + 1];
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_get_source_session_reply_encode(AIMEE_DB2_RESULT_OK, "sess-1", reply,
                                                    sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_get_source_session_reply_decode(reply, reply_len, &result, session_id,
                                                    sizeof(session_id)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && strcmp(session_id, "sess-1") == 0);

   assert(aimee_db2_get_source_session_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, NULL, reply,
                                                    sizeof(reply), &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_GET_SOURCE_SESSION_ERROR_LEN);
   assert(aimee_db2_get_source_session_reply_decode(reply, reply_len, &result, session_id,
                                                    sizeof(session_id)) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND && session_id[0] == '\0');

   /* THE CONTRAST WITH get_content, WHICH SHARES THIS WIRE FORMAT. There an
    * empty ok is a real answer -- a row holding "". Here the backend succeeds
    * only for a non-empty session and reports a blank column exactly as it
    * reports an absent memory, so an empty ok would claim a distinction that
    * cannot be made and must not encode or decode. */
   assert(aimee_db2_get_source_session_reply_encode(AIMEE_DB2_RESULT_OK, "", reply, sizeof(reply),
                                                    &reply_len) == -1);
   assert(aimee_db2_get_source_session_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, "x", reply,
                                                    sizeof(reply), &reply_len) == -1);
}

static void test_get_content_wire(void)
{
   uint8_t request[AIMEE_DB2_GET_CONTENT_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_get_content_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_get_content_request_decode(request, sizeof(request), &memory_id) == 0 &&
          memory_id == 42);
   assert(aimee_db2_get_content_request_encode(0u, request, sizeof(request)) == -1);

   static uint8_t reply[AIMEE_DB2_GET_CONTENT_RESPONSE_MAX_LEN];
   static char content[AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1];
   uint32_t reply_len = 99, result = 99;
   assert(aimee_db2_get_content_reply_encode(AIMEE_DB2_RESULT_OK, "stored text", reply,
                                             sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_get_content_reply_decode(reply, reply_len, &result, content, sizeof(content)) ==
          0);
   assert(result == AIMEE_DB2_RESULT_OK && strcmp(content, "stored text") == 0);

   /* THE DISTINCTION THIS OPERATION EXISTS TO PRESERVE. A row holding an empty
    * string encodes as ok with a zero-length payload; a memory that is not
    * there encodes as not_found with no payload at all. They are different
    * lengths, different results, and must decode to different answers. */
   assert(aimee_db2_get_content_reply_encode(AIMEE_DB2_RESULT_OK, "", reply, sizeof(reply),
                                             &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_GET_CONTENT_RESPONSE_MIN_LEN);
   assert(aimee_db2_get_content_reply_decode(reply, reply_len, &result, content, sizeof(content)) ==
          0);
   assert(result == AIMEE_DB2_RESULT_OK && content[0] == '\0');

   assert(aimee_db2_get_content_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, NULL, reply, sizeof(reply),
                                             &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_GET_CONTENT_ERROR_LEN);
   assert(aimee_db2_get_content_reply_decode(reply, reply_len, &result, content, sizeof(content)) ==
          0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND && content[0] == '\0');

   /* not_found must not be encodable with content beside it. */
   assert(aimee_db2_get_content_reply_encode(AIMEE_DB2_RESULT_NOT_FOUND, "x", reply, sizeof(reply),
                                             &reply_len) == -1);

   static char too_long[AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 2];
   memset(too_long, 'c', AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1);
   too_long[AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1] = '\0';
   assert(aimee_db2_get_content_reply_encode(AIMEE_DB2_RESULT_OK, too_long, reply, sizeof(reply),
                                             &reply_len) == -1);
}

static void test_negation_tokens_update_wire(void)
{
   static uint8_t request[AIMEE_DB2_NEGATION_TOKENS_UPDATE_REQUEST_MAX_LEN];
   static char tokens[AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX + 1];
   uint32_t request_len = 99;
   uint64_t memory_id = 99;
   assert(aimee_db2_negation_tokens_update_request_encode(42u, "not never without", request,
                                                          sizeof(request), &request_len) == 0);
   assert(aimee_db2_negation_tokens_update_request_decode(request, request_len, &memory_id, tokens,
                                                          sizeof(tokens)) == 0);
   assert(memory_id == 42 && strcmp(tokens, "not never without") == 0);

   /* The extractor legitimately produces nothing for a memory with no
    * negations, and storing that empty result is the point of the call. */
   assert(aimee_db2_negation_tokens_update_request_encode(42u, "", request, sizeof(request),
                                                          &request_len) == 0);
   assert(request_len == AIMEE_DB2_NEGATION_TOKENS_UPDATE_REQUEST_MIN_LEN);
   assert(aimee_db2_negation_tokens_update_request_decode(request, request_len, &memory_id, tokens,
                                                          sizeof(tokens)) == 0);
   assert(memory_id == 42 && tokens[0] == '\0');

   assert(aimee_db2_negation_tokens_update_request_encode(0u, "not", request, sizeof(request),
                                                          &request_len) == -1);

   /* Bounded to the extractor's own output buffer width. */
   static char at_bound[AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX + 2];
   memset(at_bound, 't', AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX);
   at_bound[AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX] = '\0';
   assert(aimee_db2_negation_tokens_update_request_encode(42u, at_bound, request, sizeof(request),
                                                          &request_len) == 0);
   assert(request_len == AIMEE_DB2_NEGATION_TOKENS_UPDATE_REQUEST_MAX_LEN);
   at_bound[AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX] = 't';
   at_bound[AIMEE_DB2_NEGATION_TOKENS_UPDATE_TOKENS_MAX + 1] = '\0';
   assert(aimee_db2_negation_tokens_update_request_encode(42u, at_bound, request, sizeof(request),
                                                          &request_len) == -1);

   uint8_t reply[AIMEE_DB2_NEGATION_TOKENS_UPDATE_RESPONSE_LEN] = {0};
   assert(aimee_db2_negation_tokens_update_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_negation_tokens_update_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_negation_tokens_update_reply_encode(reply, sizeof(reply) - 1) == -1);
   assert(aimee_db2_negation_tokens_update_reply_encode(reply, sizeof(reply)) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_negation_tokens_update_reply_decode(reply, sizeof(reply)) == -1);
}

static void test_set_source_session_wire(void)
{
   uint8_t request[AIMEE_DB2_SET_SOURCE_SESSION_REQUEST_MAX_LEN];
   char session_id[AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX + 1];
   uint32_t request_len = 99;
   uint64_t memory_id = 99;
   assert(aimee_db2_set_source_session_request_encode(42u, "sess-2026-08-19", request,
                                                      sizeof(request), &request_len) == 0);
   assert(aimee_db2_set_source_session_request_decode(request, request_len, &memory_id, session_id,
                                                      sizeof(session_id)) == 0);
   assert(memory_id == 42 && strcmp(session_id, "sess-2026-08-19") == 0);

   /* THE DIFFERENCE FROM set_cognified_kind, WHICH SHARES THIS WIRE FORMAT.
    * An empty session is a real request: it clears the column. It must encode,
    * decode, and arrive as an empty string rather than being refused. */
   assert(aimee_db2_set_source_session_request_encode(42u, "", request, sizeof(request),
                                                      &request_len) == 0);
   assert(request_len == AIMEE_DB2_SET_SOURCE_SESSION_REQUEST_MIN_LEN);
   assert(aimee_db2_set_source_session_request_decode(request, request_len, &memory_id, session_id,
                                                      sizeof(session_id)) == 0);
   assert(memory_id == 42 && session_id[0] == '\0');

   assert(aimee_db2_set_source_session_request_encode(0u, "sess-1", request, sizeof(request),
                                                      &request_len) == -1);
   char at_bound[AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX + 2];
   memset(at_bound, 's', AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX);
   at_bound[AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX] = '\0';
   assert(aimee_db2_set_source_session_request_encode(42u, at_bound, request, sizeof(request),
                                                      &request_len) == 0);
   assert(request_len == AIMEE_DB2_SET_SOURCE_SESSION_REQUEST_MAX_LEN);
   at_bound[AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX] = 's';
   at_bound[AIMEE_DB2_SET_SOURCE_SESSION_SESSION_MAX + 1] = '\0';
   assert(aimee_db2_set_source_session_request_encode(42u, at_bound, request, sizeof(request),
                                                      &request_len) == -1);

   uint8_t reply[AIMEE_DB2_SET_SOURCE_SESSION_RESPONSE_LEN] = {0};
   assert(aimee_db2_set_source_session_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_set_source_session_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_set_source_session_reply_encode(reply, sizeof(reply) - 1) == -1);
   assert(aimee_db2_set_source_session_reply_encode(reply, sizeof(reply)) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_set_source_session_reply_decode(reply, sizeof(reply)) == -1);
}

static void test_set_cognified_kind_wire(void)
{
   uint8_t request[AIMEE_DB2_SET_COGNIFIED_KIND_REQUEST_MAX_LEN];
   char kind[AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX + 1];
   uint32_t request_len = 99;
   uint64_t memory_id = 99;
   assert(aimee_db2_set_cognified_kind_request_encode(42u, "preference", request, sizeof(request),
                                                      &request_len) == 0);
   assert(aimee_db2_set_cognified_kind_request_decode(request, request_len, &memory_id, kind,
                                                      sizeof(kind)) == 0);
   assert(memory_id == 42 && strcmp(kind, "preference") == 0);

   assert(aimee_db2_set_cognified_kind_request_encode(0u, "preference", request, sizeof(request),
                                                      &request_len) == -1);
   /* The backend refuses an empty kind, so the wire does too. The two setters
    * that follow this one differ: there an empty value clears the column. */
   assert(aimee_db2_set_cognified_kind_request_encode(42u, "", request, sizeof(request),
                                                      &request_len) == -1);

   char at_bound[AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX + 2];
   memset(at_bound, 'k', AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX);
   at_bound[AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX] = '\0';
   assert(aimee_db2_set_cognified_kind_request_encode(42u, at_bound, request, sizeof(request),
                                                      &request_len) == 0);
   assert(request_len == AIMEE_DB2_SET_COGNIFIED_KIND_REQUEST_MAX_LEN);
   at_bound[AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX] = 'k';
   at_bound[AIMEE_DB2_SET_COGNIFIED_KIND_KIND_MAX + 1] = '\0';
   assert(aimee_db2_set_cognified_kind_request_encode(42u, at_bound, request, sizeof(request),
                                                      &request_len) == -1);

   uint8_t reply[AIMEE_DB2_SET_COGNIFIED_KIND_RESPONSE_LEN] = {0};
   assert(aimee_db2_set_cognified_kind_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_set_cognified_kind_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_set_cognified_kind_reply_encode(reply, sizeof(reply) - 1) == -1);
   assert(aimee_db2_set_cognified_kind_reply_encode(reply, sizeof(reply)) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_set_cognified_kind_reply_decode(reply, sizeof(reply)) == -1);
}

static void test_workspace_tag_insert_wire(void)
{
   static uint8_t request[AIMEE_DB2_WORKSPACE_TAG_INSERT_REQUEST_MAX_LEN];
   static char workspace[AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX + 1];
   uint32_t request_len = 99;
   uint64_t memory_id = 99;
   assert(aimee_db2_workspace_tag_insert_request_encode(42u, "aimee", request, sizeof(request),
                                                        &request_len) == 0);
   assert(aimee_db2_workspace_tag_insert_request_decode(request, request_len, &memory_id, workspace,
                                                        sizeof(workspace)) == 0);
   assert(memory_id == 42 && strcmp(workspace, "aimee") == 0);

   assert(aimee_db2_workspace_tag_insert_request_encode(0u, "aimee", request, sizeof(request),
                                                        &request_len) == -1);
   /* An empty workspace is not an attribution; the backend refuses it too. */
   assert(aimee_db2_workspace_tag_insert_request_encode(42u, "", request, sizeof(request),
                                                        &request_len) == -1);

   /* Exactly DB2's own workspace identifier width encodes; one more does not,
    * because a truncated name is a different workspace rather than an error. */
   static char at_bound[AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX + 2];
   memset(at_bound, 'w', AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX);
   at_bound[AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX] = '\0';
   assert(aimee_db2_workspace_tag_insert_request_encode(42u, at_bound, request, sizeof(request),
                                                        &request_len) == 0);
   assert(request_len == AIMEE_DB2_WORKSPACE_TAG_INSERT_REQUEST_MAX_LEN);
   at_bound[AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX] = 'w';
   at_bound[AIMEE_DB2_WORKSPACE_TAG_INSERT_WORKSPACE_MAX + 1] = '\0';
   assert(aimee_db2_workspace_tag_insert_request_encode(42u, at_bound, request, sizeof(request),
                                                        &request_len) == -1);

   uint8_t reply[AIMEE_DB2_WORKSPACE_TAG_INSERT_RESPONSE_LEN] = {0};
   assert(aimee_db2_workspace_tag_insert_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_workspace_tag_insert_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_workspace_tag_insert_reply_encode(reply, sizeof(reply) - 1) == -1);
   assert(aimee_db2_workspace_tag_insert_reply_encode(reply, sizeof(reply)) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_workspace_tag_insert_reply_decode(reply, sizeof(reply)) == -1);
}

static void test_decay_confidence_wire(void)
{
   uint64_t multiplier_bits = AIMEE_DB2_DECAY_CONFIDENCE_MULTIPLIER_BITS;
   double multiplier = 0.0;
   memcpy(&multiplier, &multiplier_bits, sizeof(multiplier));
   assert(multiplier == 0.7);
   /* Distinct from the other two confidence movers on this bus, so a copied
    * constant would show up here rather than silently decaying by the wrong
    * factor. */
   assert(multiplier_bits != AIMEE_DB2_DEMOTE_ID_MULTIPLIER_BITS);

   uint8_t request[AIMEE_DB2_DECAY_CONFIDENCE_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_decay_confidence_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_decay_confidence_request_decode(request, sizeof(request), &memory_id) == 0 &&
          memory_id == 42);
   assert(aimee_db2_decay_confidence_request_encode(0u, request, sizeof(request)) == -1);
   assert(aimee_db2_decay_confidence_request_encode(AIMEE_DB2_DECAY_CONFIDENCE_MEMORY_ID_MAX + 1ull,
                                                    request, sizeof(request)) == -1);
   assert(aimee_db2_decay_confidence_request_encode(42u, request, sizeof(request) - 1) == -1);
   assert(aimee_db2_decay_confidence_request_encode(42u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_decay_confidence_request_decode(request, sizeof(request), &memory_id) == -1 &&
          memory_id == 0);

   uint8_t reply[AIMEE_DB2_DECAY_CONFIDENCE_RESPONSE_LEN] = {0};
   assert(aimee_db2_decay_confidence_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_decay_confidence_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_decay_confidence_reply_encode(reply, sizeof(reply) - 1) == -1);
   assert(aimee_db2_decay_confidence_reply_encode(reply, sizeof(reply)) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_decay_confidence_reply_decode(reply, sizeof(reply)) == -1);
}

static void test_update_content_wire(void)
{
   static uint8_t request[AIMEE_DB2_UPDATE_CONTENT_REQUEST_MAX_LEN];
   static char content[AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX + 1];
   uint32_t request_len = 99;
   uint64_t memory_id = 99;
   assert(aimee_db2_update_content_request_encode(42u, "revised text", request, sizeof(request),
                                                  &request_len) == 0);
   assert(aimee_db2_update_content_request_decode(request, request_len, &memory_id, content,
                                                  sizeof(content)) == 0);
   assert(memory_id == 42 && strcmp(content, "revised text") == 0);

   assert(aimee_db2_update_content_request_encode(0u, "revised text", request, sizeof(request),
                                                  &request_len) == -1);
   /* Empty content is not an update, it is a deletion of the text; the backend
    * refuses it and so must the wire. */
   assert(aimee_db2_update_content_request_encode(42u, "", request, sizeof(request),
                                                  &request_len) == -1);

   /* Exactly the memory record's content width encodes; one byte more does
    * not, because a longer value would be truncated by whatever reads the row
    * back into a memory_t rather than rejected. */
   static char at_bound[AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX + 2];
   memset(at_bound, 'x', AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX);
   at_bound[AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX] = '\0';
   assert(aimee_db2_update_content_request_encode(42u, at_bound, request, sizeof(request),
                                                  &request_len) == 0);
   assert(request_len == AIMEE_DB2_UPDATE_CONTENT_REQUEST_MAX_LEN);
   at_bound[AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX] = 'x';
   at_bound[AIMEE_DB2_UPDATE_CONTENT_CONTENT_MAX + 1] = '\0';
   assert(aimee_db2_update_content_request_encode(42u, at_bound, request, sizeof(request),
                                                  &request_len) == -1);

   uint8_t reply[AIMEE_DB2_UPDATE_CONTENT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, updated = 99;
   assert(aimee_db2_update_content_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_update_content_reply_decode(reply, reply_len, &updated) == 0 && updated == 1);
   assert(aimee_db2_update_content_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_update_content_reply_decode(reply, reply_len, &updated) == 0 && updated == 0);
   assert(aimee_db2_update_content_reply_encode(2, reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_update_content_reply_encode(1, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_update_content_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_update_content_reply_decode(reply, reply_len, &updated) == -1 && updated == 0);
}

static void test_reject_wire(void)
{
   /* The penalty and the floor are policy, pinned as bit patterns. Compared as
    * bits so a constant that rounds differently on one side cannot penalise by
    * a different amount while every envelope still matches. */
   uint64_t penalty_bits = AIMEE_DB2_REJECT_PENALTY_BITS;
   uint64_t floor_bits = AIMEE_DB2_REJECT_FLOOR_BITS;
   double penalty = 0.0, floor_value = 0.0;
   memcpy(&penalty, &penalty_bits, sizeof(penalty));
   memcpy(&floor_value, &floor_bits, sizeof(floor_value));
   assert(penalty == 0.1 && floor_value == 0.0);

   uint8_t request[AIMEE_DB2_REJECT_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_reject_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_reject_request_decode(request, sizeof(request), &memory_id) == 0 &&
          memory_id == 42);
   /* The request is exactly a memory: there is no reason field to carry, so a
    * payload of any other size is malformed rather than an extended request. */
   assert(sizeof(request) == AIMEE_DB2_ENVELOPE_HEADER_LEN + 8u);
   assert(aimee_db2_reject_request_encode(0u, request, sizeof(request)) == -1);
   assert(aimee_db2_reject_request_encode(AIMEE_DB2_REJECT_MEMORY_ID_MAX + 1ull, request,
                                          sizeof(request)) == -1);
   assert(aimee_db2_reject_request_encode(42u, request, sizeof(request) - 1) == -1);
   assert(aimee_db2_reject_request_encode(42u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_reject_request_decode(request, sizeof(request), &memory_id) == -1 &&
          memory_id == 0);

   uint8_t reply[AIMEE_DB2_REJECT_RESPONSE_LEN] = {0};
   assert(aimee_db2_reject_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_reject_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_reject_reply_encode(reply, sizeof(reply) - 1) == -1);
   assert(aimee_db2_reject_reply_encode(reply, sizeof(reply)) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_reject_reply_decode(reply, sizeof(reply)) == -1);
}

static void test_has_scope_type_wire(void)
{
   static uint8_t request[AIMEE_DB2_HAS_SCOPE_TYPE_REQUEST_MAX_LEN];
   uint32_t request_len = 99;
   uint64_t memory_id = 99;
   char scope_kind[AIMEE_DB2_HAS_SCOPE_TYPE_SCOPE_MAX + 1] = "";
   assert(aimee_db2_has_scope_type_request_encode(42u, "workspace", request, sizeof(request),
                                                  &request_len) == 0);
   assert(aimee_db2_has_scope_type_request_decode(request, request_len, &memory_id, scope_kind,
                                                  sizeof(scope_kind)) == 0);
   assert(memory_id == 42 && strcmp(scope_kind, "workspace") == 0);

   assert(aimee_db2_has_scope_type_request_encode(0u, "workspace", request, sizeof(request),
                                                  &request_len) == -1);
   assert(aimee_db2_has_scope_type_request_encode(42u, "", request, sizeof(request),
                                                  &request_len) == -1);
   char too_long[AIMEE_DB2_HAS_SCOPE_TYPE_SCOPE_MAX + 2];
   memset(too_long, 'x', sizeof(too_long) - 1);
   too_long[sizeof(too_long) - 1] = '\0';
   assert(aimee_db2_has_scope_type_request_encode(42u, too_long, request, sizeof(request),
                                                  &request_len) == -1);

   uint8_t reply[AIMEE_DB2_HAS_SCOPE_TYPE_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, present = 99;
   assert(aimee_db2_has_scope_type_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_has_scope_type_reply_decode(reply, reply_len, &present) == 0 && present == 1);
   assert(aimee_db2_has_scope_type_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_has_scope_type_reply_decode(reply, reply_len, &present) == 0 && present == 0);
   assert(aimee_db2_has_scope_type_reply_encode(2, reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_has_scope_type_reply_encode(1, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_has_scope_type_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_has_scope_type_reply_decode(reply, reply_len, &present) == -1 && present == 0);
}

static void test_valid_at_wire(void)
{
   static uint8_t request[AIMEE_DB2_VALID_AT_REQUEST_MAX_LEN];
   uint32_t request_len = 99;
   uint64_t memory_id = 99;
   char as_of[AIMEE_DB2_VALID_AT_AS_OF_MAX + 1] = "";
   assert(aimee_db2_valid_at_request_encode(42u, "2026-08-18 12:00:00", request, sizeof(request),
                                            &request_len) == 0);
   assert(aimee_db2_valid_at_request_decode(request, request_len, &memory_id, as_of,
                                            sizeof(as_of)) == 0);
   assert(memory_id == 42 && strcmp(as_of, "2026-08-18 12:00:00") == 0);

   assert(aimee_db2_valid_at_request_encode(0u, "2026-08-18", request, sizeof(request),
                                            &request_len) == -1);
   /* An instant is required: "valid at no particular time" is not a question. */
   assert(aimee_db2_valid_at_request_encode(42u, "", request, sizeof(request), &request_len) == -1);
   char too_long[AIMEE_DB2_VALID_AT_AS_OF_MAX + 2];
   memset(too_long, 'x', sizeof(too_long) - 1);
   too_long[sizeof(too_long) - 1] = '\0';
   assert(aimee_db2_valid_at_request_encode(42u, too_long, request, sizeof(request),
                                            &request_len) == -1);

   uint8_t reply[AIMEE_DB2_VALID_AT_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, result = 99, in_force = 99;
   assert(aimee_db2_valid_at_reply_encode(AIMEE_DB2_RESULT_OK, 1, reply, sizeof(reply),
                                          &reply_len) == 0);
   assert(aimee_db2_valid_at_reply_decode(reply, reply_len, &result, &in_force) == 0 &&
          result == AIMEE_DB2_RESULT_OK && in_force == 1);
   assert(aimee_db2_valid_at_reply_encode(AIMEE_DB2_RESULT_OK, 0, reply, sizeof(reply),
                                          &reply_len) == 0);
   assert(aimee_db2_valid_at_reply_decode(reply, reply_len, &result, &in_force) == 0 &&
          result == AIMEE_DB2_RESULT_OK && in_force == 0);

   /* "Could not evaluate" carries no verdict at all, and must not be
    * encodable alongside one. */
   assert(aimee_db2_valid_at_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 0, reply, sizeof(reply),
                                          &reply_len) == 0);
   assert(reply_len == AIMEE_DB2_VALID_AT_ERROR_LEN);
   assert(aimee_db2_valid_at_reply_decode(reply, reply_len, &result, &in_force) == 0 &&
          result == AIMEE_DB2_RESULT_INVALID_STATE && in_force == 0);
   assert(aimee_db2_valid_at_reply_encode(AIMEE_DB2_RESULT_INVALID_STATE, 1, reply, sizeof(reply),
                                          &reply_len) == -1);
   assert(aimee_db2_valid_at_reply_encode(AIMEE_DB2_RESULT_OK, 2, reply, sizeof(reply),
                                          &reply_len) == -1);
}

static void test_link_delete_wire(void)
{
   uint8_t request[AIMEE_DB2_LINK_DELETE_REQUEST_LEN] = {0};
   uint64_t link_id = 99;
   assert(aimee_db2_link_delete_request_encode(7u, request, sizeof(request)) == 0);
   assert(aimee_db2_link_delete_request_decode(request, sizeof(request), &link_id) == 0 &&
          link_id == 7);
   assert(aimee_db2_link_delete_request_encode(0u, request, sizeof(request)) == -1);
   assert(aimee_db2_link_delete_request_encode(AIMEE_DB2_LINK_DELETE_LINK_ID_MAX + 1ull, request,
                                               sizeof(request)) == -1);
   assert(aimee_db2_link_delete_request_encode(7u, request, sizeof(request) - 1) == -1);
   assert(aimee_db2_link_delete_request_encode(7u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_link_delete_request_decode(request, sizeof(request), &link_id) == -1 &&
          link_id == 0);

   uint8_t reply[AIMEE_DB2_LINK_DELETE_RESPONSE_LEN] = {0};
   assert(aimee_db2_link_delete_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_link_delete_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_link_delete_reply_encode(reply, sizeof(reply) - 1) == -1);
   assert(aimee_db2_link_delete_reply_encode(reply, sizeof(reply)) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_link_delete_reply_decode(reply, sizeof(reply)) == -1);
}

static void test_touch_wire(void)
{
   uint8_t request[AIMEE_DB2_TOUCH_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_touch_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_touch_request_decode(request, sizeof(request), &memory_id) == 0 &&
          memory_id == 42);
   assert(aimee_db2_touch_request_encode(0u, request, sizeof(request)) == -1);
   assert(aimee_db2_touch_request_encode(AIMEE_DB2_TOUCH_MEMORY_ID_MAX + 1ull, request,
                                         sizeof(request)) == -1);
   assert(aimee_db2_touch_request_encode(42u, request, sizeof(request) - 1) == -1);
   assert(aimee_db2_touch_request_encode(42u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_touch_request_decode(request, sizeof(request), &memory_id) == -1 &&
          memory_id == 0);

   uint8_t reply[AIMEE_DB2_TOUCH_RESPONSE_LEN] = {0};
   assert(aimee_db2_touch_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_touch_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_touch_reply_encode(reply, sizeof(reply) - 1) == -1);
   assert(aimee_db2_touch_reply_encode(reply, sizeof(reply)) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_touch_reply_decode(reply, sizeof(reply)) == -1);
}

static void test_delete_row_wire(void)
{
   uint8_t request[AIMEE_DB2_DELETE_ROW_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_delete_row_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_delete_row_request_decode(request, sizeof(request), &memory_id) == 0 &&
          memory_id == 42);
   assert(aimee_db2_delete_row_request_encode(0u, request, sizeof(request)) == -1);
   assert(aimee_db2_delete_row_request_encode(AIMEE_DB2_DELETE_ROW_MEMORY_ID_MAX + 1ull, request,
                                              sizeof(request)) == -1);
   assert(aimee_db2_delete_row_request_encode(42u, request, sizeof(request) - 1) == -1);
   assert(aimee_db2_delete_row_request_encode(42u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_delete_row_request_decode(request, sizeof(request), &memory_id) == -1 &&
          memory_id == 0);

   uint8_t reply[AIMEE_DB2_DELETE_ROW_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, removed = 99;
   assert(aimee_db2_delete_row_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_delete_row_reply_decode(reply, reply_len, &removed) == 0 && removed == 1);
   assert(aimee_db2_delete_row_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_delete_row_reply_decode(reply, reply_len, &removed) == 0 && removed == 0);
   /* A primary-key delete can remove at most one row. */
   assert(aimee_db2_delete_row_reply_encode(2, reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_delete_row_reply_encode(1, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_delete_row_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_delete_row_reply_decode(reply, reply_len, &removed) == -1 && removed == 0);
}

static void test_has_workspace_tag_wire(void)
{
   uint8_t request[AIMEE_DB2_HAS_WORKSPACE_TAG_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_has_workspace_tag_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_has_workspace_tag_request_decode(request, sizeof(request), &memory_id) == 0 &&
          memory_id == 42);
   assert(aimee_db2_has_workspace_tag_request_encode(0u, request, sizeof(request)) == -1);
   assert(aimee_db2_has_workspace_tag_request_encode(
              AIMEE_DB2_HAS_WORKSPACE_TAG_MEMORY_ID_MAX + 1ull, request, sizeof(request)) == -1);
   assert(aimee_db2_has_workspace_tag_request_encode(42u, request, sizeof(request) - 1) == -1);
   assert(aimee_db2_has_workspace_tag_request_encode(42u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_has_workspace_tag_request_decode(request, sizeof(request), &memory_id) == -1 &&
          memory_id == 0);

   uint8_t reply[AIMEE_DB2_HAS_WORKSPACE_TAG_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, tagged = 99;
   assert(aimee_db2_has_workspace_tag_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_has_workspace_tag_reply_decode(reply, reply_len, &tagged) == 0 && tagged == 1);
   assert(aimee_db2_has_workspace_tag_reply_encode(0, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_has_workspace_tag_reply_decode(reply, reply_len, &tagged) == 0 && tagged == 0);
   /* The probe is LIMIT 1, so the flag is Boolean and nothing wider encodes. */
   assert(aimee_db2_has_workspace_tag_reply_encode(2, reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_has_workspace_tag_reply_encode(1, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_has_workspace_tag_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_has_workspace_tag_reply_decode(reply, reply_len, &tagged) == -1 && tagged == 0);
}

static void test_demote_id_wire(void)
{
   /* The decay multiplier and the floor are policy, pinned as binary64 bit
    * patterns because the catalog refuses float literals. They are compared as
    * bits so a rounding-different constant on either side is a failure. */
   uint64_t multiplier_bits = AIMEE_DB2_DEMOTE_ID_MULTIPLIER_BITS;
   uint64_t floor_bits = AIMEE_DB2_DEMOTE_ID_MINIMUM_CONFIDENCE_BITS;
   double multiplier = 0.0, floor_value = 0.0;
   memcpy(&multiplier, &multiplier_bits, sizeof(multiplier));
   memcpy(&floor_value, &floor_bits, sizeof(floor_value));
   assert(multiplier == 0.9 && floor_value == 0.3);

   uint8_t request[AIMEE_DB2_DEMOTE_ID_REQUEST_LEN] = {0};
   uint64_t memory_id = 99;
   assert(aimee_db2_demote_id_request_encode(42u, request, sizeof(request)) == 0);
   assert(aimee_db2_demote_id_request_decode(request, sizeof(request), &memory_id) == 0 &&
          memory_id == 42);
   /* Zero is not a memory: it must not survive either direction. */
   assert(aimee_db2_demote_id_request_encode(0u, request, sizeof(request)) == -1);
   assert(aimee_db2_demote_id_request_encode(AIMEE_DB2_DEMOTE_ID_MEMORY_ID_MAX + 1ull, request,
                                             sizeof(request)) == -1);
   assert(aimee_db2_demote_id_request_encode(42u, request, sizeof(request) - 1) == -1);
   assert(aimee_db2_demote_id_request_encode(42u, request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_demote_id_request_decode(request, sizeof(request), &memory_id) == -1 &&
          memory_id == 0);

   uint8_t reply[AIMEE_DB2_DEMOTE_ID_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, demoted = 99;
   assert(aimee_db2_demote_id_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_demote_id_reply_decode(reply, reply_len, &demoted) == 0 && demoted == 1);
   /* A primary-key equality can touch at most one row. */
   assert(aimee_db2_demote_id_reply_encode(2, reply, sizeof(reply), &reply_len) == -1);
   assert(aimee_db2_demote_id_reply_encode(1, reply, sizeof(reply) - 1, &reply_len) == -1);
   assert(aimee_db2_demote_id_reply_encode(1, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_demote_id_reply_decode(reply, reply_len, &demoted) == -1 && demoted == 0);
}

static void test_lifecycle_sweep_expired_wire(void)
{
   /* Both lifecycle states and the archive reason are compiled-in policy that
    * must match the reviewed catalog; they are never encoded. */
   assert(strcmp(AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_SOURCE_STATE, "pending") == 0);
   assert(strcmp(AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_TARGET_STATE, "archived") == 0);
   assert(strcmp(AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_REASON, "pending_ttl_expired") == 0);

   uint8_t request[AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_REQUEST_LEN] = {0};
   assert(aimee_db2_lifecycle_sweep_expired_request_encode(request, sizeof(request)) == 0);
   assert(aimee_db2_lifecycle_sweep_expired_request_decode(request, sizeof(request)) == 0);
   aimee_db2_put_u32(request + 12, 1u);
   assert(aimee_db2_lifecycle_sweep_expired_request_decode(request, sizeof(request)) == -1);

   uint8_t reply[AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_RESPONSE_LEN] = {0};
   uint32_t reply_len = 99, archived = 99;
   assert(aimee_db2_lifecycle_sweep_expired_reply_encode(4, reply, sizeof(reply), &reply_len) == 0);
   assert(aimee_db2_lifecycle_sweep_expired_reply_decode(reply, reply_len, &archived) == 0 &&
          archived == 4);
   assert(aimee_db2_lifecycle_sweep_expired_reply_encode(
              AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_COUNT_MAX + 1u, reply, sizeof(reply), &reply_len) ==
          -1);
   assert(aimee_db2_lifecycle_sweep_expired_reply_encode(4, reply, sizeof(reply) - 1, &reply_len) ==
          -1);
   assert(aimee_db2_lifecycle_sweep_expired_reply_encode(4, reply, sizeof(reply), &reply_len) == 0);
   aimee_db2_put_u32(reply + 12, AIMEE_DB2_RESULT_INVALID_STATE);
   assert(aimee_db2_lifecycle_sweep_expired_reply_decode(reply, reply_len, &archived) == -1 &&
          archived == 0);
}

static void test_record_l4_approval_wire(void)
{
   assert(strcmp(AIMEE_DB2_RECORD_L4_APPROVAL_TIER, "L4") == 0);

   static uint8_t request[AIMEE_DB2_RECORD_L4_APPROVAL_REQUEST_MAX_LEN];
   uint32_t request_len = 99;
   uint64_t memory_id = 99;
   char approver[AIMEE_DB2_RECORD_L4_APPROVAL_APPROVER_MAX + 1] = "";
   char note[AIMEE_DB2_RECORD_L4_APPROVAL_NOTE_MAX + 1] = "";
   assert(aimee_db2_record_l4_approval_request_encode(42u, "operator", "reviewed", request,
                                                      sizeof(request), &request_len) == 0);
   assert(aimee_db2_record_l4_approval_request_decode(request, request_len, &memory_id, approver,
                                                      sizeof(approver), note, sizeof(note)) == 0);
   assert(memory_id == 42u && strcmp(approver, "operator") == 0 && strcmp(note, "reviewed") == 0);

   /* An empty note is legal and produces the shortest request. */
   assert(aimee_db2_record_l4_approval_request_encode(42u, "o", "", request, sizeof(request),
                                                      &request_len) == 0);
   assert(request_len == AIMEE_DB2_RECORD_L4_APPROVAL_REQUEST_MIN_LEN);
   assert(aimee_db2_record_l4_approval_request_decode(request, request_len, &memory_id, approver,
                                                      sizeof(approver), note, sizeof(note)) == 0);
   assert(note[0] == '\0');

   /* An empty approver is not: someone must be accountable. */
   assert(aimee_db2_record_l4_approval_request_encode(42u, "", "reviewed", request, sizeof(request),
                                                      &request_len) == -1);
   assert(request_len == 0);

   /* Both string bounds and the identifier bound hold. */
   char long_approver[AIMEE_DB2_RECORD_L4_APPROVAL_APPROVER_MAX + 2];
   memset(long_approver, 'a', sizeof(long_approver) - 1);
   long_approver[sizeof(long_approver) - 1] = '\0';
   assert(aimee_db2_record_l4_approval_request_encode(42u, long_approver, "", request,
                                                      sizeof(request), &request_len) == -1);
   assert(aimee_db2_record_l4_approval_request_encode(0u, "operator", "", request, sizeof(request),
                                                      &request_len) == -1);

   /* The longest legal request fits the declared maximum exactly. */
   char max_approver[AIMEE_DB2_RECORD_L4_APPROVAL_APPROVER_MAX + 1];
   memset(max_approver, 'a', sizeof(max_approver) - 1);
   max_approver[sizeof(max_approver) - 1] = '\0';
   static char max_note[AIMEE_DB2_RECORD_L4_APPROVAL_NOTE_MAX + 1];
   memset(max_note, 'n', sizeof(max_note) - 1);
   max_note[sizeof(max_note) - 1] = '\0';
   assert(aimee_db2_record_l4_approval_request_encode(42u, max_approver, max_note, request,
                                                      sizeof(request), &request_len) == 0);
   assert(request_len == AIMEE_DB2_RECORD_L4_APPROVAL_REQUEST_MAX_LEN);

   /* A caller buffer too small for the decoded strings is refused. */
   char tiny[4] = "";
   assert(aimee_db2_record_l4_approval_request_decode(request, request_len, &memory_id, tiny,
                                                      sizeof(tiny), note, sizeof(note)) == -1);

   uint8_t reply[AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN] = {0};
   assert(aimee_db2_record_l4_approval_reply_encode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_record_l4_approval_reply_decode(reply, sizeof(reply)) == 0);
   assert(aimee_db2_record_l4_approval_reply_encode(reply, sizeof(reply) - 1) == -1);
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

static void test_health_retention_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.prune_health = prune_health,
                                               .prune_contradictions = prune_contradictions};
   uint8_t request[AIMEE_DB2_HEALTH_RETENTION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_HEALTH_RETENTION_RESPONSE_LEN];
   uint32_t response_len = 99, snapshots = 99, contradictions = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_HEALTH_RETENTION};
   assert(aimee_db2_health_retention_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_health_retention_reply_decode(response, response_len, &snapshots,
                                                  &contradictions) == 0);
   assert(snapshots == 11 && contradictions == 3);
   assert(prune_health_days == AIMEE_DB2_HEALTH_RETENTION_SNAPSHOT_DAYS &&
          prune_contradictions_days == AIMEE_DB2_HEALTH_RETENTION_CONTRADICTION_DAYS);

   /* Either half failing fails the whole action. */
   prune_health_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   prune_health_value = 11;
   prune_contradictions_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   prune_contradictions_value = 3;

   /* Neither half is reachable without the other. */
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   const aimee_db2_module_backend_t snapshots_only = {.prune_health = prune_health};
   assert(invoke(&snapshots_only, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   const aimee_db2_module_backend_t contradictions_only = {.prune_contradictions =
                                                               prune_contradictions};
   assert(invoke(&contradictions_only, &invocation, request, sizeof(request), response,
                 sizeof(response), &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_health_counters_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.health_counters = health_counters};
   uint8_t request[AIMEE_DB2_HEALTH_COUNTERS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_HEALTH_COUNTERS_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_db2_health_counters_t decoded = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_HEALTH_COUNTERS};
   assert(aimee_db2_health_counters_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(health_counters_calls == 1 &&
          health_counters_use_count == (int)AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_USE_COUNT &&
          health_counters_confidence == AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_CONFIDENCE);
   assert(aimee_db2_health_counters_reply_decode(response, response_len, &decoded) == 0);
   assert(decoded.cycles == 7 && decoded.total_contradictions == 13 && decoded.l2_total == 30 &&
          decoded.l2_stale_30_days == 6);

   health_counters_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   health_counters_result = 0;

   /* A counter past the declared bound never reaches the wire. */
   health_counters_cycles = AIMEE_DB2_HEALTH_COUNTERS_MAX + 1u;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   health_counters_cycles = 7;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_stats_counts_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.stats_counts = stats_counts};
   uint8_t request[AIMEE_DB2_STATS_COUNTS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_STATS_COUNTS_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_db2_memory_stats_t decoded = {0};
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_STATS_COUNTS};
   assert(aimee_db2_stats_counts_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(stats_counts_calls == 1);
   assert(aimee_db2_stats_counts_reply_decode(response, response_len, &decoded) == 0);
   assert(decoded.tier_counts[0] == 3 && decoded.tier_counts[5] == 1 &&
          decoded.kind_counts[0] == 14 && decoded.kind_counts[9] == 5 && decoded.total == 56 &&
          decoded.conflicts == 4);

   stats_counts_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   stats_counts_result = 0;

   /* A bucket past the declared bound never reaches the wire. */
   stats_counts_last_kind = AIMEE_DB2_STATS_COUNTS_MAX + 1u;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   stats_counts_last_kind = 5;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_expire_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .delete_l0_provenance = delete_l0_provenance,
       .delete_l0 = delete_l0,
       .list_kinds_in_tier = list_kinds_in_tier,
       .kind_expire_days = kind_expire_days,
       .delete_stale_l1_provenance = delete_stale_l1_provenance,
       .delete_stale_l1 = delete_stale_l1,
   };
   uint8_t request[AIMEE_DB2_EXPIRE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EXPIRE_RESPONSE_LEN];
   uint32_t response_len = 99, level0 = 99, stale = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EXPIRE};
   assert(aimee_db2_expire_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_expire_reply_decode(response, response_len, &level0, &stale) == 0);
   /* Two kinds at 4 stale rows each, and the per-kind window reaches the delete. */
   assert(level0 == 9 && stale == 8 && expire_l0_provenance_calls == 1 &&
          expire_stale_provenance_calls == 2);
   assert(strcmp(expire_last_window, "-7") == 0);

   /* An empty L1 tier still answers, with no stale work done. */
   reset();
   expire_kind_count = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_expire_reply_decode(response, response_len, &level0, &stale) == 0);
   assert(level0 == 9 && stale == 0 && expire_stale_provenance_calls == 0);

   /* Every stage's failure fails the whole action. */
   reset();
   expire_l0_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   reset();
   expire_kind_count = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   reset();
   expire_stale_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   /* A kind whose window is missing must not silently expire everything. */
   reset();
   expire_days_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   /* More kinds than the declared bound is refused, not truncated. */
   reset();
   expire_kind_count = (int)AIMEE_DB2_EXPIRE_KINDS_MAX + 1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   /* Every composed capability is required. */
   reset();
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   aimee_db2_module_backend_t partial = backend;
   partial.delete_stale_l1 = NULL;
   assert(invoke(&partial, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   partial = backend;
   partial.kind_expire_days = NULL;
   assert(invoke(&partial, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_demote_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .now_utc = contract_now_utc,
       .list_kinds_in_tier = list_kinds_in_tier,
       .kind_demote_policy = kind_demote_policy,
       .demote_kind = demote_kind,
       .demote_cascade = demote_cascade,
   };
   uint8_t request[AIMEE_DB2_DEMOTE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DEMOTE_RESPONSE_LEN];
   uint32_t response_len = 99, demoted = 99, cascaded = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_DEMOTE};
   assert(aimee_db2_demote_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_demote_reply_decode(response, response_len, &demoted, &cascaded) == 0);
   /* Two kinds at 3 rows each, one cascade, and the per-kind window applied. */
   assert(demoted == 6 && cascaded == 2 && demote_cascade_calls == 1);
   assert(strcmp(expire_last_window, "-14") == 0);
   /* The cascade must run against exactly the stamp the demotions carried. */
   assert(demote_kind_stamp[0] && strcmp(demote_kind_stamp, demote_cascade_stamp) == 0);

   /* Nothing demoted means no cascade at all, not a zero-row cascade. */
   reset();
   demote_kind_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_demote_reply_decode(response, response_len, &demoted, &cascaded) == 0);
   assert(demoted == 0 && cascaded == 0 && demote_cascade_calls == 0);

   /* Every stage's failure fails the whole action. */
   reset();
   demote_policy_result = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   reset();
   demote_kind_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   reset();
   demote_cascade_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   /* A kind whose window is missing must not demote on an unbounded one. */
   reset();
   demote_days_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   /* Every composed capability is required. */
   reset();
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   aimee_db2_module_backend_t partial = backend;
   partial.demote_cascade = NULL;
   assert(invoke(&partial, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   partial = backend;
   partial.now_utc = NULL;
   assert(invoke(&partial, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_promote_stable_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.now_utc = contract_now_utc,
                                               .promote_stable = promote_stable};
   uint8_t request[AIMEE_DB2_PROMOTE_STABLE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PROMOTE_STABLE_RESPONSE_LEN];
   uint32_t response_len = 99, promoted = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_PROMOTE_STABLE};
   assert(aimee_db2_promote_stable_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(promote_stable_calls == 1 && promote_stable_stamp[0]);
   assert(aimee_db2_promote_stable_reply_decode(response, response_len, &promoted) == 0 &&
          promoted == 4);

   promote_stable_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   promote_stable_value = 4;

   /* Both the stamp and the promotion are required. */
   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   const aimee_db2_module_backend_t no_clock = {.promote_stable = promote_stable};
   assert(invoke(&no_clock, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_reclassify_directives_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.reclassify_directives = reclassify_directives};
   uint8_t request[AIMEE_DB2_RECLASSIFY_DIRECTIVES_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RECLASSIFY_DIRECTIVES_RESPONSE_LEN];
   uint32_t response_len = 99, reclassified = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_RECLASSIFY_DIRECTIVES};

   /* The gate the caller sent is the gate the backend sees. */
   assert(aimee_db2_reclassify_directives_request_encode(1u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(reclassify_calls == 1 && reclassify_last_gate == 1);
   assert(aimee_db2_reclassify_directives_reply_decode(response, response_len, &reclassified) ==
              0 &&
          reclassified == 3);

   assert(aimee_db2_reclassify_directives_request_encode(0u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(reclassify_calls == 2 && reclassify_last_gate == 0);

   reclassify_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   reclassify_value = 3;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_prune_orphaned_l0_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.prune_orphaned_l0 = prune_orphaned_l0};
   uint8_t request[AIMEE_DB2_PRUNE_ORPHANED_L0_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PRUNE_ORPHANED_L0_RESPONSE_LEN];
   uint32_t response_len = 99, deleted = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_PRUNE_ORPHANED_L0};
   assert(aimee_db2_prune_orphaned_l0_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(prune_orphaned_l0_calls == 1);
   assert(aimee_db2_prune_orphaned_l0_reply_decode(response, response_len, &deleted) == 0 &&
          deleted == 3);

   /* An empty sweep is a success reporting zero, not a fault. */
   prune_orphaned_l0_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_prune_orphaned_l0_reply_decode(response, response_len, &deleted) == 0 &&
          deleted == 0);

   /* The backend signals a connection or statement failure with -1. */
   prune_orphaned_l0_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   /* A count past the declared bound is a fault, not a truncated reply. */
   prune_orphaned_l0_value = (int)AIMEE_DB2_PRUNE_ORPHANED_L0_COUNT_MAX;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   prune_orphaned_l0_value = 3;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_curator_reenqueue_extract_all_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.curator_reenqueue_extract_all =
                                                   curator_reenqueue_extract_all};
   uint8_t request[AIMEE_DB2_CURATOR_REENQUEUE_EXTRACT_ALL_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_CURATOR_REENQUEUE_EXTRACT_ALL_RESPONSE_LEN];
   uint32_t response_len = 99, jobs = 99;
   aimee_module_invocation_t invocation = {.stage_id =
                                               AIMEE_DB2_STAGE_CURATOR_REENQUEUE_EXTRACT_ALL};
   assert(aimee_db2_curator_reenqueue_extract_all_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(extract_reenqueue_calls == 1);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_decode(response, response_len, &jobs) ==
              0 &&
          jobs == 14);

   /* The number is the queue after the pass, not the rows the pass touched.
    * A second call over an unchanged corpus therefore returns the same number
    * rather than zero -- the count is a size, and sizes do not go to zero
    * because nothing moved. */
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_decode(response, response_len, &jobs) ==
              0 &&
          jobs == 14);
   assert(extract_reenqueue_calls == 2);

   /* No connection is zero here, which an empty corpus also produces. */
   extract_reenqueue_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_curator_reenqueue_extract_all_reply_decode(response, response_len, &jobs) ==
              0 &&
          jobs == 0);
   extract_reenqueue_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   extract_reenqueue_value = 14;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_synth_reenqueue_all_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.synth_reenqueue_all = synth_reenqueue_all};
   uint8_t request[AIMEE_DB2_SYNTH_REENQUEUE_ALL_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_SYNTH_REENQUEUE_ALL_RESPONSE_LEN];
   uint32_t response_len = 99, ops = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_SYNTH_REENQUEUE_ALL};
   assert(aimee_db2_synth_reenqueue_all_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(synth_reenqueue_calls == 1);
   assert(aimee_db2_synth_reenqueue_all_reply_decode(response, response_len, &ops) == 0 &&
          ops == 13);

   /* Same shape as the evidence reset it mirrors: an empty table and a failed
    * statement are both zero, and replaying loses no failure history that has
    * already been cleared. */
   synth_reenqueue_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_synth_reenqueue_all_reply_decode(response, response_len, &ops) == 0 &&
          ops == 0);
   synth_reenqueue_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   synth_reenqueue_value = 13;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_curator_reembed_all_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.curator_reembed_all = curator_reembed_all};
   uint8_t request[AIMEE_DB2_CURATOR_REEMBED_ALL_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_CURATOR_REEMBED_ALL_RESPONSE_LEN];
   uint32_t response_len = 99, demoted = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_CURATOR_REEMBED_ALL};
   assert(aimee_db2_curator_reembed_all_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(curator_reembed_calls == 1);
   assert(aimee_db2_curator_reembed_all_reply_decode(response, response_len, &demoted) == 0 &&
          demoted == 12);

   /* Nothing committed in a re-derivable kind is zero, and replaying finds the
    * same zero because the rows are already proposed. That is what the
    * catalog's safe idempotency claims for a state transition like this. */
   curator_reembed_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_curator_reembed_all_reply_decode(response, response_len, &demoted) == 0 &&
          demoted == 0);

   /* A failed statement is also zero from this backend, so the count cannot
    * separate it from an empty set. Pinned so the limitation stays visible. */
   curator_reembed_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   curator_reembed_value = 12;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_evidence_reembed_all_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.evidence_reembed_all = evidence_reembed_all};
   uint8_t request[AIMEE_DB2_EVIDENCE_REEMBED_ALL_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_EVIDENCE_REEMBED_ALL_RESPONSE_LEN];
   uint32_t response_len = 99, rows = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_EVIDENCE_REEMBED_ALL};
   assert(aimee_db2_evidence_reembed_all_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(evidence_reembed_calls == 1);
   assert(aimee_db2_evidence_reembed_all_reply_decode(response, response_len, &rows) == 0 &&
          rows == 11);

   /* An empty evidence index has nothing to requeue. Replaying the reset finds
    * the same nothing, which is what the catalog's safe idempotency claims --
    * a second call cannot lose failure history that has already been cleared. */
   evidence_reembed_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_evidence_reembed_all_reply_decode(response, response_len, &rows) == 0 &&
          rows == 0);

   /* This backend reports a missing connection or statement as zero, so an
    * empty result and a failed one are the same number. Pinned so the
    * limitation stays visible; the negative path is still refused. */
   evidence_reembed_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   evidence_reembed_value = 11;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_ingest_queue_reset_running_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.ingest_queue_reset_running =
                                                   ingest_queue_reset_running};
   uint8_t request[AIMEE_DB2_INGEST_QUEUE_RESET_RUNNING_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_INGEST_QUEUE_RESET_RUNNING_RESPONSE_LEN];
   uint32_t response_len = 99, reset_rows = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_INGEST_QUEUE_RESET_RUNNING};
   assert(aimee_db2_ingest_queue_reset_running_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(queue_reset_calls == 1);
   assert(aimee_db2_ingest_queue_reset_running_reply_decode(response, response_len, &reset_rows) ==
              0 &&
          reset_rows == 10);

   /* No abandoned row is the ordinary case: the previous run drained cleanly.
    * Zero says so, and replaying the recovery finds nothing to hand back,
    * which is what the catalog's safe idempotency claims. */
   queue_reset_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_ingest_queue_reset_running_reply_decode(response, response_len, &reset_rows) ==
              0 &&
          reset_rows == 0);

   /* No connection and no statement are -1 here, and reporting that as zero
    * would claim a clean queue while abandoned rows are still stranded. */
   queue_reset_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   queue_reset_value = 10;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_mark_revisit_due_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.mark_revisit_due = mark_revisit_due};
   uint8_t request[AIMEE_DB2_MARK_REVISIT_DUE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_MARK_REVISIT_DUE_RESPONSE_LEN];
   uint32_t response_len = 99, marked = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_MARK_REVISIT_DUE};
   assert(aimee_db2_mark_revisit_due_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(mark_revisit_calls == 1);
   assert(aimee_db2_mark_revisit_due_reply_decode(response, response_len, &marked) == 0 &&
          marked == 9);

   /* Unlike the two sweeps beside it, this backend does separate a failed
    * statement from an empty one: no connection, no statement and a step that
    * did not finish are all -1, while nothing being due is zero. The boundary
    * keeps that apart instead of reporting an untouched log as success. */
   mark_revisit_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_mark_revisit_due_reply_decode(response, response_len, &marked) == 0 &&
          marked == 0);
   mark_revisit_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   mark_revisit_value = 9;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_project_clear_operations_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.file_index_delete_project =
                                                   file_index_delete_project,
                                               .clear_project = clear_project,
                                               .clear_current_project = clear_current_project};
   uint8_t request[AIMEE_DB2_CLEAR_PROJECT_REQUEST_MAX];
   uint8_t response[AIMEE_DB2_CLEAR_PROJECT_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99, deleted = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_CLEAR_PROJECT};
   assert(aimee_db2_clear_project_request_encode("demo", request, sizeof(request), &request_len) ==
          0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(clear_project_calls == 1 && strcmp(clear_project_seen, "demo") == 0);
   assert(aimee_db2_clear_project_reply_decode(response, response_len, &deleted) == 0 &&
          deleted == 52);

   /* The name that arrives here is not necessarily the name that gets matched:
    * the backend normalises it before building the statement. That is recorded
    * as policy because it means a caller cannot predict from its own string
    * which rows will go, and no test on this side can show it. */
   assert(strcmp(clear_project_seen, "demo") == 0);

   /* A negative from the backend is a failed statement rather than an empty
    * project, and the boundary keeps it as one. */
   clear_project_value = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   clear_project_value = 52;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_by_id_operations_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.anti_pattern_bump = anti_pattern_bump,
                                               .anti_pattern_delete = anti_pattern_delete,
                                               .doc_delete = doc_delete,
                                               .task_delete = task_delete};
   uint8_t request[AIMEE_DB2_ANTI_PATTERN_BUMP_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_ANTI_PATTERN_BUMP_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t bump = {.stage_id = AIMEE_DB2_STAGE_ANTI_PATTERN_BUMP};
   assert(aimee_db2_anti_pattern_bump_request_encode(41, request, sizeof(request)) == 0);
   assert(invoke(&backend, &bump, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(anti_pattern_bump_calls == 1 && anti_pattern_bump_seen == 41);

   /* The bump and the delete sit on one table and disagree about a missing
    * row: the bump reports success whether or not it counted anything, the
    * delete reports an error. Neither backend distinguishes that from a real
    * failure, so on the wire both arrive as internal and the difference is
    * only visible in the catalog. Pinned here so the disagreement is a
    * decision on the record rather than something a caller trips over. */
   anti_pattern_bump_value = -1;
   assert(invoke(&backend, &bump, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   anti_pattern_bump_value = 0;

   uint8_t task_request[AIMEE_DB2_TASK_DELETE_REQUEST_LEN];
   uint8_t task_response[AIMEE_DB2_TASK_DELETE_RESPONSE_LEN];
   aimee_module_invocation_t task = {.stage_id = AIMEE_DB2_STAGE_TASK_DELETE};
   assert(aimee_db2_task_delete_request_encode(44, task_request, sizeof(task_request)) == 0);
   assert(invoke(&backend, &task, task_request, sizeof(task_request), task_response,
                 sizeof(task_response), &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(task_delete_calls == 1 && task_delete_seen == 44);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &bump, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &bump, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_directive_id_operations_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.directive_suppress = directive_suppress,
                                               .directive_record_surface =
                                                   directive_record_surface};
   uint8_t request[AIMEE_DB2_DIRECTIVE_SUPPRESS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DIRECTIVE_SUPPRESS_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_DIRECTIVE_SUPPRESS};
   assert(aimee_db2_directive_suppress_request_encode(31, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(directive_suppress_calls == 1 && directive_suppress_id == 31);
   assert(aimee_db2_directive_suppress_reply_decode(response, response_len) == 0);

   /* A directive that was not open and a statement that failed produce the
    * same negative from this backend, so both arrive as internal. The wire
    * could carry not_found, but the backend keeps nothing to build it from,
    * and inventing the distinction here would be a claim the data cannot
    * support. Recorded in the catalog as absent_collapsed_into_error. */
   directive_suppress_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   directive_suppress_value = 0;

   aimee_module_invocation_t surfacing = {.stage_id = AIMEE_DB2_STAGE_DIRECTIVE_RECORD_SURFACE};
   uint8_t surface_request[AIMEE_DB2_DIRECTIVE_RECORD_SURFACE_REQUEST_LEN];
   assert(aimee_db2_directive_record_surface_request_encode(32, surface_request,
                                                            sizeof(surface_request)) == 0);
   assert(invoke(&backend, &surfacing, surface_request, sizeof(surface_request), response,
                 sizeof(response), &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(directive_surface_calls == 1 && directive_surface_id == 32);

   /* Both stages resolve to the same family, so a suppression request sent on
    * the surfacing stage still reaches this branch. It is the operation number
    * in the envelope that keeps it from being surfaced instead. */
   assert(invoke(&backend, &surfacing, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   /* Three suppressions have now reached the backend: the first, the one
    * that returned a failure, and this one arriving on the other stage. */
   assert(directive_suppress_calls == 3 && directive_surface_calls == 1);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_directive_sweep_expired_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.directive_sweep_expired = directive_sweep_expired};
   uint8_t request[AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DIRECTIVE_SWEEP_EXPIRED_RESPONSE_LEN];
   uint32_t response_len = 99, directives = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_DIRECTIVE_SWEEP_EXPIRED};
   assert(aimee_db2_directive_sweep_expired_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(directive_sweep_calls == 1);
   assert(aimee_db2_directive_sweep_expired_reply_decode(response, response_len, &directives) ==
              0 &&
          directives == 8);

   /* Two C symbols run this statement. The production boundary binds the one
    * that returns -1 on failure, so a failed sweep arrives as internal rather
    * than as an untouched set of directives; the other collapses both into
    * zero. Only the reporting differs, and this pins which behaviour the wire
    * gets. */
   directive_sweep_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_directive_sweep_expired_reply_decode(response, response_len, &directives) ==
              0 &&
          directives == 0);
   directive_sweep_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   directive_sweep_value = 8;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_prospective_sweep_expired_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.prospective_sweep_expired =
                                                   prospective_sweep_expired};
   uint8_t request[AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PROSPECTIVE_SWEEP_EXPIRED_RESPONSE_LEN];
   uint32_t response_len = 99, expired = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_PROSPECTIVE_SWEEP_EXPIRED};
   assert(aimee_db2_prospective_sweep_expired_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(prospective_sweep_calls == 1);
   assert(aimee_db2_prospective_sweep_expired_reply_decode(response, response_len, &expired) == 0 &&
          expired == 7);

   /* The stage is what separates this from index.entity_edge_prune_orphans:
    * the two share operation number 1 and produce identical request bytes.
    * Sent on the index stage the same bytes reach the index backend, which is
    * absent here, so the answer is capability-absent rather than a sweep. */
   aimee_module_invocation_t crossed = {.stage_id = AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS};
   assert(invoke(&backend, &crossed, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(prospective_sweep_calls == 1);

   /* Nothing armed has expired yet, and a failed statement, are both zero from
    * this backend. Pinned so the limitation stays visible. */
   prospective_sweep_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_prospective_sweep_expired_reply_decode(response, response_len, &expired) == 0 &&
          expired == 0);
   prospective_sweep_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   prospective_sweep_value = 7;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_release_get_active_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.release_get_active = release_get_active};
   uint8_t request[AIMEE_DB2_RELEASE_GET_ACTIVE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RELEASE_GET_ACTIVE_RESPONSE_LEN];
   uint32_t response_len = 99;
   uint64_t release_id = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_RELEASE_GET_ACTIVE};
   assert(aimee_db2_release_get_active_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(release_active_calls == 1);
   assert(aimee_db2_release_get_active_reply_decode(response, response_len, &release_id) == 0 &&
          release_id == 21);

   /* Zero is the interesting value and this is the only place it gets said:
    * it covers no key at all, a state value that would not parse, and one that
    * parsed to something not positive. The backend collapses all three before
    * the boundary sees anything, so no test here can separate them either --
    * which is exactly why the catalog carries both collapses in writing. */
   release_active_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_release_get_active_reply_decode(response, response_len, &release_id) == 0 &&
          release_id == 0);

   /* The backend clamps its own negatives to zero, so a negative reaching the
    * boundary would mean the backend changed underneath this operation. */
   release_active_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   release_active_value = 21;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_vector_rebuild_lock_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {
       .vector_rebuild_lock_try_acquire = vector_rebuild_lock_try_acquire,
       .vector_rebuild_lock_release = vector_rebuild_lock_release};
   uint8_t request[AIMEE_DB2_VECTOR_REBUILD_LOCK_TRY_ACQUIRE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_VECTOR_REBUILD_LOCK_TRY_ACQUIRE_RESPONSE_LEN];
   uint32_t response_len = 99, acquired = 99;
   aimee_module_invocation_t invocation = {.stage_id =
                                               AIMEE_DB2_STAGE_VECTOR_REBUILD_LOCK_TRY_ACQUIRE};
   assert(aimee_db2_vector_rebuild_lock_try_acquire_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(lock_acquire_calls == 1);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_decode(response, response_len,
                                                                 &acquired) == 0 &&
          acquired == 1);

   /* A zero is the lock already held by someone within the lease window. */
   lock_acquire_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_decode(response, response_len,
                                                                 &acquired) == 0 &&
          acquired == 0);

   /* The handler returns whatever the backend says, including two consecutive
    * ones. That is all this stub can show: it demonstrates the pass-through,
    * not that the real backend races. The race is real and lives in the
    * backend -- a read and a separate write, so two callers that both read
    * before either writes are both told they hold the lock -- but showing it
    * needs concurrency neither this test nor the replay has. The catalog
    * records it as mutually_exclusive false, which is where the claim belongs
    * when no test can carry it. */
   lock_acquire_value = 1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_decode(response, response_len,
                                                                 &acquired) == 0 &&
          acquired == 1);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_vector_rebuild_lock_try_acquire_reply_decode(response, response_len,
                                                                 &acquired) == 0 &&
          acquired == 1);

   /* The release takes no holder and reports no failure, so it is the same
    * acknowledgement whether or not this caller ever held the lock. */
   uint8_t release_request[AIMEE_DB2_VECTOR_REBUILD_LOCK_RELEASE_REQUEST_LEN];
   uint8_t release_response[AIMEE_DB2_VECTOR_REBUILD_LOCK_RELEASE_RESPONSE_LEN];
   aimee_module_invocation_t release = {.stage_id = AIMEE_DB2_STAGE_VECTOR_REBUILD_LOCK_RELEASE};
   assert(aimee_db2_vector_rebuild_lock_release_request_encode(release_request,
                                                               sizeof(release_request)) == 0);
   assert(invoke(&backend, &release, release_request, sizeof(release_request), release_response,
                 sizeof(release_response), &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(lock_release_calls == 1);
   assert(aimee_db2_vector_rebuild_lock_release_reply_decode(release_response, response_len) == 0);
   assert(invoke(&backend, &release, release_request, sizeof(release_request), release_response,
                 sizeof(release_response), &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(lock_release_calls == 2);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&absent, &release, release_request, sizeof(release_request), release_response,
                 sizeof(release_response), &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_rel_types_ensure_seed_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.rel_types_ensure_seed = rel_types_ensure_seed};
   uint8_t request[AIMEE_DB2_REL_TYPES_ENSURE_SEED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REL_TYPES_ENSURE_SEED_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_REL_TYPES_ENSURE_SEED};
   assert(aimee_db2_rel_types_ensure_seed_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(rel_types_seed_calls == 1);
   assert(aimee_db2_rel_types_ensure_seed_reply_decode(response, response_len) == 0);

   /* Four families now share operation number 1, so these bytes decode on any
    * of their first stages. Sent elsewhere they are refused, but not always
    * for the reason one would guess: the response buffer sized for an
    * acknowledgement is too small for a counted reply, and the capacity check
    * runs before the capability check. So the index and learning stages answer
    * invalid_request rather than capability_absent. Pinned as it behaves. */
   aimee_module_invocation_t as_index = {.stage_id = AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS};
   assert(invoke(&backend, &as_index, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   aimee_module_invocation_t as_learning = {.stage_id = AIMEE_DB2_STAGE_RULES_DECAY};
   assert(invoke(&backend, &as_learning, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);

   /* Given a buffer large enough for a counted reply, the same bytes on the
    * index stage get as far as the capability check and stop there. */
   uint8_t wide[AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_RESPONSE_LEN];
   assert(invoke(&backend, &as_index, request, sizeof(request), wide, sizeof(wide),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(rel_types_seed_calls == 1);

   /* Reseeding is the ordinary case: every relation type already exists, every
    * insert conflicts and does nothing, and the answer is unchanged. */
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(rel_types_seed_calls == 2);

   /* The backend reports failure if any single seed statement fails, and there
    * is no count to turn that into a partial success. */
   rel_types_seed_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   rel_types_seed_value = 0;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_trace_mining_last_id_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.trace_mining_last_id = trace_mining_last_id};
   uint8_t request[AIMEE_DB2_TRACE_MINING_LAST_ID_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_TRACE_MINING_LAST_ID_RESPONSE_LEN];
   uint32_t response_len = 99;
   uint64_t watermark = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_TRACE_MINING_LAST_ID};
   assert(aimee_db2_trace_mining_last_id_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(trace_watermark_calls == 1);
   assert(aimee_db2_trace_mining_last_id_reply_decode(response, response_len, &watermark) == 0 &&
          watermark == 22);

   /* Zero is a corpus never mined and also a read that failed. Unlike the
    * other collapses on this bus the two are not equally cheap: a zero
    * watermark restarts the next mining pass from the beginning, so a failed
    * read buys a rescan of everything instead of losing anything. */
   trace_watermark_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_trace_mining_last_id_reply_decode(response, response_len, &watermark) == 0 &&
          watermark == 0);
   trace_watermark_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   trace_watermark_value = 22;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_proposals_archive_expired_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.proposals_archive_expired =
                                                   proposals_archive_expired};
   uint8_t request[AIMEE_DB2_PROPOSALS_ARCHIVE_EXPIRED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PROPOSALS_ARCHIVE_EXPIRED_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_PROPOSALS_ARCHIVE_EXPIRED};
   assert(aimee_db2_proposals_archive_expired_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(proposals_archive_calls == 1);
   assert(aimee_db2_proposals_archive_expired_reply_decode(response, response_len) == 0);

   /* There is no failure case to pin here, and that absence is the finding.
    * The backend returns void: a broken statement is logged and the caller is
    * told nothing, so the boundary answers ok whatever happened. Every other
    * operation on this bus can distinguish a failed sweep from an empty one;
    * this one cannot, and the catalog says so with reports_failure false. */
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(proposals_archive_calls == 2);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_mining_seed_job_defaults_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.mining_seed_job_defaults =
                                                   mining_seed_job_defaults};
   uint8_t request[AIMEE_DB2_MINING_SEED_JOB_DEFAULTS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_MINING_SEED_JOB_DEFAULTS_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_MINING_SEED_JOB_DEFAULTS};
   assert(aimee_db2_mining_seed_job_defaults_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(mining_seed_calls == 1);
   assert(aimee_db2_mining_seed_job_defaults_reply_decode(response, response_len) == 0);

   /* Replaying the seed pass is the ordinary case, not the exceptional one:
    * the jobs already exist, every insert conflicts and does nothing, and the
    * answer is the same acknowledgement. An operator's tuned interval survives
    * that, which is the whole reason the conflict rule is policy. */
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_mining_seed_job_defaults_reply_decode(response, response_len) == 0);
   assert(mining_seed_calls == 2);

   /* Any non-zero return is a failed seed pass. There is no count to soften it
    * into a partial result, which is the point of the acknowledgement shape. */
   mining_seed_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   mining_seed_value = 0;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_curiosity_rescore_all_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.curiosity_rescore_all = curiosity_rescore_all};
   uint8_t request[AIMEE_DB2_CURIOSITY_RESCORE_ALL_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_CURIOSITY_RESCORE_ALL_RESPONSE_LEN];
   uint32_t response_len = 99, rescored = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_CURIOSITY_RESCORE_ALL};
   assert(aimee_db2_curiosity_rescore_all_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(curiosity_rescore_calls == 1);
   assert(aimee_db2_curiosity_rescore_all_reply_decode(response, response_len, &rescored) == 0 &&
          rescored == 19);

   /* No open curiosity item is zero, and so is a failed statement. Rescoring
    * an unchanged corpus is also not a no-op in the count: every open item is
    * rewritten with the same numbers, so the count stays at the item total
    * rather than falling to zero on replay. */
   curiosity_rescore_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_curiosity_rescore_all_reply_decode(response, response_len, &rescored) == 0 &&
          rescored == 0);
   curiosity_rescore_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   curiosity_rescore_value = 19;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_rules_decay_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.rules_decay = rules_decay};
   uint8_t request[AIMEE_DB2_RULES_DECAY_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_RULES_DECAY_RESPONSE_LEN];
   uint32_t response_len = 99, touched = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_RULES_DECAY};
   assert(aimee_db2_rules_decay_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(rules_decay_calls == 1);
   assert(aimee_db2_rules_decay_reply_decode(response, response_len, &touched) == 0 &&
          touched == 18);

   /* Three families now share operation number 1 and produce identical request
    * bytes. Sent on the index or maintenance stage these same bytes reach
    * those backends, absent here, so the answer is capability-absent rather
    * than a decay pass. That is the whole of the separation. */
   aimee_module_invocation_t as_index = {.stage_id = AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS};
   assert(invoke(&backend, &as_index, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   aimee_module_invocation_t as_maintenance = {.stage_id =
                                                   AIMEE_DB2_STAGE_PROSPECTIVE_SWEEP_EXPIRED};
   assert(invoke(&backend, &as_maintenance, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(rules_decay_calls == 1);

   /* Nothing due for decay is zero, and so is a failed statement. The number
    * also sums three statements, so a decayed rule and an archived one cannot
    * be told apart in it. Both limitations pinned. */
   rules_decay_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_rules_decay_reply_decode(response, response_len, &touched) == 0 &&
          touched == 0);
   rules_decay_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   rules_decay_value = 18;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_drift_candidates_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.drift_candidates = drift_candidates};
   uint8_t request[AIMEE_DB2_DRIFT_CANDIDATES_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DRIFT_CANDIDATES_RESPONSE_LEN];
   uint32_t response_len = 99;
   uint64_t drift = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_DRIFT_CANDIDATES};
   assert(aimee_db2_drift_candidates_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(drift_candidates_calls == 1);
   assert(aimee_db2_drift_candidates_reply_decode(response, response_len, &drift) == 0 &&
          drift == 20);

   /* Nothing drifted and a failed statement are both zero, the same collapse
    * the requeue it previews carries. Reading this as a preview is fair only
    * because both share the predicate; the catalog pins that. */
   drift_candidates_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_drift_candidates_reply_decode(response, response_len, &drift) == 0 &&
          drift == 0);
   drift_candidates_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   drift_candidates_value = 20;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_cross_repo_rebuild_build_deps_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.cross_repo_rebuild_build_deps =
                                                   cross_repo_rebuild_build_deps};
   uint8_t request[AIMEE_DB2_CROSS_REPO_REBUILD_BUILD_DEPS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_CROSS_REPO_REBUILD_BUILD_DEPS_RESPONSE_LEN];
   uint32_t response_len = 99, deps = 99;
   aimee_module_invocation_t invocation = {.stage_id =
                                               AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_BUILD_DEPS};
   assert(aimee_db2_cross_repo_rebuild_build_deps_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(rebuild_build_deps_calls == 1);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_decode(response, response_len, &deps) ==
              0 &&
          deps == 17);

   /* A corpus whose manifests declare no cross-repo dependency is a genuinely
    * empty table, and zero says so. */
   rebuild_build_deps_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_cross_repo_rebuild_build_deps_reply_decode(response, response_len, &deps) ==
              0 &&
          deps == 0);

   /* -1 covers a rolled-back rebuild and, distinctively here, a mid-cursor
    * error on the project list. Reporting either as zero would claim no
    * repository depends on another, so the boundary keeps it a failure. */
   rebuild_build_deps_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   rebuild_build_deps_value = 17;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_cross_repo_rebuild_identities_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.cross_repo_rebuild_identities =
                                                   cross_repo_rebuild_identities};
   uint8_t request[AIMEE_DB2_CROSS_REPO_REBUILD_IDENTITIES_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_CROSS_REPO_REBUILD_IDENTITIES_RESPONSE_LEN];
   uint32_t response_len = 99, written = 99;
   aimee_module_invocation_t invocation = {.stage_id =
                                               AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_IDENTITIES};
   assert(aimee_db2_cross_repo_rebuild_identities_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(rebuild_identities_calls == 1);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_decode(response, response_len, &written) ==
              0 &&
          written == 16);

   /* No manifest in the index is a genuinely empty identity table. */
   rebuild_identities_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_cross_repo_rebuild_identities_reply_decode(response, response_len, &written) ==
              0 &&
          written == 0);

   /* A rolled-back rebuild is -1 and stays a failure, exactly as the route
    * rebuild does: reporting it as zero would claim no repository declares an
    * identity at all. */
   rebuild_identities_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   rebuild_identities_value = 16;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_cross_repo_rebuild_routes_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.cross_repo_rebuild_routes =
                                                   cross_repo_rebuild_routes};
   uint8_t request[AIMEE_DB2_CROSS_REPO_REBUILD_ROUTES_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_CROSS_REPO_REBUILD_ROUTES_RESPONSE_LEN];
   uint32_t response_len = 99, routes = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_CROSS_REPO_REBUILD_ROUTES};
   assert(aimee_db2_cross_repo_rebuild_routes_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(rebuild_routes_calls == 1);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_decode(response, response_len, &routes) == 0 &&
          routes == 15);

   /* An index with no cross-repo include is a genuinely empty route table, and
    * the rebuild says so. Zero is a size here, not a failure. */
   rebuild_routes_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_cross_repo_rebuild_routes_reply_decode(response, response_len, &routes) == 0 &&
          routes == 0);

   /* A rolled-back rebuild is -1 and stays a failure. Reporting it as zero
    * would claim every cross-repo include had stopped resolving, which is the
    * one wrong answer this operation must never give. */
   rebuild_routes_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   rebuild_routes_value = 15;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_requeue_drifted_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.requeue_drifted = requeue_drifted};
   uint8_t request[AIMEE_DB2_REQUEUE_DRIFTED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REQUEUE_DRIFTED_RESPONSE_LEN];
   uint32_t response_len = 99, requeued = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_REQUEUE_DRIFTED};
   assert(aimee_db2_requeue_drifted_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(requeue_drifted_calls == 1);
   assert(aimee_db2_requeue_drifted_reply_decode(response, response_len, &requeued) == 0 &&
          requeued == 6);

   /* Nothing drifted, and every drifted project already queued, are both zero.
    * So is a failed statement -- the backend collapses all three, and the wire
    * cannot separate what it is not told apart. Pinned so the limitation stays
    * visible rather than being rediscovered. */
   requeue_drifted_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_requeue_drifted_reply_decode(response, response_len, &requeued) == 0 &&
          requeued == 0);
   requeue_drifted_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   requeue_drifted_value = 6;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_purge_hidden_pollution_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.purge_hidden_pollution = purge_hidden_pollution};
   uint8_t request[AIMEE_DB2_PURGE_HIDDEN_POLLUTION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PURGE_HIDDEN_POLLUTION_RESPONSE_LEN];
   uint32_t response_len = 99, purged = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_PURGE_HIDDEN_POLLUTION};
   assert(aimee_db2_purge_hidden_pollution_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(purge_pollution_calls == 1);
   assert(aimee_db2_purge_hidden_pollution_reply_decode(response, response_len, &purged) == 0 &&
          purged == 5);

   /* An index with nothing inadmissible left in it. Zero is the sweep having
    * run and found nothing, and the replay depends on that: a second call
    * deletes nothing more, which is what safe idempotency claims. */
   purge_pollution_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_purge_hidden_pollution_reply_decode(response, response_len, &purged) == 0 &&
          purged == 0);

   /* No connection and no statement are both -1 here, and the boundary keeps
    * them as a failure rather than reporting a clean index. */
   purge_pollution_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   purge_pollution_value = 5;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_project_count_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.project_count = project_count};
   uint8_t request[AIMEE_DB2_PROJECT_COUNT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PROJECT_COUNT_RESPONSE_LEN];
   uint32_t response_len = 99, projects = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_PROJECT_COUNT};
   assert(aimee_db2_project_count_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(project_count_calls == 1);
   assert(aimee_db2_project_count_reply_decode(response, response_len, &projects) == 0 &&
          projects == 4);

   /* Zero is an index with no current projects, and also a failed statement:
    * the backend collapses them and the wire cannot separate what it is not
    * told apart. Pinned so the limitation stays visible. */
   project_count_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_project_count_reply_decode(response, response_len, &projects) == 0 &&
          projects == 0);
   project_count_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   project_count_value = 4;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_entity_edge_normalize_weights_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.entity_edge_normalize_weights =
                                                   entity_edge_normalize_weights};
   uint8_t request[AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_ENTITY_EDGE_NORMALIZE_WEIGHTS_RESPONSE_LEN];
   uint32_t response_len = 99, normalized = 99;
   aimee_module_invocation_t invocation = {.stage_id =
                                               AIMEE_DB2_STAGE_ENTITY_EDGE_NORMALIZE_WEIGHTS};
   assert(aimee_db2_entity_edge_normalize_weights_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(edge_normalize_calls == 1);
   assert(aimee_db2_entity_edge_normalize_weights_reply_decode(response, response_len,
                                                               &normalized) == 0 &&
          normalized == 3);

   /* Converged graph: zero rewrites is a success, not a failure. */
   edge_normalize_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_entity_edge_normalize_weights_reply_decode(response, response_len,
                                                               &normalized) == 0 &&
          normalized == 0);
   edge_normalize_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   edge_normalize_value = 3;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_entity_edge_prune_orphans_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.entity_edge_prune_orphans =
                                                   entity_edge_prune_orphans};
   uint8_t request[AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_ENTITY_EDGE_PRUNE_ORPHANS_RESPONSE_LEN];
   uint32_t response_len = 99, pruned = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_ENTITY_EDGE_PRUNE_ORPHANS};
   assert(aimee_db2_entity_edge_prune_orphans_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(edge_prune_calls == 1);
   assert(aimee_db2_entity_edge_prune_orphans_reply_decode(response, response_len, &pruned) == 0 &&
          pruned == 2);

   edge_prune_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   edge_prune_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   edge_prune_value = 2;

   /* THE FAMILY BOUNDARY IS THE STAGE, NOT THE ENVELOPE, AND THAT IS LOAD
    * BEARING. Operation numbers are unique only within a family, so this
    * request -- index operation 1, empty payload -- is BYTE-IDENTICAL to
    * lifecycle's health and to memory's level3_count. Nothing in the envelope
    * can tell them apart.
    *
    * Delivered on the memory stage it is therefore decoded as level3_count and
    * dispatched to that backend, not refused. The assertions below pin that
    * real behaviour rather than a rejection the wire cannot perform: here the
    * memory backend is absent, so it surfaces as CAPABILITY_ABSENT.
    *
    * What keeps this correct in production is that the transport routes by
    * event kind and stage, and the generated client always pairs the matching
    * event, stage and operation -- a caller cannot assemble a mismatched pair.
    * The hazard is that a future routing change, or a hand-built request, would
    * be silently mis-decoded rather than rejected. */
   aimee_module_invocation_t wrong_family = {.stage_id = AIMEE_DB2_FAMILY_MEMORY};
   assert(invoke(&backend, &wrong_family, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(AIMEE_DB2_OPERATION_ENTITY_EDGE_PRUNE_ORPHANS == AIMEE_DB2_OPERATION_LEVEL3_COUNT);
   assert(AIMEE_DB2_OPERATION_ENTITY_EDGE_PRUNE_ORPHANS == AIMEE_DB2_OPERATION_HEALTH);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_count_and_max_updated_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.count_and_max_updated = count_and_max_updated};
   uint8_t request[AIMEE_DB2_COUNT_AND_MAX_UPDATED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_COUNT_AND_MAX_UPDATED_RESPONSE_MAX_LEN];
   char stamp[AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX + 1];
   uint32_t response_len = 99, result = 99, count = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_COUNT_AND_MAX_UPDATED};
   assert(aimee_db2_count_and_max_updated_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(corpus_stat_calls == 1);
   assert(aimee_db2_count_and_max_updated_reply_decode(response, response_len, &result, &count,
                                                       stamp, sizeof(stamp)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && count == 7);

   /* Empty corpus: ok with zero and no stamp. */
   corpus_stat_count = 0;
   corpus_stat_stamp[0] = '\0';
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_count_and_max_updated_reply_decode(response, response_len, &result, &count,
                                                       stamp, sizeof(stamp)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && count == 0 && stamp[0] == '\0');

   /* Aggregate did not run: invalid_state, not a zero count that a caller
    * would read as an empty corpus. */
   corpus_stat_rc = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(response_len == AIMEE_DB2_COUNT_AND_MAX_UPDATED_ERROR_LEN);
   assert(aimee_db2_count_and_max_updated_reply_decode(response, response_len, &result, &count,
                                                       stamp, sizeof(stamp)) == 0);
   assert(result == AIMEE_DB2_RESULT_INVALID_STATE);
   corpus_stat_rc = 1;
   corpus_stat_count = 7;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response,
                 AIMEE_DB2_COUNT_AND_MAX_UPDATED_ERROR_LEN - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_pick_first_temporal_ref_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.pick_first_temporal_ref = pick_first_temporal_ref};
   uint8_t request[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_RESPONSE_MAX_LEN];
   char ref_key[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX + 1];
   uint32_t response_len = 99, result = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_PICK_FIRST_TEMPORAL_REF};
   assert(aimee_db2_pick_first_temporal_ref_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(temporal_ref_calls == 1);
   assert(aimee_db2_pick_first_temporal_ref_reply_decode(response, response_len, &result, ref_key,
                                                         sizeof(ref_key)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && strcmp(ref_key, "2026-08-19") == 0);

   temporal_ref_hit = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(response_len == AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_ERROR_LEN);
   assert(aimee_db2_pick_first_temporal_ref_reply_decode(response, response_len, &result, ref_key,
                                                         sizeof(ref_key)) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND);
   temporal_ref_hit = 1;

   /* A hit with an empty key is a broken backend contract, refused rather than
    * encoded as an empty ok. */
   temporal_ref_value[0] = '\0';
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   snprintf(temporal_ref_value, sizeof(temporal_ref_value), "%s", "2026-08-19");

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response,
                 AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_ERROR_LEN - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_get_source_session_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.get_source_session = get_source_session};
   uint8_t request[AIMEE_DB2_GET_SOURCE_SESSION_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_GET_SOURCE_SESSION_RESPONSE_MAX_LEN];
   char session_id[AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX + 1];
   uint32_t response_len = 99, result = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_GET_SOURCE_SESSION};
   assert(aimee_db2_get_source_session_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(get_source_session_calls == 1);
   assert(aimee_db2_get_source_session_reply_decode(response, response_len, &result, session_id,
                                                    sizeof(session_id)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && strcmp(session_id, "sess-1") == 0);

   get_source_session_rc = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(response_len == AIMEE_DB2_GET_SOURCE_SESSION_ERROR_LEN);
   assert(aimee_db2_get_source_session_reply_decode(response, response_len, &result, session_id,
                                                    sizeof(session_id)) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND);
   get_source_session_rc = 0;

   /* A backend that reports success while leaving the buffer empty has broken
    * its own contract; the handler refuses rather than encoding an empty ok
    * that a caller would read as a blank session. */
   get_source_session_value[0] = '\0';
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   snprintf(get_source_session_value, sizeof(get_source_session_value), "%s", "sess-1");

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response,
                 AIMEE_DB2_GET_SOURCE_SESSION_ERROR_LEN - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_get_content_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.get_content = get_content};
   uint8_t request[AIMEE_DB2_GET_CONTENT_REQUEST_LEN];
   static uint8_t response[AIMEE_DB2_GET_CONTENT_RESPONSE_MAX_LEN];
   static char content[AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1];
   uint32_t response_len = 99, result = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_GET_CONTENT};
   assert(aimee_db2_get_content_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(get_content_calls == 1);
   assert(aimee_db2_get_content_reply_decode(response, response_len, &result, content,
                                             sizeof(content)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && strcmp(content, "stored text") == 0);

   /* A stored empty string still reports ok. */
   get_content_value[0] = '\0';
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_get_content_reply_decode(response, response_len, &result, content,
                                             sizeof(content)) == 0);
   assert(result == AIMEE_DB2_RESULT_OK && content[0] == '\0');

   /* A missing memory reports not_found, and the handler must not turn that
    * into the empty-string answer above. */
   get_content_hit = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(response_len == AIMEE_DB2_GET_CONTENT_ERROR_LEN);
   assert(aimee_db2_get_content_reply_decode(response, response_len, &result, content,
                                             sizeof(content)) == 0);
   assert(result == AIMEE_DB2_RESULT_NOT_FOUND);
   get_content_hit = 1;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response,
                 AIMEE_DB2_GET_CONTENT_ERROR_LEN - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_negation_tokens_update_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.negation_tokens_update = negation_tokens_update};
   static uint8_t request[AIMEE_DB2_NEGATION_TOKENS_UPDATE_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_NEGATION_TOKENS_UPDATE_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_NEGATION_TOKENS_UPDATE};
   assert(aimee_db2_negation_tokens_update_request_encode(42u, "not never", request,
                                                          sizeof(request), &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(negation_tokens_calls == 1 && strcmp(negation_tokens_last, "not never") == 0);
   assert(aimee_db2_negation_tokens_update_reply_decode(response, response_len) == 0);

   /* The empty extraction must reach the backend rather than being dropped:
    * dropping it would leave stale negations on a memory that no longer has
    * any, and the caller would be told the write succeeded. */
   assert(aimee_db2_negation_tokens_update_request_encode(42u, "", request, sizeof(request),
                                                          &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(negation_tokens_calls == 2 && negation_tokens_last[0] == '\0');

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_set_source_session_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.set_source_session = set_source_session};
   uint8_t request[AIMEE_DB2_SET_SOURCE_SESSION_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_SET_SOURCE_SESSION_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_SET_SOURCE_SESSION};
   assert(aimee_db2_set_source_session_request_encode(42u, "sess-1", request, sizeof(request),
                                                      &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(source_session_calls == 1 && strcmp(source_session_last, "sess-1") == 0);
   assert(aimee_db2_set_source_session_reply_decode(response, response_len) == 0);

   /* The clear must reach the backend as an empty string, not be dropped by
    * the handler: dropping it would leave the old session in place while the
    * caller was told the write succeeded. */
   assert(aimee_db2_set_source_session_request_encode(42u, "", request, sizeof(request),
                                                      &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(source_session_calls == 2 && source_session_last[0] == '\0');

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_set_cognified_kind_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.set_cognified_kind = set_cognified_kind};
   uint8_t request[AIMEE_DB2_SET_COGNIFIED_KIND_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_SET_COGNIFIED_KIND_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_SET_COGNIFIED_KIND};
   assert(aimee_db2_set_cognified_kind_request_encode(42u, "preference", request, sizeof(request),
                                                      &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(cognified_kind_calls == 1 && strcmp(cognified_kind_last, "preference") == 0);
   assert(aimee_db2_set_cognified_kind_reply_decode(response, response_len) == 0);

   /* Void backend: no failure path exists, which is pinned rather than left
    * looking unfinished. */
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(cognified_kind_calls == 2);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_workspace_tag_insert_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.workspace_tag_insert = workspace_tag_insert};
   static uint8_t request[AIMEE_DB2_WORKSPACE_TAG_INSERT_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_WORKSPACE_TAG_INSERT_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_WORKSPACE_TAG_INSERT};
   assert(aimee_db2_workspace_tag_insert_request_encode(42u, "aimee", request, sizeof(request),
                                                        &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(workspace_tag_insert_calls == 1 && strcmp(workspace_tag_insert_last, "aimee") == 0);
   assert(aimee_db2_workspace_tag_insert_reply_decode(response, response_len) == 0);

   /* The statement is ON CONFLICT DO NOTHING and the backend returns void, so
    * a repeat acknowledges exactly like the first call. There is no failure
    * path here, which is pinned rather than left looking unfinished. */
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(workspace_tag_insert_calls == 2);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_decay_confidence_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.decay_confidence = decay_confidence};
   uint8_t request[AIMEE_DB2_DECAY_CONFIDENCE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DECAY_CONFIDENCE_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_DECAY_CONFIDENCE};
   assert(aimee_db2_decay_confidence_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(decay_confidence_calls == 1 && decay_confidence_last == 42);
   assert(aimee_db2_decay_confidence_reply_decode(response, response_len) == 0);

   /* There is no failure path to exercise: the backend returns void, so a
    * memory that does not exist and a statement that did not run both arrive
    * here as an acknowledgement. That is the honest limit of this operation
    * and it is pinned so it is not mistaken for a working fault path. */
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(decay_confidence_calls == 2);

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_update_content_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.update_content = update_content};
   static uint8_t request[AIMEE_DB2_UPDATE_CONTENT_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_UPDATE_CONTENT_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99, updated = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_UPDATE_CONTENT};
   assert(aimee_db2_update_content_request_encode(42u, "revised text", request, sizeof(request),
                                                  &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(update_content_calls == 1 && strcmp(update_content_last, "revised text") == 0);
   assert(aimee_db2_update_content_reply_decode(response, response_len, &updated) == 0 &&
          updated == 1);

   /* Rewriting a memory that is not there is a success reporting zero; the
    * backend reports a fault the same way, so zero carries both. */
   update_content_value = 0;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_update_content_reply_decode(response, response_len, &updated) == 0 &&
          updated == 0);

   update_content_value = 2;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   update_content_value = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   update_content_value = 1;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_reject_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.reject = reject};
   uint8_t request[AIMEE_DB2_REJECT_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_REJECT_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_REJECT};
   assert(aimee_db2_reject_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(reject_calls == 1 && reject_last == 42);
   assert(aimee_db2_reject_reply_decode(response, response_len) == 0);

   /* A memory that is not there cannot be penalised, and the backend reports
    * that the same way it reports a statement failure. */
   reject_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   reject_value = 0;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_has_scope_type_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.has_scope_type = has_scope_type};
   static uint8_t request[AIMEE_DB2_HAS_SCOPE_TYPE_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_HAS_SCOPE_TYPE_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99, present = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_HAS_SCOPE_TYPE};
   assert(aimee_db2_has_scope_type_request_encode(42u, "workspace", request, sizeof(request),
                                                  &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(scope_type_calls == 1 && strcmp(scope_type_last, "workspace") == 0);
   assert(aimee_db2_has_scope_type_reply_decode(response, response_len, &present) == 0 &&
          present == 1);

   /* Unlike valid_at, which shares this wire format, this backend folds a
    * fault into a miss, so zero is all a caller ever learns from a failure. */
   scope_type_value = 0;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_has_scope_type_reply_decode(response, response_len, &present) == 0 &&
          present == 0);

   scope_type_value = 2;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   scope_type_value = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   scope_type_value = 1;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_valid_at_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.valid_at = valid_at};
   static uint8_t request[AIMEE_DB2_VALID_AT_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_VALID_AT_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99, result = 99, in_force = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_VALID_AT};
   assert(aimee_db2_valid_at_request_encode(42u, "2026-08-18 12:00:00", request, sizeof(request),
                                            &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(valid_at_calls == 1 && strcmp(valid_at_last, "2026-08-18 12:00:00") == 0);
   assert(aimee_db2_valid_at_reply_decode(response, response_len, &result, &in_force) == 0 &&
          result == AIMEE_DB2_RESULT_OK && in_force == 1);

   valid_at_value = 0;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_valid_at_reply_decode(response, response_len, &result, &in_force) == 0 &&
          result == AIMEE_DB2_RESULT_OK && in_force == 0);

   /* THE POINT OF THIS OPERATION. A backend that could not evaluate the bounds
    * must not arrive as "not in force": the two are different claims and the
    * backend deliberately distinguishes them. */
   valid_at_value = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_valid_at_reply_decode(response, response_len, &result, &in_force) == 0 &&
          result == AIMEE_DB2_RESULT_INVALID_STATE && in_force == 0);
   valid_at_value = 1;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_link_delete_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.link_delete = link_delete};
   uint8_t request[AIMEE_DB2_LINK_DELETE_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_LINK_DELETE_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_LINK_DELETE};
   assert(aimee_db2_link_delete_request_encode(7u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(link_delete_calls == 1 && link_delete_last == 7);
   assert(aimee_db2_link_delete_reply_decode(response, response_len) == 0);

   link_delete_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   link_delete_value = 0;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_touch_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.touch = touch};
   uint8_t request[AIMEE_DB2_TOUCH_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_TOUCH_RESPONSE_LEN];
   uint32_t response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_TOUCH};
   assert(aimee_db2_touch_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(touch_calls == 1 && touch_last == 42);
   assert(aimee_db2_touch_reply_decode(response, response_len) == 0);

   /* The backend collapses a missing row and a statement failure into the same
    * non-zero, so both arrive as INTERNAL. There is deliberately no count in
    * the reply that a caller could mistake for proof the memory existed. */
   touch_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   touch_value = 0;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_delete_row_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.delete_row = delete_row};
   uint8_t request[AIMEE_DB2_DELETE_ROW_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DELETE_ROW_RESPONSE_LEN];
   uint32_t response_len = 99, removed = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_DELETE_ROW};
   assert(aimee_db2_delete_row_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(delete_row_calls == 1 && delete_row_last == 42);
   assert(aimee_db2_delete_row_reply_decode(response, response_len, &removed) == 0 && removed == 1);

   /* Deleting a memory that is not there is a success reporting zero. The
    * backend cannot distinguish that from a connection failure, which it also
    * reports as zero -- the same limitation the sweep operations carry, kept
    * rather than changed under a bus migration. */
   delete_row_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_delete_row_reply_decode(response, response_len, &removed) == 0 && removed == 0);

   delete_row_value = 2;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   delete_row_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   delete_row_value = 1;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_has_workspace_tag_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.has_workspace_tag = has_workspace_tag};
   uint8_t request[AIMEE_DB2_HAS_WORKSPACE_TAG_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_HAS_WORKSPACE_TAG_RESPONSE_LEN];
   uint32_t response_len = 99, tagged = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_HAS_WORKSPACE_TAG};
   assert(aimee_db2_has_workspace_tag_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(workspace_tag_calls == 1 && workspace_tag_last == 42);
   assert(aimee_db2_has_workspace_tag_reply_decode(response, response_len, &tagged) == 0 &&
          tagged == 1);

   /* An untagged memory is a miss, not an error: the row may simply carry no
    * workspace attribution. */
   workspace_tag_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_has_workspace_tag_reply_decode(response, response_len, &tagged) == 0 &&
          tagged == 0);

   /* Anything wider than Boolean means the LIMIT 1 probe changed shape. */
   workspace_tag_value = 2;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   workspace_tag_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   workspace_tag_value = 1;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_demote_id_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.demote_id = demote_id};
   uint8_t request[AIMEE_DB2_DEMOTE_ID_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_DEMOTE_ID_RESPONSE_LEN];
   uint32_t response_len = 99, demoted = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_DEMOTE_ID};
   assert(aimee_db2_demote_id_request_encode(42u, request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(demote_id_calls == 1 && demote_id_last == 42);
   assert(aimee_db2_demote_id_reply_decode(response, response_len, &demoted) == 0 && demoted == 1);

   /* A row already at or below the floor matches nothing and decays no
    * further. That is a success reporting zero, not a missing row. */
   demote_id_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_demote_id_reply_decode(response, response_len, &demoted) == 0 && demoted == 0);

   /* More than one row for a primary-key equality means the statement drifted
    * from the reviewed operation, so it must not be encodable as a reply. */
   demote_id_value = 2;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);

   demote_id_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   demote_id_value = 1;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_lifecycle_sweep_expired_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.lifecycle_sweep_expired = lifecycle_sweep_expired};
   uint8_t request[AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_REQUEST_LEN];
   uint8_t response[AIMEE_DB2_LIFECYCLE_SWEEP_EXPIRED_RESPONSE_LEN];
   uint32_t response_len = 99, archived = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_LIFECYCLE_SWEEP_EXPIRED};
   assert(aimee_db2_lifecycle_sweep_expired_request_encode(request, sizeof(request)) == 0);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(lifecycle_sweep_calls == 1);
   assert(aimee_db2_lifecycle_sweep_expired_reply_decode(response, response_len, &archived) == 0 &&
          archived == 4);

   /* The backend reports both an empty sweep and a database fault as zero, so
    * zero must stay a success here. Pinning it keeps that known limitation
    * visible instead of letting a later reader assume a fault would surface. */
   lifecycle_sweep_value = 0;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_db2_lifecycle_sweep_expired_reply_decode(response, response_len, &archived) == 0 &&
          archived == 0);

   /* Refused in case the backend contract is later tightened to signal faults. */
   lifecycle_sweep_value = -1;
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   lifecycle_sweep_value = 4;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, sizeof(request), response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, sizeof(request), response, sizeof(response) - 1,
                 &response_len) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_record_l4_approval_handler(void)
{
   reset();
   const aimee_db2_module_backend_t backend = {.record_l4_approval = record_l4_approval};
   static uint8_t request[AIMEE_DB2_RECORD_L4_APPROVAL_REQUEST_MAX_LEN];
   uint8_t response[AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN];
   uint32_t request_len = 0, response_len = 99;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DB2_STAGE_RECORD_L4_APPROVAL};
   assert(aimee_db2_record_l4_approval_request_encode(42u, "operator", "reviewed", request,
                                                      sizeof(request), &request_len) == 0);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_OK);
   assert(approval_calls == 1 && approval_last_id == 42 &&
          strcmp(approval_last_approver, "operator") == 0 &&
          strcmp(approval_last_note, "reviewed") == 0);
   assert(aimee_db2_record_l4_approval_reply_decode(response, response_len) == 0);

   approval_result = -1;
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_INTERNAL);
   approval_result = 0;

   const aimee_db2_module_backend_t absent = {0};
   assert(invoke(&absent, &invocation, request, request_len, response, sizeof(response),
                 &response_len) == AIMEE_MODULE_STATUS_CAPABILITY_ABSENT);
   assert(invoke(&backend, &invocation, request, request_len, response, sizeof(response) - 1,
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

static void test_health_retention_typed_client(void)
{
   reset();
   transport_expect_health_retention = 1;
   uint32_t snapshots = 99, contradictions = 99;
   assert(aimee_db2_health_retention_call(NULL, NULL, 77, 88, &snapshots, &contradictions, NULL,
                                          NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(snapshots == 0 && contradictions == 0);
   assert(aimee_db2_health_retention_reply_encode(11u, 3u, transport_response,
                                                  sizeof(transport_response),
                                                  &transport_response_len) == 0);
   assert(aimee_db2_health_retention_call(transport, (void *)0x1234, 77, 88, &snapshots,
                                          &contradictions, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(snapshots == 11 && contradictions == 3 && transport_calls == 1);
}

static void test_health_counters_typed_client(void)
{
   reset();
   transport_expect_health_counters = 1;
   const aimee_db2_health_counters_t counters = {
       .cycles = 7,
       .total_contradictions = 13,
       .total_promotions = 5,
       .total_demotions = 2,
       .total_expirations = 4,
       .new_memories = 21,
       .l1_eligible = 9,
       .l2_total = 30,
       .l2_stale_30_days = 6,
   };
   aimee_db2_health_counters_t received = {.cycles = 99};
   assert(aimee_db2_health_counters_call(NULL, NULL, 77, 88, &received, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(received.cycles == 0);
   assert(aimee_db2_health_counters_reply_encode(&counters, transport_response,
                                                 sizeof(transport_response),
                                                 &transport_response_len) == 0);
   assert(aimee_db2_health_counters_call(transport, (void *)0x1234, 77, 88, &received, NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
   assert(memcmp(&received, &counters, sizeof(received)) == 0 && transport_calls == 1);
}

static void test_stats_counts_typed_client(void)
{
   reset();
   transport_expect_stats_counts = 1;
   const aimee_db2_memory_stats_t stats = {
       .tier_counts = {3, 12, 30, 8, 2, 1},
       .kind_counts = {14, 5, 6, 9, 4, 3, 2, 1, 7, 5},
       .total = 56,
       .conflicts = 4,
   };
   aimee_db2_memory_stats_t received = {.total = 99};
   assert(aimee_db2_stats_counts_call(NULL, NULL, 77, 88, &received, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(received.total == 0);
   assert(aimee_db2_stats_counts_reply_encode(&stats, transport_response,
                                              sizeof(transport_response),
                                              &transport_response_len) == 0);
   assert(aimee_db2_stats_counts_call(transport, (void *)0x1234, 77, 88, &received, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(memcmp(&received, &stats, sizeof(received)) == 0 && transport_calls == 1);
}

static void test_expire_typed_client(void)
{
   reset();
   transport_expect_expire = 1;
   uint32_t level0 = 99, stale = 99;
   assert(aimee_db2_expire_call(NULL, NULL, 77, 88, &level0, &stale, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(level0 == 0 && stale == 0);
   assert(aimee_db2_expire_reply_encode(9u, 17u, transport_response, sizeof(transport_response),
                                        &transport_response_len) == 0);
   assert(aimee_db2_expire_call(transport, (void *)0x1234, 77, 88, &level0, &stale, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(level0 == 9 && stale == 17 && transport_calls == 1);
}

static void test_demote_typed_client(void)
{
   reset();
   transport_expect_demote = 1;
   uint32_t demoted = 99, cascaded = 99;
   assert(aimee_db2_demote_call(NULL, NULL, 77, 88, &demoted, &cascaded, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(demoted == 0 && cascaded == 0);
   assert(aimee_db2_demote_reply_encode(6u, 2u, transport_response, sizeof(transport_response),
                                        &transport_response_len) == 0);
   assert(aimee_db2_demote_call(transport, (void *)0x1234, 77, 88, &demoted, &cascaded, NULL,
                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(demoted == 6 && cascaded == 2 && transport_calls == 1);
}

static void test_promote_stable_typed_client(void)
{
   reset();
   transport_expect_promote_stable = 1;
   uint32_t promoted = 99;
   assert(aimee_db2_promote_stable_call(NULL, NULL, 77, 88, &promoted, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(promoted == 0);
   assert(aimee_db2_promote_stable_reply_encode(4u, transport_response, sizeof(transport_response),
                                                &transport_response_len) == 0);
   assert(aimee_db2_promote_stable_call(transport, (void *)0x1234, 77, 88, &promoted, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(promoted == 4 && transport_calls == 1);
}

static void test_reclassify_directives_typed_client(void)
{
   reset();
   transport_expect_reclassify = 1;
   uint32_t reclassified = 99;
   assert(aimee_db2_reclassify_directives_call(NULL, NULL, 77, 88, 1u, &reclassified, NULL, NULL) ==
          AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(reclassified == 0);
   /* A gate outside its range never reaches the transport. */
   assert(aimee_db2_reclassify_directives_call(
              transport, (void *)0x1234, 77, 88, AIMEE_DB2_RECLASSIFY_DIRECTIVES_GATE_MAX + 1u,
              &reclassified, NULL, NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(transport_calls == 0);
   assert(aimee_db2_reclassify_directives_reply_encode(
              3u, transport_response, sizeof(transport_response), &transport_response_len) == 0);
   assert(aimee_db2_reclassify_directives_call(transport, (void *)0x1234, 77, 88, 1u, &reclassified,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reclassified == 3 && transport_calls == 1);
}

static void test_record_l4_approval_typed_client(void)
{
   reset();
   transport_expect_approval = 1;
   assert(aimee_db2_record_l4_approval_call(NULL, NULL, 77, 88, 42u, "operator", "reviewed", NULL,
                                            NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   /* An out-of-range field never reaches the transport. */
   assert(aimee_db2_record_l4_approval_call(transport, (void *)0x1234, 77, 88, 0u, "operator",
                                            "reviewed", NULL,
                                            NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(aimee_db2_record_l4_approval_call(transport, (void *)0x1234, 77, 88, 42u, "", "reviewed",
                                            NULL, NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(transport_calls == 0);
   assert(aimee_db2_record_l4_approval_reply_encode(transport_response,
                                                    sizeof(transport_response)) == 0);
   transport_response_len = AIMEE_DB2_RECORD_L4_APPROVAL_RESPONSE_LEN;
   assert(aimee_db2_record_l4_approval_call(transport, (void *)0x1234, 77, 88, 42u, "operator",
                                            "reviewed", NULL, NULL) == AIMEE_MODULE_CALL_OK);
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
   test_health_retention_wire();
   test_health_counters_wire();
   test_stats_counts_wire();
   test_expire_wire();
   test_demote_wire();
   test_promote_stable_wire();
   test_reclassify_directives_wire();
   test_record_l4_approval_wire();
   test_prune_orphaned_l0_wire();
   test_lifecycle_sweep_expired_wire();
   test_demote_id_wire();
   test_has_workspace_tag_wire();
   test_delete_row_wire();
   test_touch_wire();
   test_link_delete_wire();
   test_valid_at_wire();
   test_has_scope_type_wire();
   test_reject_wire();
   test_update_content_wire();
   test_decay_confidence_wire();
   test_workspace_tag_insert_wire();
   test_set_cognified_kind_wire();
   test_set_source_session_wire();
   test_negation_tokens_update_wire();
   test_get_content_wire();
   test_get_source_session_wire();
   test_pick_first_temporal_ref_wire();
   test_count_and_max_updated_wire();
   test_entity_edge_prune_orphans_wire();
   test_entity_edge_normalize_weights_wire();
   test_project_count_wire();
   test_purge_hidden_pollution_wire();
   test_requeue_drifted_wire();
   test_cross_repo_rebuild_routes_wire();
   test_cross_repo_rebuild_identities_wire();
   test_cross_repo_rebuild_build_deps_wire();
   test_drift_candidates_wire();
   test_rules_decay_wire();
   test_curiosity_rescore_all_wire();
   test_mining_seed_job_defaults_wire();
   test_proposals_archive_expired_wire();
   test_trace_mining_last_id_wire();
   test_rel_types_ensure_seed_wire();
   test_vector_rebuild_lock_wire();
   test_release_get_active_wire();
   test_prospective_sweep_expired_wire();
   test_directive_sweep_expired_wire();
   test_directive_id_operations_wire();
   test_by_id_operations_wire();
   test_project_clear_operations_wire();
   test_mark_revisit_due_wire();
   test_ingest_queue_reset_running_wire();
   test_evidence_reembed_all_wire();
   test_curator_reembed_all_wire();
   test_synth_reenqueue_all_wire();
   test_curator_reenqueue_extract_all_wire();
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
   test_health_retention_handler();
   test_health_counters_handler();
   test_stats_counts_handler();
   test_expire_handler();
   test_demote_handler();
   test_promote_stable_handler();
   test_reclassify_directives_handler();
   test_record_l4_approval_handler();
   test_prune_orphaned_l0_handler();
   test_lifecycle_sweep_expired_handler();
   test_demote_id_handler();
   test_has_workspace_tag_handler();
   test_delete_row_handler();
   test_touch_handler();
   test_link_delete_handler();
   test_valid_at_handler();
   test_has_scope_type_handler();
   test_reject_handler();
   test_update_content_handler();
   test_decay_confidence_handler();
   test_workspace_tag_insert_handler();
   test_set_cognified_kind_handler();
   test_set_source_session_handler();
   test_negation_tokens_update_handler();
   test_get_content_handler();
   test_get_source_session_handler();
   test_pick_first_temporal_ref_handler();
   test_count_and_max_updated_handler();
   test_entity_edge_prune_orphans_handler();
   test_entity_edge_normalize_weights_handler();
   test_project_count_handler();
   test_purge_hidden_pollution_handler();
   test_requeue_drifted_handler();
   test_cross_repo_rebuild_routes_handler();
   test_cross_repo_rebuild_identities_handler();
   test_cross_repo_rebuild_build_deps_handler();
   test_drift_candidates_handler();
   test_rules_decay_handler();
   test_curiosity_rescore_all_handler();
   test_mining_seed_job_defaults_handler();
   test_proposals_archive_expired_handler();
   test_trace_mining_last_id_handler();
   test_rel_types_ensure_seed_handler();
   test_vector_rebuild_lock_handler();
   test_release_get_active_handler();
   test_prospective_sweep_expired_handler();
   test_directive_sweep_expired_handler();
   test_directive_id_operations_handler();
   test_by_id_operations_handler();
   test_project_clear_operations_handler();
   test_mark_revisit_due_handler();
   test_ingest_queue_reset_running_handler();
   test_evidence_reembed_all_handler();
   test_curator_reembed_all_handler();
   test_synth_reenqueue_all_handler();
   test_curator_reenqueue_extract_all_handler();
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
   test_health_retention_typed_client();
   test_health_counters_typed_client();
   test_stats_counts_typed_client();
   test_expire_typed_client();
   test_demote_typed_client();
   test_promote_stable_typed_client();
   test_reclassify_directives_typed_client();
   test_record_l4_approval_typed_client();
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
