/* kb_service_agent.c: aimee-kb dispatch handlers for the rules,
 * collab_rules, agent, maintenance, decision_log, and anti_pattern
 * RPC families.  Split out of kb_service.c so the file stays under
 * the per-file line cap. */

#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "db2/demotion.h"
#include "db2/feedback.h"
#include "db2/kb_service_backend.h"
#include "kb_calibrate.h"
#include "kb_demote.h"
#include "kb_ranker_fit.h"
#include "kb_service_agent.h"
#include "learning_evidence.h"

#include <stdlib.h>
#include <stdint.h>

/* Defined in kb_service.c; non-static so this file can call them. */
int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);
int kb_reply_or_error(int fd, cJSON *resp, const char *err_msg);

int kb_handle_rules_list(int fd, cJSON *req)
{
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 64;
   if (limit < 1)
      limit = 1;
   if (limit > 1024)
      limit = 1024;

   cJSON *resp = db2_kb_service_rules_list_json(limit);
   return kb_reply_or_error(fd, resp, "failed to list rules");
}

int kb_handle_rules_generate(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_rules_generate_json();
   return kb_reply_or_error(fd, resp, "failed to generate rules");
}

int kb_handle_rules_export_jsonl(int fd, cJSON *req)
{
   cJSON *path_j = cJSON_GetObjectItemCaseSensitive(req, "path");
   if (!cJSON_IsString(path_j) || !path_j->valuestring[0])
      return kb_send_error(fd, "missing path");
   cJSON *resp = db2_kb_service_rules_export_jsonl_json(path_j->valuestring);
   return kb_reply_or_error(fd, resp, "rules export failed");
}

int kb_handle_tool_registry_snapshot(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_tool_registry_snapshot_json();
   return kb_reply_or_error(fd, resp, "tool_registry snapshot failed");
}

int kb_handle_tool_registry_lookup(int fd, cJSON *req)
{
   cJSON *name = cJSON_GetObjectItemCaseSensitive(req, "name");
   if (!cJSON_IsString(name))
      return kb_send_error(fd, "missing name");
   cJSON *resp = db2_kb_service_tool_registry_lookup_json(name->valuestring);
   return kb_reply_or_error(fd, resp, "tool_registry lookup failed");
}

int kb_handle_relations_schema_list(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_relations_schema_list_json();
   return kb_reply_or_error(fd, resp, "relations schema list failed");
}

int kb_handle_rules_insert(int fd, cJSON *req)
{
   cJSON *pol = cJSON_GetObjectItemCaseSensitive(req, "polarity");
   cJSON *title = cJSON_GetObjectItemCaseSensitive(req, "title");
   cJSON *desc = cJSON_GetObjectItemCaseSensitive(req, "description");
   cJSON *wt = cJSON_GetObjectItemCaseSensitive(req, "weight");
   if (!cJSON_IsString(title))
      return kb_send_error(fd, "missing title");
   const char *pol_s = cJSON_IsString(pol) ? pol->valuestring : "positive";
   const char *desc_s = cJSON_IsString(desc) ? desc->valuestring : "";
   int weight = cJSON_IsNumber(wt) ? (int)wt->valuedouble : 5;
   cJSON *resp = db2_kb_service_rules_insert_json(pol_s, title->valuestring, desc_s, weight);
   return kb_reply_or_error(fd, resp, "rule insert failed");
}

int kb_handle_collab_rules_propose(int fd, cJSON *req)
{
   cJSON *text_j = cJSON_GetObjectItemCaseSensitive(req, "text");
   cJSON *reason_j = cJSON_GetObjectItemCaseSensitive(req, "reason");
   cJSON *by_j = cJSON_GetObjectItemCaseSensitive(req, "proposed_by");
   if (!cJSON_IsString(text_j) || !text_j->valuestring[0])
      return kb_send_error(fd, "collab_rules.propose requires text");
   const char *reason = cJSON_IsString(reason_j) ? reason_j->valuestring : "";
   const char *by = cJSON_IsString(by_j) ? by_j->valuestring : "agent";

   cJSON *resp = db2_kb_service_collab_rules_propose_json(text_j->valuestring, reason, by);
   return kb_reply_or_error(fd, resp, "failed to propose collab rule");
}

int kb_handle_collab_rules_list(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_collab_rules_list_json();
   return kb_reply_or_error(fd, resp, "failed to list collab rules");
}

int kb_handle_collab_rules_list_active(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_collab_rules_list_active_json();
   return kb_reply_or_error(fd, resp, "failed to list active collab rules");
}

static int kb_handle_collab_rules_action(int fd, cJSON *req, cJSON *(*action)(int rule_id),
                                         const char *err_msg)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "rule_id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "rule_id required");
   cJSON *resp = action((int)id_j->valuedouble);
   if (!resp)
      return kb_send_error(fd, err_msg);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_collab_rules_approve(int fd, cJSON *req)
{
   return kb_handle_collab_rules_action(fd, req, db2_kb_service_collab_rules_approve_json,
                                        "failed to approve collab rule");
}

int kb_handle_collab_rules_reject(int fd, cJSON *req)
{
   return kb_handle_collab_rules_action(fd, req, db2_kb_service_collab_rules_reject_json,
                                        "failed to reject collab rule");
}

int kb_handle_collab_rules_retire(int fd, cJSON *req)
{
   return kb_handle_collab_rules_action(fd, req, db2_kb_service_collab_rules_retire_json,
                                        "failed to retire collab rule");
}

int kb_handle_collab_rules_inject(int fd, cJSON *req)
{
   cJSON *epoch_j = cJSON_GetObjectItemCaseSensitive(req, "agent_last_epoch");
   int epoch = cJSON_IsNumber(epoch_j) ? (int)epoch_j->valuedouble : -1;
   cJSON *resp = db2_kb_service_collab_rules_inject_json(epoch);
   return kb_reply_or_error(fd, resp, "failed to render collab rules");
}

int kb_handle_learning_propose_signal(int fd, cJSON *req)
{
   cJSON *resp = db2_kb_service_learning_propose_signal_json(req);
   return kb_reply_or_error(fd, resp, "failed to record learning signal");
}

int kb_handle_agent_outcome_record(int fd, cJSON *req)
{
   cJSON *agent_j = cJSON_GetObjectItemCaseSensitive(req, "agent_name");
   cJSON *role_j = cJSON_GetObjectItemCaseSensitive(req, "role");
   cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(req, "outcome_kind");
   cJSON *reason_j = cJSON_GetObjectItemCaseSensitive(req, "reason");
   cJSON *turns_j = cJSON_GetObjectItemCaseSensitive(req, "turns_used");
   cJSON *tools_j = cJSON_GetObjectItemCaseSensitive(req, "tools_called");
   cJSON *tokens_j = cJSON_GetObjectItemCaseSensitive(req, "tokens_used");
   cJSON *tep_j = cJSON_GetObjectItemCaseSensitive(req, "tool_error_pattern");

   const char *agent = cJSON_IsString(agent_j) ? agent_j->valuestring : "";
   const char *role = cJSON_IsString(role_j) ? role_j->valuestring : "";
   const char *kind = cJSON_IsString(kind_j) ? kind_j->valuestring : "";
   const char *reason = cJSON_IsString(reason_j) ? reason_j->valuestring : "";
   int turns = cJSON_IsNumber(turns_j) ? (int)turns_j->valuedouble : 0;
   int tools = cJSON_IsNumber(tools_j) ? (int)tools_j->valuedouble : 0;
   int64_t tokens = cJSON_IsNumber(tokens_j) ? (int64_t)tokens_j->valuedouble : 0;
   const char *tep = cJSON_IsString(tep_j) ? tep_j->valuestring : "";

   cJSON *resp = db2_kb_service_agent_outcome_record_json(agent, role, kind, reason, turns, tools,
                                                          tokens, tep);
   return kb_reply_or_error(fd, resp, "failed to record agent outcome");
}

int kb_handle_agent_hint_consume(int fd, cJSON *req)
{
   cJSON *role_j = cJSON_GetObjectItemCaseSensitive(req, "role");
   cJSON *prompt_j = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   const char *role = cJSON_IsString(role_j) ? role_j->valuestring : "";
   const char *prompt = cJSON_IsString(prompt_j) ? prompt_j->valuestring : "";

   cJSON *resp = db2_kb_service_agent_hint_consume_json(role, prompt);
   return kb_reply_or_error(fd, resp, "failed to consume agent hint");
}

int kb_handle_anti_pattern_extract_from_feedback(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_anti_pattern_extract_from_feedback_json();
   return kb_reply_or_error(fd, resp, "failed to extract anti-patterns from feedback");
}

int kb_handle_anti_pattern_extract_from_failures(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_anti_pattern_extract_from_failures_json();
   return kb_reply_or_error(fd, resp, "failed to extract anti-patterns from failures");
}

int kb_handle_anti_pattern_escalate(int fd, cJSON *req)
{
   cJSON *th_j = cJSON_GetObjectItemCaseSensitive(req, "hit_threshold");
   int hit = cJSON_IsNumber(th_j) ? (int)th_j->valuedouble : 5;

   cJSON *resp = db2_kb_service_anti_pattern_escalate_json(hit);
   return kb_reply_or_error(fd, resp, "failed to escalate anti-patterns");
}

int kb_handle_rules_decay(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_rules_decay_json();
   return kb_reply_or_error(fd, resp, "failed to decay rules");
}

int kb_handle_memory_learn_style(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_memory_learn_style_json();
   return kb_reply_or_error(fd, resp, "failed to learn style");
}

int kb_handle_decision_log_insert(int fd, cJSON *req)
{
   cJSON *task_j = cJSON_GetObjectItemCaseSensitive(req, "task_id");
   cJSON *opts_j = cJSON_GetObjectItemCaseSensitive(req, "options");
   cJSON *chosen_j = cJSON_GetObjectItemCaseSensitive(req, "chosen");
   cJSON *rat_j = cJSON_GetObjectItemCaseSensitive(req, "rationale");
   cJSON *as_j = cJSON_GetObjectItemCaseSensitive(req, "assumptions");
   if (!cJSON_IsString(chosen_j))
      return kb_send_error(fd, "decision_log.insert requires chosen");
   int64_t task_id = cJSON_IsNumber(task_j) ? (int64_t)task_j->valuedouble : 0;
   const char *options = cJSON_IsString(opts_j) ? opts_j->valuestring : "";
   const char *rationale = cJSON_IsString(rat_j) ? rat_j->valuestring : "";
   const char *assumptions = cJSON_IsString(as_j) ? as_j->valuestring : "";

   cJSON *resp = db2_kb_service_decision_log_insert_json(task_id, options, chosen_j->valuestring,
                                                         rationale, assumptions);
   return kb_reply_or_error(fd, resp, "failed to log decision");
}

int kb_handle_decision_log_list(int fd, cJSON *req)
{
   cJSON *out_j = cJSON_GetObjectItemCaseSensitive(req, "outcome");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *outcome = cJSON_IsString(out_j) ? out_j->valuestring : "";
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 50;

   cJSON *resp = db2_kb_service_decision_log_list_json(outcome, limit);
   return kb_reply_or_error(fd, resp, "failed to list decisions");
}

int kb_handle_anti_pattern_list(int fd, cJSON *req)
{
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 64;

   cJSON *resp = db2_kb_service_anti_pattern_list_json(max);
   return kb_reply_or_error(fd, resp, "failed to list anti-patterns");
}

int kb_handle_anti_pattern_insert(int fd, cJSON *req)
{
   cJSON *pat_j = cJSON_GetObjectItemCaseSensitive(req, "pattern");
   cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(req, "description");
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(req, "source");
   cJSON *ref_j = cJSON_GetObjectItemCaseSensitive(req, "source_ref");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(req, "confidence");
   if (!cJSON_IsString(pat_j))
      return kb_send_error(fd, "anti_pattern.insert requires pattern");
   const char *desc = cJSON_IsString(desc_j) ? desc_j->valuestring : "";
   const char *src = cJSON_IsString(src_j) ? src_j->valuestring : "";
   const char *ref = cJSON_IsString(ref_j) ? ref_j->valuestring : "";
   double conf = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 1.0;

   cJSON *resp = db2_kb_service_anti_pattern_insert_json(pat_j->valuestring, desc, src, ref, conf);
   return kb_reply_or_error(fd, resp, "failed to insert anti-pattern");
}

int kb_handle_anti_pattern_delete(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "anti_pattern.delete requires id");

   cJSON *resp = db2_kb_service_anti_pattern_delete_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to delete anti-pattern");
}

int kb_handle_memory_fold_session(int fd, cJSON *req)
{
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   if (!cJSON_IsString(sid_j))
      return kb_send_error(fd, "maintenance.fold_session requires session_id");

   cJSON *resp = db2_kb_service_memory_fold_session_json(sid_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to fold session");
}

int kb_handle_rules_delete(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "rules.delete requires id");

   cJSON *resp = db2_kb_service_rules_delete_json((int)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to delete rule");
}

int kb_handle_rules_update_directive_type(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *dt_j = cJSON_GetObjectItemCaseSensitive(req, "directive_type");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(dt_j))
      return kb_send_error(fd, "rules.update_directive_type requires id and directive_type");

   cJSON *resp =
       db2_kb_service_rules_update_directive_type_json((int)id_j->valuedouble, dt_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to update directive type");
}

int kb_handle_feedback_record(int fd, cJSON *req)
{
   cJSON *pol_j = cJSON_GetObjectItemCaseSensitive(req, "polarity");
   cJSON *title_j = cJSON_GetObjectItemCaseSensitive(req, "title");
   cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(req, "description");
   cJSON *weight_j = cJSON_GetObjectItemCaseSensitive(req, "weight");
   if (!cJSON_IsString(pol_j) || !cJSON_IsString(title_j))
      return kb_send_error(fd, "feedback.record requires polarity and title");
   const char *desc = cJSON_IsString(desc_j) ? desc_j->valuestring : "";
   int weight = cJSON_IsNumber(weight_j) ? (int)weight_j->valuedouble : -1;

   cJSON *resp =
       db2_kb_service_feedback_record_json(pol_j->valuestring, title_j->valuestring, desc, weight);
   if (!resp)
      return kb_send_error(fd, "failed to record feedback");

   /* Emit a charter evidence artifact for this feedback signal.
    * Advisory: failure is logged but does not fail the feedback RPC. */
   {
      const char *canon_pol = db2_feedback_parse_polarity(pol_j->valuestring);
      if (canon_pol && (strcmp(canon_pol, "positive") == 0 || strcmp(canon_pol, "negative") == 0))
      {
         learning_evidence_write_feedback(canon_pol, title_j->valuestring, desc, NULL, NULL, 0);
      }
   }

   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_directive_expire_session(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_directive_expire_session_json();
   return kb_reply_or_error(fd, resp, "failed to expire session directives");
}

int kb_handle_memory_scan_conversations(int fd, cJSON *req)
{
   cJSON *dirs_j = cJSON_GetObjectItemCaseSensitive(req, "dirs");
   cJSON *resp = db2_kb_service_memory_scan_conversations_json(dirs_j);
   return kb_reply_or_error(fd, resp, "failed to scan conversations");
}

int kb_handle_anti_pattern_check(int fd, cJSON *req)
{
   cJSON *fp_j = cJSON_GetObjectItemCaseSensitive(req, "file_path");
   cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(req, "command");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   const char *fp = (cJSON_IsString(fp_j) && fp_j->valuestring[0]) ? fp_j->valuestring : NULL;
   const char *cmd = (cJSON_IsString(cmd_j) && cmd_j->valuestring[0]) ? cmd_j->valuestring : NULL;
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 4;
   cJSON *resp = db2_kb_service_anti_pattern_check_json(fp, cmd, max);
   return kb_reply_or_error(fd, resp, "failed to check anti-patterns");
}

int kb_handle_anti_pattern_bump(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "anti_pattern.bump requires id");
   cJSON *resp = db2_kb_service_anti_pattern_bump_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to bump anti-pattern");
}

int kb_handle_dashboard_memory_stats(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_dashboard_memory_stats_json();
   return kb_reply_or_error(fd, resp, "failed to fetch dashboard memory stats");
}

int kb_handle_dashboard_logs(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_dashboard_logs_json();
   return kb_reply_or_error(fd, resp, "failed to fetch dashboard logs");
}

int kb_handle_dashboard_reminders(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_dashboard_reminders_json();
   return kb_reply_or_error(fd, resp, "failed to fetch dashboard reminders");
}

int kb_handle_dashboard_recall(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_dashboard_recall_json();
   return kb_reply_or_error(fd, resp, "failed to fetch dashboard recall");
}

int kb_handle_dashboard_directives(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_dashboard_directives_json();
   return kb_reply_or_error(fd, resp, "failed to fetch dashboard directives");
}

int kb_handle_maintenance_calibrate_promotions(int fd, cJSON *req)
{
   (void)req;
   config_t cfg;
   config_load(&cfg);

   int signals = kb_calibrate_consume_drift_signals(&cfg);
   int n = kb_calibrate_run(&cfg);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", n >= 0 ? "ok" : "error");
   cJSON_AddNumberToObject(resp, "profiles_written", n >= 0 ? n : 0);
   cJSON_AddNumberToObject(resp, "drift_signals_consumed", signals);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_memory_record_retrieval_outcome(int fd, cJSON *req)
{
   cJSON *ev_j = cJSON_GetObjectItemCaseSensitive(req, "retrieval_event_id");
   cJSON *rows_j = cJSON_GetObjectItemCaseSensitive(req, "rows");
   if (!cJSON_IsString(ev_j) || !ev_j->valuestring[0])
      return kb_send_error(fd, "memory.record_retrieval_outcome: retrieval_event_id required");

   const char *ev_id = ev_j->valuestring;
   int written = 0;

   if (cJSON_IsArray(rows_j))
   {
      cJSON *row;
      cJSON_ArrayForEach(row, rows_j)
      {
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(row, "id");
         cJSON *vj = cJSON_GetObjectItemCaseSensitive(row, "verdict");
         cJSON *wj = cJSON_GetObjectItemCaseSensitive(row, "weight");
         if (!cJSON_IsNumber(id_j) || !cJSON_IsString(vj))
            continue;
         double w = cJSON_IsNumber(wj) ? wj->valuedouble : 1.0;
         if (learning_evidence_write_retrieval_attribution(ev_id, (int64_t)id_j->valuedouble,
                                                           vj->valuestring, w) == 0)
            written++;
      }
   }
   else
   {
      /* Single-row form: {retrieval_event_id, surfaced_row_id, verdict, weight} */
      cJSON *rid_j = cJSON_GetObjectItemCaseSensitive(req, "surfaced_row_id");
      cJSON *vj = cJSON_GetObjectItemCaseSensitive(req, "verdict");
      cJSON *wj = cJSON_GetObjectItemCaseSensitive(req, "weight");
      if (cJSON_IsNumber(rid_j) && cJSON_IsString(vj))
      {
         double w = cJSON_IsNumber(wj) ? wj->valuedouble : 1.0;
         if (learning_evidence_write_retrieval_attribution(ev_id, (int64_t)rid_j->valuedouble,
                                                           vj->valuestring, w) == 0)
            written++;
      }
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "written", written);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

/* ranker.emit_event {doc_ids:[...], query_fingerprint?} -> {retrieval_event_id}.
 * Option B: the kb_hybrid analogue of evidence.emit_retrieval_event — mints an
 * event over the surfaced kb_document candidates so a caller can attribute
 * outcomes to it. Endpoint-driven (no kb.c hot-path change). */
int kb_handle_ranker_emit_event(int fd, cJSON *req)
{
   cJSON *ids_j = cJSON_GetObjectItemCaseSensitive(req, "doc_ids");
   cJSON *fp_j = cJSON_GetObjectItemCaseSensitive(req, "query_fingerprint");
   const char *fp = cJSON_IsString(fp_j) ? fp_j->valuestring : "";

   int n = cJSON_IsArray(ids_j) ? cJSON_GetArraySize(ids_j) : 0;
   int64_t *ids = NULL;
   int n_ids = 0;
   if (n > 0)
   {
      ids = (int64_t *)calloc((size_t)n, sizeof(int64_t));
      if (!ids)
         return kb_send_error(fd, "out of memory");
      for (int i = 0; i < n; i++)
      {
         cJSON *e = cJSON_GetArrayItem(ids_j, i);
         if (cJSON_IsNumber(e) && e->valuedouble > 0)
            ids[n_ids++] = (int64_t)e->valuedouble;
      }
   }

   char ev_id[64] = "";
   int rc = kb_ranker_emit_event(ids, n_ids, fp, ev_id, sizeof(ev_id));
   free(ids);
   if (rc != 0)
      return kb_send_error(fd, "failed to write retrieval_event");

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "retrieval_event_id", ev_id);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

/* ranker.record_outcome {retrieval_event_id, rows:[{id,verdict,weight}]}
 * (or single {retrieval_event_id, surfaced_row_id, verdict, weight}).
 * Writes ranker_outcome artifacts the fitter's training view consumes. */
int kb_handle_ranker_record_outcome(int fd, cJSON *req)
{
   cJSON *ev_j = cJSON_GetObjectItemCaseSensitive(req, "retrieval_event_id");
   cJSON *rows_j = cJSON_GetObjectItemCaseSensitive(req, "rows");
   if (!cJSON_IsString(ev_j) || !ev_j->valuestring[0])
      return kb_send_error(fd, "ranker.record_outcome: retrieval_event_id required");

   const char *ev_id = ev_j->valuestring;
   int written = 0;

   if (cJSON_IsArray(rows_j))
   {
      cJSON *row;
      cJSON_ArrayForEach(row, rows_j)
      {
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(row, "id");
         cJSON *vj = cJSON_GetObjectItemCaseSensitive(row, "verdict");
         cJSON *wj = cJSON_GetObjectItemCaseSensitive(row, "weight");
         if (!cJSON_IsNumber(id_j) || !cJSON_IsString(vj))
            continue;
         double w = cJSON_IsNumber(wj) ? wj->valuedouble : 1.0;
         if (kb_ranker_outcome_write(ev_id, (int64_t)id_j->valuedouble, vj->valuestring, w) == 0)
            written++;
      }
   }
   else
   {
      cJSON *rid_j = cJSON_GetObjectItemCaseSensitive(req, "surfaced_row_id");
      cJSON *vj = cJSON_GetObjectItemCaseSensitive(req, "verdict");
      cJSON *wj = cJSON_GetObjectItemCaseSensitive(req, "weight");
      if (cJSON_IsNumber(rid_j) && cJSON_IsString(vj))
      {
         double w = cJSON_IsNumber(wj) ? wj->valuedouble : 1.0;
         if (kb_ranker_outcome_write(ev_id, (int64_t)rid_j->valuedouble, vj->valuestring, w) == 0)
            written++;
      }
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "written", written);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_maintenance_compute_demotions(int fd, cJSON *req)
{
   (void)req;
   config_t cfg;
   config_load(&cfg);

   int n = kb_demote_run(&cfg);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", n >= 0 ? "ok" : "error");
   cJSON_AddNumberToObject(resp, "profiles_written", n >= 0 ? n : 0);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}
