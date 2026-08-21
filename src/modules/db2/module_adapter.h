#ifndef AIMEE_DB2_MODULE_ADAPTER_H
#define AIMEE_DB2_MODULE_ADAPTER_H 1

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/db2/module_api.h>

/* The vtable binds backends directly, so it names the row types they fill.
 * aimee.h and memory.h come first because the DB2 headers that declare those
 * rows are written to be included after them -- entity_edges.h uses edge_t,
 * which lives in memory.h, which in turn needs aimee.h. Naming the real types is what keeps this
 * header and the backends from drifting: a copy of a row struct here would be a second definition
 * to keep in step, which is the defect the hosted bus test had. */
#include "aimee.h"
#include "memory.h"

#include "code_projection.h"
#include "corpus_jobs.h"
#include "entity_edges.h"
#include "bandit.h"
#include "entity_nodes.h"
#include "kb_service_backend.h"
#include "memory_relations.h"
#include "tasks.h"
#include "memory_query.h"
#include "memory_health.h"
#include "memory_promotion.h"
#include "memory_scope_query.h"
#include "lifecycle.h"
#include "decision_log.h"
#include "memory_lint.h"
#include "typed_facts.h"
#include "memory_briefing.h"

typedef struct
{
   int (*is_initialized)(void);
   int (*health_probe)(int *schema_ok, int *have_pg_trgm);
   int (*kb_health_probe)(int *kb_tables_ok);
   int (*embedding_dimension)(void);
   int (*level3_count)(void);
   int (*level2_count)(void);
   int (*orphaned_l0_count)(void);
   int64_t (*total_count)(void);
   int (*session_l2_count)(const char *source_session);
   int (*key_exists)(const char *key);
   int64_t (*find_id_by_key_kind)(const char *key, const char *kind);
   int (*key_exists_in_tier_pair)(const char *key, const char *tier_a, const char *tier_b);
   int (*clear_effectiveness)(int64_t memory_id);
   int (*set_effectiveness)(int64_t memory_id, double value);
   int (*retention_delete)(const char *sensitivity, int days);
   int (*demote_effectiveness)(double threshold);
   int (*effectiveness_stats)(double low_threshold, double *avg_effectiveness,
                              int *low_effectiveness, int *high_impact);
   int (*list_l2_memory_ids)(int64_t *out, int max);
   /* These two return identifiers rather than rows: the generated client sizes
    * its reply buffer as a local array, and sixty-four memory rows do not
    * belong on a caller's stack. The scope travels because the backend reads
    * it from a thread-local that does not follow a call between processes. */
   int (*top_l2_facts)(int scope_active, int include_all, const char *workspace,
                       const char *project, int64_t *out, int max);
   int (*list_session_scope_priority)(int scope_active, int include_all, const char *workspace,
                                      const char *project, int64_t *out, int max);
   /* The six term probes share one shape: one search term, one limit, the
    * session scope, and identifiers back for the reason above. */
   int (*collect_alias_matches)(const char *term, int limit, int scope_active, int include_all,
                                const char *workspace, const char *project, int64_t *out, int max);
   int (*collect_entity_matches)(const char *term, int limit, int scope_active, int include_all,
                                 const char *workspace, const char *project, int64_t *out, int max);
   int (*collect_event_frame_matches)(const char *term, int limit, int scope_active,
                                      int include_all, const char *workspace, const char *project,
                                      int64_t *out, int max);
   int (*collect_relation_token_matches)(const char *term, int limit, int scope_active,
                                         int include_all, const char *workspace,
                                         const char *project, int64_t *out, int max);
   int (*collect_summary_matches)(const char *term, int limit, int scope_active, int include_all,
                                  const char *workspace, const char *project, int64_t *out,
                                  int max);
   int (*collect_temporal_matches)(const char *term, int limit, int scope_active, int include_all,
                                   const char *workspace, const char *project, int64_t *out,
                                   int max);
   int (*find_facts_like)(const char *term, int limit, int scope_active, int include_all,
                          const char *workspace, const char *project, int64_t *out, int max);
   int (*list_session_scope_priority_like)(const char *term, int limit, int scope_active,
                                           int include_all, const char *workspace,
                                           const char *project, int64_t *out, int max);
   int (*negation_fts_search)(const char *term, int limit, int scope_active, int include_all,
                              const char *workspace, const char *project, int64_t *out, int max);
   int (*search_facts_patterns_by_keyword)(const char *term, int limit, int scope_active,
                                           int include_all, const char *workspace,
                                           const char *project, int64_t *out, int max);
   /* Unscoped: a fact's history is its history whatever the session. */
   int (*fact_history)(const char *normalized_key, int limit, int64_t *out, int max);
   int (*list_rows)(const char *tier, const char *kind, int hide_archived, int limit,
                    int scope_active, int include_all, const char *workspace, const char *project,
                    int64_t *out, int max);
   /* The aggregate reports truncation separately: a full list and a truncated
    * one are the same list. */
   int (*aggregate)(const char *entity_seed, const char *keyword, int limit, int *truncated_out,
                    int64_t *out, int max);
   /* The corpus reports which of its three plans answered. */
   int (*load_eval_corpus)(int limit, char *label_out, size_t label_len, int64_t *out, int max);
   /* record_exists answers for two tables at once and never says which. */
   int (*record_exists)(int64_t record_id);
   int (*document_exists)(int64_t document_id);
   int (*trace_mining_record)(int64_t last_trace_id);
   int (*anti_pattern_exists_exact)(const char *pattern);
   int (*anti_pattern_exists_by_source_ref)(const char *source_ref);
   int (*artifact_citation_count)(const char *artifact_id);
   int (*commits_in_last_7_days)(const char *sink);
   int (*entity_observation_count)(const char *entity_id);
   int (*fidelity_attribution_count)(const char *turn_id);
   int (*blob_referenced)(const char *blob_ref);
   int (*async_pending_count)(const char *kind);
   int (*artifact_stamp_reflected)(const char *artifact_id);
   int (*failed_query_bump)(const char *query_norm);
   int (*fence_active)(const char *project);
   int (*runtime_state_touch)(const char *state_key);
   int (*synth_enqueue)(const char *artifact_id);
   int (*synth_mark_done)(const char *artifact_id);
   int (*reembed_mark_finished)(const char *finished_at);
   int (*mining_job_try_lock)(const char *job_id);
   int (*artifact_set_state)(const char *state, const char *artifact_id);
   int (*artifact_register_exemplar)(const char *artifact_id, const char *collection);
   int (*evidence_enqueue)(const char *artifact_id, const char *collection);
   int (*evidence_mark_failed)(const char *artifact_id, const char *last_error);
   int (*synth_mark_failed)(const char *artifact_id, const char *last_error);
   int (*runtime_state_set)(const char *state_key, const char *state_value);
   int (*set_active_embedder_version)(const char *version, const char *updated_at);
   int (*entity_profile_fresh)(const char *entity_id, const char *window);
   int (*doc_exists_by_hash)(const char *content_hash, const char *scope);
   int (*pdf_quarantine_confirm)(const char *project, const char *file_path);
   int (*pdf_quarantine_reject)(const char *project, const char *file_path);
   int (*enrollment_active)(const char *cert_issuer, const char *cert_serial_norm);
   /* The described format's operations carry their own shapes, so each is
    * bound by the signature its schema implies rather than a shared one. */
   int (*runtime_state_get)(const char *state_key, char *state_value, size_t capacity);
   /* The session walks take no scope: the session identifier is the filter. */
   int (*session_neighbors_before)(const char *session_id, int64_t anchor_id, int limit,
                                   int64_t *out, int max);
   int (*session_neighbors_after)(const char *session_id, int64_t anchor_id, int limit,
                                  int64_t *out, int max);
   /* The only two backends whose whole row crosses. Zero on found, non-zero
    * for both an absent row and a statement that did not run. */
   int (*row_get)(int64_t memory_id, aimee_db2_memory_row_t *row);
   int (*row_get_by_unit_id)(int64_t unit_id, aimee_db2_memory_row_t *row);
   /* health_record composes these three: DB2 owns the corpus total and the
    * fixed conflict window, so only the cycle counters cross the bus. */
   int (*count_memories)(void);
   int (*count_recent_conflicts)(int days);
   void (*health_record)(int total_memories, int contradictions_detected, int promotions,
                         int demotions, int expirations);
   /* health_retention runs both halves; neither is reachable on its own. */
   int (*prune_health)(int days);
   int (*prune_contradictions)(int days);
   int (*health_counters)(int promote_use_count, double promote_confidence,
                          aimee_db2_health_counters_t *counters);
   int (*stats_counts)(aimee_db2_memory_stats_t *stats);
   /* expire composes these: DB2 owns the kind set and each kind's idle window,
    * and pairs every row delete with its provenance delete. */
   int (*delete_l0_provenance)(void);
   int (*delete_l0)(void);
   int (*list_kinds_in_tier)(const char *tier, char (*kinds)[16], int max);
   int (*kind_expire_days)(const char *kind);
   int (*delete_stale_l1_provenance)(const char *kind, const char *days_neg);
   int (*delete_stale_l1)(const char *kind, const char *days_neg);
   /* demote composes these. The stamp is issued once so the cascade matches
    * exactly the rows this action demoted. */
   void (*now_utc)(char *buf, size_t len);
   int (*kind_demote_policy)(const char *kind, double *confidence, int *days);
   int (*demote_kind)(const char *ts, const char *kind, double confidence, const char *days_neg);
   int (*demote_cascade)(const char *ts);
   int (*promote_stable)(const char *ts);
   int (*reclassify_directives)(int require_approval);
   int (*record_l4_approval)(int64_t memory_id, const char *approver, const char *note);
   int (*prune_orphaned_l0)(void);
   int (*lifecycle_sweep_expired)(void);
   int (*demote_id)(int64_t memory_id);
   int (*has_workspace_tag)(int64_t memory_id);
   int (*delete_row)(int64_t memory_id);
   int (*touch)(int64_t memory_id);
   int (*link_delete)(int64_t link_id);
   int (*valid_at)(int64_t memory_id, const char *as_of);
   int (*has_scope_type)(int64_t memory_id, const char *scope_type);
   int (*reject)(int64_t memory_id);
   int (*update_content)(int64_t memory_id, const char *content);
   void (*decay_confidence)(int64_t memory_id);
   void (*workspace_tag_insert)(int64_t memory_id, const char *workspace);
   void (*set_cognified_kind)(int64_t memory_id, const char *kind);
   void (*set_source_session)(int64_t memory_id, const char *session_id);
   void (*negation_tokens_update)(int64_t memory_id, const char *tokens);
   int (*get_content)(int64_t memory_id, char *out, int out_len);
   int (*get_source_session)(int64_t memory_id, char *out, int out_len);
   int (*pick_first_temporal_ref)(int64_t memory_id, char *out, int out_len);
   int (*count_and_max_updated)(int *out_count, char *out_ts, int out_ts_len);
   int (*entity_edge_prune_orphans)(void);
   int (*entity_edge_normalize_weights)(void);
   int (*project_count)(void);
   int (*purge_hidden_pollution)(void);
   int (*requeue_drifted)(void);
   int (*cross_repo_rebuild_routes)(void);
   int (*cross_repo_rebuild_identities)(void);
   int (*cross_repo_rebuild_build_deps)(void);
   int64_t (*drift_candidates)(void);
   int (*rules_decay)(void);
   int (*curiosity_rescore_all)(void);
   int (*mining_seed_job_defaults)(void);
   void (*proposals_archive_expired)(void);
   int64_t (*trace_mining_last_id)(void);
   int (*rel_types_ensure_seed)(void);
   int (*vector_rebuild_lock_try_acquire)(void);
   void (*vector_rebuild_lock_release)(void);
   int64_t (*release_get_active)(void);
   int (*prospective_sweep_expired)(void);
   int (*directive_sweep_expired)(void);
   int (*directive_suppress)(int64_t directive_id);
   int (*directive_record_surface)(int64_t directive_id);
   int (*anti_pattern_bump)(int64_t anti_pattern_id);
   int (*anti_pattern_delete)(int64_t anti_pattern_id);
   int (*doc_delete)(int64_t doc_id);
   int (*task_delete)(int64_t task_id);
   int (*file_index_delete_project)(const char *project);
   int (*clear_project)(const char *project);
   int (*clear_current_project)(const char *project);
   int (*mark_revisit_due)(void);
   int (*ingest_queue_reset_running)(void);
   int (*evidence_reembed_all)(void);
   int (*curator_reembed_all)(void);
   int (*synth_reenqueue_all)(void);
   int (*curator_reenqueue_extract_all)(void);
   int (*pool_status)(aimee_db2_pool_status_t *status);
   int (*embedding_refusals)(aimee_db2_embedding_refusals_t *status);
   int (*postgres_status)(aimee_db2_postgres_status_t *status);
   int (*reembed_status)(aimee_db2_reembed_status_t *status);
   int (*reembed_clear)(void);
   int (*reembed_clear_maintenance)(int force, int *was_in_progress, int *recorded, int *running);
   const char *(*embedder_serving_id)(void);
   int (*dimension_reset)(uint32_t target_dimension, uint32_t force, uint32_t dry_run,
                          aimee_db2_dimension_reset_t *status);
   int (*bandit_arms_list)(const char *decision_point, char *arms, size_t capacity);
   int (*bandit_promotion_get)(const char *decision_point, char *arm_id, size_t capacity);
   int (*project_fingerprint)(const char *project, char *fingerprint, size_t capacity);
   int (*visible_source_hash)(const char *project, char *source_hash, size_t capacity);
   int (*entity_profile_card)(const char *entity_id, char *card_json, size_t capacity);
   int (*ontology_eval_status)(const char *rel_type, char *status, size_t capacity);
   int (*decision_log_set_outcome)(int64_t decision_id, const char *outcome);
   int (*decision_log_set_status)(int64_t decision_id, const char *status);
   int (*decision_log_set_revisit)(int64_t decision_id, const char *revisit_when);
   int (*prospective_set_state)(int64_t prospective_id, const char *new_state);
   int (*task_update_state)(int64_t task_id, const char *state);
   int (*ingest_queue_fail)(int64_t job_id, const char *error_message);
   int (*generation_abort)(int64_t generation_id, const char *error_message);
   int (*generation_set_source_hash)(int64_t generation_id, const char *source_hash);
   int (*generation_publish)(int64_t generation_id, const char *project);
   int (*purge_files_matching)(int64_t project_id, const char *path_glob);
   int (*collab_rule_approve)(int rule_id);
   int (*collab_rule_reject)(int rule_id);
   int (*collab_rule_retire)(int rule_id);
   int (*proposal_bump_corroboration)(int proposal_id);
   int (*proposal_mark_committed)(int proposal_id);
   int (*rules_delete_by_id)(int rule_row_id);
   int (*calibration_surfaces_with_data)(int min_rows);
   int (*reset_stuck_vector_ops)(int max_attempts);
   int (*dedupe_by_key)(int dry_run);
   int (*directive_resolve)(int64_t directive_id, int64_t resolution_memory_id);
   int (*release_add_doc)(int64_t release_id, int64_t doc_id);
   int (*scene_member_exists)(int64_t scene_memory_id, int64_t scene_id);
   int (*unit_edge_exists)(int64_t unit_id_a, int64_t unit_id_b);
   int (*artifact_cite)(const char *citing_artifact_id, const char *source_kind,
                        const char *source_id);
   int (*artifact_link)(const char *from_artifact_id, const char *to_artifact_id,
                        const char *link_kind);
   int (*bandit_promotion_set)(const char *decision_point, const char *arm_id,
                               const char *rollback_arm);
   int (*collab_rule_propose)(const char *rule_text, const char *rule_reason,
                              const char *proposed_by);
   int (*file_index_delete_current_generation)(const char *project);
   int (*project_delete)(const char *project);
   int (*minhash_delete_current_generation)(const char *project);
   int (*css_migration_enumerate)(const char *project);
   int (*ontology_approve)(const char *rel_type);
   int (*ontology_reject)(const char *rel_type);
   int (*rules_delete_by_directive_type)(const char *directive_type);
   int (*artifact_flag_review)(const char *artifact_id, const char *flag_reason);
   int (*verdict_suppressed)(const char *verdict_tag, const char *verdict_scope);
   int (*css_migration_assert_conventions)(const char *project, const char *now_iso);
   int (*curator_invalidate_doc)(const char *project, const char *file_path);
   int (*doc_assets_delete_for_doc)(const char *project, const char *document_key);
   int (*ontology_map)(const char *rel_type, const char *mapped_to);
   int (*minhash_delete_file)(const char *project, const char *file_path);
   int (*project_current_generation)(const char *project, int64_t *generation_out);
   int64_t (*projection_generation_create)(const char *project);
   int64_t (*projection_visible_id)(const char *project);
   int64_t (*release_create)(const char *release_name);
   int (*css_migration_rules_doc)(const char *exemplar_project, char *out, size_t capacity);
   int (*unique_file_basename)(const char *project, const char *basename, char *out,
                               size_t capacity);
   int (*purge_fence_heartbeat)(const char *project, const char *generation, const char *purge_id);
   int (*purge_fence_clear)(const char *project, const char *generation, const char *purge_id);
   int (*document_stored_hash)(const char *project, const char *file_path, char *out,
                               size_t capacity);
   int (*document_hash_exists)(const char *project, const char *file_hash, char *sample,
                               size_t capacity);
   int (*pdf_tsr_state)(const char *project, const char *document_key, char *out, size_t capacity);
   int (*match_error_keys)(const char *error_lowered, int64_t *ids_out, int max);
   int (*document_chunk_ids)(const char *project, const char *file_path, int64_t *out, int max);
   int (*memory_ids_by_updated)(int limit, int64_t *ids, int max_ids);
   int (*unit_ids_for_memory)(int64_t memory_id, int64_t *out, int max);
   int (*retryable_index_failures)(int max_attempts, int limit, int64_t *out, int max);
   int (*entity_neighbors)(const char *entity, db2_entity_neighbor_t *out, int max, int limit_sql);
   int (*entity_neighbors_filtered)(const char *entity, const char *relation_a,
                                    const char *relation_b, int order_by_weight,
                                    db2_entity_neighbor_t *out, int max, int limit_sql);
   int (*entity_outbound_neighbors)(const char *entity, db2_entity_neighbor_t *out, int max,
                                    int limit_sql);
   int (*entity_top_partners)(const char *entity, const char *relation, db2_entity_neighbor_t *out,
                              int max);
   int (*entity_top_targets)(const char *entity, const char *relation, db2_entity_neighbor_t *out,
                             int max);
   int (*file_definitions)(const char *project, const char *file_path, definition_t *out, int max);
   int (*code_search)(const char *query, const char *project, code_search_hit_t *out, int max,
                      int enrich);
   int (*code_search_excluding_project)(const char *query, const char *excluded_project,
                                        code_search_hit_t *out, int max, int enrich);
   int (*project_last_scan)(char *out, size_t capacity);
   int (*active_embedder_version)(char *out, size_t capacity);
   int (*bandit_decision_points)(char *out, size_t capacity);
   int (*corpus_pipeline_stage_counts)(db2_corpus_pipeline_stage_count_t *out, int max);
   int (*briefing_active_entities)(db2_memory_briefing_entity_t *out, int max);
   int (*entity_walk_step_typed)(const char *node, db2_entity_edge_typed_t *out, int max);
   int (*projection_generations_list)(const char *project, code_projection_generation_row_t *out,
                                      int max);
   int (*entity_edge_bump_utility)(const char *entity, double utility_delta);
   int (*bandit_decision_close)(const char *decision_id, double reward);
   int (*entity_neighbors_weighted)(const char *entity, db2_entity_edge_weighted_neighbor_t *out,
                                    int max, int limit_sql, int utility_scoring_enabled);
   int (*prospective_list)(const char *state, memory_prospective_t *out, int max);
   int (*prospective_list_armed)(memory_prospective_t *out, int max);
   int (*prospective_by_entity)(const char *entity_lowered, memory_prospective_t *out, int max);
   int (*prospective_by_file)(const char *file_anchor, memory_prospective_t *out, int max);
   int (*prospective_by_trigger_terms)(const char *turn_text, memory_prospective_t *out, int max);
   int (*directive_list)(const char *state, const char *cause, memory_directive_t *out, int max);
   int (*directive_by_entity)(const char *entity_lowered, memory_directive_t *out, int max);
   int (*directive_by_file)(const char *file_anchor, memory_directive_t *out, int max);
   int (*directive_by_lexical)(const char *match_clause, memory_directive_t *out, int max);
   int (*relations_for_entity)(const char *entity, int limit, memory_relation_t *out, int max);
   int (*relations_search)(const char *relation_query, int limit, memory_relation_t *out, int max);
   int (*relations_search_as_of)(const char *relation_query, const char *as_of, int limit,
                                 memory_relation_t *out, int max);
   int (*relations_supporting)(const char *entity_token, int limit, memory_relation_t *out,
                               int max);
   int (*entity_edges_for_entity)(const char *entity, edge_t *out, int max);
   int (*entity_edges_by_token)(const char *token, edge_t *out, int max, int limit_sql);
   int (*entity_top_triples)(edge_t *out, int max);
   int (*projection_edges)(const char *project, code_projection_edge_t *out, int max);
   int (*projection_edges_for_generation)(int64_t generation, code_projection_edge_t *out, int max);
   int (*task_edges)(int64_t task_id, task_edge_t *out, int max);
   int (*term_find)(const char *identifier, term_hit_t *out, int max);
   int (*term_find_in_project)(const char *project, const char *identifier, term_hit_t *out,
                               int max);
   int (*term_find_excluding_project)(const char *excluded_project, const char *identifier,
                                      term_hit_t *out, int max);
   int (*callers_find)(const char *project, const char *callee, caller_hit_t *out, int max);
   int (*callers_find_scoped)(const char *project, const char *callee, caller_hit_t *out, int max);
   int (*callers_find_excluding_project)(const char *excluded_project, const char *callee,
                                         caller_hit_t *out, int max);
   int (*rules_list)(rule_t *out, int max_rules);
   int (*rules_list_by_tier)(int min_weight, rule_t *out, int max_rules);
   int (*rules_list_hard)(rule_t *out, int max_rules);
   int (*anti_pattern_list)(anti_pattern_t *out, int max);
   int (*anti_pattern_list_hot)(int hit_threshold, anti_pattern_t *out, int max);
   int (*anti_pattern_check)(const char *file_path, const char *command, anti_pattern_t *out,
                             int max);
   int (*task_list)(const char *state, const char *session_id, int limit, aimee_task_t *out,
                    int max);
   int (*task_subtasks)(int64_t parent_task, aimee_task_t *out, int max);
   int (*typed_fact_recall)(const char *subject, const char *relation_filter, typed_fact_t *out,
                            int max);
   int (*memory_lint)(memory_lint_issue_t *out, int max);
   int (*decision_log_list)(const char *outcome, int limit, db2_decision_log_row_t *out, int max);
   int (*decision_log_list_scoped)(const char *subject, const char *status, int limit,
                                   db2_decision_log_row_t *out, int max);
   int (*global_constraints)(db2_memory_kv_row_t *rows, int max);
   int (*kv_section)(db2_memory_section_t section, db2_memory_kv_row_t *rows, int max);
   int (*memories_by_key)(const char *key, db2_memory_id_content_row_t *out, int max);
   int (*session_memories)(const char *session_id, int limit, db2_memory_id_content_row_t *out,
                           int max);
   int (*memory_candidates)(db2_memory_cand_filter_t filter, db2_memory_cand_row_t *rows, int max);
   int (*recall_section)(db2_memory_recall_section_t section, db2_memory_cand_row_t *rows, int max);
   int (*l2_cross_key_pairs)(int max_pairs, db2_memory_pair_row_t *out, int max);
   int (*l2_fact_decision_pairs)(int max_pairs, db2_memory_pair_row_t *out, int max);
   int (*kb_directive_resolve)(int64_t directive_id, int64_t resolution_memory_id,
                               const char *note);
   int (*memory_link_create)(int64_t source_id, int64_t target_id, const char *relation);
   int (*task_add_edge)(int64_t source, int64_t target, const char *relation);
   int64_t (*decision_log_active_id)(const char *subject, int64_t linked_policy_id);
   int (*entity_node_get)(const char *node_key, db2_entity_node_t *out);
   int (*entity_node_alias_upsert)(const char *alias, const char *node_key, const char *alias_kind,
                                   const char *project, int64_t generation_id);
   int (*entity_edge_upsert)(const char *source, const char *relation, const char *target,
                             int64_t window_id, int relation_id, int subject_kind, int object_kind,
                             int *out_added);
   int (*bandit_decision_insert)(const char *id, const char *decision_point, const char *arm_id,
                                 const char *context_hash, double propensity, int is_exploration);
} aimee_db2_module_backend_t;

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data);

#endif /* AIMEE_DB2_MODULE_ADAPTER_H */
