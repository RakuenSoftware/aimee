/* kb_service_agent.h: prototypes for the rules + collab_rules +
 * agent + maintenance + decision_log + anti_pattern dispatch
 * handlers defined in kb_service_agent.c.  Included from kb_service.c
 * so the dispatch table can call them. */
#ifndef DEC_KB_SERVICE_AGENT_H
#define DEC_KB_SERVICE_AGENT_H 1

#include "cJSON.h"

int kb_handle_rules_list(int fd, cJSON *req);
int kb_handle_rules_generate(int fd, cJSON *req);
int kb_handle_rules_export_jsonl(int fd, cJSON *req);
int kb_handle_rules_insert(int fd, cJSON *req);
int kb_handle_tool_registry_snapshot(int fd, cJSON *req);
int kb_handle_tool_registry_lookup(int fd, cJSON *req);
int kb_handle_relations_schema_list(int fd, cJSON *req);
int kb_handle_collab_rules_propose(int fd, cJSON *req);
int kb_handle_collab_rules_list(int fd, cJSON *req);
int kb_handle_collab_rules_list_active(int fd, cJSON *req);
int kb_handle_collab_rules_approve(int fd, cJSON *req);
int kb_handle_collab_rules_reject(int fd, cJSON *req);
int kb_handle_collab_rules_retire(int fd, cJSON *req);
int kb_handle_collab_rules_inject(int fd, cJSON *req);
int kb_handle_learning_propose_signal(int fd, cJSON *req);
int kb_handle_agent_outcome_record(int fd, cJSON *req);
int kb_handle_agent_hint_consume(int fd, cJSON *req);
int kb_handle_anti_pattern_extract_from_feedback(int fd, cJSON *req);
int kb_handle_anti_pattern_extract_from_failures(int fd, cJSON *req);
int kb_handle_anti_pattern_escalate(int fd, cJSON *req);
int kb_handle_rules_decay(int fd, cJSON *req);
int kb_handle_memory_learn_style(int fd, cJSON *req);
int kb_handle_decision_log_insert(int fd, cJSON *req);
int kb_handle_decision_log_list(int fd, cJSON *req);
int kb_handle_anti_pattern_list(int fd, cJSON *req);
int kb_handle_anti_pattern_insert(int fd, cJSON *req);
int kb_handle_anti_pattern_delete(int fd, cJSON *req);
int kb_handle_anti_pattern_check(int fd, cJSON *req);
int kb_handle_anti_pattern_bump(int fd, cJSON *req);
int kb_handle_memory_fold_session(int fd, cJSON *req);
int kb_handle_rules_delete(int fd, cJSON *req);
int kb_handle_rules_update_directive_type(int fd, cJSON *req);
int kb_handle_feedback_record(int fd, cJSON *req);
int kb_handle_directive_expire_session(int fd, cJSON *req);
int kb_handle_memory_scan_conversations(int fd, cJSON *req);
int kb_handle_dashboard_memory_stats(int fd, cJSON *req);
int kb_handle_dashboard_logs(int fd, cJSON *req);
int kb_handle_dashboard_reminders(int fd, cJSON *req);
int kb_handle_dashboard_recall(int fd, cJSON *req);
int kb_handle_dashboard_directives(int fd, cJSON *req);
int kb_handle_maintenance_calibrate_promotions(int fd, cJSON *req);
int kb_handle_memory_record_retrieval_outcome(int fd, cJSON *req);
int kb_handle_ranker_emit_event(int fd, cJSON *req);
int kb_handle_ranker_record_outcome(int fd, cJSON *req);
int kb_handle_maintenance_compute_demotions(int fd, cJSON *req);

#endif /* DEC_KB_SERVICE_AGENT_H */
