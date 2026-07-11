/* db2/kb_service_backend_agent.c: aimee-kb RPC backends for the
 * agent + rules domain (rules.list / rules.generate, collab_rules.*,
 * agent.outcome_record, agent.hint_consume, learning.propose_signal).
 * Split out of kb_service_backend.c so the file stays under the
 * per-file line cap. */

#include "kb_service_backend.h"

#include "aimee.h"

#include "agent_coord.h"
#include "agent_eval.h"
#include "agent_hints.h"
#include "dashboard.h"
#include "agent_outcomes.h"
#include "anti_patterns.h"
#include "collab_rules.h"
#include "decision_log.h"
#include "entity_edges.h"
#include "learning.h"
#include "learning_evidence.h" /* learning_evidence_write_event — session_summary emission */
#include "learning_implicit.h"
#include "memory.h"
#include "feedback.h"
#include "rules.h"
#include "tool_registry.h"
#include "trace_analysis.h"

#include <stdlib.h>
#include <string.h>

cJSON *db2_kb_service_rules_list_json(int max_rules)
{
   if (max_rules < 1)
      return NULL;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "rules") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   rule_t *rows = calloc((size_t)max_rules, sizeof(*rows));
   if (!rows)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   int n = db2_rules_list(rows, max_rules);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
      {
         free(rows);
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(obj, "id", rows[i].id);
      cJSON_AddStringToObject(obj, "polarity", rows[i].polarity);
      cJSON_AddStringToObject(obj, "title", rows[i].title);
      cJSON_AddStringToObject(obj, "description", rows[i].description);
      cJSON_AddNumberToObject(obj, "weight", rows[i].weight);
      cJSON_AddStringToObject(obj, "domain", rows[i].domain);
      cJSON_AddStringToObject(obj, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(obj, "updated_at", rows[i].updated_at);
      cJSON_AddStringToObject(obj, "tier", db2_rules_tier(rows[i].weight));
      cJSON_AddItemToArray(arr, obj);
   }
   free(rows);
   return resp;
}

/* Snapshot the full tool_registry table.  Used by aimee-server to
 * cache the registry locally so per-tool-call validation does not
 * round-trip through aimee-kb on every agent turn. */
static int kbs_collect_tool_prompt(const char *name, const char *prompt, void *user)
{
   cJSON *prompts = (cJSON *)user;
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return -1;
   cJSON_AddStringToObject(o, "name", name ? name : "");
   cJSON_AddStringToObject(o, "prompt", prompt ? prompt : "");
   cJSON_AddItemToArray(prompts, o);
   return 0;
}

cJSON *db2_kb_service_tool_registry_snapshot_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");

   /* Per-tool prompts (covers all enabled tools). */
   cJSON *prompts = cJSON_AddArrayToObject(resp, "prompts");
   db2_tool_registry_iter_prompts(kbs_collect_tool_prompt, prompts);
   return resp;
}

cJSON *db2_kb_service_tool_registry_lookup_json(const char *name)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   tool_registry_entry_t entry;
   memset(&entry, 0, sizeof(entry));
   int rc = db2_tool_registry_lookup(name ? name : "", &entry);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "lookup failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "found", entry.found ? 1 : 0);
   if (entry.found)
   {
      cJSON_AddStringToObject(resp, "input_schema", entry.input_schema);
      cJSON_AddStringToObject(resp, "side_effect", entry.side_effect);
      cJSON_AddBoolToObject(resp, "enabled", entry.enabled ? 1 : 0);
   }
   return resp;
}

cJSON *db2_kb_service_relations_schema_list_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *rows = cJSON_AddArrayToObject(resp, "rows");
   db2_relation_schema_row_t buf[256];
   int n = db2_relation_schema_list(buf, (int)(sizeof(buf) / sizeof(buf[0])));
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      cJSON_AddNumberToObject(row, "relation_id", buf[i].relation_id);
      cJSON_AddNumberToObject(row, "subject_kind", buf[i].subject_kind);
      cJSON_AddNumberToObject(row, "object_kind", buf[i].object_kind);
      cJSON_AddItemToArray(rows, row);
   }
   return resp;
}

cJSON *db2_kb_service_rules_export_jsonl_json(const char *path)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (!path || !path[0])
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "missing path");
      return resp;
   }
   int rc = db2_rules_export_jsonl(path);
   if (rc < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "rules export failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", rc);
   return resp;
}

cJSON *db2_kb_service_rules_insert_json(const char *polarity, const char *title,
                                        const char *description, int weight)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_rules_insert(polarity ? polarity : "positive", title ? title : "",
                             description ? description : "", weight);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "rule insert failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_rules_generate_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *markdown = db2_rules_generate();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "content", markdown ? markdown : "");
   free(markdown);
   return resp;
}

cJSON *db2_kb_service_collab_rules_propose_json(const char *text, const char *reason,
                                                const char *proposed_by)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   int id = db2_collab_rules_propose(text ? text : "", reason ? reason : "",
                                     proposed_by ? proposed_by : "agent");
   if (id < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "could not propose collab rule");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "id", id);
   return resp;
}

cJSON *db2_kb_service_collab_rules_list_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   char *raw = db2_collab_rules_json_all();
   cJSON *arr = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(arr);
      arr = cJSON_CreateArray();
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "rules", arr);
   return resp;
}

cJSON *db2_kb_service_collab_rules_list_active_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   char *raw = db2_collab_rules_json_active();
   cJSON *body = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   if (!cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      body = cJSON_CreateObject();
      cJSON_AddNumberToObject(body, "epoch", 0);
      cJSON_AddArrayToObject(body, "rules");
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "active", body);
   return resp;
}

cJSON *db2_kb_service_collab_rules_approve_json(int rule_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (db2_collab_rules_approve(rule_id) == 0)
      cJSON_AddStringToObject(resp, "status", "ok");
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "approve failed");
   }
   return resp;
}

cJSON *db2_kb_service_collab_rules_reject_json(int rule_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (db2_collab_rules_reject(rule_id) == 0)
      cJSON_AddStringToObject(resp, "status", "ok");
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "reject failed");
   }
   return resp;
}

cJSON *db2_kb_service_collab_rules_inject_json(int agent_last_epoch)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = db2_collab_rules_inject(agent_last_epoch);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "body", body ? body : "");
   free(body);
   return resp;
}

cJSON *db2_kb_service_collab_rules_retire_json(int rule_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (db2_collab_rules_retire(rule_id) == 0)
      cJSON_AddStringToObject(resp, "status", "ok");
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "retire failed");
   }
   return resp;
}

cJSON *db2_kb_service_agent_outcome_record_json(const char *agent_name, const char *role,
                                                const char *outcome_kind, const char *reason,
                                                int turns_used, int tools_called,
                                                int64_t tokens_used, const char *tool_error_pattern)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_agent_outcome_record(agent_name ? agent_name : "", role ? role : "",
                                     outcome_kind ? outcome_kind : "", reason ? reason : "",
                                     turns_used, tools_called, tokens_used,
                                     tool_error_pattern ? tool_error_pattern : "");
   if (rc < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to record agent outcome");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_agent_hint_consume_json(const char *role, const char *prompt)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *hint = db2_agent_hint_find_and_consume(role ? role : "", prompt ? prompt : "");
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "hint", hint ? hint : "");
   free(hint);
   return resp;
}

cJSON *db2_kb_service_learning_propose_signal_json(const cJSON *req)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (!req)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "missing request body");
      return resp;
   }

   const cJSON *sig_j = cJSON_GetObjectItemCaseSensitive(req, "signal_type");
   if (!cJSON_IsString(sig_j) || !sig_j->valuestring[0])
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "missing signal_type");
      return resp;
   }

   learning_signal_input_t input;
   memset(&input, 0, sizeof(input));
   snprintf(input.signal_type, sizeof(input.signal_type), "%s", sig_j->valuestring);

   const cJSON *src_j = cJSON_GetObjectItemCaseSensitive(req, "source");
   snprintf(input.source, sizeof(input.source), "%s",
            (cJSON_IsString(src_j) && src_j->valuestring[0]) ? src_j->valuestring : "explicit");

   const cJSON *pol_j = cJSON_GetObjectItemCaseSensitive(req, "polarity");
   if (cJSON_IsString(pol_j))
      snprintf(input.polarity, sizeof(input.polarity), "%s", pol_j->valuestring);
   const cJSON *title_j = cJSON_GetObjectItemCaseSensitive(req, "title");
   if (cJSON_IsString(title_j))
      snprintf(input.title, sizeof(input.title), "%s", title_j->valuestring);
   const cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(req, "description");
   if (cJSON_IsString(desc_j))
      snprintf(input.description, sizeof(input.description), "%s", desc_j->valuestring);
   const cJSON *tk_j = cJSON_GetObjectItemCaseSensitive(req, "target_key");
   if (cJSON_IsString(tk_j))
      snprintf(input.target_key, sizeof(input.target_key), "%s", tk_j->valuestring);
   const cJSON *tmid_j = cJSON_GetObjectItemCaseSensitive(req, "target_memory_id");
   if (cJSON_IsNumber(tmid_j))
      input.target_memory_id = (int64_t)tmid_j->valuedouble;
   const cJSON *ct_j = cJSON_GetObjectItemCaseSensitive(req, "correction_text");
   if (cJSON_IsString(ct_j))
      snprintf(input.correction_text, sizeof(input.correction_text), "%s", ct_j->valuestring);
   const cJSON *wp_j = cJSON_GetObjectItemCaseSensitive(req, "workflow_project");
   if (cJSON_IsString(wp_j))
      snprintf(input.workflow_project, sizeof(input.workflow_project), "%s", wp_j->valuestring);
   const cJSON *ws_j = cJSON_GetObjectItemCaseSensitive(req, "workflow_signal_type");
   if (cJSON_IsString(ws_j))
      snprintf(input.workflow_signal_type, sizeof(input.workflow_signal_type), "%s",
               ws_j->valuestring);
   const cJSON *hc_j = cJSON_GetObjectItemCaseSensitive(req, "high_confidence");
   if (cJSON_IsBool(hc_j))
      input.high_confidence = cJSON_IsTrue(hc_j) ? 1 : 0;

   char *evidence_json = NULL;
   const cJSON *ev_j = cJSON_GetObjectItemCaseSensitive(req, "evidence_refs");
   if (ev_j)
      evidence_json = cJSON_PrintUnformatted(ev_j);
   input.evidence_refs_json = evidence_json ? evidence_json : "[]";

   learning_dispatch_result_t dispatch;
   memset(&dispatch, 0, sizeof(dispatch));
   int rc = learning_router_record_signal(&input, &dispatch);
   if (rc > 0 && strcmp(input.signal_type, "correction") == 0)
      learning_implicit_record_correction(input.target_key, input.target_memory_id);
   free(evidence_json);
   if (rc < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to record learning signal");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *dispatch_obj = learning_dispatch_result_to_json(&dispatch);
   if (dispatch_obj)
      cJSON_AddItemToObject(resp, "dispatch", dispatch_obj);
   return resp;
}

cJSON *db2_kb_service_anti_pattern_extract_from_feedback_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int n = anti_pattern_extract_from_feedback();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", n);
   return resp;
}

cJSON *db2_kb_service_anti_pattern_extract_from_failures_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int n = anti_pattern_extract_from_failures();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", n);
   return resp;
}

cJSON *db2_kb_service_anti_pattern_escalate_json(int hit_threshold)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int n = anti_pattern_escalate(hit_threshold);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", n);
   return resp;
}

cJSON *db2_kb_service_rules_decay_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int n = db2_rules_decay();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", n);
   return resp;
}

cJSON *db2_kb_service_memory_learn_style_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int n = memory_learn_style();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", n);
   return resp;
}

static cJSON *kbs_decision_row_to_json(const db2_decision_log_row_t *r)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)r->id);
   cJSON_AddNumberToObject(obj, "task_id", (double)r->task_id);
   cJSON_AddStringToObject(obj, "options", r->options);
   cJSON_AddStringToObject(obj, "chosen", r->chosen);
   cJSON_AddStringToObject(obj, "rationale", r->rationale);
   cJSON_AddStringToObject(obj, "assumptions", r->assumptions);
   cJSON_AddStringToObject(obj, "outcome", r->outcome);
   cJSON_AddStringToObject(obj, "created_at", r->created_at);
   return obj;
}

cJSON *db2_kb_service_decision_log_insert_json(int64_t task_id, const char *options,
                                               const char *chosen, const char *rationale,
                                               const char *assumptions)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   db2_decision_log_row_t row;
   memset(&row, 0, sizeof(row));
   if (db2_decision_log_insert(task_id, options ? options : "", chosen ? chosen : "",
                               rationale ? rationale : "", assumptions ? assumptions : "", NULL,
                               &row) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to log decision");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *obj = kbs_decision_row_to_json(&row);
   if (obj)
      cJSON_AddItemToObject(resp, "decision", obj);
   return resp;
}

cJSON *db2_kb_service_decision_log_list_json(const char *outcome, int limit)
{
   if (limit < 1)
      limit = 50;
   if (limit > 256)
      limit = 256;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "decisions") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   db2_decision_log_row_t rows[256];
   int n = db2_decision_log_list((outcome && outcome[0]) ? outcome : NULL, limit, rows, limit);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_decision_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

static cJSON *kbs_anti_pattern_row_to_json(const anti_pattern_t *a)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)a->id);
   cJSON_AddStringToObject(obj, "pattern", a->pattern);
   cJSON_AddStringToObject(obj, "description", a->description);
   cJSON_AddStringToObject(obj, "source", a->source);
   cJSON_AddStringToObject(obj, "source_ref", a->source_ref);
   cJSON_AddNumberToObject(obj, "hit_count", a->hit_count);
   cJSON_AddNumberToObject(obj, "confidence", a->confidence);
   return obj;
}

cJSON *db2_kb_service_anti_pattern_list_json(int max)
{
   if (max < 1)
      max = 64;
   if (max > 256)
      max = 256;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "anti_patterns") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   anti_pattern_t rows[256];
   int n = db2_anti_pattern_list(rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_anti_pattern_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_anti_pattern_insert_json(const char *pattern, const char *description,
                                               const char *source, const char *source_ref,
                                               double confidence)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   anti_pattern_t row;
   memset(&row, 0, sizeof(row));
   if (db2_anti_pattern_insert(pattern ? pattern : "", description ? description : "",
                               source ? source : "", source_ref ? source_ref : "", confidence,
                               &row) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to insert anti-pattern");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *obj = kbs_anti_pattern_row_to_json(&row);
   if (obj)
      cJSON_AddItemToObject(resp, "anti_pattern", obj);
   return resp;
}

cJSON *db2_kb_service_anti_pattern_check_json(const char *file_path, const char *command, int max)
{
   if (max < 1)
      max = 4;
   if (max > 16)
      max = 16;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "matches") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   anti_pattern_t rows[16];
   int n = db2_anti_pattern_check(file_path, command, rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *m = cJSON_CreateObject();
      if (!m)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(m, "id", (double)rows[i].id);
      cJSON_AddStringToObject(m, "pattern", rows[i].pattern);
      cJSON_AddStringToObject(m, "description", rows[i].description);
      cJSON_AddStringToObject(m, "source", rows[i].source);
      cJSON_AddStringToObject(m, "source_ref", rows[i].source_ref);
      cJSON_AddNumberToObject(m, "hit_count", rows[i].hit_count);
      cJSON_AddNumberToObject(m, "confidence", rows[i].confidence);
      cJSON_AddItemToArray(arr, m);
   }
   return resp;
}

cJSON *db2_kb_service_anti_pattern_bump_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   db2_anti_pattern_bump(id);
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_anti_pattern_delete_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (db2_anti_pattern_delete(id) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to delete anti-pattern");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_fold_session_json(const char *session_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char summary[256] = "";
   int rc = memory_fold_session(session_id ? session_id : "", summary, sizeof(summary));

   /* Surface the folded session digest as a `session_summary` evidence artifact
    * so the idle-reflection scheduler and the evidence-synth drain have a real
    * candidate stream to work over — this is the producer that was otherwise
    * missing. Cheap (one artifact write from the digest already computed, no LLM
    * on this path), idempotent via content-hash dedup, and emitted unconditionally
    * like other evidence capture; the LLM-heavy consumers are separately gated. */
   if (rc >= 0 && summary[0])
      learning_evidence_write_event("session_summary", "session", session_id ? session_id : "",
                                    summary, "kb.fold_session", NULL, 0);

   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", rc < 0 ? 0 : rc);
   return resp;
}

cJSON *db2_kb_service_rules_delete_json(int id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (db2_rules_delete(id) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to delete rule");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_rules_update_directive_type_json(int id, const char *directive_type)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (db2_rules_update_directive_type(id, directive_type ? directive_type : "soft") != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to update directive type");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_feedback_record_json(const char *polarity, const char *title,
                                           const char *description, int weight)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int reinforced = 0;
   int id = db2_feedback_record(polarity ? polarity : "", title ? title : "",
                                description ? description : "", weight, &reinforced);
   if (id < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to record feedback");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "id", id);
   cJSON_AddBoolToObject(resp, "reinforced", reinforced);
   return resp;
}

cJSON *db2_kb_service_directive_expire_session_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   (void)db2_rules_delete_by_directive_type("session");
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_scan_conversations_json(const cJSON *dirs)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (!cJSON_IsArray(dirs))
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "missing dirs array");
      return resp;
   }
   char buf[8][MAX_PATH_LEN];
   int n = 0;
   const cJSON *d;
   cJSON_ArrayForEach(d, dirs)
   {
      if (n >= 8)
         break;
      if (cJSON_IsString(d))
         snprintf(buf[n++], MAX_PATH_LEN, "%s", d->valuestring);
   }
   int rc = memory_scan_conversations(buf, n);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", rc);
   return resp;
}

cJSON *db2_kb_service_dashboard_memory_stats_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = api_memory_stats();
   if (!body)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory stats unavailable");
      return resp;
   }
   cJSON *payload = cJSON_Parse(body);
   free(body);
   cJSON_AddStringToObject(resp, "status", "ok");
   if (payload)
      cJSON_AddItemToObject(resp, "payload", payload);
   else
      cJSON_AddItemToObject(resp, "payload", cJSON_CreateObject());
   return resp;
}

cJSON *db2_kb_service_dashboard_logs_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = api_logs();
   cJSON *payload = body ? cJSON_Parse(body) : NULL;
   free(body);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "payload", payload ? payload : cJSON_CreateArray());
   return resp;
}

cJSON *db2_kb_service_dashboard_reminders_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = api_dashboard_reminders();
   cJSON *payload = body ? cJSON_Parse(body) : NULL;
   free(body);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "payload", payload ? payload : cJSON_CreateObject());
   return resp;
}

cJSON *db2_kb_service_dashboard_recall_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = api_dashboard_recall();
   cJSON *payload = body ? cJSON_Parse(body) : NULL;
   free(body);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "payload", payload ? payload : cJSON_CreateObject());
   return resp;
}

cJSON *db2_kb_service_dashboard_directives_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = api_dashboard_directives();
   cJSON *payload = body ? cJSON_Parse(body) : NULL;
   free(body);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "payload", payload ? payload : cJSON_CreateObject());
   return resp;
}
