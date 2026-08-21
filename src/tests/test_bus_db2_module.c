#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/db2/client.h>
#include <aimee/db2/module_api.h>

#include "module_adapter.h"

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
static int initialized_calls;
static int embedding_dimension_calls;
static int level3_count_calls;
static int level2_count_calls;
static int orphaned_l0_count_calls;
static int prune_orphaned_l0_calls;
static int lifecycle_sweep_calls;
static int demote_id_calls;
static int64_t demote_id_last;
static int workspace_tag_calls;
static int delete_row_calls;
static int touch_calls;
static int64_t touch_last;
static int link_delete_calls;
static int64_t link_delete_last;
static int valid_at_calls;
static char valid_at_last[64];
static int scope_type_calls;
static char scope_type_last[64];
static int reject_calls;
static int64_t reject_last;
static int update_content_calls;
static char update_content_last[2048];
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
static int get_content_calls;
static int get_source_session_calls;
static int temporal_ref_calls;
static int corpus_stat_calls;
static int edge_prune_calls;
static int edge_normalize_calls;
static int project_count_calls;
static int purge_pollution_calls;
static int requeue_drifted_calls;
static int rebuild_routes_calls;
static int rebuild_identities_calls;
static int rebuild_build_deps_calls;
static int drift_candidates_calls;
static int rules_decay_calls;
static int curiosity_rescore_calls;
static int mining_seed_calls;
static int proposals_archive_calls;
static int trace_watermark_calls;
static int rel_types_seed_calls;
static int lock_acquire_calls;
static int lock_release_calls;
static int release_active_calls;
static int prospective_sweep_calls;
static int directive_sweep_calls;
static int64_t directive_suppress_id;
static int directive_suppress_calls;
static int64_t directive_surface_id;
static int directive_surface_calls;
static int64_t anti_pattern_bump_seen;
static int anti_pattern_bump_calls;
static int64_t anti_pattern_delete_seen;
static int anti_pattern_delete_calls;
static int64_t doc_delete_seen;
static int doc_delete_calls;
static int64_t task_delete_seen;
static int task_delete_calls;
static char file_index_delete_project_seen[128];
static int file_index_delete_project_calls;
static char clear_project_seen[128];
static int clear_project_calls;
static char clear_current_project_seen[128];
static int clear_current_project_calls;
static int mark_revisit_calls;
static int queue_reset_calls;
static int evidence_reembed_calls;
static int curator_reembed_calls;
static int synth_reenqueue_calls;
static int extract_reenqueue_calls;
static int total_count_calls;
static int session_l2_count_calls;
static int key_exists_calls;
static int find_id_by_key_kind_calls;
static int key_exists_in_tier_pair_calls;
static int clear_effectiveness_calls;
static int set_effectiveness_calls;
static int retention_delete_calls;
static int demote_effectiveness_calls;
static int effectiveness_stats_calls;
static int list_l2_memory_ids_calls;
static int top_l2_facts_calls;
static int list_session_scope_priority_calls;
static int term_probe_calls[10];
static int history_calls;
static int list_rows_calls;
static int aggregate_calls;
static char aggregate_entity_seen[64];
static char aggregate_keyword_seen[64];
static int corpus_calls;
static int probe_calls[2];
static int64_t probe_identifier_seen;
static int mining_calls;
static int string_read_calls[4];
static int cross_family_calls[4];
static int batch8_calls[8];
static int pair_calls[7];
static int pair_read_calls[5];
static int runtime_state_get_calls;
static char runtime_state_key_seen[64];
static char pair_first_seen[64];
static char pair_second_seen[64];
static char string_read_argument_seen[64];
static int64_t mining_watermark_seen;
static int corpus_limit_seen;
static char list_tier_seen[8];
static char list_kind_seen[24];
static int list_hide_seen;
static char history_key_seen[64];
static int walk_calls[2];
static int row_calls[2];
static int64_t row_identifier_seen;
static char walk_session_seen[64];
static int64_t walk_anchor_seen;
static int walk_limit_seen;
static char term_probe_term_seen[64];
static int term_probe_limit_seen;
static int term_probe_active_seen;
static int scoped_active_seen;
static int scoped_include_all_seen;
static int scoped_limit_seen;
static char scoped_workspace_seen[64];
static char scoped_project_seen[64];
static int health_record_calls;
static int health_record_total;
static int health_record_contradictions;
static int health_record_promotions;
static int prune_health_calls;
static int prune_contradictions_calls;
static int health_counters_calls;
static int stats_counts_calls;
static int expire_l0_provenance_calls;
static int expire_stale_provenance_calls;
static int demote_cascade_calls;
static char demote_kind_stamp[32];
static int promote_stable_calls;
static int reclassify_last_gate;
static int approval_calls;
static char approval_last_approver[64];
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

static int is_initialized(void)
{
   initialized_calls++;
   return 1;
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
   return 384;
}

static int embedding_dimension(void)
{
   embedding_dimension_calls++;
   return 384;
}

int db2_memory_count_l3(void)
{
   level3_count_calls++;
   return 42;
}

static int level3_count(void)
{
   level3_count_calls++;
   return 42;
}

int db2_memory_count_l2(void)
{
   level2_count_calls++;
   return 17;
}

static int level2_count(void)
{
   level2_count_calls++;
   return 17;
}

int db2_memory_count_orphaned_l0(void)
{
   orphaned_l0_count_calls++;
   return 5;
}

static int orphaned_l0_count(void)
{
   orphaned_l0_count_calls++;
   return 5;
}

int64_t db2_memory_count(void)
{
   total_count_calls++;
   return 1234567890123LL;
}

static int64_t total_count(void)
{
   total_count_calls++;
   return 1234567890123LL;
}

int db2_memory_count_l2_for_session(const char *source_session)
{
   session_l2_count_calls++;
   return strcmp(source_session, "session-123") == 0 ? 3 : 0;
}

static int session_l2_count(const char *source_session)
{
   session_l2_count_calls++;
   return strcmp(source_session, "session-123") == 0 ? 3 : 0;
}

int db2_memory_key_exists(const char *key)
{
   key_exists_calls++;
   return strcmp(key, "recovery:tool-a->tool-b") == 0 ? 1 : 0;
}

static int key_exists(const char *key)
{
   key_exists_calls++;
   return strcmp(key, "recovery:tool-a->tool-b") == 0 ? 1 : 0;
}

int64_t db2_memory_find_id_by_key_kind(const char *key, const char *kind)
{
   find_id_by_key_kind_calls++;
   return strcmp(key, "task:deploy-fix") == 0 && strcmp(kind, "task") == 0 ? 42 : 0;
}

static int64_t find_id_by_key_kind(const char *key, const char *kind)
{
   find_id_by_key_kind_calls++;
   return strcmp(key, "task:deploy-fix") == 0 && strcmp(kind, "task") == 0 ? 42 : 0;
}

int db2_memory_key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b)
{
   key_exists_in_tier_pair_calls++;
   return strcmp(key, "recovery:tool-a->tool-b") == 0 && strcmp(tier_a, "L3") == 0 &&
          strcmp(tier_b, "L4") == 0;
}

static int key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b)
{
   key_exists_in_tier_pair_calls++;
   return strcmp(key, "recovery:tool-a->tool-b") == 0 && strcmp(tier_a, "L3") == 0 &&
          strcmp(tier_b, "L4") == 0;
}

int db2_memory_health_clear_effectiveness(int64_t memory_id)
{
   clear_effectiveness_calls++;
   return memory_id == 42 ? 0 : -1;
}

int db2_memory_health_set_effectiveness(int64_t memory_id, double value)
{
   set_effectiveness_calls++;
   return memory_id == 42 && value == 0.75 ? 0 : -1;
}

static int clear_effectiveness(int64_t memory_id)
{
   clear_effectiveness_calls++;
   return memory_id == 42 ? 0 : -1;
}

static int set_effectiveness(int64_t memory_id, double value)
{
   set_effectiveness_calls++;
   return memory_id == 42 && value == 0.75 ? 0 : -1;
}

static int retention_delete_impl(const char *sensitivity, int days)
{
   retention_delete_calls++;
   if (strcmp(sensitivity, AIMEE_DB2_RETENTION_RESTRICTED) == 0)
      return days == (int)AIMEE_DB2_RETENTION_RESTRICTED_DAYS ? 2 : -1;
   if (strcmp(sensitivity, AIMEE_DB2_RETENTION_SENSITIVE) == 0)
      return days == (int)AIMEE_DB2_RETENTION_SENSITIVE_DAYS ? 3 : -1;
   return -1;
}

int db2_memory_health_delete_by_sensitivity(const char *sensitivity, int days)
{
   return retention_delete_impl(sensitivity, days);
}

static int retention_delete(const char *sensitivity, int days)
{
   return retention_delete_impl(sensitivity, days);
}

static int demote_effectiveness_impl(double threshold)
{
   demote_effectiveness_calls++;
   return threshold == AIMEE_DB2_EFFECTIVENESS_DEMOTE_THRESHOLD ? 2 : -1;
}

int db2_memory_health_demote_low_effectiveness(double threshold)
{
   return demote_effectiveness_impl(threshold);
}

static int demote_effectiveness(double threshold)
{
   return demote_effectiveness_impl(threshold);
}

static int effectiveness_stats_impl(double low_threshold, double *avg_effectiveness,
                                    int *low_effectiveness, int *high_impact)
{
   effectiveness_stats_calls++;
   if (low_threshold != AIMEE_DB2_EFFECTIVENESS_STATS_LOW_THRESHOLD)
      return -1;
   if (avg_effectiveness)
      *avg_effectiveness = 0.5;
   if (low_effectiveness)
      *low_effectiveness = 3;
   if (high_impact)
      *high_impact = 1;
   return 0;
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

static int term_probe_impl(int which, const char *term, int limit, int scope_active,
                           int include_all, const char *workspace, const char *project,
                           int64_t *out, int max)
{
   (void)include_all;
   (void)workspace;
   (void)project;
   term_probe_calls[which]++;
   term_probe_limit_seen = limit;
   term_probe_active_seen = scope_active;
   snprintf(term_probe_term_seen, sizeof(term_probe_term_seen), "%s", term ? term : "");
   int listed = 0;
   /* The identifier encodes which probe answered, so a request that reached the
    * wrong operation is visible in the reply rather than merely plausible. */
   for (; listed < 2 && listed < max; listed++)
      out[listed] = (int64_t)(which + 1) * 100 + listed;
   return listed;
}

static int find_facts_like(const char *term, int limit, int scope_active, int include_all,
                           const char *workspace, const char *project, int64_t *out, int max)
{
   return term_probe_impl(6, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int list_session_scope_priority_like(const char *term, int limit, int scope_active,
                                            int include_all, const char *workspace,
                                            const char *project, int64_t *out, int max)
{
   return term_probe_impl(7, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int negation_fts_search(const char *term, int limit, int scope_active, int include_all,
                               const char *workspace, const char *project, int64_t *out, int max)
{
   return term_probe_impl(8, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int row_impl(int which, int64_t identifier, aimee_db2_memory_row_t *row)
{
   row_calls[which]++;
   row_identifier_seen = identifier;
   /* Identifier 404 is the absent row: the getters cannot tell that apart from
    * a statement that did not run, and the test pins that they do not try. */
   if (identifier == 404)
      return -1;
   memset(row, 0, sizeof(*row));
   row->id = (uint64_t)(which + 1) * 10;
   row->confidence = 0.5;
   row->salience = 0.25;
   row->use_count = 3u;
   snprintf(row->tier, sizeof(row->tier), "%s", "L2");
   snprintf(row->kind, sizeof(row->kind), "%s", "fact");
   snprintf(row->key, sizeof(row->key), "%s", "row-key");
   snprintf(row->content, sizeof(row->content), "%s", "row content");
   return 0;
}

static int row_get(int64_t memory_id, aimee_db2_memory_row_t *row)
{
   return row_impl(0, memory_id, row);
}

static int row_get_by_unit_id(int64_t unit_id, aimee_db2_memory_row_t *row)
{
   return row_impl(1, unit_id, row);
}

static int walk_impl(int which, const char *session_id, int64_t anchor_id, int limit, int64_t *out,
                     int max)
{
   walk_calls[which]++;
   walk_anchor_seen = anchor_id;
   walk_limit_seen = limit;
   snprintf(walk_session_seen, sizeof(walk_session_seen), "%s", session_id ? session_id : "");
   int listed = 0;
   for (; listed < 2 && listed < max; listed++)
      out[listed] = (int64_t)(which + 1) * 1000 + listed;
   return listed;
}

static int session_neighbors_before(const char *session_id, int64_t anchor_id, int limit,
                                    int64_t *out, int max)
{
   return walk_impl(0, session_id, anchor_id, limit, out, max);
}

static int session_neighbors_after(const char *session_id, int64_t anchor_id, int limit,
                                   int64_t *out, int max)
{
   return walk_impl(1, session_id, anchor_id, limit, out, max);
}

static int search_facts_patterns_by_keyword(const char *term, int limit, int scope_active,
                                            int include_all, const char *workspace,
                                            const char *project, int64_t *out, int max)
{
   return term_probe_impl(9, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int aggregate(const char *entity_seed, const char *keyword, int limit, int *truncated_out,
                     int64_t *out, int max)
{
   aggregate_calls++;
   snprintf(aggregate_entity_seen, sizeof(aggregate_entity_seen), "%s",
            entity_seed ? entity_seed : "");
   snprintf(aggregate_keyword_seen, sizeof(aggregate_keyword_seen), "%s", keyword ? keyword : "");
   if (truncated_out)
      *truncated_out = 1;
   int listed = 0;
   for (; listed < 2 && listed < max && listed < limit; listed++)
      out[listed] = 7000 + listed;
   return listed;
}

/* Identifier 404 is the absent one, and 500 is the statement that could not
 * run: the probes report false for both, which is what they can honestly say. */
static int probe_impl(int which, int64_t identifier)
{
   probe_calls[which]++;
   probe_identifier_seen = identifier;
   if (identifier == 404)
      return 0;
   if (identifier == 500)
      return -1;
   return 1;
}

static int record_exists(int64_t record_id)
{
   return probe_impl(0, record_id);
}

static int document_exists(int64_t document_id)
{
   return probe_impl(1, document_id);
}

/* Each read answers a different number so the reply says which one ran, and
 * the third answers past the Boolean bound to show the handler clamps. */
static int string_read_impl(int which, const char *argument)
{
   string_read_calls[which]++;
   snprintf(string_read_argument_seen, sizeof(string_read_argument_seen), "%s",
            argument ? argument : "");
   return which == 2 ? 77 : which;
}

/* Each answers a different number so the reply says which one ran; the third
 * answers past its Boolean bound to show the handler clamps. */
static int cross_family_impl(int which, const char *argument)
{
   cross_family_calls[which]++;
   snprintf(string_read_argument_seen, sizeof(string_read_argument_seen), "%s",
            argument ? argument : "");
   return which == 2 ? 9 : which + 11;
}

/* An acknowledging backend reports success as zero and anything else as a
 * failure, so those answer zero; the counting ones answer a number that
 * identifies which of them ran. */
static const int BATCH8_ACKNOWLEDGES[8] = {1, 0, 0, 1, 1, 1, 1, 0};

static int batch8_impl(int which, const char *argument)
{
   batch8_calls[which]++;
   snprintf(string_read_argument_seen, sizeof(string_read_argument_seen), "%s",
            argument ? argument : "");
   return BATCH8_ACKNOWLEDGES[which] ? 0 : which;
}

static int pair_impl(int which, const char *first, const char *second)
{
   pair_calls[which]++;
   snprintf(pair_first_seen, sizeof(pair_first_seen), "%s", first ? first : "");
   snprintf(pair_second_seen, sizeof(pair_second_seen), "%s", second ? second : "");
   return 0;
}

/* The last two are bounded at a full count rather than a Boolean, so they
 * answer past one to show the handler clamps only where the contract says. */
static int pair_read_impl(int which, const char *first, const char *second)
{
   pair_read_calls[which]++;
   snprintf(pair_first_seen, sizeof(pair_first_seen), "%s", first ? first : "");
   snprintf(pair_second_seen, sizeof(pair_second_seen), "%s", second ? second : "");
   return which + 1;
}

static int runtime_state_get(const char *state_key, char *state_value, size_t capacity)
{
   runtime_state_get_calls++;
   snprintf(runtime_state_key_seen, sizeof(runtime_state_key_seen), "%s",
            state_key ? state_key : "");
   snprintf(state_value, capacity, "%s", "probe-value");
   return 0;
}

static int entity_profile_fresh(const char *entity_id, const char *window)
{
   return pair_read_impl(0, entity_id, window);
}

static int doc_exists_by_hash(const char *content_hash, const char *scope)
{
   return pair_read_impl(1, content_hash, scope);
}

static int pdf_quarantine_confirm(const char *project, const char *file_path)
{
   return pair_read_impl(2, project, file_path);
}

static int pdf_quarantine_reject(const char *project, const char *file_path)
{
   return pair_read_impl(3, project, file_path);
}

static int enrollment_active(const char *cert_issuer, const char *cert_serial_norm)
{
   return pair_read_impl(4, cert_issuer, cert_serial_norm);
}

static int artifact_set_state(const char *state, const char *artifact_id)
{
   return pair_impl(0, state, artifact_id);
}

static int artifact_register_exemplar(const char *artifact_id, const char *collection)
{
   return pair_impl(1, artifact_id, collection);
}

static int evidence_enqueue(const char *artifact_id, const char *collection)
{
   return pair_impl(2, artifact_id, collection);
}

static int evidence_mark_failed(const char *artifact_id, const char *last_error)
{
   return pair_impl(3, artifact_id, last_error);
}

static int synth_mark_failed(const char *artifact_id, const char *last_error)
{
   return pair_impl(4, artifact_id, last_error);
}

static int runtime_state_set(const char *state_key, const char *state_value)
{
   return pair_impl(5, state_key, state_value);
}

static int set_active_embedder_version(const char *version, const char *updated_at)
{
   return pair_impl(6, version, updated_at);
}

static int artifact_stamp_reflected(const char *artifact_id)
{
   return batch8_impl(0, artifact_id);
}

static int failed_query_bump(const char *query_norm)
{
   return batch8_impl(1, query_norm);
}

static int fence_active(const char *project)
{
   return batch8_impl(2, project);
}

static int runtime_state_touch(const char *state_key)
{
   return batch8_impl(3, state_key);
}

static int synth_enqueue(const char *artifact_id)
{
   return batch8_impl(4, artifact_id);
}

static int synth_mark_done(const char *artifact_id)
{
   return batch8_impl(5, artifact_id);
}

static int reembed_mark_finished(const char *finished_at)
{
   return batch8_impl(6, finished_at);
}

static int mining_job_try_lock(const char *job_id)
{
   return batch8_impl(7, job_id);
}

static int entity_observation_count(const char *entity_id)
{
   return cross_family_impl(0, entity_id);
}

static int fidelity_attribution_count(const char *turn_id)
{
   return cross_family_impl(1, turn_id);
}

static int blob_referenced(const char *blob_ref)
{
   return cross_family_impl(2, blob_ref);
}

static int async_pending_count(const char *kind)
{
   return cross_family_impl(3, kind);
}

static int anti_pattern_exists_exact(const char *pattern)
{
   return string_read_impl(0, pattern);
}

static int anti_pattern_exists_by_source_ref(const char *source_ref)
{
   return string_read_impl(1, source_ref);
}

static int artifact_citation_count(const char *artifact_id)
{
   return string_read_impl(2, artifact_id);
}

static int commits_in_last_7_days(const char *sink)
{
   return string_read_impl(3, sink);
}

static int trace_mining_record(int64_t last_trace_id)
{
   mining_calls++;
   mining_watermark_seen = last_trace_id;
   return 0;
}

static int load_eval_corpus(int limit, char *label_out, size_t label_len, int64_t *out, int max)
{
   corpus_calls++;
   corpus_limit_seen = limit;
   snprintf(label_out, label_len, "%s", "L2 facts");
   int listed = 0;
   for (; listed < 2 && listed < max && listed < limit; listed++)
      out[listed] = 8000 + listed;
   return listed;
}

static int list_rows(const char *tier, const char *kind, int hide_archived, int limit,
                     int scope_active, int include_all, const char *workspace, const char *project,
                     int64_t *out, int max)
{
   (void)scope_active;
   (void)include_all;
   (void)workspace;
   (void)project;
   list_rows_calls++;
   list_hide_seen = hide_archived;
   snprintf(list_tier_seen, sizeof(list_tier_seen), "%s", tier ? tier : "");
   snprintf(list_kind_seen, sizeof(list_kind_seen), "%s", kind ? kind : "");
   int listed = 0;
   for (; listed < 2 && listed < max && listed < limit; listed++)
      out[listed] = 6000 + listed;
   return listed;
}

static int fact_history(const char *normalized_key, int limit, int64_t *out, int max)
{
   history_calls++;
   snprintf(history_key_seen, sizeof(history_key_seen), "%s", normalized_key ? normalized_key : "");
   int listed = 0;
   for (; listed < 2 && listed < max && listed < limit; listed++)
      out[listed] = 5000 + listed;
   return listed;
}

static int collect_alias_matches(const char *term, int limit, int scope_active, int include_all,
                                 const char *workspace, const char *project, int64_t *out, int max)
{
   return term_probe_impl(0, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int collect_entity_matches(const char *term, int limit, int scope_active, int include_all,
                                  const char *workspace, const char *project, int64_t *out, int max)
{
   return term_probe_impl(1, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int collect_event_frame_matches(const char *term, int limit, int scope_active,
                                       int include_all, const char *workspace, const char *project,
                                       int64_t *out, int max)
{
   return term_probe_impl(2, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int collect_relation_token_matches(const char *term, int limit, int scope_active,
                                          int include_all, const char *workspace,
                                          const char *project, int64_t *out, int max)
{
   return term_probe_impl(3, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int collect_summary_matches(const char *term, int limit, int scope_active, int include_all,
                                   const char *workspace, const char *project, int64_t *out,
                                   int max)
{
   return term_probe_impl(4, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int collect_temporal_matches(const char *term, int limit, int scope_active, int include_all,
                                    const char *workspace, const char *project, int64_t *out,
                                    int max)
{
   return term_probe_impl(5, term, limit, scope_active, include_all, workspace, project, out, max);
}

static int scoped_ids_impl(int scope_active, int include_all, const char *workspace,
                           const char *project, int64_t *out, int max, int64_t first)
{
   scoped_active_seen = scope_active;
   scoped_include_all_seen = include_all;
   scoped_limit_seen = max;
   snprintf(scoped_workspace_seen, sizeof(scoped_workspace_seen), "%s", workspace ? workspace : "");
   snprintf(scoped_project_seen, sizeof(scoped_project_seen), "%s", project ? project : "");
   int listed = 0;
   for (; listed < 2 && listed < max; listed++)
      out[listed] = first + listed;
   return listed;
}

static int top_l2_facts(int scope_active, int include_all, const char *workspace,
                        const char *project, int64_t *out, int max)
{
   top_l2_facts_calls++;
   return scoped_ids_impl(scope_active, include_all, workspace, project, out, max, 31);
}

static int list_session_scope_priority(int scope_active, int include_all, const char *workspace,
                                       const char *project, int64_t *out, int max)
{
   list_session_scope_priority_calls++;
   return scoped_ids_impl(scope_active, include_all, workspace, project, out, max, 71);
}

static int list_l2_memory_ids_impl(int64_t *out, int max)
{
   list_l2_memory_ids_calls++;
   static const int64_t rows[] = {7, 19, 4242};
   int listed = 0;
   for (; listed < (int)(sizeof(rows) / sizeof(rows[0])) && listed < max; listed++)
      out[listed] = rows[listed];
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
   return 512;
}

static int count_memories(void)
{
   return 512;
}

int db2_memory_health_count_recent_conflicts(int days)
{
   return days == AIMEE_DB2_HEALTH_RECORD_CONFLICT_WINDOW_DAYS ? 6 : -1;
}

static int count_recent_conflicts(int days)
{
   return days == AIMEE_DB2_HEALTH_RECORD_CONFLICT_WINDOW_DAYS ? 6 : -1;
}

static void health_record_impl(int total_memories, int contradictions_detected, int promotions,
                               int demotions, int expirations)
{
   health_record_calls++;
   health_record_total = total_memories;
   health_record_contradictions = contradictions_detected;
   health_record_promotions = promotions;
   (void)demotions;
   (void)expirations;
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
   prune_health_calls++;
   return days == AIMEE_DB2_HEALTH_RETENTION_SNAPSHOT_DAYS ? 11 : -1;
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
   prune_contradictions_calls++;
   return days == AIMEE_DB2_HEALTH_RETENTION_CONTRADICTION_DAYS ? 3 : -1;
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

/* Returns rows so the generic row-list reply is exercised with something in
 * it. The replay can only ever see the empty case: seeding a memory there
 * would be visible to the fresh-schema counts it also asserts. */
static char match_error_keys_seen[256] = "";
static int match_error_keys_calls;

int db2_memory_promotion_match_error_keys(const char *error_lowered, int64_t *ids_out, int max)
{
   match_error_keys_calls++;
   snprintf(match_error_keys_seen, sizeof(match_error_keys_seen), "%s",
            error_lowered ? error_lowered : "");
   if (!ids_out || max < 3)
      return 0;
   ids_out[0] = 11;
   ids_out[1] = 22;
   ids_out[2] = 9007199254740993;
   return 3;
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

/* Returns rows so a row reply carrying a string field is exercised with
 * something in it. The replay only ever sees the empty list: seeding a graph
 * there would be visible to the fresh-schema counts it also asserts. */
static char entity_neighbors_seen[512] = "";
static int entity_neighbors_limit_seen = -1;
static int entity_neighbors_calls;

int db2_entity_edge_neighbors(const char *entity, db2_entity_neighbor_t *out, int max,
                              int limit_sql)
{
   entity_neighbors_calls++;
   entity_neighbors_limit_seen = limit_sql;
   snprintf(entity_neighbors_seen, sizeof(entity_neighbors_seen), "%s", entity ? entity : "");
   if (!out || max < 2)
      return 0;
   snprintf(out[0].node, sizeof(out[0].node), "%s", "alpha");
   out[0].weight = 7;
   /* An empty node and a zero weight are both inside the schema's bounds, and
    * a codec that treated either as absent would lose this row. */
   out[1].node[0] = '\0';
   out[1].weight = 0;
   return 2;
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

/* Returns a row so the float in a row reply is exercised with a value in it.
 * The replay only ever sees an empty result: indexing a file there would be
 * visible to the fresh-schema counts it also asserts. */
static int code_search_enrich_seen = -1;
static int code_search_calls;

int db2_code_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                               int max, int enrich)
{
   (void)query;
   (void)project;
   code_search_calls++;
   code_search_enrich_seen = enrich;
   if (!out || max < 1)
      return 0;
   snprintf(out[0].project, sizeof(out[0].project), "%s", "alpha-project");
   snprintf(out[0].file_path, sizeof(out[0].file_path), "%s", "src/alpha.c");
   snprintf(out[0].snippet, sizeof(out[0].snippet), "%s", ">>>alpha<<<");
   /* Not representable exactly in fewer than 53 bits of mantissa, so a rank
    * that ever went through a float would come back changed. */
   out[0].rank = 0.1234567890123456;
   snprintf(out[0].content_hash, sizeof(out[0].content_hash), "%s", "deadbeef");
   out[0].line = 42;
   return 1;
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
   return 9;
}

/* Both tier-cycle operations enumerate kinds through this one entry: expire
 * walks L1, demote walks L2. */
static int list_kinds_in_tier(const char *tier, char (*kinds)[16], int max)
{
   if (max < 2)
      return -1;
   if (strcmp(tier, AIMEE_DB2_EXPIRE_STALE_TIER) != 0 && strcmp(tier, AIMEE_DB2_DEMOTE_TIER) != 0)
      return -1;
   snprintf(kinds[0], sizeof(kinds[0]), "%s", "scratch");
   snprintf(kinds[1], sizeof(kinds[1]), "%s", "fact");
   return 2;
}

static int kind_expire_days(const char *kind)
{
   return strcmp(kind, "scratch") == 0 ? 7 : 30;
}

static int delete_stale_l1_provenance(const char *kind, const char *days_neg)
{
   (void)kind;
   (void)days_neg;
   expire_stale_provenance_calls++;
   return 0;
}

static int delete_stale_l1(const char *kind, const char *days_neg)
{
   if (strcmp(kind, "scratch") == 0)
      return strcmp(days_neg, "-7") == 0 ? 5 : -1;
   return strcmp(days_neg, "-30") == 0 ? 12 : -1;
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

static void bus_now_utc(char *buf, size_t len)
{
   snprintf(buf, len, "%s", "2026-08-18T09:00:00Z");
}

static int kind_demote_policy(const char *kind, double *confidence, int *days)
{
   *confidence = strcmp(kind, "scratch") == 0 ? 0.4 : 0.6;
   *days = strcmp(kind, "scratch") == 0 ? 3 : 14;
   return 0;
}

static int demote_kind(const char *ts, const char *kind, double confidence, const char *days_neg)
{
   snprintf(demote_kind_stamp, sizeof(demote_kind_stamp), "%s", ts);
   if (strcmp(kind, "scratch") == 0)
      return (confidence == 0.4 && strcmp(days_neg, "-3") == 0) ? 4 : -1;
   return (confidence == 0.6 && strcmp(days_neg, "-14") == 0) ? 2 : -1;
}

static int demote_cascade(const char *ts)
{
   demote_cascade_calls++;
   /* The cascade must see exactly the stamp the demotions carried. */
   return strcmp(ts, demote_kind_stamp) == 0 ? 3 : -1;
}

int db2_memory_promotion_promote_stable_l2_to_l3(const char *ts)
{
   (void)ts;
   return 0;
}

static int promote_stable(const char *ts)
{
   promote_stable_calls++;
   return ts && ts[0] ? 4 : -1;
}

int db2_memory_promotion_reclassify_directives(int require_approval)
{
   (void)require_approval;
   return 0;
}

static int reclassify_directives(int require_approval)
{
   reclassify_last_gate = require_approval;
   /* The gated path promotes fewer rows than the open one. */
   return require_approval ? 3 : 7;
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
   snprintf(approval_last_approver, sizeof(approval_last_approver), "%s", approver);
   return (memory_id == 42 && note && note[0]) ? 0 : -1;
}

int db2_memory_prune_orphaned_l0(void)
{
   prune_orphaned_l0_calls++;
   return 3;
}

static int prune_orphaned_l0(void)
{
   prune_orphaned_l0_calls++;
   return 3;
}

int db2_memory_lifecycle_sweep_expired(void)
{
   lifecycle_sweep_calls++;
   return 4;
}

static int lifecycle_sweep_expired(void)
{
   lifecycle_sweep_calls++;
   return 4;
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
   return 1;
}

int db2_memory_has_any_workspace_tag(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

static int has_workspace_tag(int64_t memory_id)
{
   workspace_tag_calls++;
   return memory_id == 42 ? 1 : 0;
}

int db2_memory_delete_row(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

static int delete_row(int64_t memory_id)
{
   delete_row_calls++;
   return memory_id == 42 ? 1 : 0;
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
   return memory_id == 42 ? 0 : -1;
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
   return link_id == 7 ? 0 : -1;
}

int db2_memory_valid_at(int64_t memory_id, const char *as_of)
{
   (void)memory_id;
   (void)as_of;
   return -1;
}

static int valid_at(int64_t memory_id, const char *as_of)
{
   valid_at_calls++;
   snprintf(valid_at_last, sizeof(valid_at_last), "%s", as_of);
   if (memory_id == 42)
      return 1;
   if (memory_id == 43)
      return 0;
   return -1;
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
   return strcmp(scope_type, "workspace") == 0 ? 1 : 0;
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
   return memory_id == 42 ? 0 : -1;
}

int db2_memory_update_content(int64_t memory_id, const char *content)
{
   (void)memory_id;
   (void)content;
   return 0;
}

static int update_content(int64_t memory_id, const char *content)
{
   update_content_calls++;
   snprintf(update_content_last, sizeof(update_content_last), "%s", content);
   return memory_id == 42 ? 1 : 0;
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
   get_content_calls++;
   if (memory_id != 42)
      return 0;
   snprintf(out, (size_t)out_len, "%s", "stored text");
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
   get_source_session_calls++;
   if (memory_id != 42)
      return -1;
   snprintf(out, (size_t)out_len, "%s", "sess-1");
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
   temporal_ref_calls++;
   if (memory_id != 42)
      return 0;
   snprintf(out, (size_t)out_len, "%s", "2026-08-19");
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
   *out_count = 7;
   snprintf(out_ts, (size_t)out_ts_len, "%s", "2026-08-19 09:00:00");
   return 1;
}

int db2_entity_edge_prune_orphans(void)
{
   return 0;
}

static int entity_edge_prune_orphans(void)
{
   edge_prune_calls++;
   return 2;
}

int db2_entity_edge_normalize_weights(void)
{
   return 0;
}

static int entity_edge_normalize_weights(void)
{
   edge_normalize_calls++;
   return 3;
}

int db2_code_index_project_count(void)
{
   return 0;
}

static int project_count(void)
{
   project_count_calls++;
   return 4;
}

int db2_code_index_purge_hidden_pollution(void)
{
   return 0;
}

static int purge_hidden_pollution(void)
{
   purge_pollution_calls++;
   return 5;
}

int db2_code_index_requeue_drifted(void)
{
   return 0;
}

static int requeue_drifted(void)
{
   requeue_drifted_calls++;
   return 6;
}

int db2_cross_repo_rebuild_routes(void)
{
   return 0;
}

static int cross_repo_rebuild_routes(void)
{
   rebuild_routes_calls++;
   return 15;
}

int db2_cross_repo_rebuild_identities(void)
{
   return 0;
}

static int cross_repo_rebuild_identities(void)
{
   rebuild_identities_calls++;
   return 16;
}

int db2_cross_repo_rebuild_build_deps(void)
{
   return 0;
}

static int cross_repo_rebuild_build_deps(void)
{
   rebuild_build_deps_calls++;
   return 17;
}

int64_t db2_code_index_drift_candidates(void)
{
   return 0;
}

static int64_t drift_candidates(void)
{
   drift_candidates_calls++;
   return 20;
}

int db2_rules_decay(void)
{
   return 0;
}

static int rules_decay(void)
{
   rules_decay_calls++;
   return 18;
}

int db2_curiosity_rescore_all(void)
{
   return 0;
}

static int curiosity_rescore_all(void)
{
   curiosity_rescore_calls++;
   return 19;
}

int db2_mining_seed_job_defaults(void)
{
   return 0;
}

static int mining_seed_job_defaults(void)
{
   mining_seed_calls++;
   return 0;
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
   return 22;
}

int db2_rel_types_ensure_seed(void)
{
   return 0;
}

static int rel_types_ensure_seed(void)
{
   rel_types_seed_calls++;
   return 0;
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
   return 1;
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
   return 21;
}

int db2_prospective_sweep_expired(void)
{
   return 0;
}

static int prospective_sweep_expired(void)
{
   prospective_sweep_calls++;
   return 7;
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
   return 0;
}

static int directive_record_surface(int64_t directive_id)
{
   directive_surface_calls++;
   directive_surface_id = directive_id;
   return 0;
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
   return 0;
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
   return 0;
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
   return 0;
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
   return 0;
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
   return 51;
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
   return 52;
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
   return 53;
}

static int directive_sweep_expired(void)
{
   directive_sweep_calls++;
   return 8;
}

int db2_decision_log_mark_revisit_due(void)
{
   return 0;
}

static int mark_revisit_due(void)
{
   mark_revisit_calls++;
   return 9;
}

int db2_kb_ingest_queue_reset_running(void)
{
   return 0;
}

static int ingest_queue_reset_running(void)
{
   queue_reset_calls++;
   return 10;
}

int db2_evidence_reembed_all(void)
{
   return 0;
}

static int evidence_reembed_all(void)
{
   evidence_reembed_calls++;
   return 11;
}

int db2_curator_reembed_all(void)
{
   return 0;
}

static int curator_reembed_all(void)
{
   curator_reembed_calls++;
   return 12;
}

int db2_synth_reenqueue_all(void)
{
   return 0;
}

static int synth_reenqueue_all(void)
{
   synth_reenqueue_calls++;
   return 13;
}

int db2_curator_reenqueue_extract_all(void)
{
   return 0;
}

static int curator_reenqueue_extract_all(void)
{
   extract_reenqueue_calls++;
   return 14;
}

static int stats_counts(aimee_db2_memory_stats_t *stats)
{
   stats_counts_calls++;
   *stats = (aimee_db2_memory_stats_t){
       .tier_counts = {3, 12, 30, 8, 2, 1},
       .kind_counts = {14, 5, 6, 9, 4, 3, 2, 1, 7, 5},
       .total = 56,
       .conflicts = 4,
   };
   return 0;
}

static int health_counters(int promote_use_count, double promote_confidence,
                           aimee_db2_health_counters_t *counters)
{
   health_counters_calls++;
   if (promote_use_count != (int)AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_USE_COUNT ||
       promote_confidence != AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_CONFIDENCE)
      return -1;
   *counters = (aimee_db2_health_counters_t){
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
   return 0;
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
   return 1;
}

static int dimension_reset(uint32_t target_dimension, uint32_t force, uint32_t dry_run,
                           aimee_db2_dimension_reset_t *status)
{
   assert(target_dimension == 384 && force == 0 && dry_run == 1);
   *status = (aimee_db2_dimension_reset_t){768, 384, 6, 0, 1234, -1, 7};
   return 0;
}

int db2_reembed_in_progress_clear(void)
{
   return 0;
}

int db2_reembed_clear_maintenance(int force, int *was_in_progress, int *recorded, int *running)
{
   (void)force;
   if (was_in_progress)
      *was_in_progress = 1;
   if (recorded)
      *recorded = 384;
   if (running)
      *running = 384;
   return 0;
}

const char *db2_embedder_serving_id(void)
{
   static char serving_id[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1];
   if (serving_id[0] == '\0')
   {
      memset(serving_id, 'x', sizeof(serving_id) - 1);
      serving_id[sizeof(serving_id) - 1] = '\0';
   }
   return serving_id;
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

   /* Three families now: lifecycle, memory, and index. Each is a distinct
    * event kind, and the module must serve a kind before any operation in
    * that family can be routed to it. */
   /* One entry per family, not per operation. Every operation in a family
    * resolves to the same event kind, so a per-operation list repeats each
    * kind up to eight times and reads like coverage it is not. These seven
    * kinds are the whole of what the module serves. */
   const uint32_t served[] = {AIMEE_DB2_EVENT_LIFECYCLE,    AIMEE_DB2_EVENT_MEMORY,
                              AIMEE_DB2_EVENT_INDEX,        AIMEE_DB2_EVENT_LEARNING,
                              AIMEE_DB2_EVENT_ORGANIZATION, AIMEE_DB2_EVENT_CUSTODY,
                              AIMEE_DB2_EVENT_MAINTENANCE};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = MODULE_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
        .serve = served,
        .serve_count = (uint32_t)(sizeof(served) / sizeof(served[0]))},
       {.principal_class = 1,
        .principal_ref = CALLER_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
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

   /* Same shape as `served`: the stage is the family, so one entry each. */
   static const aimee_module_stage_t stages[] = {
       {AIMEE_DB2_EVENT_LIFECYCLE, AIMEE_DB2_FAMILY_LIFECYCLE},
       {AIMEE_DB2_EVENT_MEMORY, AIMEE_DB2_FAMILY_MEMORY},
       {AIMEE_DB2_EVENT_INDEX, AIMEE_DB2_FAMILY_INDEX},
       {AIMEE_DB2_EVENT_LEARNING, AIMEE_DB2_FAMILY_LEARNING},
       {AIMEE_DB2_EVENT_ORGANIZATION, AIMEE_DB2_FAMILY_ORGANIZATION},
       {AIMEE_DB2_EVENT_CUSTODY, AIMEE_DB2_FAMILY_CUSTODY},
       {AIMEE_DB2_EVENT_MAINTENANCE, AIMEE_DB2_FAMILY_MAINTENANCE}};
   static const aimee_db2_module_backend_t backend = {
       .is_initialized = is_initialized,
       .health_probe = health_probe,
       .kb_health_probe = kb_health_probe,
       .embedding_dimension = embedding_dimension,
       .level3_count = level3_count,
       .level2_count = level2_count,
       .orphaned_l0_count = orphaned_l0_count,
       .total_count = total_count,
       .session_l2_count = session_l2_count,
       .key_exists = key_exists,
       .find_id_by_key_kind = find_id_by_key_kind,
       .key_exists_in_tier_pair = key_exists_in_tier_pair,
       .clear_effectiveness = clear_effectiveness,
       .set_effectiveness = set_effectiveness,
       .retention_delete = retention_delete,
       .demote_effectiveness = demote_effectiveness,
       .effectiveness_stats = effectiveness_stats,
       .list_l2_memory_ids = list_l2_memory_ids,
       .top_l2_facts = top_l2_facts,
       .list_session_scope_priority = list_session_scope_priority,
       .collect_alias_matches = collect_alias_matches,
       .collect_entity_matches = collect_entity_matches,
       .collect_event_frame_matches = collect_event_frame_matches,
       .collect_relation_token_matches = collect_relation_token_matches,
       .collect_summary_matches = collect_summary_matches,
       .collect_temporal_matches = collect_temporal_matches,
       .find_facts_like = find_facts_like,
       .list_session_scope_priority_like = list_session_scope_priority_like,
       .negation_fts_search = negation_fts_search,
       .search_facts_patterns_by_keyword = search_facts_patterns_by_keyword,
       .fact_history = fact_history,
       .list_rows = list_rows,
       .aggregate = aggregate,
       .load_eval_corpus = load_eval_corpus,
       .record_exists = record_exists,
       .document_exists = document_exists,
       .trace_mining_record = trace_mining_record,
       .anti_pattern_exists_exact = anti_pattern_exists_exact,
       .anti_pattern_exists_by_source_ref = anti_pattern_exists_by_source_ref,
       .artifact_citation_count = artifact_citation_count,
       .commits_in_last_7_days = commits_in_last_7_days,
       .entity_observation_count = entity_observation_count,
       .fidelity_attribution_count = fidelity_attribution_count,
       .blob_referenced = blob_referenced,
       .async_pending_count = async_pending_count,
       .artifact_stamp_reflected = artifact_stamp_reflected,
       .failed_query_bump = failed_query_bump,
       .fence_active = fence_active,
       .runtime_state_touch = runtime_state_touch,
       .synth_enqueue = synth_enqueue,
       .synth_mark_done = synth_mark_done,
       .reembed_mark_finished = reembed_mark_finished,
       .mining_job_try_lock = mining_job_try_lock,
       .artifact_set_state = artifact_set_state,
       .artifact_register_exemplar = artifact_register_exemplar,
       .evidence_enqueue = evidence_enqueue,
       .evidence_mark_failed = evidence_mark_failed,
       .synth_mark_failed = synth_mark_failed,
       .runtime_state_set = runtime_state_set,
       .set_active_embedder_version = set_active_embedder_version,
       .entity_profile_fresh = entity_profile_fresh,
       .doc_exists_by_hash = doc_exists_by_hash,
       .pdf_quarantine_confirm = pdf_quarantine_confirm,
       .pdf_quarantine_reject = pdf_quarantine_reject,
       .enrollment_active = enrollment_active,
       .runtime_state_get = runtime_state_get,
       .session_neighbors_before = session_neighbors_before,
       .session_neighbors_after = session_neighbors_after,
       .row_get = row_get,
       .row_get_by_unit_id = row_get_by_unit_id,
       .count_memories = count_memories,
       .count_recent_conflicts = count_recent_conflicts,
       .health_record = health_record,
       .prune_health = prune_health,
       .prune_contradictions = prune_contradictions,
       .health_counters = health_counters,
       .stats_counts = stats_counts,
       .delete_l0_provenance = delete_l0_provenance,
       .delete_l0 = delete_l0,
       .list_kinds_in_tier = list_kinds_in_tier,
       .kind_expire_days = kind_expire_days,
       .delete_stale_l1_provenance = delete_stale_l1_provenance,
       .delete_stale_l1 = delete_stale_l1,
       .now_utc = bus_now_utc,
       .kind_demote_policy = kind_demote_policy,
       .demote_kind = demote_kind,
       .demote_cascade = demote_cascade,
       .promote_stable = promote_stable,
       .reclassify_directives = reclassify_directives,
       .record_l4_approval = record_l4_approval,
       .prune_orphaned_l0 = prune_orphaned_l0,
       .lifecycle_sweep_expired = lifecycle_sweep_expired,
       .demote_id = demote_id,
       .has_workspace_tag = has_workspace_tag,
       .delete_row = delete_row,
       .touch = touch,
       .link_delete = link_delete,
       .valid_at = valid_at,
       .has_scope_type = has_scope_type,
       .reject = reject,
       .update_content = update_content,
       .decay_confidence = decay_confidence,
       .workspace_tag_insert = workspace_tag_insert,
       .set_cognified_kind = set_cognified_kind,
       .set_source_session = set_source_session,
       .negation_tokens_update = negation_tokens_update,
       .get_content = get_content,
       .get_source_session = get_source_session,
       .pick_first_temporal_ref = pick_first_temporal_ref,
       .count_and_max_updated = count_and_max_updated,
       .entity_edge_prune_orphans = entity_edge_prune_orphans,
       .entity_edge_normalize_weights = entity_edge_normalize_weights,
       .project_count = project_count,
       .purge_hidden_pollution = purge_hidden_pollution,
       .requeue_drifted = requeue_drifted,
       .cross_repo_rebuild_routes = cross_repo_rebuild_routes,
       .cross_repo_rebuild_identities = cross_repo_rebuild_identities,
       .cross_repo_rebuild_build_deps = cross_repo_rebuild_build_deps,
       .drift_candidates = drift_candidates,
       .rules_decay = rules_decay,
       .curiosity_rescore_all = curiosity_rescore_all,
       .mining_seed_job_defaults = mining_seed_job_defaults,
       .proposals_archive_expired = proposals_archive_expired,
       .trace_mining_last_id = trace_mining_last_id,
       .rel_types_ensure_seed = rel_types_ensure_seed,
       .vector_rebuild_lock_try_acquire = vector_rebuild_lock_try_acquire,
       .vector_rebuild_lock_release = vector_rebuild_lock_release,
       .release_get_active = release_get_active,
       .prospective_sweep_expired = prospective_sweep_expired,
       .directive_sweep_expired = directive_sweep_expired,
       .directive_suppress = directive_suppress,
       .directive_record_surface = directive_record_surface,
       .anti_pattern_bump = anti_pattern_bump,
       .anti_pattern_delete = anti_pattern_delete,
       .doc_delete = doc_delete,
       .task_delete = task_delete,
       .file_index_delete_project = file_index_delete_project,
       .clear_project = clear_project,
       .clear_current_project = clear_current_project,
       .mark_revisit_due = mark_revisit_due,
       .ingest_queue_reset_running = ingest_queue_reset_running,
       .evidence_reembed_all = evidence_reembed_all,
       .curator_reembed_all = curator_reembed_all,
       .synth_reenqueue_all = synth_reenqueue_all,
       .curator_reenqueue_extract_all = curator_reenqueue_extract_all,
       .pool_status = pool_status,
       .embedding_refusals = embedding_refusals,
       .postgres_status = postgres_status,
       .reembed_status = reembed_status,
       .reembed_clear = db2_reembed_in_progress_clear,
       .reembed_clear_maintenance = db2_reembed_clear_maintenance,
       .embedder_serving_id = db2_embedder_serving_id,
       .dimension_reset = dimension_reset,
       .entity_neighbors = db2_entity_edge_neighbors,
       .code_search = db2_code_index_code_search,
       .match_error_keys = db2_memory_promotion_match_error_keys,
   };
   process_thread_t process = {
       .config = {.socket_path = socket_path,
                  .module_name = "db2",
                  .principal_class = 1,
                  .principal_ref = MODULE_REF,
                  .stages = stages,
                  .stage_count = (uint32_t)(sizeof(stages) / sizeof(stages[0])),
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
   assert(initialized_calls == 1 && health_calls == 1 && kb_health_calls == 1);

   uint32_t domain_result = 9, dimension = 9;
   assert(aimee_db2_embedding_dimension_call(call_client, &client, 7010, 0, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && dimension == 384);
   assert(embedding_dimension_calls == 1);

   uint32_t level3_total = 99;
   assert(aimee_db2_level3_count_call(call_client, &client, 7020, 0, &level3_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(level3_total == 42 && level3_count_calls == 1);

   uint32_t level2_total = 99;
   assert(aimee_db2_level2_count_call(call_client, &client, 7021, 0, &level2_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(level2_total == 17 && level2_count_calls == 1);

   uint32_t orphaned_l0_total = 99;
   assert(aimee_db2_orphaned_l0_count_call(call_client, &client, 7022, 0, &orphaned_l0_total, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(orphaned_l0_total == 5 && orphaned_l0_count_calls == 1);

   uint64_t memory_total = 99;
   assert(aimee_db2_total_count_call(call_client, &client, 7023, 0, &memory_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(memory_total == 1234567890123ULL && total_count_calls == 1);

   uint32_t session_l2_total = 99;
   assert(aimee_db2_session_l2_count_call(call_client, &client, 7024, 0, "session-123",
                                          &session_l2_total, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(session_l2_total == 3 && session_l2_count_calls == 1);

   uint32_t exists = 99;
   assert(aimee_db2_key_exists_call(call_client, &client, 7025, 0, "recovery:tool-a->tool-b",
                                    &exists, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(exists == 1 && key_exists_calls == 1);

   uint32_t found = 99;
   uint64_t memory_id = 99;
   assert(aimee_db2_find_id_by_key_kind_call(call_client, &client, 7026, 0, "task:deploy-fix",
                                             "task", &found, &memory_id, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(found == 1 && memory_id == 42 && find_id_by_key_kind_calls == 1);

   exists = 99;
   assert(aimee_db2_key_exists_in_tier_pair_call(call_client, &client, 7027, 0,
                                                 "recovery:tool-a->tool-b", "L3", "L4", &exists,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(exists == 1 && key_exists_in_tier_pair_calls == 1);

   domain_result = 99;
   assert(aimee_db2_effectiveness_update_call(call_client, &client, 7028, 0, 42, 1, 0.75,
                                              &domain_result, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && set_effectiveness_calls == 1);

   uint32_t deleted_count = 99;
   assert(aimee_db2_retention_enforce_call(call_client, &client, 7029, 0, &deleted_count, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(deleted_count == 5 && retention_delete_calls == 2);

   uint32_t demoted_count = 99;
   assert(aimee_db2_effectiveness_demote_call(call_client, &client, 7030, 0, &demoted_count, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(demoted_count == 2 && demote_effectiveness_calls == 1);

   aimee_db2_effectiveness_stats_t stats = {0};
   assert(aimee_db2_effectiveness_stats_call(call_client, &client, 7031, 0, &stats, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(stats.avg_effectiveness == 0.5 && stats.low_effectiveness_count == 3 &&
          stats.high_impact_count == 1 && effectiveness_stats_calls == 1);

   uint64_t l2_ids[AIMEE_DB2_L2_MEMORY_IDS_MAX];
   uint32_t l2_count = 99;
   assert(aimee_db2_l2_memory_ids_call(call_client, &client, 7032, 0, l2_ids,
                                       AIMEE_DB2_L2_MEMORY_IDS_MAX, &l2_count, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(l2_count == 3 && l2_ids[0] == 7 && l2_ids[1] == 19 && l2_ids[2] == 4242 &&
          list_l2_memory_ids_calls == 1);

   /* The row-list reply, carrying rows. The third identifier is past what a
    * double can hold exactly, so a codec that ever routed one through a
    * floating-point value would return a different number here. */
   aimee_db2_match_error_keys_row_t matched[AIMEE_DB2_MATCH_ERROR_KEYS_MAX_ROWS];
   uint32_t matched_count = 99;
   assert(aimee_db2_match_error_keys_call(call_client, &client, 7043, 0, "boom: alpha not found",
                                          matched, AIMEE_DB2_MATCH_ERROR_KEYS_MAX_ROWS,
                                          &matched_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(matched_count == 3 && matched[0].memory_id == 11 && matched[1].memory_id == 22 &&
          matched[2].memory_id == 9007199254740993 && match_error_keys_calls == 1 &&
          strcmp(match_error_keys_seen, "boom: alpha not found") == 0);

   /* A row reply with a string field in it, carrying two rows: one ordinary,
    * one whose node is empty and whose weight is zero. Both are inside the
    * schema's bounds, so a codec that read either as the end of the list would
    * come back with one row. The limit travels as its own argument, separate
    * from the number of rows the caller can hold. */
   static aimee_db2_entity_neighbors_row_t neighbor_rows[AIMEE_DB2_ENTITY_NEIGHBORS_MAX_ROWS];
   uint32_t neighbor_count = 99;
   assert(aimee_db2_entity_neighbors_call(call_client, &client, 7044, 0, "alpha-entity", 12u,
                                          neighbor_rows, AIMEE_DB2_ENTITY_NEIGHBORS_MAX_ROWS,
                                          &neighbor_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(neighbor_count == 2 && strcmp(neighbor_rows[0].node, "alpha") == 0 &&
          neighbor_rows[0].weight == 7 && neighbor_rows[1].node[0] == '\0' &&
          neighbor_rows[1].weight == 0 && entity_neighbors_calls == 1 &&
          entity_neighbors_limit_seen == 12 && strcmp(entity_neighbors_seen, "alpha-entity") == 0);

   /* A row reply carrying a float. The rank needs the full mantissa, so a
    * codec that ever narrowed it would come back with a different number, and
    * the enrichment flag is checked at the backend because the reply cannot
    * show whether it arrived. */
   static aimee_db2_code_search_row_t search_rows[AIMEE_DB2_CODE_SEARCH_MAX_ROWS];
   uint32_t search_count = 99;
   assert(aimee_db2_code_search_call(call_client, &client, 7045, 0, "alpha", "alpha-project", 1u,
                                     search_rows, AIMEE_DB2_CODE_SEARCH_MAX_ROWS, &search_count,
                                     NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(search_count == 1 && strcmp(search_rows[0].project, "alpha-project") == 0 &&
          strcmp(search_rows[0].file_path, "src/alpha.c") == 0 &&
          strcmp(search_rows[0].snippet, ">>>alpha<<<") == 0 &&
          search_rows[0].rank == 0.1234567890123456 &&
          strcmp(search_rows[0].content_hash, "deadbeef") == 0 && search_rows[0].line == 42 &&
          code_search_calls == 1 && code_search_enrich_seen == 1);

   uint64_t scoped_ids[AIMEE_DB2_TOP_L2_FACTS_MAX];
   uint32_t scoped_count = 99;
   assert(aimee_db2_top_l2_facts_call(call_client, &client, 7040, 0, 5u, 3u, "alpha-workspace",
                                      "beta-project", scoped_ids, AIMEE_DB2_TOP_L2_FACTS_MAX,
                                      &scoped_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   /* The scope the caller sent is the scope the backend was given: this is the
    * whole reason it travels rather than being read from a thread-local. */
   assert(scoped_count == 2 && scoped_ids[0] == 31 && scoped_ids[1] == 32 &&
          top_l2_facts_calls == 1 && scoped_active_seen == 1 && scoped_include_all_seen == 1 &&
          scoped_limit_seen == 5 && strcmp(scoped_workspace_seen, "alpha-workspace") == 0 &&
          strcmp(scoped_project_seen, "beta-project") == 0);

   scoped_count = 99;
   assert(aimee_db2_list_session_scope_priority_call(
              call_client, &client, 7041, 0, 4u, 0u, "", "", scoped_ids,
              AIMEE_DB2_LIST_SESSION_SCOPE_PRIORITY_MAX, &scoped_count, NULL,
              NULL) == AIMEE_MODULE_CALL_OK);
   /* An inactive scope with empty names is a distinct request, not a missing
    * one, and it reaches the second backend rather than the first. */
   assert(scoped_count == 2 && scoped_ids[0] == 71 && scoped_ids[1] == 72 &&
          list_session_scope_priority_calls == 1 && top_l2_facts_calls == 1 &&
          scoped_active_seen == 0 && scoped_include_all_seen == 0 && scoped_limit_seen == 4 &&
          scoped_workspace_seen[0] == '\0' && scoped_project_seen[0] == '\0');

   /* A limit of zero is outside the contract, so the encoder refuses it before
    * anything reaches the bus. */
   assert(aimee_db2_top_l2_facts_call(call_client, &client, 7042, 0, 0u, 0u, "", "", scoped_ids,
                                      AIMEE_DB2_TOP_L2_FACTS_MAX, &scoped_count, NULL,
                                      NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(top_l2_facts_calls == 1);

   /* Six operations share one envelope shape and are told apart only by the
    * operation number, so each is called in turn and identified by what comes
    * back. A request routed to the wrong probe fails here. */
   uint64_t probe_ids[AIMEE_DB2_COLLECT_ALIAS_MATCHES_MAX];
   uint32_t probe_count = 99;
   assert(aimee_db2_collect_alias_matches_call(call_client, &client, 7050, 0, "needle", 2u, 1u,
                                               "probe-workspace", "probe-project", probe_ids,
                                               AIMEE_DB2_COLLECT_ALIAS_MATCHES_MAX, &probe_count,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 100 && probe_ids[1] == 101 &&
          term_probe_calls[0] == 1 && term_probe_limit_seen == 2 && term_probe_active_seen == 1 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   assert(aimee_db2_collect_entity_matches_call(call_client, &client, 7051, 0, "needle", 3u, 1u,
                                                "probe-workspace", "probe-project", probe_ids,
                                                AIMEE_DB2_COLLECT_ENTITY_MATCHES_MAX, &probe_count,
                                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 200 && probe_ids[1] == 201 &&
          term_probe_calls[1] == 1 && term_probe_limit_seen == 3 && term_probe_active_seen == 1 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   assert(aimee_db2_collect_event_frame_matches_call(
              call_client, &client, 7052, 0, "needle", 4u, 1u, "probe-workspace", "probe-project",
              probe_ids, AIMEE_DB2_COLLECT_EVENT_FRAME_MATCHES_MAX, &probe_count, NULL,
              NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 300 && probe_ids[1] == 301 &&
          term_probe_calls[2] == 1 && term_probe_limit_seen == 4 && term_probe_active_seen == 1 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   assert(aimee_db2_collect_relation_token_matches_call(
              call_client, &client, 7053, 0, "needle", 5u, 1u, "probe-workspace", "probe-project",
              probe_ids, AIMEE_DB2_COLLECT_RELATION_TOKEN_MATCHES_MAX, &probe_count, NULL,
              NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 400 && probe_ids[1] == 401 &&
          term_probe_calls[3] == 1 && term_probe_limit_seen == 5 && term_probe_active_seen == 1 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   assert(aimee_db2_collect_summary_matches_call(call_client, &client, 7054, 0, "needle", 6u, 1u,
                                                 "probe-workspace", "probe-project", probe_ids,
                                                 AIMEE_DB2_COLLECT_SUMMARY_MATCHES_MAX,
                                                 &probe_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 500 && probe_ids[1] == 501 &&
          term_probe_calls[4] == 1 && term_probe_limit_seen == 6 && term_probe_active_seen == 1 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   assert(aimee_db2_collect_temporal_matches_call(
              call_client, &client, 7055, 0, "needle", 7u, 1u, "probe-workspace", "probe-project",
              probe_ids, AIMEE_DB2_COLLECT_TEMPORAL_MATCHES_MAX, &probe_count, NULL,
              NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 600 && probe_ids[1] == 601 &&
          term_probe_calls[5] == 1 && term_probe_limit_seen == 7 && term_probe_active_seen == 1 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   probe_count = 99;
   assert(aimee_db2_find_facts_like_call(call_client, &client, 7070, 0, "needle", 2u, 1u,
                                         "probe-workspace", "probe-project", probe_ids,
                                         AIMEE_DB2_FIND_FACTS_LIKE_MAX, &probe_count, NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 700 && probe_ids[1] == 701 &&
          term_probe_calls[6] == 1 && term_probe_limit_seen == 2 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   probe_count = 99;
   assert(aimee_db2_list_session_scope_priority_like_call(
              call_client, &client, 7071, 0, "needle", 3u, 1u, "probe-workspace", "probe-project",
              probe_ids, AIMEE_DB2_LIST_SESSION_SCOPE_PRIORITY_LIKE_MAX, &probe_count, NULL,
              NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 800 && probe_ids[1] == 801 &&
          term_probe_calls[7] == 1 && term_probe_limit_seen == 3 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   probe_count = 99;
   assert(aimee_db2_negation_fts_search_call(call_client, &client, 7072, 0, "needle", 4u, 1u,
                                             "probe-workspace", "probe-project", probe_ids,
                                             AIMEE_DB2_NEGATION_FTS_SEARCH_MAX, &probe_count, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 900 && probe_ids[1] == 901 &&
          term_probe_calls[8] == 1 && term_probe_limit_seen == 4 &&
          strcmp(term_probe_term_seen, "needle") == 0);

   /* The two session walks share an envelope shape as well, and a zero anchor is
    * a legal request on both -- so both are called with one. */
   uint64_t walk_ids[AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_MAX];
   uint32_t walk_count = 99;
   walk_count = 99;
   assert(aimee_db2_session_neighbors_before_call(call_client, &client, 7080, 0, "session-9f2a", 0u,
                                                  3u, walk_ids,
                                                  AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_MAX,
                                                  &walk_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(walk_count == 2 && walk_ids[0] == 1000 && walk_ids[1] == 1001 && walk_calls[0] == 1 &&
          walk_anchor_seen == 0 && walk_limit_seen == 3 &&
          strcmp(walk_session_seen, "session-9f2a") == 0);

   walk_count = 99;
   assert(aimee_db2_session_neighbors_after_call(call_client, &client, 7081, 0, "session-9f2a", 0u,
                                                 4u, walk_ids,
                                                 AIMEE_DB2_SESSION_NEIGHBORS_AFTER_MAX, &walk_count,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(walk_count == 2 && walk_ids[0] == 2000 && walk_ids[1] == 2001 && walk_calls[1] == 1 &&
          walk_anchor_seen == 0 && walk_limit_seen == 4 &&
          strcmp(walk_session_seen, "session-9f2a") == 0);

   /* An empty session identifier is refused: it is the whole filter, and an
    * empty filter would select every session's memories rather than none. */
   assert(aimee_db2_session_neighbors_before_call(call_client, &client, 7090, 0, "", 4u, 4u,
                                                  walk_ids, AIMEE_DB2_SESSION_NEIGHBORS_BEFORE_MAX,
                                                  &walk_count, NULL,
                                                  NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(walk_calls[0] == 1);

   /* The row getters are the only operations whose reply carries a whole memory
    * row, so the round trip is checked field by field rather than by count. */
   aimee_db2_memory_row_t fetched;
   uint32_t row_result = 99;
   assert(aimee_db2_row_get_call(call_client, &client, 7100, 0, 2048u, &row_result, &fetched, NULL,
                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(row_result == AIMEE_DB2_RESULT_OK && fetched.id == 10 && fetched.confidence == 0.5 &&
          fetched.salience == 0.25 && fetched.use_count == 3u && strcmp(fetched.tier, "L2") == 0 &&
          strcmp(fetched.kind, "fact") == 0 && strcmp(fetched.key, "row-key") == 0 &&
          strcmp(fetched.content, "row content") == 0 && fetched.use_cases[0] == '\0' &&
          row_calls[0] == 1 && row_identifier_seen == 2048);

   assert(aimee_db2_row_get_by_unit_id_call(call_client, &client, 7101, 0, 2048u, &row_result,
                                            &fetched, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(row_result == AIMEE_DB2_RESULT_OK && fetched.id == 20 && fetched.confidence == 0.5 &&
          fetched.salience == 0.25 && fetched.use_count == 3u && strcmp(fetched.tier, "L2") == 0 &&
          strcmp(fetched.kind, "fact") == 0 && strcmp(fetched.key, "row-key") == 0 &&
          strcmp(fetched.content, "row content") == 0 && fetched.use_cases[0] == '\0' &&
          row_calls[1] == 1 && row_identifier_seen == 2048);

   /* An absent row is a result, not a transport failure, and it clears the row
    * the caller passed rather than leaving the previous answer in it. */
   row_result = 99;
   assert(aimee_db2_row_get_call(call_client, &client, 7102, 0, 404u, &row_result, &fetched, NULL,
                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(row_result == AIMEE_DB2_RESULT_NOT_FOUND && fetched.id == 0 && fetched.tier[0] == '\0' &&
          row_calls[0] == 2);

   /* Zero is not an identifier, so the encoder refuses it before the bus. */
   assert(aimee_db2_row_get_call(call_client, &client, 7103, 0, 0u, &row_result, &fetched, NULL,
                                 NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(row_calls[0] == 2);

   probe_count = 99;
   assert(aimee_db2_search_facts_patterns_by_keyword_call(
              call_client, &client, 7110, 0, "needle", 5u, 1u, "probe-workspace", "probe-project",
              probe_ids, AIMEE_DB2_SEARCH_FACTS_PATTERNS_BY_KEYWORD_MAX, &probe_count, NULL,
              NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_count == 2 && probe_ids[0] == 1000 && probe_ids[1] == 1001 &&
          term_probe_calls[9] == 1 && strcmp(term_probe_term_seen, "needle") == 0);

   /* The history is the one search that carries no scope. */
   uint64_t history_ids[AIMEE_DB2_FACT_HISTORY_MAX];
   uint32_t history_count = 99;
   assert(aimee_db2_fact_history_call(call_client, &client, 7111, 0, "fact:deploy-target", 6u,
                                      history_ids, AIMEE_DB2_FACT_HISTORY_MAX, &history_count, NULL,
                                      NULL) == AIMEE_MODULE_CALL_OK);
   assert(history_count == 2 && history_ids[0] == 5000 && history_ids[1] == 5001 &&
          history_calls == 1 && strcmp(history_key_seen, "fact:deploy-target") == 0);

   /* An empty key would match every unversioned row, so the encoder refuses it. */
   assert(aimee_db2_fact_history_call(call_client, &client, 7112, 0, "", 6u, history_ids,
                                      AIMEE_DB2_FACT_HISTORY_MAX, &history_count, NULL,
                                      NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(history_calls == 1);

   /* An empty tier or kind is no filter on that column, so the backend has to
    * receive the empty string rather than a placeholder; both shapes are sent. */
   uint64_t list_ids[AIMEE_DB2_LIST_ROWS_MAX];
   uint32_t list_count = 99;
   assert(aimee_db2_list_rows_call(call_client, &client, 7120, 0, 7u, 3u, 1u, "L2", "fact",
                                   "probe-workspace", "probe-project", list_ids,
                                   AIMEE_DB2_LIST_ROWS_MAX, &list_count, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(list_count == 2 && list_ids[0] == 6000 && list_ids[1] == 6001 && list_rows_calls == 1 &&
          list_hide_seen == 1 && strcmp(list_tier_seen, "L2") == 0 &&
          strcmp(list_kind_seen, "fact") == 0);

   list_count = 99;
   assert(aimee_db2_list_rows_call(call_client, &client, 7121, 0, 7u, 0u, 0u, "", "", "", "",
                                   list_ids, AIMEE_DB2_LIST_ROWS_MAX, &list_count, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(list_count == 2 && list_rows_calls == 2 && list_hide_seen == 0 &&
          list_tier_seen[0] == '\0' && list_kind_seen[0] == '\0');

   /* A tier longer than the column it filters is refused before the bus. */
   assert(aimee_db2_list_rows_call(call_client, &client, 7122, 0, 7u, 0u, 0u, "L2XY", "", "", "",
                                   list_ids, AIMEE_DB2_LIST_ROWS_MAX, &list_count, NULL,
                                   NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(list_rows_calls == 2);

   /* Truncation is the whole reason the aggregate's reply is not a bare list. */
   uint64_t aggregate_ids[AIMEE_DB2_AGGREGATE_MAX];
   uint32_t aggregate_count = 99, aggregate_truncated = 99;
   assert(aimee_db2_aggregate_call(call_client, &client, 7130, 0, "deployment", "rollout", 5u,
                                   &aggregate_truncated, aggregate_ids, AIMEE_DB2_AGGREGATE_MAX,
                                   &aggregate_count, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(aggregate_count == 2 && aggregate_truncated == 1 && aggregate_ids[0] == 7000 &&
          aggregate_calls == 1 && strcmp(aggregate_entity_seen, "deployment") == 0 &&
          strcmp(aggregate_keyword_seen, "rollout") == 0);

   /* Both selectors empty is the third statement, not a missing argument. */
   aggregate_count = 99;
   assert(aimee_db2_aggregate_call(call_client, &client, 7131, 0, "", "", 5u, &aggregate_truncated,
                                   aggregate_ids, AIMEE_DB2_AGGREGATE_MAX, &aggregate_count, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(aggregate_count == 2 && aggregate_calls == 2 && aggregate_entity_seen[0] == '\0' &&
          aggregate_keyword_seen[0] == '\0');

   /* The corpus names the plan that answered; the identifiers do not. */
   uint64_t eval_corpus_ids[AIMEE_DB2_LOAD_EVAL_CORPUS_MAX];
   uint32_t eval_corpus_count = 99;
   char eval_corpus_label[AIMEE_DB2_LOAD_EVAL_CORPUS_LABEL_MAX + 1] = "";
   assert(aimee_db2_load_eval_corpus_call(call_client, &client, 7132, 0, 9u, eval_corpus_label,
                                          sizeof(eval_corpus_label), eval_corpus_ids,
                                          AIMEE_DB2_LOAD_EVAL_CORPUS_MAX, &eval_corpus_count, NULL,
                                          NULL) == AIMEE_MODULE_CALL_OK);
   assert(eval_corpus_count == 2 && eval_corpus_ids[0] == 8000 && corpus_calls == 1 &&
          corpus_limit_seen == 9 && strcmp(eval_corpus_label, "L2 facts") == 0);

   /* The two probes share an envelope shape and are told apart only by the
    * operation number, so each is called and identified by which backend ran. */
   uint32_t probe_exists = 99;
   assert(aimee_db2_record_exists_call(call_client, &client, 7140, 0, 2048u, &probe_exists, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_exists == 1 && probe_calls[0] == 1 && probe_calls[1] == 0 &&
          probe_identifier_seen == 2048);

   probe_exists = 99;
   assert(aimee_db2_document_exists_call(call_client, &client, 7141, 0, 2049u, &probe_exists, NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_exists == 1 && probe_calls[1] == 1 && probe_identifier_seen == 2049);

   /* An absent row and a statement that did not run are both reported false:
    * neither invents a true, and the caller cannot tell them apart. */
   probe_exists = 99;
   assert(aimee_db2_record_exists_call(call_client, &client, 7142, 0, 404u, &probe_exists, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_exists == 0);
   probe_exists = 99;
   assert(aimee_db2_record_exists_call(call_client, &client, 7143, 0, 500u, &probe_exists, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(probe_exists == 0 && probe_calls[0] == 3);

   /* Zero is not an identifier on any of the three. */
   assert(aimee_db2_record_exists_call(call_client, &client, 7144, 0, 0u, &probe_exists, NULL,
                                       NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(probe_calls[0] == 3);

   assert(aimee_db2_trace_mining_record_call(call_client, &client, 7145, 0, 90210u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(mining_calls == 1 && mining_watermark_seen == 90210);

   /* Four reads on one envelope shape, told apart only by the operation number,
    * so each is called and identified by the number it answers. */
   uint32_t string_answer = 99;
   string_answer = 99;
   assert(aimee_db2_anti_pattern_exists_exact_call(call_client, &client, 7150, 0, "probe-argument",
                                                   &string_answer, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 0 && string_read_calls[0] == 1 &&
          strcmp(string_read_argument_seen, "probe-argument") == 0);

   string_answer = 99;
   assert(aimee_db2_anti_pattern_exists_by_source_ref_call(call_client, &client, 7151, 0,
                                                           "probe-argument", &string_answer, NULL,
                                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1 && string_read_calls[1] == 1 &&
          strcmp(string_read_argument_seen, "probe-argument") == 0);

   string_answer = 99;
   assert(aimee_db2_artifact_citation_count_call(call_client, &client, 7152, 0, "probe-argument",
                                                 &string_answer, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 77 && string_read_calls[2] == 1 &&
          strcmp(string_read_argument_seen, "probe-argument") == 0);

   string_answer = 99;
   assert(aimee_db2_commits_in_last_7_days_call(call_client, &client, 7153, 0, "probe-argument",
                                                &string_answer, NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 3 && string_read_calls[3] == 1 &&
          strcmp(string_read_argument_seen, "probe-argument") == 0);

   /* The existence probes clamp: their backend answering two would otherwise
    * put a value on the wire that the contract says cannot appear. */
   string_answer = 99;
   assert(aimee_db2_anti_pattern_exists_by_source_ref_call(call_client, &client, 7160, 0, "probe",
                                                           &string_answer, NULL,
                                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1);

   /* An empty argument is refused before the bus: none of these statements
    * means anything against the empty string. */
   assert(aimee_db2_artifact_citation_count_call(call_client, &client, 7161, 0, "", &string_answer,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(string_read_calls[2] == 1);

   /* Four more of the same shape, in four different families, so each also
    * proves it reached the family branch that owns it. */
   string_answer = 99;
   assert(aimee_db2_entity_observation_count_call(call_client, &client, 7170, 0, "probe-argument",
                                                  &string_answer, NULL,
                                                  NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 11 && cross_family_calls[0] == 1 &&
          strcmp(string_read_argument_seen, "probe-argument") == 0);

   string_answer = 99;
   assert(aimee_db2_fidelity_attribution_count_call(call_client, &client, 7171, 0, "probe-argument",
                                                    &string_answer, NULL,
                                                    NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 12 && cross_family_calls[1] == 1 &&
          strcmp(string_read_argument_seen, "probe-argument") == 0);

   string_answer = 99;
   assert(aimee_db2_blob_referenced_call(call_client, &client, 7172, 0, "probe-argument",
                                         &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1 && cross_family_calls[2] == 1 &&
          strcmp(string_read_argument_seen, "probe-argument") == 0);

   string_answer = 99;
   assert(aimee_db2_async_pending_count_call(call_client, &client, 7173, 0, "probe-argument",
                                             &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 14 && cross_family_calls[3] == 1 &&
          strcmp(string_read_argument_seen, "probe-argument") == 0);

   /* Eight more on the two single-string formats, spread across three families.
    * The acknowledging ones carry nothing back, so what they prove is that the
    * request reached the family and backend that owns it. */
   assert(aimee_db2_artifact_stamp_reflected_call(call_client, &client, 7180, 0, "probe-argument",
                                                  NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(batch8_calls[0] == 1 && strcmp(string_read_argument_seen, "probe-argument") == 0);

   string_answer = 99;
   assert(aimee_db2_failed_query_bump_call(call_client, &client, 7181, 0, "probe-argument",
                                           &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1 && batch8_calls[1] == 1);

   string_answer = 99;
   assert(aimee_db2_fence_active_call(call_client, &client, 7182, 0, "probe-argument",
                                      &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1 && batch8_calls[2] == 1);

   assert(aimee_db2_runtime_state_touch_call(call_client, &client, 7183, 0, "probe-argument", NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(batch8_calls[3] == 1 && strcmp(string_read_argument_seen, "probe-argument") == 0);

   assert(aimee_db2_synth_enqueue_call(call_client, &client, 7184, 0, "probe-argument", NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(batch8_calls[4] == 1 && strcmp(string_read_argument_seen, "probe-argument") == 0);

   assert(aimee_db2_synth_mark_done_call(call_client, &client, 7185, 0, "probe-argument", NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
   assert(batch8_calls[5] == 1 && strcmp(string_read_argument_seen, "probe-argument") == 0);

   assert(aimee_db2_reembed_mark_finished_call(call_client, &client, 7186, 0, "probe-argument",
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(batch8_calls[6] == 1 && strcmp(string_read_argument_seen, "probe-argument") == 0);

   string_answer = 99;
   assert(aimee_db2_mining_job_try_lock_call(call_client, &client, 7187, 0, "probe-argument",
                                             &string_answer, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1 && batch8_calls[7] == 1);

   /* Seven on the string-pair format. Both strings are checked on arrival
    * because a decoder that read them in the wrong order would still decode. */
   assert(aimee_db2_artifact_set_state_call(call_client, &client, 7200, 0, "probe-first",
                                            "probe-second", NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(pair_calls[0] == 1 && strcmp(pair_first_seen, "probe-first") == 0 &&
          strcmp(pair_second_seen, "probe-second") == 0);

   assert(aimee_db2_artifact_register_exemplar_call(call_client, &client, 7201, 0, "probe-first",
                                                    "probe-second", NULL,
                                                    NULL) == AIMEE_MODULE_CALL_OK);
   assert(pair_calls[1] == 1 && strcmp(pair_first_seen, "probe-first") == 0 &&
          strcmp(pair_second_seen, "probe-second") == 0);

   assert(aimee_db2_evidence_enqueue_call(call_client, &client, 7202, 0, "probe-first",
                                          "probe-second", NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(pair_calls[2] == 1 && strcmp(pair_first_seen, "probe-first") == 0 &&
          strcmp(pair_second_seen, "probe-second") == 0);

   assert(aimee_db2_evidence_mark_failed_call(call_client, &client, 7203, 0, "probe-first",
                                              "probe-second", NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(pair_calls[3] == 1 && strcmp(pair_first_seen, "probe-first") == 0 &&
          strcmp(pair_second_seen, "probe-second") == 0);

   assert(aimee_db2_synth_mark_failed_call(call_client, &client, 7204, 0, "probe-first",
                                           "probe-second", NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(pair_calls[4] == 1 && strcmp(pair_first_seen, "probe-first") == 0 &&
          strcmp(pair_second_seen, "probe-second") == 0);

   assert(aimee_db2_runtime_state_set_call(call_client, &client, 7205, 0, "probe-first",
                                           "probe-second", NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(pair_calls[5] == 1 && strcmp(pair_first_seen, "probe-first") == 0 &&
          strcmp(pair_second_seen, "probe-second") == 0);

   assert(aimee_db2_set_active_embedder_version_call(call_client, &client, 7206, 0, "probe-first",
                                                     "probe-second", NULL,
                                                     NULL) == AIMEE_MODULE_CALL_OK);
   assert(pair_calls[6] == 1 && strcmp(pair_first_seen, "probe-first") == 0 &&
          strcmp(pair_second_seen, "probe-second") == 0);

   /* An empty first string is refused; an empty second is allowed where the
    * operation says so, which is what evidence_mark_failed is for. */
   assert(aimee_db2_artifact_set_state_call(call_client, &client, 7210, 0, "", "artifact", NULL,
                                            NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(aimee_db2_evidence_mark_failed_call(call_client, &client, 7211, 0, "artifact", "", NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(pair_calls[3] == 2 && pair_second_seen[0] == '\0');

   /* Five reads on the pair format. The three Boolean-bounded ones clamp what
    * their backend answers; the two counts pass it through. */
   string_answer = 99;
   assert(aimee_db2_entity_profile_fresh_call(call_client, &client, 7220, 0, "probe-first",
                                              "probe-second", &string_answer, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1 && pair_read_calls[0] == 1 &&
          strcmp(pair_first_seen, "probe-first") == 0);

   string_answer = 99;
   assert(aimee_db2_doc_exists_by_hash_call(call_client, &client, 7221, 0, "probe-first",
                                            "probe-second", &string_answer, NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1 && pair_read_calls[1] == 1 &&
          strcmp(pair_first_seen, "probe-first") == 0);

   string_answer = 99;
   assert(aimee_db2_pdf_quarantine_confirm_call(call_client, &client, 7222, 0, "probe-first",
                                                "probe-second", &string_answer, NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 3 && pair_read_calls[2] == 1 &&
          strcmp(pair_first_seen, "probe-first") == 0);

   string_answer = 99;
   assert(aimee_db2_pdf_quarantine_reject_call(call_client, &client, 7223, 0, "probe-first",
                                               "probe-second", &string_answer, NULL,
                                               NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 4 && pair_read_calls[3] == 1 &&
          strcmp(pair_first_seen, "probe-first") == 0);

   string_answer = 99;
   assert(aimee_db2_enrollment_active_call(call_client, &client, 7224, 0, "probe-first",
                                           "probe-second", &string_answer, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(string_answer == 1 && pair_read_calls[4] == 1 &&
          strcmp(pair_first_seen, "probe-first") == 0);

   /* The first operation on the described format: its request and reply are a
    * schema in the catalog rather than codecs someone wrote, so this checks the
    * value survives the round trip rather than only that the call succeeded. */
   {
      char state_value[AIMEE_DB2_RUNTIME_STATE_GET_STATE_VALUE_MAX + 1] = "";
      assert(aimee_db2_runtime_state_get_call(call_client, &client, 7240, 0, "probe-key",
                                              state_value, sizeof(state_value), NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
      assert(strcmp(state_value, "probe-value") == 0 && runtime_state_get_calls == 1 &&
             strcmp(runtime_state_key_seen, "probe-key") == 0);

      /* An empty key is refused before the bus: the schema says one byte at
       * least, and an empty key names nothing. */
      assert(aimee_db2_runtime_state_get_call(call_client, &client, 7241, 0, "", state_value,
                                              sizeof(state_value), NULL,
                                              NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
      assert(runtime_state_get_calls == 1);
   }

   /* An empty term is not a wildcard: every one of these statements would match
    * nothing, so the encoder refuses it rather than asking. */
   assert(aimee_db2_collect_alias_matches_call(call_client, &client, 7060, 0, "", 4u, 0u, "", "",
                                               probe_ids, AIMEE_DB2_COLLECT_ALIAS_MATCHES_MAX,
                                               &probe_count, NULL,
                                               NULL) == AIMEE_MODULE_CALL_INVALID_ARGUMENT);
   assert(term_probe_calls[0] == 1);

   assert(aimee_db2_health_record_call(call_client, &client, 7033, 0, 4u, 2u, 9u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(health_record_calls == 1 && health_record_total == 512 &&
          health_record_contradictions == 6 && health_record_promotions == 4);

   uint32_t snapshots_deleted = 99, contradictions_deleted = 99;
   assert(aimee_db2_health_retention_call(call_client, &client, 7034, 0, &snapshots_deleted,
                                          &contradictions_deleted, NULL,
                                          NULL) == AIMEE_MODULE_CALL_OK);
   assert(snapshots_deleted == 11 && contradictions_deleted == 3 && prune_health_calls == 1 &&
          prune_contradictions_calls == 1);

   aimee_db2_health_counters_t counters = {0};
   assert(aimee_db2_health_counters_call(call_client, &client, 7035, 0, &counters, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(counters.cycles == 7 && counters.total_contradictions == 13 &&
          counters.l2_stale_30_days == 6 && health_counters_calls == 1);

   aimee_db2_memory_stats_t corpus = {0};
   assert(aimee_db2_stats_counts_call(call_client, &client, 7036, 0, &corpus, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(corpus.tier_counts[0] == 3 && corpus.tier_counts[5] == 1 && corpus.kind_counts[0] == 14 &&
          corpus.kind_counts[9] == 5 && corpus.total == 56 && corpus.conflicts == 4 &&
          stats_counts_calls == 1);

   uint32_t level0_deleted = 99, stale_deleted = 99;
   assert(aimee_db2_expire_call(call_client, &client, 7037, 0, &level0_deleted, &stale_deleted,
                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   /* Each kind expires on its own window: scratch at -7, fact at -30. */
   assert(level0_deleted == 9 && stale_deleted == 17 && expire_l0_provenance_calls == 1 &&
          expire_stale_provenance_calls == 2);

   uint32_t tier_demoted = 99, tier_cascaded = 99;
   assert(aimee_db2_demote_call(call_client, &client, 7038, 0, &tier_demoted, &tier_cascaded, NULL,
                                NULL) == AIMEE_MODULE_CALL_OK);
   /* Both kinds demote on their own threshold and window, then one cascade
    * runs against the stamp they shared. */
   assert(tier_demoted == 6 && tier_cascaded == 3 && demote_cascade_calls == 1);

   uint32_t tier_promoted = 99;
   assert(aimee_db2_promote_stable_call(call_client, &client, 7039, 0, &tier_promoted, NULL,
                                        NULL) == AIMEE_MODULE_CALL_OK);
   assert(tier_promoted == 4 && promote_stable_calls == 1);

   uint32_t reclassified = 99;
   assert(aimee_db2_reclassify_directives_call(call_client, &client, 7040, 0, 1u, &reclassified,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reclassified == 3 && reclassify_last_gate == 1);
   assert(aimee_db2_reclassify_directives_call(call_client, &client, 7041, 0, 0u, &reclassified,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reclassified == 7 && reclassify_last_gate == 0);

   assert(aimee_db2_record_l4_approval_call(call_client, &client, 7042, 0, 42u, "operator",
                                            "reviewed", NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(approval_calls == 1 && strcmp(approval_last_approver, "operator") == 0);

   uint32_t pruned = 99u;
   assert(aimee_db2_prune_orphaned_l0_call(call_client, &client, 7043, 0, &pruned, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(pruned == 3 && prune_orphaned_l0_calls == 1);

   uint32_t archived = 99u;
   assert(aimee_db2_lifecycle_sweep_expired_call(call_client, &client, 7044, 0, &archived, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(archived == 4 && lifecycle_sweep_calls == 1);

   uint32_t decayed = 99u;
   assert(aimee_db2_demote_id_call(call_client, &client, 7045, 0, 42u, &decayed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(decayed == 1 && demote_id_calls == 1 && demote_id_last == 42);

   uint32_t tagged = 99u;
   assert(aimee_db2_has_workspace_tag_call(call_client, &client, 7046, 0, 42u, &tagged, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(tagged == 1 && workspace_tag_calls == 1);
   assert(aimee_db2_has_workspace_tag_call(call_client, &client, 7047, 0, 43u, &tagged, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(tagged == 0 && workspace_tag_calls == 2);

   uint32_t removed = 99u;
   assert(aimee_db2_delete_row_call(call_client, &client, 7048, 0, 42u, &removed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(removed == 1 && delete_row_calls == 1);
   assert(aimee_db2_delete_row_call(call_client, &client, 7049, 0, 43u, &removed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(removed == 0 && delete_row_calls == 2);

   assert(aimee_db2_touch_call(call_client, &client, 7050, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(touch_calls == 1 && touch_last == 42);
   /* A memory the backend refuses surfaces as INTERNAL, not as a quiet ok. */
   assert(aimee_db2_touch_call(call_client, &client, 7051, 0, 43u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);

   assert(aimee_db2_link_delete_call(call_client, &client, 7052, 0, 7u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(link_delete_calls == 1 && link_delete_last == 7);
   assert(aimee_db2_link_delete_call(call_client, &client, 7053, 0, 8u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);

   uint32_t valid_result = 99u, in_force = 99u;
   assert(aimee_db2_valid_at_call(call_client, &client, 7054, 0, 42u, "2026-08-18 12:00:00",
                                  &valid_result, &in_force, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(valid_result == AIMEE_DB2_RESULT_OK && in_force == 1 && valid_at_calls == 1);
   assert(strcmp(valid_at_last, "2026-08-18 12:00:00") == 0);
   assert(aimee_db2_valid_at_call(call_client, &client, 7055, 0, 43u, "2026-08-18 12:00:00",
                                  &valid_result, &in_force, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(valid_result == AIMEE_DB2_RESULT_OK && in_force == 0);
   /* Could not evaluate is its own answer, not a negative verdict. */
   assert(aimee_db2_valid_at_call(call_client, &client, 7056, 0, 44u, "2026-08-18 12:00:00",
                                  &valid_result, &in_force, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(valid_result == AIMEE_DB2_RESULT_INVALID_STATE && in_force == 0);

   uint32_t scoped = 99u;
   assert(aimee_db2_has_scope_type_call(call_client, &client, 7057, 0, 42u, "workspace", &scoped,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(scoped == 1 && scope_type_calls == 1 && strcmp(scope_type_last, "workspace") == 0);
   assert(aimee_db2_has_scope_type_call(call_client, &client, 7058, 0, 42u, "project", &scoped,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(scoped == 0 && scope_type_calls == 2);

   assert(aimee_db2_reject_call(call_client, &client, 7059, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(reject_calls == 1 && reject_last == 42);
   assert(aimee_db2_reject_call(call_client, &client, 7060, 0, 43u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);

   uint32_t rewritten = 99u;
   assert(aimee_db2_update_content_call(call_client, &client, 7061, 0, 42u, "revised text",
                                        &rewritten, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(rewritten == 1 && update_content_calls == 1 &&
          strcmp(update_content_last, "revised text") == 0);
   assert(aimee_db2_update_content_call(call_client, &client, 7062, 0, 43u, "revised text",
                                        &rewritten, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(rewritten == 0);

   assert(aimee_db2_decay_confidence_call(call_client, &client, 7063, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(decay_confidence_calls == 1 && decay_confidence_last == 42);

   assert(aimee_db2_workspace_tag_insert_call(call_client, &client, 7064, 0, 42u, "aimee", NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(workspace_tag_insert_calls == 1 && strcmp(workspace_tag_insert_last, "aimee") == 0);

   assert(aimee_db2_set_cognified_kind_call(call_client, &client, 7065, 0, 42u, "preference", NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(cognified_kind_calls == 1 && strcmp(cognified_kind_last, "preference") == 0);

   assert(aimee_db2_set_source_session_call(call_client, &client, 7066, 0, 42u, "sess-1", NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(source_session_calls == 1 && strcmp(source_session_last, "sess-1") == 0);
   /* Clearing is a real call, and the empty value must reach the backend. */
   assert(aimee_db2_set_source_session_call(call_client, &client, 7067, 0, 42u, "", NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(source_session_calls == 2 && source_session_last[0] == '\0');

   assert(aimee_db2_negation_tokens_update_call(call_client, &client, 7068, 0, 42u, "not never",
                                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(negation_tokens_calls == 1 && strcmp(negation_tokens_last, "not never") == 0);
   /* A memory with no negations extracts to nothing, and that must store. */
   assert(aimee_db2_negation_tokens_update_call(call_client, &client, 7069, 0, 42u, "", NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(negation_tokens_calls == 2 && negation_tokens_last[0] == '\0');

   uint32_t content_result = 99u;
   static char read_back[AIMEE_DB2_GET_CONTENT_CONTENT_MAX + 1];
   assert(aimee_db2_get_content_call(call_client, &client, 7070, 0, 42u, &content_result, read_back,
                                     sizeof(read_back), NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(content_result == AIMEE_DB2_RESULT_OK && strcmp(read_back, "stored text") == 0 &&
          get_content_calls == 1);
   /* A memory that is not there is not_found, not empty content. */
   assert(aimee_db2_get_content_call(call_client, &client, 7071, 0, 43u, &content_result, read_back,
                                     sizeof(read_back), NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(content_result == AIMEE_DB2_RESULT_NOT_FOUND && read_back[0] == '\0');

   uint32_t session_result = 99u;
   char session_back[AIMEE_DB2_GET_SOURCE_SESSION_SESSION_MAX + 1];
   assert(aimee_db2_get_source_session_call(call_client, &client, 7072, 0, 42u, &session_result,
                                            session_back, sizeof(session_back), NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(session_result == AIMEE_DB2_RESULT_OK && strcmp(session_back, "sess-1") == 0 &&
          get_source_session_calls == 1);
   assert(aimee_db2_get_source_session_call(call_client, &client, 7073, 0, 43u, &session_result,
                                            session_back, sizeof(session_back), NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(session_result == AIMEE_DB2_RESULT_NOT_FOUND && session_back[0] == '\0');

   uint32_t ref_result = 99u;
   char ref_back[AIMEE_DB2_PICK_FIRST_TEMPORAL_REF_KEY_MAX + 1];
   assert(aimee_db2_pick_first_temporal_ref_call(call_client, &client, 7074, 0, 42u, &ref_result,
                                                 ref_back, sizeof(ref_back), NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(ref_result == AIMEE_DB2_RESULT_OK && strcmp(ref_back, "2026-08-19") == 0 &&
          temporal_ref_calls == 1);
   assert(aimee_db2_pick_first_temporal_ref_call(call_client, &client, 7075, 0, 43u, &ref_result,
                                                 ref_back, sizeof(ref_back), NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(ref_result == AIMEE_DB2_RESULT_NOT_FOUND && ref_back[0] == '\0');

   uint32_t corpus_result = 99u, corpus_count = 99u;
   char corpus_stamp[AIMEE_DB2_COUNT_AND_MAX_UPDATED_STAMP_MAX + 1];
   assert(aimee_db2_count_and_max_updated_call(call_client, &client, 7076, 0, &corpus_result,
                                               &corpus_count, corpus_stamp, sizeof(corpus_stamp),
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(corpus_result == AIMEE_DB2_RESULT_OK && corpus_count == 7 &&
          strcmp(corpus_stamp, "2026-08-19 09:00:00") == 0 && corpus_stat_calls == 1);

   /* First index-family call over the bus: a different event kind and stage
    * from every operation above it. */
   uint32_t edges_pruned = 99u;
   assert(aimee_db2_entity_edge_prune_orphans_call(call_client, &client, 7077, 0, &edges_pruned,
                                                   NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(edges_pruned == 2 && edge_prune_calls == 1);

   uint32_t edges_normalized = 99u;
   assert(aimee_db2_entity_edge_normalize_weights_call(call_client, &client, 7078, 0,
                                                       &edges_normalized, NULL,
                                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(edges_normalized == 3 && edge_normalize_calls == 1);

   uint32_t projects = 99u;
   assert(aimee_db2_project_count_call(call_client, &client, 7079, 0, &projects, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(projects == 4 && project_count_calls == 1);

   uint32_t purged = 99u;
   assert(aimee_db2_purge_hidden_pollution_call(call_client, &client, 7080, 0, &purged, NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(purged == 5 && purge_pollution_calls == 1);

   uint32_t requeued = 99u;
   assert(aimee_db2_requeue_drifted_call(call_client, &client, 7081, 0, &requeued, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(requeued == 6 && requeue_drifted_calls == 1);

   uint32_t route_count = 99u;
   assert(aimee_db2_cross_repo_rebuild_routes_call(call_client, &client, 7090, 0, &route_count,
                                                   NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(route_count == 15 && rebuild_routes_calls == 1);

   uint32_t identities_written = 99u;
   assert(aimee_db2_cross_repo_rebuild_identities_call(call_client, &client, 7091, 0,
                                                       &identities_written, NULL,
                                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(identities_written == 16 && rebuild_identities_calls == 1);

   uint32_t build_deps_written = 99u;
   assert(aimee_db2_cross_repo_rebuild_build_deps_call(call_client, &client, 7092, 0,
                                                       &build_deps_written, NULL,
                                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(build_deps_written == 17 && rebuild_build_deps_calls == 1);

   uint64_t drift = 99u;
   assert(aimee_db2_drift_candidates_call(call_client, &client, 7100, 0, &drift, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(drift == 20 && drift_candidates_calls == 1);

   /* The learning family's first crossing of the bus: a new event kind
    * and a new stage, not another operation on one already carrying work. */
   uint32_t rules_touched = 99u;
   assert(aimee_db2_rules_decay_call(call_client, &client, 7093, 0, &rules_touched, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(rules_touched == 18 && rules_decay_calls == 1);

   uint32_t items_rescored = 99u;
   assert(aimee_db2_curiosity_rescore_all_call(call_client, &client, 7094, 0, &items_rescored, NULL,
                                               NULL) == AIMEE_MODULE_CALL_OK);
   assert(items_rescored == 19 && curiosity_rescore_calls == 1);

   /* An acknowledgement-only call: it returns no out-parameter at all. */
   assert(aimee_db2_mining_seed_job_defaults_call(call_client, &client, 7095, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(mining_seed_calls == 1);

   assert(aimee_db2_proposals_archive_expired_call(call_client, &client, 7097, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(proposals_archive_calls == 1);

   uint64_t watermark = 99u;
   assert(aimee_db2_trace_mining_last_id_call(call_client, &client, 7102, 0, &watermark, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(watermark == 22 && trace_watermark_calls == 1);

   /* The organization family's first crossing of the bus. */
   assert(aimee_db2_rel_types_ensure_seed_call(call_client, &client, 7096, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(rel_types_seed_calls == 1);

   /* The custody family's first crossing of the bus, and the first pair
    * of operations that are meant to be used together. */
   uint32_t acquired = 99u;
   assert(aimee_db2_vector_rebuild_lock_try_acquire_call(call_client, &client, 7098, 0, &acquired,
                                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(acquired == 1 && lock_acquire_calls == 1);
   assert(aimee_db2_vector_rebuild_lock_release_call(call_client, &client, 7099, 0, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(lock_release_calls == 1);

   uint64_t release_id = 99u;
   assert(aimee_db2_release_get_active_call(call_client, &client, 7101, 0, &release_id, NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(release_id == 21 && release_active_calls == 1);

   /* The maintenance family's first crossing of the bus: a new event kind
    * and a new stage, not another operation on one already carrying work. */
   uint32_t expired = 99u;
   assert(aimee_db2_prospective_sweep_expired_call(call_client, &client, 7082, 0, &expired, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(expired == 7 && prospective_sweep_calls == 1);

   uint32_t directives = 99u;
   assert(aimee_db2_directive_sweep_expired_call(call_client, &client, 7083, 0, &directives, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(directives == 8 && directive_sweep_calls == 1);

   /* The first operations on this bus that carry an argument to a
    * maintenance stage: the id has to arrive intact, not merely arrive. */
   assert(aimee_db2_directive_suppress_call(call_client, &client, 7103, 0, 31, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(directive_suppress_calls == 1 && directive_suppress_id == 31);
   assert(aimee_db2_directive_record_surface_call(call_client, &client, 7104, 0, 32, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(directive_surface_calls == 1 && directive_surface_id == 32);

   assert(aimee_db2_anti_pattern_bump_call(call_client, &client, 7105, 0, 41, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(anti_pattern_bump_calls == 1 && anti_pattern_bump_seen == 41);

   assert(aimee_db2_anti_pattern_delete_call(call_client, &client, 7106, 0, 42, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(anti_pattern_delete_calls == 1 && anti_pattern_delete_seen == 42);

   assert(aimee_db2_doc_delete_call(call_client, &client, 7107, 0, 43, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(doc_delete_calls == 1 && doc_delete_seen == 43);

   assert(aimee_db2_task_delete_call(call_client, &client, 7108, 0, 44, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(task_delete_calls == 1 && task_delete_seen == 44);

   /* The project name has to survive the round trip, not merely be sent. */
   uint32_t file_index_delete_project_count = 99u;
   assert(aimee_db2_file_index_delete_project_call(call_client, &client, 7109, 0, "demo",
                                                   &file_index_delete_project_count, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(file_index_delete_project_count == 51 && file_index_delete_project_calls == 1);
   assert(strcmp(file_index_delete_project_seen, "demo") == 0);

   uint32_t clear_project_count = 99u;
   assert(aimee_db2_clear_project_call(call_client, &client, 7110, 0, "demo", &clear_project_count,
                                       NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(clear_project_count == 52 && clear_project_calls == 1);
   assert(strcmp(clear_project_seen, "demo") == 0);

   uint32_t clear_current_project_count = 99u;
   assert(aimee_db2_clear_current_project_call(call_client, &client, 7111, 0, "demo",
                                               &clear_current_project_count, NULL,
                                               NULL) == AIMEE_MODULE_CALL_OK);
   assert(clear_current_project_count == 53 && clear_current_project_calls == 1);
   assert(strcmp(clear_current_project_seen, "demo") == 0);

   uint32_t marked = 99u;
   assert(aimee_db2_mark_revisit_due_call(call_client, &client, 7084, 0, &marked, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(marked == 9 && mark_revisit_calls == 1);

   uint32_t requeued_rows = 99u;
   assert(aimee_db2_ingest_queue_reset_running_call(call_client, &client, 7085, 0, &requeued_rows,
                                                    NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(requeued_rows == 10 && queue_reset_calls == 1);

   uint32_t evidence_rows = 99u;
   assert(aimee_db2_evidence_reembed_all_call(call_client, &client, 7086, 0, &evidence_rows, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(evidence_rows == 11 && evidence_reembed_calls == 1);

   uint32_t demoted_artifacts = 99u;
   assert(aimee_db2_curator_reembed_all_call(call_client, &client, 7087, 0, &demoted_artifacts,
                                             NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(demoted_artifacts == 12 && curator_reembed_calls == 1);

   uint32_t reenqueued_ops = 99u;
   assert(aimee_db2_synth_reenqueue_all_call(call_client, &client, 7088, 0, &reenqueued_ops, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(reenqueued_ops == 13 && synth_reenqueue_calls == 1);

   uint32_t extract_jobs = 99u;
   assert(aimee_db2_curator_reenqueue_extract_all_call(call_client, &client, 7089, 0, &extract_jobs,
                                                       NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(extract_jobs == 14 && extract_reenqueue_calls == 1);

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

   aimee_db2_reembed_status_t reembed = {0};
   domain_result = 9;
   assert(aimee_db2_reembed_status_call(call_client, &client, 7014, 0, &domain_result, &reembed,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && reembed.target_dimension == 384 &&
          reembed.started_epoch == 1700000000);

   domain_result = 9;
   assert(aimee_db2_reembed_clear_call(call_client, &client, 7015, 0, &domain_result, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK);

   aimee_db2_reembed_clear_maintenance_t maintenance = {0};
   domain_result = 9;
   assert(aimee_db2_reembed_clear_maintenance_call(call_client, &client, 7016, 0, 1, &domain_result,
                                                   &maintenance, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && maintenance.was_in_progress == 1 &&
          maintenance.recorded_dimension == 384 && maintenance.running_dimension == 384);

   char serving_id[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1] = {0};
   domain_result = 9;
   assert(aimee_db2_embedder_serving_id_call(call_client, &client, 7017, 0, &domain_result,
                                             serving_id, sizeof(serving_id), NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK &&
          strlen(serving_id) == AIMEE_DB2_EMBEDDER_SERVING_ID_MAX);
   for (size_t index = 0; index < strlen(serving_id); ++index)
      assert(serving_id[index] == 'x');

   aimee_db2_dimension_reset_t reset = {0};
   domain_result = 9;
   assert(aimee_db2_dimension_reset_call(call_client, &client, 7018, 0, 384, 0, 1, &domain_result,
                                         &reset, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && reset.recorded_dimension == 768 &&
          reset.target_dimension == 384 && reset.tables_discovered == 6 &&
          reset.tables_dropped == 0 && reset.rows_cleared == 1234 && reset.curator_requeued == -1 &&
          reset.evidence_requeued == 7);

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
   assert(initialized_calls == 3 && health_calls == 3 && kb_health_calls == 3);

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
