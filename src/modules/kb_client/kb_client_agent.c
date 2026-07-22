/* kb_client_agent.c: kb_client wrappers for the rules + collab_rules
 * + agent telemetry + maintenance + decision_log + anti_pattern +
 * feedback RPC families.  Split out of kb_client.c so the file stays
 * under the per-file line cap. */

#include "kb_client.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in kb_client.c. */
char *kb_v1_action_request(const char *method, cJSON *req);

char *kb_client_rules_list_json(int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("rules.list", req);
}

int kb_client_rules_export_jsonl(const char *path)
{
   if (!path)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "path", path);
   char *json = kb_v1_action_request("rules.export_jsonl", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *count = cJSON_GetObjectItemCaseSensitive(resp, "count");
   int rc = -1;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(count))
      rc = (int)count->valuedouble;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_rules_insert(const char *polarity, const char *title, const char *description,
                           int weight)
{
   if (!title)
      return -1;
   cJSON *req = cJSON_CreateObject();
   if (polarity && polarity[0])
      cJSON_AddStringToObject(req, "polarity", polarity);
   cJSON_AddStringToObject(req, "title", title);
   if (description && description[0])
      cJSON_AddStringToObject(req, "description", description);
   cJSON_AddNumberToObject(req, "weight", weight);
   char *json = kb_v1_action_request("rules.insert", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

char *kb_client_rules_generate_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("rules.generate", req);
}

char *kb_client_rules_generate(void)
{
   char *envelope = kb_client_rules_generate_json();
   if (!envelope)
      return NULL;
   cJSON *resp = cJSON_Parse(envelope);
   free(envelope);
   if (!resp)
      return NULL;
   cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
   char *out =
       (cJSON_IsString(content) && content->valuestring[0]) ? strdup(content->valuestring) : NULL;
   cJSON_Delete(resp);
   return out;
}

int kb_client_rules_list(rule_t *out, int max_rules)
{
   if (!out || max_rules <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "limit", max_rules);
   char *json = kb_v1_action_request("rules.list", req);
   if (!json)
      return 0;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *rules = cJSON_GetObjectItemCaseSensitive(resp, "rules");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsArray(rules))
   {
      cJSON_Delete(resp);
      return 0;
   }

   int n = 0;
   cJSON *r;
   cJSON_ArrayForEach(r, rules)
   {
      if (n >= max_rules)
         break;
      memset(&out[n], 0, sizeof(out[n]));
      cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
      cJSON *pol_j = cJSON_GetObjectItemCaseSensitive(r, "polarity");
      cJSON *title_j = cJSON_GetObjectItemCaseSensitive(r, "title");
      cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(r, "description");
      cJSON *weight_j = cJSON_GetObjectItemCaseSensitive(r, "weight");
      cJSON *domain_j = cJSON_GetObjectItemCaseSensitive(r, "domain");
      cJSON *created_j = cJSON_GetObjectItemCaseSensitive(r, "created_at");
      cJSON *updated_j = cJSON_GetObjectItemCaseSensitive(r, "updated_at");
      if (cJSON_IsNumber(id_j))
         out[n].id = (int)id_j->valuedouble;
      if (cJSON_IsString(pol_j))
         snprintf(out[n].polarity, sizeof(out[n].polarity), "%s", pol_j->valuestring);
      if (cJSON_IsString(title_j))
         snprintf(out[n].title, sizeof(out[n].title), "%s", title_j->valuestring);
      if (cJSON_IsString(desc_j))
         snprintf(out[n].description, sizeof(out[n].description), "%s", desc_j->valuestring);
      if (cJSON_IsNumber(weight_j))
         out[n].weight = (int)weight_j->valuedouble;
      if (cJSON_IsString(domain_j))
         snprintf(out[n].domain, sizeof(out[n].domain), "%s", domain_j->valuestring);
      if (cJSON_IsString(created_j))
         snprintf(out[n].created_at, sizeof(out[n].created_at), "%s", created_j->valuestring);
      if (cJSON_IsString(updated_j))
         snprintf(out[n].updated_at, sizeof(out[n].updated_at), "%s", updated_j->valuestring);
      n++;
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_collab_rules_propose(const char *text, const char *reason, const char *proposed_by)
{
   if (!text || !text[0])
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "text", text);
   if (reason && reason[0])
      cJSON_AddStringToObject(req, "reason", reason);
   if (proposed_by && proposed_by[0])
      cJSON_AddStringToObject(req, "proposed_by", proposed_by);
   char *json = kb_v1_action_request("collab_rules.propose", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(resp, "id");
   int id = -1;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(id_j))
      id = (int)id_j->valuedouble;
   cJSON_Delete(resp);
   return id;
}

static int kb_client_v1_collab_rules_action(const char *method, int rule_id)
{
   if (rule_id <= 0)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "rule_id", rule_id);
   char *json = kb_v1_action_request(method, req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_collab_rules_approve(int rule_id)
{
   return kb_client_v1_collab_rules_action("collab_rules.approve", rule_id);
}

int kb_client_collab_rules_reject(int rule_id)
{
   return kb_client_v1_collab_rules_action("collab_rules.reject", rule_id);
}

int kb_client_collab_rules_retire(int rule_id)
{
   return kb_client_v1_collab_rules_action("collab_rules.retire", rule_id);
}

char *kb_client_collab_rules_list_json(void)
{
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("collab_rules.list", req);
   if (!json)
      return NULL;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *rules = cJSON_GetObjectItemCaseSensitive(resp, "rules");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsArray(rules))
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON *detached = cJSON_DetachItemViaPointer(resp, rules);
   char *out = detached ? cJSON_PrintUnformatted(detached) : NULL;
   cJSON_Delete(detached);
   cJSON_Delete(resp);
   return out;
}

int kb_client_agent_outcome_record(const char *agent_name, const char *role,
                                   const char *outcome_kind, const char *reason, int turns_used,
                                   int tools_called, int64_t tokens_used,
                                   const char *tool_error_pattern)
{
   cJSON *req = cJSON_CreateObject();
   if (agent_name && agent_name[0])
      cJSON_AddStringToObject(req, "agent_name", agent_name);
   if (role && role[0])
      cJSON_AddStringToObject(req, "role", role);
   if (outcome_kind && outcome_kind[0])
      cJSON_AddStringToObject(req, "outcome_kind", outcome_kind);
   if (reason && reason[0])
      cJSON_AddStringToObject(req, "reason", reason);
   cJSON_AddNumberToObject(req, "turns_used", turns_used);
   cJSON_AddNumberToObject(req, "tools_called", tools_called);
   cJSON_AddNumberToObject(req, "tokens_used", (double)tokens_used);
   if (tool_error_pattern && tool_error_pattern[0])
      cJSON_AddStringToObject(req, "tool_error_pattern", tool_error_pattern);

   char *json = kb_v1_action_request("agent.outcome_record", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

static int kb_client_v1_simple_count_request(const char *method, cJSON *req)
{
   char *json = kb_v1_action_request(method, req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *count_j = cJSON_GetObjectItemCaseSensitive(resp, "count");
   int n = -1;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(count_j))
      n = (int)count_j->valuedouble;
   cJSON_Delete(resp);
   return n;
}

int kb_client_anti_pattern_extract_from_feedback(void)
{
   return kb_client_v1_simple_count_request("maintenance.anti_pattern_extract_from_feedback",
                                            cJSON_CreateObject());
}

int kb_client_anti_pattern_extract_from_failures(void)
{
   return kb_client_v1_simple_count_request("maintenance.anti_pattern_extract_from_failures",
                                            cJSON_CreateObject());
}

int kb_client_anti_pattern_escalate(int hit_threshold)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "hit_threshold", hit_threshold);
   return kb_client_v1_simple_count_request("maintenance.anti_pattern_escalate", req);
}

int kb_client_rules_decay(void)
{
   return kb_client_v1_simple_count_request("maintenance.rules_decay", cJSON_CreateObject());
}

int kb_client_memory_learn_style(void)
{
   return kb_client_v1_simple_count_request("maintenance.memory_learn_style", cJSON_CreateObject());
}

static void kbc_decision_row_from_json(cJSON *d, db2_decision_log_row_t *out)
{
   memset(out, 0, sizeof(*out));
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(d, "id");
   cJSON *task_j = cJSON_GetObjectItemCaseSensitive(d, "task_id");
   cJSON *opts_j = cJSON_GetObjectItemCaseSensitive(d, "options");
   cJSON *chosen_j = cJSON_GetObjectItemCaseSensitive(d, "chosen");
   cJSON *rat_j = cJSON_GetObjectItemCaseSensitive(d, "rationale");
   cJSON *as_j = cJSON_GetObjectItemCaseSensitive(d, "assumptions");
   cJSON *out_j = cJSON_GetObjectItemCaseSensitive(d, "outcome");
   cJSON *cre_j = cJSON_GetObjectItemCaseSensitive(d, "created_at");
   if (cJSON_IsNumber(id_j))
      out->id = (int64_t)id_j->valuedouble;
   if (cJSON_IsNumber(task_j))
      out->task_id = (int64_t)task_j->valuedouble;
   if (cJSON_IsString(opts_j))
      snprintf(out->options, sizeof(out->options), "%s", opts_j->valuestring);
   if (cJSON_IsString(chosen_j))
      snprintf(out->chosen, sizeof(out->chosen), "%s", chosen_j->valuestring);
   if (cJSON_IsString(rat_j))
      snprintf(out->rationale, sizeof(out->rationale), "%s", rat_j->valuestring);
   if (cJSON_IsString(as_j))
      snprintf(out->assumptions, sizeof(out->assumptions), "%s", as_j->valuestring);
   if (cJSON_IsString(out_j))
      snprintf(out->outcome, sizeof(out->outcome), "%s", out_j->valuestring);
   if (cJSON_IsString(cre_j))
      snprintf(out->created_at, sizeof(out->created_at), "%s", cre_j->valuestring);
}

int kb_client_decision_log_insert(int64_t task_id, const char *options, const char *chosen,
                                  const char *rationale, const char *assumptions,
                                  db2_decision_log_row_t *out)
{
   if (!chosen)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "task_id", (double)task_id);
   cJSON_AddStringToObject(req, "options", options ? options : "");
   cJSON_AddStringToObject(req, "chosen", chosen);
   cJSON_AddStringToObject(req, "rationale", rationale ? rationale : "");
   cJSON_AddStringToObject(req, "assumptions", assumptions ? assumptions : "");
   char *json = kb_v1_action_request("decision_log.insert", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }
   if (out)
   {
      cJSON *d = cJSON_GetObjectItemCaseSensitive(resp, "decision");
      if (cJSON_IsObject(d))
         kbc_decision_row_from_json(d, out);
      else
         memset(out, 0, sizeof(*out));
   }
   cJSON_Delete(resp);
   return 0;
}

int kb_client_decision_log_list(const char *outcome, int limit, db2_decision_log_row_t *out,
                                int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   if (outcome && outcome[0])
      cJSON_AddStringToObject(req, "outcome", outcome);
   cJSON_AddNumberToObject(req, "limit", limit > 0 ? limit : 50);
   char *json = kb_v1_action_request("decision_log.list", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return 0;
   }
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "decisions");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         kbc_decision_row_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

static void kbc_anti_pattern_row_from_json(cJSON *a, anti_pattern_t *out)
{
   memset(out, 0, sizeof(*out));
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(a, "id");
   cJSON *pat_j = cJSON_GetObjectItemCaseSensitive(a, "pattern");
   cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(a, "description");
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(a, "source");
   cJSON *ref_j = cJSON_GetObjectItemCaseSensitive(a, "source_ref");
   cJSON *hit_j = cJSON_GetObjectItemCaseSensitive(a, "hit_count");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(a, "confidence");
   if (cJSON_IsNumber(id_j))
      out->id = (int64_t)id_j->valuedouble;
   if (cJSON_IsString(pat_j))
      snprintf(out->pattern, sizeof(out->pattern), "%s", pat_j->valuestring);
   if (cJSON_IsString(desc_j))
      snprintf(out->description, sizeof(out->description), "%s", desc_j->valuestring);
   if (cJSON_IsString(src_j))
      snprintf(out->source, sizeof(out->source), "%s", src_j->valuestring);
   if (cJSON_IsString(ref_j))
      snprintf(out->source_ref, sizeof(out->source_ref), "%s", ref_j->valuestring);
   if (cJSON_IsNumber(hit_j))
      out->hit_count = (int)hit_j->valuedouble;
   if (cJSON_IsNumber(conf_j))
      out->confidence = conf_j->valuedouble;
}

int kb_client_anti_pattern_list(anti_pattern_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("anti_pattern.list", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return 0;
   }
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "anti_patterns");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         kbc_anti_pattern_row_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_anti_pattern_insert(const char *pattern, const char *description, const char *source,
                                  const char *source_ref, double confidence, anti_pattern_t *out)
{
   if (!pattern)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "pattern", pattern);
   if (description && description[0])
      cJSON_AddStringToObject(req, "description", description);
   if (source && source[0])
      cJSON_AddStringToObject(req, "source", source);
   if (source_ref && source_ref[0])
      cJSON_AddStringToObject(req, "source_ref", source_ref);
   cJSON_AddNumberToObject(req, "confidence", confidence);
   char *json = kb_v1_action_request("anti_pattern.insert", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }
   if (out)
   {
      cJSON *a = cJSON_GetObjectItemCaseSensitive(resp, "anti_pattern");
      if (cJSON_IsObject(a))
         kbc_anti_pattern_row_from_json(a, out);
      else
         memset(out, 0, sizeof(*out));
   }
   cJSON_Delete(resp);
   return 0;
}

int kb_client_anti_pattern_delete(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   char *json = kb_v1_action_request("anti_pattern.delete", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_memory_fold_session(const char *session_id)
{
   if (!session_id || !session_id[0])
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "session_id", session_id);
   return kb_client_v1_simple_count_request("maintenance.fold_session", req);
}

int kb_client_rules_delete(int id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", id);
   char *json = kb_v1_action_request("rules.delete", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_rules_update_directive_type(int id, const char *directive_type)
{
   if (!directive_type)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", id);
   cJSON_AddStringToObject(req, "directive_type", directive_type);
   char *json = kb_v1_action_request("rules.update_directive_type", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_feedback_record(const char *polarity, const char *title, const char *description,
                              int weight, int *reinforced_out)
{
   if (!polarity || !title)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "polarity", polarity);
   cJSON_AddStringToObject(req, "title", title);
   if (description && description[0])
      cJSON_AddStringToObject(req, "description", description);
   cJSON_AddNumberToObject(req, "weight", weight);
   char *json = kb_v1_action_request("feedback.record", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(resp, "id");
   cJSON *rein_j = cJSON_GetObjectItemCaseSensitive(resp, "reinforced");
   int id = -1;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(id_j))
   {
      id = (int)id_j->valuedouble;
      if (reinforced_out)
         *reinforced_out = (cJSON_IsBool(rein_j) && cJSON_IsTrue(rein_j)) ? 1 : 0;
   }
   cJSON_Delete(resp);
   return id;
}

char *kb_client_agent_hint_consume(const char *role, const char *prompt)
{
   cJSON *req = cJSON_CreateObject();
   if (role && role[0])
      cJSON_AddStringToObject(req, "role", role);
   if (prompt && prompt[0])
      cJSON_AddStringToObject(req, "prompt", prompt);
   char *json = kb_v1_action_request("agent.hint_consume", req);
   if (!json)
      return NULL;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *hint_j = cJSON_GetObjectItemCaseSensitive(resp, "hint");
   char *out = NULL;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsString(hint_j) &&
       hint_j->valuestring[0])
   {
      out = strdup(hint_j->valuestring);
   }
   cJSON_Delete(resp);
   return out;
}

/* Forward an MCP-style learning_propose payload (signal_type +
 * optional descriptive fields + optional evidence_refs array) to
 * aimee-kb's learning.propose_signal RPC.  Caller passes the raw
 * args object straight from cJSON_Parse; this wrapper duplicates it
 * before sending so the caller retains ownership.  Returns the
 * stringified dispatch result (caller frees) or NULL if kb is
 * unreachable. */
char *kb_client_learning_propose_signal_json(const cJSON *args)
{
   if (!args)
      return NULL;
   cJSON *req = cJSON_Duplicate(args, 1);
   if (!req)
      return NULL;
   return kb_v1_action_request("learning.propose_signal", req);
}

char *kb_client_collab_rules_list_active_json(void)
{
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("collab_rules.list_active", req);
   if (!json)
      return NULL;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *active = cJSON_GetObjectItemCaseSensitive(resp, "active");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsObject(active))
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON *detached = cJSON_DetachItemViaPointer(resp, active);
   char *out = detached ? cJSON_PrintUnformatted(detached) : NULL;
   cJSON_Delete(detached);
   cJSON_Delete(resp);
   return out;
}

int kb_client_directive_expire_session(void)
{
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("maintenance.expire_session_directives", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_memory_scan_conversations(char dirs[][4096], int dir_count)
{
   if (dir_count < 0)
      dir_count = 0;
   cJSON *req = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(req, "dirs");
   for (int i = 0; i < dir_count; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateString(dirs[i]));
   char *json = kb_v1_action_request("maintenance.scan_conversations", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *count_j = cJSON_GetObjectItemCaseSensitive(resp, "count");
   int n =
       (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(count_j))
           ? (int)count_j->valuedouble
           : -1;
   cJSON_Delete(resp);
   return n;
}

int kb_client_anti_pattern_check(const char *file_path, const char *command, anti_pattern_t *out,
                                 int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   if (file_path && file_path[0])
      cJSON_AddStringToObject(req, "file_path", file_path);
   if (command && command[0])
      cJSON_AddStringToObject(req, "command", command);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("anti_pattern.check", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return 0;
   }
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "matches");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         memset(&out[n], 0, sizeof(out[n]));
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
         cJSON *pat_j = cJSON_GetObjectItemCaseSensitive(r, "pattern");
         cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(r, "description");
         cJSON *src_j = cJSON_GetObjectItemCaseSensitive(r, "source");
         cJSON *ref_j = cJSON_GetObjectItemCaseSensitive(r, "source_ref");
         cJSON *hit_j = cJSON_GetObjectItemCaseSensitive(r, "hit_count");
         cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(r, "confidence");
         if (cJSON_IsNumber(id_j))
            out[n].id = (int64_t)id_j->valuedouble;
         if (cJSON_IsString(pat_j))
            snprintf(out[n].pattern, sizeof(out[n].pattern), "%s", pat_j->valuestring);
         if (cJSON_IsString(desc_j))
            snprintf(out[n].description, sizeof(out[n].description), "%s", desc_j->valuestring);
         if (cJSON_IsString(src_j))
            snprintf(out[n].source, sizeof(out[n].source), "%s", src_j->valuestring);
         if (cJSON_IsString(ref_j))
            snprintf(out[n].source_ref, sizeof(out[n].source_ref), "%s", ref_j->valuestring);
         if (cJSON_IsNumber(hit_j))
            out[n].hit_count = (int)hit_j->valuedouble;
         if (cJSON_IsNumber(conf_j))
            out[n].confidence = conf_j->valuedouble;
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_anti_pattern_bump(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   char *json = kb_v1_action_request("anti_pattern.bump", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_relations_schema_list(db2_relation_schema_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("relations.schema_list", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "rows");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsArray(rows))
   {
      cJSON_Delete(resp);
      return 0;
   }
   int n = 0;
   cJSON *r;
   cJSON_ArrayForEach(r, rows)
   {
      if (n >= max)
         break;
      cJSON *rid = cJSON_GetObjectItemCaseSensitive(r, "relation_id");
      cJSON *sk = cJSON_GetObjectItemCaseSensitive(r, "subject_kind");
      cJSON *ok = cJSON_GetObjectItemCaseSensitive(r, "object_kind");
      out[n].relation_id = cJSON_IsNumber(rid) ? (int)rid->valuedouble : 0;
      out[n].subject_kind = cJSON_IsNumber(sk) ? (int)sk->valuedouble : 0;
      out[n].object_kind = cJSON_IsNumber(ok) ? (int)ok->valuedouble : 0;
      n++;
   }
   cJSON_Delete(resp);
   return n;
}
