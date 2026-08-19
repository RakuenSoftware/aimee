#ifndef AIMEE_DB2_MODULE_ADAPTER_H
#define AIMEE_DB2_MODULE_ADAPTER_H 1

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/db2/module_api.h>

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
} aimee_db2_module_backend_t;

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data);

#endif /* AIMEE_DB2_MODULE_ADAPTER_H */
