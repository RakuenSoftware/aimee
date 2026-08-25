/* db1_optional.h: DB1 domain calls that may be absent from DB2-only
 * binaries such as aimee-kb.
 *
 * Server builds link the real DB1 objects, so these weak references resolve.
 * KB builds intentionally do not link DB1; callers must guard optional calls
 * before invoking them. */
#ifndef DEC_DB1_OPTIONAL_H
#define DEC_DB1_OPTIONAL_H 1

#include "db1_client/db1.h"

#if defined(AIMEE_DB1_DISABLED)
#define db1_agent_log_list_delegation_patterns                ((int (*)())0)
#define db1_agent_log_list_failure_episode_seeds              ((int (*)())0)
#define db1_agent_log_list_recent_errors                      ((int (*)())0)
#define db1_cognify_job_claim_next                            ((int (*)())0)
#define db1_cognify_job_enqueue                               ((int (*)())0)
#define db1_cognify_job_mark                                  ((int (*)())0)
#define db1_cognify_job_status                                ((int (*)())0)
#define db1_context_cache_get                                 ((int (*)())0)
#define db1_context_cache_invalidate                          ((void (*)())0)
#define db1_context_cache_put                                 ((void (*)())0)
#define db1_context_snapshot_count_for_memory                 ((int (*)())0)
#define db1_context_snapshot_count_memories_with_min_samples  ((int (*)())0)
#define db1_context_snapshot_has_memory                       ((int (*)())0)
#define db1_context_snapshot_insert                           ((int (*)())0)
#define db1_context_snapshot_insert_turn                      ((int (*)())0)
#define db1_context_snapshot_activation                       ((int (*)())0)
#define db1_context_snapshot_list_memory_ids_with_min_samples ((int (*)())0)
#define db1_context_snapshot_list_sessions_for_memory         ((int (*)())0)
#define db1_decision_record                                   ((int (*)())0)
#define db1_maintenance_state_load                            ((int (*)())0)
#define db1_maintenance_state_save                            ((int (*)())0)
#define db1_runtime_state_add_int                             ((int (*)())0)
#define db1_runtime_state_get                                 ((int (*)())0)
#define db1_runtime_state_set                                 ((int (*)())0)
#define db1_server_session_get                                ((int (*)())0)
#define db1_window_add_file                                   ((int (*)())0)
#define db1_window_add_term                                   ((int (*)())0)
#define db1_window_create_raw                                 ((int64_t (*)())0)
#define db1_window_delete_all_files                           ((int (*)())0)
#define db1_window_index_summary                              ((int (*)())0)
#define db1_window_list_files                                 ((int (*)())0)
#define db1_window_prune_files_keep_top                       ((int (*)())0)
#define db1_window_prune_terms_keep_top                       ((int (*)())0)
#define db1_window_session_id                                 ((int (*)())0)
#define db1_window_set_tier                                   ((int (*)())0)
#define db1_windows_find_candidates_by_terms                  ((int (*)())0)
#define db1_windows_find_lexical_hits                         ((int (*)())0)
#define db1_windows_list_ids_by_tier_before_days              ((int (*)())0)
#define db1_windows_session_scan_state                        ((int (*)())0)
#elif defined(__GNUC__) || defined(__clang__)
#pragma weak db1_agent_log_list_delegation_patterns
#pragma weak db1_agent_log_list_failure_episode_seeds
#pragma weak db1_agent_log_list_recent_errors
#pragma weak db1_cognify_job_claim_next
#pragma weak db1_cognify_job_enqueue
#pragma weak db1_cognify_job_mark
#pragma weak db1_cognify_job_status
#pragma weak db1_context_cache_get
#pragma weak db1_context_cache_invalidate
#pragma weak db1_context_cache_put
#pragma weak db1_context_snapshot_count_for_memory
#pragma weak db1_context_snapshot_count_memories_with_min_samples
#pragma weak db1_context_snapshot_has_memory
#pragma weak db1_context_snapshot_insert
#pragma weak db1_context_snapshot_insert_turn
#pragma weak db1_context_snapshot_activation
#pragma weak db1_context_snapshot_list_memory_ids_with_min_samples
#pragma weak db1_context_snapshot_list_sessions_for_memory
#pragma weak db1_decision_record
#pragma weak db1_maintenance_state_load
#pragma weak db1_maintenance_state_save
#pragma weak db1_runtime_state_add_int
#pragma weak db1_runtime_state_get
#pragma weak db1_runtime_state_set
#pragma weak db1_server_session_get
#pragma weak db1_window_add_file
#pragma weak db1_window_add_term
#pragma weak db1_window_create_raw
#pragma weak db1_window_delete_all_files
#pragma weak db1_window_index_summary
#pragma weak db1_window_list_files
#pragma weak db1_window_prune_files_keep_top
#pragma weak db1_window_prune_terms_keep_top
#pragma weak db1_window_session_id
#pragma weak db1_window_set_tier
#pragma weak db1_windows_find_candidates_by_terms
#pragma weak db1_windows_find_lexical_hits
#pragma weak db1_windows_list_ids_by_tier_before_days
#pragma weak db1_windows_session_scan_state
#endif

#endif /* DEC_DB1_OPTIONAL_H */
