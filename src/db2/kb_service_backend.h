#ifndef DEC_DB2_KB_SERVICE_BACKEND_H
#define DEC_DB2_KB_SERVICE_BACKEND_H 1

#include "vector_index_ops.h"
#include "cJSON.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      db2_vector_index_ops_summary_t ops;
      db2_vector_index_op_failed_t failed_detail[20];
      int failed_detail_count;
      char stored_schema_ver[64];
      int rebuild_lock_held;
   } db2_kb_service_memory_verify_t;

   typedef struct
   {
      int64_t mem_rows;
      int64_t unit_rows;
      int64_t kb_rows;
      char active_ver[256];
   } db2_kb_service_verify_snapshot_t;

   typedef struct
   {
      char target_version[256];
      int last_id;
      int total;
      int done;
      char started_at[64];
      char finished_at[64];
      int have_job;
   } db2_kb_service_reembed_status_t;

   typedef struct
   {
      int total_count;
      int resume_last_id;
   } db2_kb_service_reembed_start_t;

   typedef struct
   {
      int pending;
      int running;
      int done;
      int failed;
      int total;
      int processed;
   } db2_kb_service_async_queue_stats_t;

   typedef struct
   {
      int64_t id;
      int64_t document_id;
      char kind[64];
      char project[256];
      char status[32];
      int attempts;
      char last_error[512];
      char claimed_by[128];
      char claimed_at[64];
      char created_at[64];
      char updated_at[64];
   } db2_kb_service_async_job_t;

   typedef struct
   {
      char project[256];
      int files;
      int chunks;
      int tokens;
      int embeddings;
      db2_kb_service_async_queue_stats_t queue;
   } db2_kb_service_project_status_t;

   int db2_kb_service_reset_stuck_vector_ops(int max_attempts);

   int db2_kb_service_collect_memory_verify(int include_failed_detail, int max_attempts,
                                            db2_kb_service_memory_verify_t *out);
   int db2_kb_service_collect_verify_snapshot(db2_kb_service_verify_snapshot_t *out);
   int db2_kb_service_get_active_embedder_version(char *out, size_t out_len);
   int db2_kb_service_set_active_embedder_version(const char *version, const char *updated_at);
   int db2_kb_service_collect_reembed_status(db2_kb_service_reembed_status_t *out);
   int db2_kb_service_mark_reembed_finished(const char *finished_at);
   int db2_kb_service_prepare_reembed_start(const char *version, const char *started_at,
                                            db2_kb_service_reembed_start_t *out);
   int db2_kb_service_update_reembed_progress(int last_id, int done);
   int db2_kb_service_list_unembedded_memory_ids(const char *version, int64_t *ids, int max_ids);
   int db2_kb_service_list_pending_reembed_memory_ids(const char *version, int resume_last_id,
                                                      int64_t *ids, int max_ids);
   int db2_kb_service_count_embeddings_for_version(const char *version);
   int db2_kb_service_list_memory_ids_by_updated(int limit, int64_t *ids, int max_ids);
   int db2_kb_service_memory_record_exists(int64_t record_id);
   int db2_kb_service_kb_document_exists(int64_t document_id);
   int db2_kb_service_directive_create(const char *question, const char *topic,
                                       const char *anchor_entity, const char *anchor_file,
                                       const char *cause, int priority, int64_t memory_a_id,
                                       int64_t memory_b_id, const char *evidence,
                                       const char *source_session, const char *valid_until,
                                       int *dedup_out, cJSON **directive_out);
   int db2_kb_service_directive_resolve(int64_t id, int64_t resolution_memory_id, const char *note);
   int db2_kb_service_directive_suppress(int64_t id);
   int db2_kb_service_directive_sweep_expired(void);
   cJSON *db2_kb_service_directive_list_json(const char *state, const char *cause, int max_rows);
   /* Graph-derived code-health audit: dead exports, import cycles, clones. */
   int db2_code_audit_edge_target_like(const char *relation, const char *project, char *out,
                                       size_t cap);
   cJSON *db2_kb_service_code_audit_json(const char *project, int limit);
   cJSON *db2_kb_service_curiosity_list_json(const char *state, int max_rows);
   cJSON *db2_kb_service_curiosity_create_json(const char *gap_type, const char *target_entity,
                                               const char *target_topic, const char *evidence,
                                               double importance, double novelty,
                                               const char *source_session);
   cJSON *db2_kb_service_curiosity_sweep_json(void);
   cJSON *db2_kb_service_curiosity_rescore_json(void);
   cJSON *db2_kb_service_curiosity_get_json(int64_t id);
   cJSON *db2_kb_service_curiosity_update_state_json(int64_t id, const char *new_state);
   /* Pull the top-N open curiosity items, route each into an epistemic
    * directive, and transition the source items to in_progress. Returns
    * {"status":"ok","routed":N}. Implements the same logic as the
    * server-side curiosity_route_top so callers can ship one RPC
    * instead of orchestrating multiple round-trips. */
   cJSON *db2_kb_service_curiosity_route_top_json(int limit, const char *source_session);
   cJSON *db2_kb_service_note_create_json(const char *title, const char *content, const char *tags,
                                          const char *author);
   cJSON *db2_kb_service_note_list_json(const char *tag, int max_rows);
   cJSON *db2_kb_service_note_search_json(const char *query, int max_rows);
   cJSON *db2_kb_service_rules_list_json(int max_rules);
   cJSON *db2_kb_service_rules_generate_json(void);
   cJSON *db2_kb_service_rules_export_jsonl_json(const char *path);
   cJSON *db2_kb_service_rules_insert_json(const char *polarity, const char *title,
                                           const char *description, int weight);
   cJSON *db2_kb_service_tool_registry_snapshot_json(void);
   cJSON *db2_kb_service_tool_registry_lookup_json(const char *name);
   cJSON *db2_kb_service_relations_schema_list_json(void);
   cJSON *db2_kb_service_memory_export_jsonl_json(const char *path);
   cJSON *db2_kb_service_memory_decisions_export_jsonl_json(const char *path);
   cJSON *db2_kb_service_memory_key_exists_json(const char *key);
   cJSON *db2_kb_service_collab_rules_propose_json(const char *text, const char *reason,
                                                   const char *proposed_by);
   cJSON *db2_kb_service_collab_rules_list_json(void);
   cJSON *db2_kb_service_collab_rules_list_active_json(void);
   cJSON *db2_kb_service_collab_rules_approve_json(int rule_id);
   cJSON *db2_kb_service_collab_rules_reject_json(int rule_id);
   cJSON *db2_kb_service_collab_rules_retire_json(int rule_id);
   cJSON *db2_kb_service_collab_rules_inject_json(int agent_last_epoch);
   /* Run the learning-router record-signal pipeline against the
    * fields in |req|.  Returns {"status":"ok","dispatch":{...}} or
    * {"status":"error","message":"..."}.  Caller frees. */
   cJSON *db2_kb_service_learning_propose_signal_json(const cJSON *req);

   cJSON *db2_kb_service_agent_outcome_record_json(const char *agent_name, const char *role,
                                                   const char *outcome_kind, const char *reason,
                                                   int turns_used, int tools_called,
                                                   int64_t tokens_used,
                                                   const char *tool_error_pattern);
   cJSON *db2_kb_service_agent_hint_consume_json(const char *role, const char *prompt);
   cJSON *db2_kb_service_memory_find_id_by_key_kind_json(const char *key, const char *kind);
   cJSON *db2_kb_service_memory_search_facts_patterns_by_keyword_json(const char *keyword, int max);
   cJSON *db2_kb_service_task_list_json(const char *state, const char *session_id, int limit);
   cJSON *db2_kb_service_task_create_json(const char *title, const char *session_id,
                                          int64_t parent_id);
   cJSON *db2_kb_service_task_update_state_json(int64_t id, const char *state);
   cJSON *db2_kb_service_task_delete_json(int64_t id);
   cJSON *db2_kb_service_task_add_edge_json(int64_t source, int64_t target, const char *relation);
   cJSON *db2_kb_service_task_get_edges_json(int64_t task_id, int max);
   /* Wrappers around memory_advanced.c maintenance routines so daemon
    * and CLI-fork callers run them inside aimee-kb where DB2 is
    * initialized.  Each returns {"status":"ok","count":N}. */
   cJSON *db2_kb_service_anti_pattern_extract_from_feedback_json(void);
   cJSON *db2_kb_service_anti_pattern_extract_from_failures_json(void);
   cJSON *db2_kb_service_anti_pattern_escalate_json(int hit_threshold);
   cJSON *db2_kb_service_rules_decay_json(void);
   cJSON *db2_kb_service_memory_learn_style_json(void);
   cJSON *db2_kb_service_decision_log_insert_json(int64_t task_id, const char *options,
                                                  const char *chosen, const char *rationale,
                                                  const char *assumptions);
   cJSON *db2_kb_service_decision_log_list_json(const char *outcome, int limit);
   cJSON *db2_kb_service_anti_pattern_list_json(int max);
   cJSON *db2_kb_service_anti_pattern_insert_json(const char *pattern, const char *description,
                                                  const char *source, const char *source_ref,
                                                  double confidence);
   cJSON *db2_kb_service_anti_pattern_delete_json(int64_t id);
   cJSON *db2_kb_service_anti_pattern_check_json(const char *file_path, const char *command,
                                                 int max);
   cJSON *db2_kb_service_anti_pattern_bump_json(int64_t id);
   cJSON *db2_kb_service_memory_fold_session_json(const char *session_id);
   cJSON *db2_kb_service_rules_delete_json(int id);
   cJSON *db2_kb_service_rules_update_directive_type_json(int id, const char *directive_type);
   cJSON *db2_kb_service_feedback_record_json(const char *polarity, const char *title,
                                              const char *description, int weight);
   cJSON *db2_kb_service_memory_supersede_json(int64_t old_id, const char *new_content,
                                               double confidence, const char *session_id);
   cJSON *db2_kb_service_memory_fact_history_json(const char *key, int max);
   cJSON *db2_kb_service_memory_check_drift_json(int64_t task_id, const char *file_path,
                                                 const char *command);
   cJSON *db2_kb_service_memory_list_session_scope_priority_json(int max);
   cJSON *db2_kb_service_memory_list_session_scope_priority_like_json(const char *pattern, int max);
   cJSON *db2_kb_service_memory_list_low_effectiveness_json(double threshold, int limit);
   cJSON *db2_kb_service_memory_list_unused_l2_json(int days, int max);
   cJSON *db2_kb_service_memory_list_superseded_keys_json(int min_versions, int max);
   cJSON *db2_kb_service_memory_set_artifact_json(int64_t memory_id, const char *artifact_type,
                                                  const char *artifact_ref,
                                                  const char *artifact_hash);
   cJSON *db2_kb_service_directive_expire_session_json(void);
   cJSON *db2_kb_service_memory_scan_conversations_json(const cJSON *dirs);
   /* Dashboard endpoints that walk DB2 tables.  Each returns
    * {"status":"ok","payload":<api_* output>}. */
   cJSON *db2_kb_service_dashboard_memory_stats_json(void);
   cJSON *db2_kb_service_dashboard_logs_json(void);
   cJSON *db2_kb_service_dashboard_reminders_json(void);
   cJSON *db2_kb_service_dashboard_recall_json(void);
   cJSON *db2_kb_service_dashboard_directives_json(void);
   cJSON *db2_kb_service_memory_find_facts_json(const char *query, int limit);
   cJSON *db2_kb_service_memory_list_json(const char *tier, const char *kind, int limit);
   cJSON *db2_kb_service_memory_get_json(int64_t id);
   cJSON *db2_kb_service_memory_load_eval_corpus_json(int max);
   cJSON *db2_kb_service_memory_top_l2_facts_json(int max);
   cJSON *db2_kb_service_session_briefing_commitments_json(int limit);
   cJSON *db2_kb_service_session_briefing_directives_json(int limit);
   cJSON *db2_kb_service_memory_prospective_list_json(const char *state, int max);
   cJSON *
   db2_kb_service_memory_prospective_create_json(const char *trigger_text, const char *action_text,
                                                 const char *anchor_entity, const char *anchor_file,
                                                 const char *recurrence, const char *valid_until);
   cJSON *db2_kb_service_memory_prospective_complete_json(int64_t id);
   cJSON *db2_kb_service_memory_prospective_match_json(const char *turn_text,
                                                       const char *active_entity,
                                                       const char *active_file, int max);
   cJSON *db2_kb_service_memory_get_provenance_json(int64_t memory_id, int max);
   cJSON *db2_kb_service_memory_scope_visibility_rank_json(const int64_t *ids, int id_count,
                                                           const char *workspace,
                                                           const char *project);
   cJSON *db2_kb_service_memory_episode_card_generate_json(const char *source_session);
   cJSON *db2_kb_service_memory_tag_workspace_json(int64_t memory_id, const char *workspace);
   cJSON *db2_kb_service_memory_tag_scope_json(int64_t memory_id, const char *scope_type,
                                               const char *scope_value);
   cJSON *db2_kb_service_memory_prospective_mark_triggered_json(int64_t id);
   cJSON *db2_kb_service_memory_prospective_sweep_expired_json(void);
   cJSON *db2_kb_service_memory_maintenance_run_json(unsigned int modes, int force, int dry_run);
   cJSON *db2_kb_service_memory_alerts_json(const char *since);
   cJSON *db2_kb_service_memory_recall_json(const char *task_hint, int limit_tokens,
                                            int session_start);
   cJSON *db2_kb_service_memory_upsert_workflow_json(const char *workspace, const char *signal_type,
                                                     const char *rule, double observed_confidence,
                                                     const char *session_id);
   cJSON *db2_kb_service_memory_delete_json(int64_t id);
   cJSON *db2_kb_service_memory_touch_json(int64_t id);
   cJSON *db2_kb_service_memory_update_json(int64_t id, const char *content);
   cJSON *db2_kb_service_memory_reject_json(int64_t id, const char *reason);
   cJSON *db2_kb_service_memory_stats_json(void);
   cJSON *db2_kb_service_memory_list_conflicts_json(int max);
   cJSON *db2_kb_service_memory_query_health_json(void);
   cJSON *db2_kb_service_memory_effectiveness_stats_json(void);
   cJSON *db2_kb_service_memory_query_edges_json(const char *entity, int max);
   cJSON *db2_kb_service_memory_compact_windows_json(void);
   cJSON *db2_kb_service_memory_assemble_context_json(const char *task_hint);
   cJSON *db2_kb_service_memory_search_json(const cJSON *clusters_arr, int limit);
   cJSON *db2_kb_service_memory_find_facts_visible_json(const char *query, const char *workspace,
                                                        const char *project, int limit);
   cJSON *db2_kb_service_memory_find_facts_scoped_json(const char *query, const char *scope_type,
                                                       const char *scope_value, int limit);
   cJSON *db2_kb_service_memory_diagnose_scoped_json(const char *query, const char *scope_type,
                                                     const char *scope_value, int limit);
   cJSON *db2_kb_service_memory_explain_match_json(const char *query, int64_t memory_id);
   cJSON *db2_kb_service_memory_link_create_json(int64_t source_id, int64_t target_id,
                                                 const char *relation);
   cJSON *db2_kb_service_memory_link_query_json(int64_t memory_id, int max);
   cJSON *db2_kb_service_memory_link_delete_json(int64_t link_id);
   cJSON *db2_kb_service_memory_insert_json(const char *tier, const char *kind, const char *key,
                                            const char *content, double confidence,
                                            const char *session_id);
   cJSON *db2_kb_service_memory_insert_ex_json(const char *tier, const char *kind, const char *key,
                                               const char *content, const char *use_cases,
                                               double confidence, const char *session_id);
   cJSON *db2_kb_service_memory_briefing_json(int limit_tokens);
   cJSON *db2_kb_service_memory_context_block_json(const char *query, const char *block_type,
                                                   int limit);
   cJSON *db2_kb_service_memory_entity_profile_json(const char *entity);
   cJSON *db2_kb_service_memory_entity_edges_json(const char *entity, int limit);
   cJSON *db2_kb_service_memory_search_graph_json(const char *query, int limit);
   cJSON *db2_kb_service_memory_search_graph_as_of_json(const char *query, const char *as_of,
                                                        int limit);
   cJSON *db2_kb_service_memory_get_episode_json(const char *episode_key);
   cJSON *db2_kb_service_memory_ask_json(const char *query, const char *scope_type,
                                         const char *scope_value, int limit);
   typedef struct
   {
      int64_t id;
      char project[256];
      char root_path[4096];
      char workspace[256];
      int force;
   } db2_kb_ingest_job_t;

   typedef struct
   {
      int pending;
      int running;
      int done_last_24h;
      int failed_last_24h;
   } db2_kb_ingest_queue_stats_t;

   typedef struct
   {
      char project[256];
      char status[32];
      char completed_at[64]; /* COALESCE(completed_at, started_at) — always populated */
      int files_indexed;
      int chunks_added;
      char error_message[512];
   } db2_kb_ingest_recent_t;

   int db2_kb_ingest_queue_reset_running(void);
   int db2_kb_ingest_queue_enqueue(const char *project, const char *root_path,
                                   const char *workspace, int force);
   int db2_kb_ingest_queue_claim_next(db2_kb_ingest_job_t *out);
   int db2_kb_ingest_queue_complete(int64_t job_id, int files_indexed, int chunks_added,
                                    int embeddings_added);
   int db2_kb_ingest_queue_fail(int64_t job_id, const char *error_message);
   int db2_kb_ingest_queue_stats(db2_kb_ingest_queue_stats_t *out);
   int db2_kb_ingest_queue_recent(db2_kb_ingest_recent_t *rows, int max_rows);

   int db2_kb_file_index_upsert(const char *project, const char *file_path, const char *file_hash,
                                const char *content);
   int db2_kb_file_index_get(const char *project, const char *file_path, char *hash_out,
                             size_t hash_cap, char *ingested_at_out, size_t ingested_at_cap);
   char *db2_kb_file_index_get_content(const char *project, const char *file_path);
   int db2_kb_file_index_delete_project(const char *project);
   cJSON *db2_kb_file_index_snapshot_json(const char *project);

   int db2_kb_service_async_queue_status(db2_kb_service_async_queue_stats_t *out);
   int db2_kb_service_async_job_get(int64_t job_id, db2_kb_service_async_job_t *out);
   int db2_kb_service_async_queue_spawn_worker(void);
   typedef int (*db2_kb_service_vector_upsert_fn)(int64_t document_id, const float *vec, int dim,
                                                  const char *payload_json, void *ctx);
   int db2_kb_service_async_queue_drain(const char *embedding_cmd, int timeout_secs,
                                        const char *vector_collection,
                                        db2_kb_service_vector_upsert_fn vector_upsert,
                                        void *vector_upsert_ctx,
                                        db2_kb_service_async_queue_stats_t *out);
   int db2_kb_service_collect_project_status(const char *project,
                                             db2_kb_service_project_status_t *out);
   int db2_kb_service_clear_project(const char *project);
   cJSON *db2_kb_service_learning_list_json(const char *state, const char *sink, int max_rows);
   cJSON *db2_kb_service_learning_get_json(int id);
   cJSON *db2_kb_service_learning_reject_json(int id);

   cJSON *db2_kb_service_scene_list_json(int max_rows);
   cJSON *db2_kb_service_scene_members_json(int64_t scene_id, int max_rows);

   cJSON *db2_kb_service_memory_lint_json(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_KB_SERVICE_BACKEND_H */
