/* kb_service_memory.h: prototypes for the memory.* dispatch handlers
 * defined in kb_service_memory.c.  Included from kb_service.c so the
 * dispatch table can call them. */
#ifndef DEC_KB_SERVICE_MEMORY_H
#define DEC_KB_SERVICE_MEMORY_H 1

#include "cJSON.h"

int kb_handle_memory_find_facts(int fd, cJSON *req);
int kb_handle_session_briefing_commitments(int fd, cJSON *req);
int kb_handle_session_briefing_directives(int fd, cJSON *req);
int kb_handle_memory_episode_card_generate(int fd, cJSON *req);
int kb_handle_memory_assemble_typed_context(int fd, cJSON *req);
int kb_handle_memory_search(int fd, cJSON *req);
int kb_handle_memory_export_jsonl(int fd, cJSON *req);
int kb_handle_memory_decisions_export_jsonl(int fd, cJSON *req);
int kb_handle_memory_find_facts_visible(int fd, cJSON *req);
int kb_handle_memory_find_facts_scoped(int fd, cJSON *req);
int kb_handle_memory_diagnose_scoped(int fd, cJSON *req);
int kb_handle_memory_explain_match(int fd, cJSON *req);
int kb_handle_memory_context_block(int fd, cJSON *req);
int kb_handle_memory_facts(int fd, cJSON *req);
/* Auditable-correctness P1: record a per-turn retrieval_event keyed by turn_id. */
int kb_handle_evidence_emit_retrieval_event(int fd, cJSON *req);
/* Auditable-correctness P1.5: merge typed code/doc refs into the turn's event. */
int kb_handle_evidence_merge_retrieval_event(int fd, cJSON *req);
/* Auditable-correctness P1: the /v1/audit/trace read (four-state status). */
int kb_handle_evidence_trace_retrieval_event(int fd, cJSON *req);
/* Auditable-correctness P2: the /v1/audit/provenance read (sources at version). */
int kb_handle_evidence_provenance(int fd, cJSON *req);
/* Auditable-correctness P3: the /v1/audit/fidelity read (answer-level report). */
int kb_handle_evidence_fidelity(int fd, cJSON *req);
int kb_handle_css_signals(int fd, cJSON *req);
int kb_handle_memory_search_assertions(int fd, cJSON *req);
int kb_handle_memory_ask(int fd, cJSON *req);
int kb_handle_memory_store(int fd, cJSON *req);
int kb_handle_memory_supersede(int fd, cJSON *req);
/* Typed-fact §4 correction + §3 entity merge/unmerge surface. */
int kb_handle_facts_retract(int fd, cJSON *req);
int kb_handle_entities_merge(int fd, cJSON *req);
int kb_handle_entities_unmerge(int fd, cJSON *req);
int kb_handle_task_list(int fd, cJSON *req);
int kb_handle_task_create(int fd, cJSON *req);
int kb_handle_task_update_state(int fd, cJSON *req);
int kb_handle_task_delete(int fd, cJSON *req);
int kb_handle_task_add_edge(int fd, cJSON *req);
int kb_handle_task_get_edges(int fd, cJSON *req);

#endif /* DEC_KB_SERVICE_MEMORY_H */
