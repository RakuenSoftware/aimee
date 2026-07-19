/* learning_router.c: explicit-signal capture, proposal gate, and sink routing. */
#include "aimee.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db1.h"
#include "db2/artifacts.h"
#include "db2/collab_rules.h"
#include "db2/db2_learning.h"
#include "dogfood.h"
#endif
#include "cJSON.h"
#include "integrity.h"
#include "learning.h"
#include "log.h"
#include <time.h>

#define LEARNING_DEFAULT_TTL_DAYS             7
#define LEARNING_DEFAULT_MAX_COMMITS_PER_WEEK 25

/* Process-local signal-ingest latency telemetry. Written from the
 * one path that calls record-signal, read by operator tooling via
 * learning_router_metrics(). Not thread-safe by design — aimee-server
 * runs a single agent loop — but see learning_router_metrics_reset()
 * for test isolation. */
static int64_t g_learning_ingest_calls = 0;
static double g_learning_ingest_ms_sum = 0.0;
static double g_learning_ingest_ms_max = 0.0;

static int64_t g_learning_detection_calls = 0;
static double g_learning_detection_ms_sum = 0.0;
static double g_learning_detection_ms_max = 0.0;

#if !defined(AIMEE_DB2_DISABLED)
static void learning_ingest_record_ms(double ms)
{
   if (ms < 0.0)
      return;
   g_learning_ingest_calls++;
   g_learning_ingest_ms_sum += ms;
   if (ms > g_learning_ingest_ms_max)
      g_learning_ingest_ms_max = ms;
}
#endif

#if !defined(AIMEE_DB2_DISABLED)
static int learning_proposal_ttl_days(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return LEARNING_DEFAULT_TTL_DAYS;
   return cfg.learning_proposal_ttl_days > 0 ? cfg.learning_proposal_ttl_days
                                             : LEARNING_DEFAULT_TTL_DAYS;
}

static int learning_max_commits_per_week(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return LEARNING_DEFAULT_MAX_COMMITS_PER_WEEK;
   return cfg.learning_max_commits_per_week > 0 ? cfg.learning_max_commits_per_week
                                                : LEARNING_DEFAULT_MAX_COMMITS_PER_WEEK;
}
#endif

int learning_router_enabled(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return 1;
   return cfg.learning_router_enabled ? 1 : 0;
}

#if !defined(AIMEE_DB2_DISABLED)
static void learning_expiry_after_days(int days, char *buf, size_t len)
{
   time_t now = time(NULL);
   time_t then = now + (time_t)days * 86400;
   struct tm tmv;
#if defined(_WIN32)
   gmtime_s(&tmv, &then);
#else
   gmtime_r(&then, &tmv);
#endif
   strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tmv);
}
#endif

static void learning_dispatch_clear(learning_dispatch_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
}

#if !defined(AIMEE_DB2_DISABLED)
static void learning_log_dogfood(const learning_signal_input_t *input)
{
   dogfood_label_t label;
   memset(&label, 0, sizeof(label));
   char notes[96];
   snprintf(notes, sizeof(notes), "learning_router:%s", input->signal_type);
   label.notes = notes;
   if (strcmp(input->signal_type, "thumb_up") == 0)
      label.outcome = "hit";
   else if (strcmp(input->signal_type, "thumb_down") == 0)
      label.outcome = "partial";
   else if (strcmp(input->signal_type, "correction") == 0)
      label.outcome = "miss";
   else
      return;

   int64_t ids[1];
   int id_count = 0;
   if (input->target_memory_id > 0)
   {
      ids[0] = input->target_memory_id;
      id_count = 1;
   }
   dogfood_log_moment_tagged("learning_router", input->title[0] ? input->title : input->description,
                             id_count > 0 ? ids : NULL, id_count, &label);
}

static int learning_apply_sink(const learning_proposal_t *proposal)
{
   cJSON *action = cJSON_Parse(proposal->action_json);
   if (!action)
      return -1;

   int rc = 0;
   if (strcmp(proposal->sink, "reranker") == 0)
   {
      cJSON *js = cJSON_GetObjectItemCaseSensitive(action, "success");
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(action, "citation_ids");
      int success =
          cJSON_IsBool(js) ? (cJSON_IsTrue(js) ? 1 : 0) : (cJSON_IsNumber(js) ? js->valueint : 0);
      int64_t ids[32];
      int n = 0;
      if (cJSON_IsArray(jc))
      {
         cJSON *item = NULL;
         cJSON_ArrayForEach(item, jc)
         {
            if (n >= (int)(sizeof(ids) / sizeof(ids[0])))
               break;
            if (cJSON_IsNumber(item))
               ids[n++] = (int64_t)item->valuedouble;
         }
      }
      if (n == 0 && proposal->target_memory_id > 0)
         ids[n++] = proposal->target_memory_id;
      if (n > 0)
         rc = memory_apply_feedback(success, ids, n);
   }
   else if (strcmp(proposal->sink, "supersede") == 0)
   {
      cJSON *jold = cJSON_GetObjectItemCaseSensitive(action, "old_memory_id");
      cJSON *jnew = cJSON_GetObjectItemCaseSensitive(action, "new_content");
      if (!cJSON_IsNumber(jold) || !cJSON_IsString(jnew) || !jnew->valuestring[0])
         rc = -1;
      else
      {
         memory_t out;
         rc = memory_supersede((int64_t)jold->valuedouble, jnew->valuestring, 1.0, session_id(),
                               &out);
      }
   }
   else if (strcmp(proposal->sink, "rule") == 0)
   {
      cJSON *jtext = cJSON_GetObjectItemCaseSensitive(action, "text");
      cJSON *jreason = cJSON_GetObjectItemCaseSensitive(action, "reason");
      if (!cJSON_IsString(jtext) || !jtext->valuestring[0])
         rc = -1;
      else
         rc = db2_collab_rules_propose(jtext->valuestring,
                                       cJSON_IsString(jreason) ? jreason->valuestring : "",
                                       "learning_router") >= 0
                  ? 0
                  : -1;
   }
   else if (strcmp(proposal->sink, "workflow") == 0)
   {
      cJSON *jp = cJSON_GetObjectItemCaseSensitive(action, "project");
      cJSON *js = cJSON_GetObjectItemCaseSensitive(action, "signal_type");
      cJSON *jr = cJSON_GetObjectItemCaseSensitive(action, "rule");
      if (!cJSON_IsString(jp) || !cJSON_IsString(js) || !cJSON_IsString(jr) ||
          !jp->valuestring[0] || !js->valuestring[0] || !jr->valuestring[0])
         rc = -1;
      else
         rc = memory_upsert_workflow(jp->valuestring, js->valuestring, jr->valuestring, 1.0,
                                     session_id()) > 0
                  ? 0
                  : -1;
   }
   else if (strcmp(proposal->sink, "artifact") == 0)
   {
      /* ingress-compression §4: a failure-mined recurrence proposal commits to the
       * durable artifact it carried (e.g. workflow_pattern) only after review /
       * Gate-Promote, instead of kb_mining writing it directly. */
      cJSON *jid = cJSON_GetObjectItemCaseSensitive(action, "artifact_id");
      cJSON *jkind = cJSON_GetObjectItemCaseSensitive(action, "artifact_kind");
      cJSON *jscope = cJSON_GetObjectItemCaseSensitive(action, "scope_kind");
      cJSON *jpayload = cJSON_GetObjectItemCaseSensitive(action, "payload_json");
      cJSON *jconf = cJSON_GetObjectItemCaseSensitive(action, "confidence");
      if (!cJSON_IsString(jid) || !jid->valuestring[0] || !cJSON_IsString(jkind) ||
          !jkind->valuestring[0] || !cJSON_IsString(jpayload) || !jpayload->valuestring[0])
         rc = -1;
      else
         rc = db2_artifact_write(
             jid->valuestring, jkind->valuestring, "proposed",
             cJSON_IsString(jscope) ? jscope->valuestring : "workspace", "", "kb-mining-learning",
             cJSON_IsNumber(jconf) ? jconf->valuedouble : 0.5, jpayload->valuestring);
   }

   cJSON_Delete(action);
   return rc;
}

static int learning_commit_proposal(int id, learning_proposal_t *out)
{
   learning_proposal_t proposal;
   if (learning_get_proposal(id, &proposal) != 0)
      return -1;

   if (strcmp(proposal.state, "committed") == 0)
   {
      if (out)
         *out = proposal;
      return 0;
   }
   if (strcmp(proposal.state, "pending") != 0)
   {
      if (out)
         *out = proposal;
      return 0;
   }

   if (db2_learning_commits_in_last_7_days(proposal.sink) >= learning_max_commits_per_week())
   {
      db2_learning_proposal_archive(id, "weekly_cap");
      return learning_get_proposal(id, out ? out : &proposal);
   }

   if (learning_apply_sink(&proposal) != 0)
      return -1;

   db2_learning_proposal_mark_committed(id);
   return learning_get_proposal(id, out ? out : &proposal);
}

static int learning_queue_sink(int signal_id, const char *sink, const char *target_key,
                               int64_t target_memory_id, const char *action_json,
                               const char *evidence_refs, int high_confidence,
                               learning_dispatch_result_t *out)
{
   int proposal_id = db2_learning_proposal_find_pending(sink, target_key, target_memory_id);
   if (proposal_id > 0)
   {
      db2_learning_proposal_bump_corroboration(proposal_id);
      if (out && out->proposal_count < LEARNING_MAX_PROPOSAL_IDS)
         out->proposal_ids[out->proposal_count++] = proposal_id;
      learning_proposal_t proposal;
      if (learning_get_proposal(proposal_id, &proposal) == 0 && proposal.corroboration_count >= 2)
      {
         if (learning_commit_proposal(proposal_id, &proposal) == 0 &&
             strcmp(proposal.state, "committed") == 0 && out &&
             out->committed_count < LEARNING_MAX_PROPOSAL_IDS)
            out->committed_ids[out->committed_count++] = proposal_id;
      }
      return proposal_id;
   }

   {
      char expires_at[32];
      learning_expiry_after_days(learning_proposal_ttl_days(), expires_at, sizeof(expires_at));
      proposal_id = db2_learning_proposal_insert(signal_id, sink, target_key, target_memory_id,
                                                 action_json, evidence_refs, expires_at);
   }
   if (proposal_id <= 0)
      return -1;
   if (out && out->proposal_count < LEARNING_MAX_PROPOSAL_IDS)
      out->proposal_ids[out->proposal_count++] = proposal_id;

   if (high_confidence)
   {
      learning_proposal_t proposal;
      if (learning_commit_proposal(proposal_id, &proposal) == 0 &&
          strcmp(proposal.state, "committed") == 0 && out &&
          out->committed_count < LEARNING_MAX_PROPOSAL_IDS)
         out->committed_ids[out->committed_count++] = proposal_id;
   }
   return proposal_id;
}

static int learning_make_reranker_action(int success, int64_t target_memory_id,
                                         const char *evidence_refs_json, char *buf, size_t cap)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return -1;
   cJSON_AddBoolToObject(obj, "success", success ? 1 : 0);
   cJSON *ids = cJSON_AddArrayToObject(obj, "citation_ids");
   if (target_memory_id > 0)
      cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)target_memory_id));
   else if (evidence_refs_json)
   {
      cJSON *parsed = cJSON_Parse(evidence_refs_json);
      if (cJSON_IsArray(parsed))
      {
         cJSON *item = NULL;
         cJSON_ArrayForEach(item, parsed) if (cJSON_IsNumber(item))
             cJSON_AddItemToArray(ids, cJSON_CreateNumber(item->valuedouble));
      }
      cJSON_Delete(parsed);
   }
   char *rendered = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (!rendered)
      return -1;
   snprintf(buf, cap, "%s", rendered);
   free(rendered);
   return 0;
}

static int learning_make_supersede_action(int64_t target_memory_id, const char *correction_text,
                                          char *buf, size_t cap)
{
   cJSON *obj = cJSON_CreateObject();
   char *rendered = NULL;
   if (!obj)
      return -1;
   cJSON_AddNumberToObject(obj, "old_memory_id", (double)target_memory_id);
   cJSON_AddStringToObject(obj, "new_content", correction_text ? correction_text : "");
   rendered = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (!rendered)
      return -1;
   snprintf(buf, cap, "%s", rendered);
   free(rendered);
   return 0;
}

static int learning_make_rule_action(const char *text, const char *reason, char *buf, size_t cap)
{
   cJSON *obj = cJSON_CreateObject();
   char *rendered = NULL;
   if (!obj)
      return -1;
   cJSON_AddStringToObject(obj, "text", text ? text : "");
   cJSON_AddStringToObject(obj, "reason", reason ? reason : "");
   rendered = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (!rendered)
      return -1;
   snprintf(buf, cap, "%s", rendered);
   free(rendered);
   return 0;
}

static int learning_make_workflow_action(const char *project, const char *signal_type,
                                         const char *rule, char *buf, size_t cap)
{
   cJSON *obj = cJSON_CreateObject();
   char *rendered = NULL;
   if (!obj)
      return -1;
   cJSON_AddStringToObject(obj, "project", project ? project : "");
   cJSON_AddStringToObject(obj, "signal_type", signal_type ? signal_type : "");
   cJSON_AddStringToObject(obj, "rule", rule ? rule : "");
   rendered = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (!rendered)
      return -1;
   snprintf(buf, cap, "%s", rendered);
   free(rendered);
   return 0;
}

static void learning_fill_target_key(learning_signal_input_t *input)
{
   if (input->target_key[0] || input->target_memory_id <= 0)
      return;

   memory_t memory;
   if (memory_get(input->target_memory_id, &memory) == 0)
      snprintf(input->target_key, sizeof(input->target_key), "%s", memory.key);
}
#endif

int learning_router_record_signal(const learning_signal_input_t *raw_input,
                                  learning_dispatch_result_t *out)
{
   if (!raw_input || !raw_input->signal_type[0])
      return -1;

#if defined(AIMEE_DB2_DISABLED)
   learning_dispatch_clear(out);
   return -1;
#else
   /* Integrity gate: shadow/dry_run mode — log what the gate would do without
    * changing routing.  Runs before signal insert so hostile delegate/tool
    * content can be surfaced in evidence even in Phase 0. */
   {
      config_t icfg;
      if (config_load(&icfg) == 0 && icfg.integrity_enabled)
      {
         const char *text = raw_input->description[0]       ? raw_input->description
                            : raw_input->correction_text[0] ? raw_input->correction_text
                                                            : raw_input->title;
         if (text && text[0])
         {
            /* Signals from explicit user actions are user_stated; implicit
             * substrate signals (workflow, tool_outcome) are tool-origin. */
            integrity_source_t src = INTEGRITY_SOURCE_TOOL;
            if (strcmp(raw_input->signal_type, "thumb_up") == 0 ||
                strcmp(raw_input->signal_type, "thumb_down") == 0 ||
                strcmp(raw_input->signal_type, "correction") == 0 ||
                strcmp(raw_input->signal_type, "preference_statement") == 0 ||
                strcmp(raw_input->signal_type, "mark_rule") == 0)
               src = INTEGRITY_SOURCE_USER_STATED;

            integrity_result_t gate = integrity_gate_check(text, src);
            if (gate.verdict != INTEGRITY_VERDICT_ACCEPT)
            {
               LOG_WARN("integrity", "gate: signal=%s source=%s verdict=%s category=%s dry_run=%d",
                        raw_input->signal_type, integrity_source_name(src),
                        integrity_verdict_name(gate.verdict), gate.match_category,
                        icfg.integrity_dry_run);
               if (!icfg.integrity_dry_run && src != INTEGRITY_SOURCE_USER_STATED &&
                   gate.verdict == INTEGRITY_VERDICT_REJECT)
               {
                  learning_dispatch_clear(out);
                  return -1;
               }
            }
         }
      }
   }
   /* Time the full ingest path — signal insert, expired-archive
    * sweep, dogfood log, sink dispatch, and any inline commits. The
    * p95 across real traffic is what operators watch to catch a
    * sink-layer slowdown before it bleeds into the agent hot path. */
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   learning_signal_input_t input = *raw_input;
   learning_dispatch_clear(out);
   learning_fill_target_key(&input);
   db2_learning_proposals_archive_expired();

   int signal_id = db2_learning_signal_insert(&input, session_id());
   if (signal_id <= 0)
      return -1;
   if (out)
      out->signal_id = signal_id;

   learning_log_dogfood(&input);

   if (strcmp(input.signal_type, "thumb_up") == 0 || strcmp(input.signal_type, "thumb_down") == 0)
   {
      char action[512];
      if (learning_make_reranker_action(strcmp(input.signal_type, "thumb_up") == 0,
                                        input.target_memory_id, input.evidence_refs_json, action,
                                        sizeof(action)) == 0)
         learning_queue_sink(signal_id, "reranker", input.target_key, input.target_memory_id,
                             action, input.evidence_refs_json, 0, out);
   }
   else if (strcmp(input.signal_type, "correction") == 0)
   {
      char action[2048];
      if ((input.target_memory_id > 0 ||
           (input.evidence_refs_json && input.evidence_refs_json[0])) &&
          learning_make_reranker_action(0, input.target_memory_id, input.evidence_refs_json, action,
                                        sizeof(action)) == 0)
         learning_queue_sink(signal_id, "reranker", input.target_key, input.target_memory_id,
                             action, input.evidence_refs_json, 0, out);

      if (input.target_memory_id > 0 && input.correction_text[0] &&
          learning_make_supersede_action(input.target_memory_id, input.correction_text, action,
                                         sizeof(action)) == 0)
         learning_queue_sink(signal_id, "supersede", input.target_key, input.target_memory_id,
                             action, input.evidence_refs_json, 0, out);

      if (input.correction_text[0] || input.title[0] || input.description[0])
      {
         char rule_text[COLLAB_RULE_TEXT_LEN + 1];
         char reason[COLLAB_RULE_REASON_LEN + 1];
         if (input.target_key[0] && input.correction_text[0])
            snprintf(rule_text, sizeof(rule_text), "For %s, use %s", input.target_key,
                     input.correction_text);
         else if (input.title[0] && input.correction_text[0])
            snprintf(rule_text, sizeof(rule_text), "For %s, use %s", input.title,
                     input.correction_text);
         else
            snprintf(rule_text, sizeof(rule_text), "%s",
                     input.correction_text[0] ? input.correction_text : input.description);
         snprintf(reason, sizeof(reason), "Repeated correction captured by learning router");
         if (learning_make_rule_action(rule_text, reason, action, sizeof(action)) == 0)
            learning_queue_sink(signal_id, "rule", input.target_key, input.target_memory_id, action,
                                input.evidence_refs_json, 0, out);
      }
   }
   else if (strcmp(input.signal_type, "preference_statement") == 0 ||
            strcmp(input.signal_type, "mark_rule") == 0)
   {
      char action[2048];
      char rule_text[COLLAB_RULE_TEXT_LEN + 1];
      char reason[COLLAB_RULE_REASON_LEN + 1];
      snprintf(rule_text, sizeof(rule_text), "%s",
               input.description[0] ? input.description : input.title);
      snprintf(reason, sizeof(reason), "%s",
               strcmp(input.signal_type, "mark_rule") == 0 ? "Explicit rule capture"
                                                           : "Explicit user preference");
      if (learning_make_rule_action(rule_text, reason, action, sizeof(action)) == 0)
         learning_queue_sink(signal_id, "rule", input.target_key, input.target_memory_id, action,
                             input.evidence_refs_json,
                             input.high_confidence || strcmp(input.signal_type, "mark_rule") == 0,
                             out);
   }
   else if (strcmp(input.signal_type, "workflow_repetition") == 0 && input.workflow_project[0] &&
            input.workflow_signal_type[0] && input.description[0])
   {
      char action[1024];
      if (learning_make_workflow_action(input.workflow_project, input.workflow_signal_type,
                                        input.description, action, sizeof(action)) == 0)
         learning_queue_sink(signal_id, "workflow", input.workflow_project, 0, action,
                             input.evidence_refs_json, 0, out);
   }

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   learning_ingest_record_ms(ms);

   return signal_id;
#endif
}

int learning_list_proposals(const char *state, const char *sink, int limit,
                            learning_proposal_t *out, int max)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)state;
   (void)sink;
   (void)limit;
   (void)out;
   (void)max;
   return -1;
#else
   db2_learning_proposals_archive_expired();
   return db2_learning_proposal_list(state, sink, limit, out, max);
#endif
}

int learning_get_proposal(int id, learning_proposal_t *out)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)id;
   (void)out;
   return -1;
#else
   db2_learning_proposals_archive_expired();
   return db2_learning_proposal_get(id, out);
#endif
}

int learning_accept_proposal(int id, learning_proposal_t *out)
{
   if (id <= 0)
      return -1;
#if defined(AIMEE_DB2_DISABLED)
   (void)out;
   return -1;
#else
   db2_learning_proposals_archive_expired();
   return learning_commit_proposal(id, out);
#endif
}

int learning_reject_proposal(int id, learning_proposal_t *out)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)id;
   (void)out;
   return -1;
#else
   learning_proposal_t proposal;
   if (id <= 0 || learning_get_proposal(id, &proposal) != 0)
      return -1;

   if (strcmp(proposal.state, "committed") != 0 && strcmp(proposal.state, "archived") != 0)
      db2_learning_proposal_archive(id, "rejected");
   return learning_get_proposal(id, out ? out : &proposal);
#endif
}

struct cJSON *learning_proposal_to_json(const learning_proposal_t *proposal)
{
   if (!proposal)
      return NULL;

   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", proposal->id);
   cJSON_AddNumberToObject(obj, "signal_id", proposal->signal_id);
   cJSON_AddStringToObject(obj, "sink", proposal->sink);
   cJSON_AddStringToObject(obj, "state", proposal->state);
   cJSON_AddStringToObject(obj, "target_key", proposal->target_key);
   cJSON_AddNumberToObject(obj, "target_memory_id", (double)proposal->target_memory_id);
   cJSON_AddStringToObject(obj, "action_json", proposal->action_json);
   cJSON_AddStringToObject(obj, "evidence_refs", proposal->evidence_refs);
   cJSON_AddNumberToObject(obj, "corroboration_count", proposal->corroboration_count);
   cJSON_AddStringToObject(obj, "expires_at", proposal->expires_at);
   cJSON_AddStringToObject(obj, "committed_at", proposal->committed_at);
   cJSON_AddStringToObject(obj, "archive_reason", proposal->archive_reason);
   cJSON_AddStringToObject(obj, "created_at", proposal->created_at);
   cJSON_AddStringToObject(obj, "updated_at", proposal->updated_at);
   return obj;
}

static void ljson_copy_str(cJSON *obj, const char *key, char *dst, size_t cap)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(dst, cap, "%s", item->valuestring);
   else
      dst[0] = '\0';
}

int learning_proposal_from_json(const struct cJSON *obj, learning_proposal_t *out)
{
   if (!obj || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *src = (cJSON *)obj;
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(src, "id");
   cJSON *sig_j = cJSON_GetObjectItemCaseSensitive(src, "signal_id");
   cJSON *mem_j = cJSON_GetObjectItemCaseSensitive(src, "target_memory_id");
   cJSON *cor_j = cJSON_GetObjectItemCaseSensitive(src, "corroboration_count");
   if (!cJSON_IsNumber(id_j))
      return -1;
   out->id = (int)id_j->valuedouble;
   out->signal_id = cJSON_IsNumber(sig_j) ? (int)sig_j->valuedouble : 0;
   out->target_memory_id = cJSON_IsNumber(mem_j) ? (int64_t)mem_j->valuedouble : 0;
   out->corroboration_count = cJSON_IsNumber(cor_j) ? (int)cor_j->valuedouble : 0;
   ljson_copy_str(src, "sink", out->sink, sizeof(out->sink));
   ljson_copy_str(src, "state", out->state, sizeof(out->state));
   ljson_copy_str(src, "target_key", out->target_key, sizeof(out->target_key));
   ljson_copy_str(src, "action_json", out->action_json, sizeof(out->action_json));
   ljson_copy_str(src, "evidence_refs", out->evidence_refs, sizeof(out->evidence_refs));
   ljson_copy_str(src, "expires_at", out->expires_at, sizeof(out->expires_at));
   ljson_copy_str(src, "committed_at", out->committed_at, sizeof(out->committed_at));
   ljson_copy_str(src, "archive_reason", out->archive_reason, sizeof(out->archive_reason));
   ljson_copy_str(src, "created_at", out->created_at, sizeof(out->created_at));
   ljson_copy_str(src, "updated_at", out->updated_at, sizeof(out->updated_at));
   return 0;
}

struct cJSON *learning_dispatch_result_to_json(const learning_dispatch_result_t *result)
{
   if (!result)
      return NULL;
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "signal_id", result->signal_id);
   cJSON *proposal_ids = cJSON_AddArrayToObject(obj, "proposal_ids");
   cJSON *committed_ids = cJSON_AddArrayToObject(obj, "committed_ids");
   for (int i = 0; i < result->proposal_count; i++)
      cJSON_AddItemToArray(proposal_ids, cJSON_CreateNumber(result->proposal_ids[i]));
   for (int i = 0; i < result->committed_count; i++)
      cJSON_AddItemToArray(committed_ids, cJSON_CreateNumber(result->committed_ids[i]));
   return obj;
}

/* --- Phase-2 metrics --- */

#if !defined(AIMEE_DB2_DISABLED)
static const char *LEARNING_CANONICAL_SINKS[LEARNING_SINK_COUNT] = {"reranker", "supersede", "rule",
                                                                    "workflow"};
#endif

int learning_metrics_commit_ratio(int window_days, learning_commit_ratio_t *out)
{
   if (!out)
      return -1;
   if (window_days <= 0)
      window_days = LEARNING_METRICS_DEFAULT_WINDOW_DAYS;

   memset(out, 0, sizeof(*out));
   out->window_days = window_days;

#if defined(AIMEE_DB2_DISABLED)
   return -1;
#else
   /* Settled-only denominator: committed + archived within the
    * window, excluding still-pending rows so the ratio reflects the
    * operator's actual accept/reject decisions rather than backlog
    * age. */
   if (db2_learning_proposals_settled_counts(window_days, &out->proposals_committed,
                                             &out->proposals_terminal) != 0)
      return -1;
   if (out->proposals_terminal > 0)
      out->commit_ratio = (double)out->proposals_committed / (double)out->proposals_terminal;
   return 0;
#endif
}

int learning_metrics_per_sink_caps(learning_sink_cap_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
#if defined(AIMEE_DB2_DISABLED)
   return -1;
#else
   int cap = learning_max_commits_per_week();
   int n = max < LEARNING_SINK_COUNT ? max : LEARNING_SINK_COUNT;
   for (int i = 0; i < n; i++)
   {
      memset(&out[i], 0, sizeof(out[i]));
      snprintf(out[i].sink, sizeof(out[i].sink), "%s", LEARNING_CANONICAL_SINKS[i]);
      out[i].committed_this_week = db2_learning_commits_in_last_7_days(LEARNING_CANONICAL_SINKS[i]);
      out[i].weekly_cap = cap;
      if (cap > 0)
         out[i].utilization = (double)out[i].committed_this_week / (double)cap;
   }
   return n;
#endif
}

void learning_router_metrics(int64_t *ingest_calls, double *ingest_ms_avg, double *ingest_ms_max)
{
   if (ingest_calls)
      *ingest_calls = g_learning_ingest_calls;
   if (ingest_ms_avg)
      *ingest_ms_avg = g_learning_ingest_calls > 0
                           ? g_learning_ingest_ms_sum / (double)g_learning_ingest_calls
                           : 0.0;
   if (ingest_ms_max)
      *ingest_ms_max = g_learning_ingest_ms_max;
}

void learning_router_metrics_reset(void)
{
   g_learning_ingest_calls = 0;
   g_learning_ingest_ms_sum = 0.0;
   g_learning_ingest_ms_max = 0.0;
}

void learning_router_record_detection_ms(double ms)
{
   if (ms < 0.0)
      return;
   g_learning_detection_calls++;
   g_learning_detection_ms_sum += ms;
   if (ms > g_learning_detection_ms_max)
      g_learning_detection_ms_max = ms;
}

void learning_router_detection_metrics(int64_t *detection_calls, double *detection_ms_avg,
                                       double *detection_ms_max)
{
   if (detection_calls)
      *detection_calls = g_learning_detection_calls;
   if (detection_ms_avg)
      *detection_ms_avg = g_learning_detection_calls > 0
                              ? g_learning_detection_ms_sum / (double)g_learning_detection_calls
                              : 0.0;
   if (detection_ms_max)
      *detection_ms_max = g_learning_detection_ms_max;
}

void learning_router_detection_metrics_reset(void)
{
   g_learning_detection_calls = 0;
   g_learning_detection_ms_sum = 0.0;
   g_learning_detection_ms_max = 0.0;
}
