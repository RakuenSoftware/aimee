/* kb_service_memory.h: prototypes for the memory.* dispatch handlers
 * defined in kb_service_memory.c.  Included from kb_service.c so the
 * dispatch table can call them. */
#ifndef DEC_KB_SERVICE_MEMORY_H
#define DEC_KB_SERVICE_MEMORY_H 1

#include "cJSON.h"

int kb_handle_memory_find_facts(int fd, cJSON *req);
int kb_handle_memory_list(int fd, cJSON *req);
int kb_handle_memory_get(int fd, cJSON *req);
int kb_handle_memory_load_eval_corpus(int fd, cJSON *req);
int kb_handle_memory_top_l2_facts(int fd, cJSON *req);
int kb_handle_session_briefing_commitments(int fd, cJSON *req);
int kb_handle_session_briefing_directives(int fd, cJSON *req);
int kb_handle_memory_prospective_list(int fd, cJSON *req);
int kb_handle_memory_prospective_create(int fd, cJSON *req);
int kb_handle_memory_prospective_complete(int fd, cJSON *req);
int kb_handle_memory_prospective_match(int fd, cJSON *req);
int kb_handle_memory_get_provenance(int fd, cJSON *req);
int kb_handle_directive_create(int fd, cJSON *req);
int kb_handle_directive_resolve(int fd, cJSON *req);
int kb_handle_directive_suppress(int fd, cJSON *req);
int kb_handle_directive_sweep_expired(int fd, cJSON *req);
int kb_handle_directive_list(int fd, cJSON *req);
int kb_handle_memory_tag_workspace(int fd, cJSON *req);
int kb_handle_memory_scope_visibility_rank(int fd, cJSON *req);
int kb_handle_memory_episode_card_generate(int fd, cJSON *req);
int kb_handle_memory_tag_scope(int fd, cJSON *req);
int kb_handle_memory_prospective_mark_triggered(int fd, cJSON *req);
int kb_handle_memory_prospective_sweep_expired(int fd, cJSON *req);
int kb_handle_memory_maintenance_run(int fd, cJSON *req);
int kb_handle_memory_alerts(int fd, cJSON *req);
int kb_handle_memory_recall(int fd, cJSON *req);
int kb_handle_memory_upsert_workflow(int fd, cJSON *req);
int kb_handle_memory_delete(int fd, cJSON *req);
int kb_handle_memory_touch(int fd, cJSON *req);
int kb_handle_memory_update(int fd, cJSON *req);
int kb_handle_memory_reject(int fd, cJSON *req);
int kb_handle_memory_stats(int fd, cJSON *req);
int kb_handle_memory_list_conflicts(int fd, cJSON *req);
int kb_handle_memory_query_health(int fd, cJSON *req);
int kb_handle_memory_effectiveness_stats(int fd, cJSON *req);
int kb_handle_memory_query_edges(int fd, cJSON *req);
int kb_handle_memory_compact_windows(int fd, cJSON *req);
int kb_handle_memory_assemble_context(int fd, cJSON *req);
int kb_handle_memory_search(int fd, cJSON *req);
int kb_handle_memory_export_jsonl(int fd, cJSON *req);
int kb_handle_memory_decisions_export_jsonl(int fd, cJSON *req);
int kb_handle_memory_key_exists(int fd, cJSON *req);
int kb_handle_memory_find_facts_visible(int fd, cJSON *req);
int kb_handle_memory_find_facts_scoped(int fd, cJSON *req);
int kb_handle_memory_diagnose_scoped(int fd, cJSON *req);
int kb_handle_memory_explain_match(int fd, cJSON *req);
int kb_handle_memory_link_create(int fd, cJSON *req);
int kb_handle_memory_link_query(int fd, cJSON *req);
int kb_handle_memory_link_delete(int fd, cJSON *req);
int kb_handle_memory_briefing(int fd, cJSON *req);
int kb_handle_memory_context_block(int fd, cJSON *req);
/* Auditable-correctness P1: record a per-turn retrieval_event keyed by turn_id. */
int kb_handle_evidence_emit_retrieval_event(int fd, cJSON *req);
int kb_handle_memory_entity_profile(int fd, cJSON *req);
int kb_handle_memory_entity_edges(int fd, cJSON *req);
int kb_handle_memory_search_graph(int fd, cJSON *req);
int kb_handle_memory_search_graph_as_of(int fd, cJSON *req);
int kb_handle_memory_get_episode(int fd, cJSON *req);
int kb_handle_memory_ask(int fd, cJSON *req);
int kb_handle_memory_store(int fd, cJSON *req);
int kb_handle_memory_find_id_by_key_kind(int fd, cJSON *req);
int kb_handle_memory_search_facts_patterns_by_keyword(int fd, cJSON *req);
int kb_handle_memory_supersede(int fd, cJSON *req);
int kb_handle_memory_fact_history(int fd, cJSON *req);
int kb_handle_memory_check_drift(int fd, cJSON *req);
int kb_handle_task_list(int fd, cJSON *req);
int kb_handle_task_create(int fd, cJSON *req);
int kb_handle_task_update_state(int fd, cJSON *req);
int kb_handle_task_delete(int fd, cJSON *req);
int kb_handle_task_add_edge(int fd, cJSON *req);
int kb_handle_task_get_edges(int fd, cJSON *req);
int kb_handle_memory_list_session_scope_priority(int fd, cJSON *req);
int kb_handle_memory_list_session_scope_priority_like(int fd, cJSON *req);
int kb_handle_memory_list_low_effectiveness(int fd, cJSON *req);
int kb_handle_memory_list_unused_l2(int fd, cJSON *req);
int kb_handle_memory_list_superseded_keys(int fd, cJSON *req);
int kb_handle_memory_set_artifact(int fd, cJSON *req);
int kb_handle_memory_lint(int fd, cJSON *req);

#endif /* DEC_KB_SERVICE_MEMORY_H */
