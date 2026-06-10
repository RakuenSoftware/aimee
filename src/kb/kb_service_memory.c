/* kb_service_memory.c: aimee-kb dispatch handlers for the memory.*
 * RPC family (find_facts, list, get, briefing, context_block,
 * entity_profile, entity_edges, search_graph, get_episode, ask,
 * store).  Split out of kb_service.c so the file stays under the
 * per-file line cap. */

#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include "config.h"
#include "db2/kb_service_backend.h"
#include "db2/bandit.h"
#include "kb_bandit.h"
#include "kb_bandit_registry.h"
#include "kb_service_memory.h"
#include "log.h"
#include "memory_graph_fusion.h"

#include <stdlib.h>

/* Defined in kb_service.c; non-static so this file can call them. */
int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);
int kb_reply_or_error(int fd, cJSON *resp, const char *err_msg);

int kb_handle_memory_find_facts(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "memory.find_facts requires query");
   int limit;
   if (cJSON_IsNumber(limit_j))
      limit = (int)limit_j->valuedouble;
   else
   {
      /* Default = the promoted arm (if an operator locked one in via
       * `aimee optimize promote --apply`), else 20. A live bandit sample below
       * overrides this when exploration is enabled. */
      limit = 20;
      char promo[KB_BANDIT_MAX_ARM_ID] = "";
      if (db2_bandit_promotion_get("kb_memory_retrieval_limit", promo, sizeof(promo)) == 0)
      {
         int p = atoi(promo);
         if (p > 0)
            limit = p;
      }
   }

   /* Shadow bandit for memory retrieval limit (no explicit limit from caller).
    * Sample an arm now; carry the decision id + chosen arm so the reward can be
    * attributed from the recall result below — otherwise the loop never closes
    * and the arm posteriors never learn from live traffic. */
   char ml_decision_id[KB_BANDIT_MAX_DECISION] = {0};
   char ml_arm_id[KB_BANDIT_MAX_ARM_ID] = {0};
   const kb_bandit_decision_point_t *ml_dp = kb_bandit_registry_get("kb_memory_retrieval_limit");
   if (!cJSON_IsNumber(limit_j) && ml_dp && ml_dp->n_arms > 0)
   {
      config_t cfg;
      config_load(&cfg);
      if (cfg.bandit_live_decision_enabled)
      {
         /* Arms come from the registry (source of truth); each arm id is the
          * literal retrieval limit. */
         char ml_arms[KB_BANDIT_MAX_ARMS][KB_BANDIT_MAX_ARM_ID];
         for (int i = 0; i < ml_dp->n_arms; i++)
            snprintf(ml_arms[i], KB_BANDIT_MAX_ARM_ID, "%s", ml_dp->arms[i]);

         int ml_arm =
             kb_bandit_sample(&cfg, ml_dp->id, NULL, ml_arms, ml_dp->n_arms, ml_decision_id);
         if (ml_arm >= 0 && ml_arm < ml_dp->n_arms)
         {
            aimee_log(LOG_DEBUG, "kb.bandit", "%s arm=%s", ml_dp->id, ml_arms[ml_arm]);
            int arm_limit = atoi(ml_arms[ml_arm]);
            if (arm_limit > 0)
               limit = arm_limit;
            snprintf(ml_arm_id, sizeof(ml_arm_id), "%s", ml_arms[ml_arm]);
         }
      }
   }

   /* Graph-code fusion: honour the request's graph_code_fusion_state ("off" |
    * "shadow" | "on") for the duration of this recall, then clear it. "on" runs
    * the graph-vector fusion rerank in the recall path; anything else leaves the
    * order unchanged. Thread-local, so concurrent kb workers don't interfere. */
   cJSON *fusion_j = cJSON_GetObjectItemCaseSensitive(req, "graph_code_fusion_state");
   memory_fusion_state_set(cJSON_IsString(fusion_j) ? fusion_j->valuestring : NULL);
   cJSON *resp = db2_kb_service_memory_find_facts_json(query_j->valuestring, limit);
   memory_fusion_state_clear();

   /* Close the bandit loop: attribute an immediate recall-sufficiency reward to
    * the sampled decision so the arm posteriors update from live traffic. */
   if (ml_dp && ml_decision_id[0] && ml_arm_id[0])
   {
      cJSON *facts = resp ? cJSON_GetObjectItemCaseSensitive(resp, "facts") : NULL;
      int n_results = cJSON_IsArray(facts) ? cJSON_GetArraySize(facts) : 0;
      double reward = kb_bandit_recall_sufficiency_reward(n_results, limit);
      kb_bandit_reward(NULL, ml_dp->id, ml_decision_id, ml_arm_id, reward);
   }
   return kb_reply_or_error(fd, resp, "failed to search memory facts");
}

int kb_handle_memory_list(int fd, cJSON *req)
{
   cJSON *tier_j = cJSON_GetObjectItemCaseSensitive(req, "tier");
   cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(req, "kind");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *tier =
       (cJSON_IsString(tier_j) && tier_j->valuestring[0]) ? tier_j->valuestring : NULL;
   const char *kind =
       (cJSON_IsString(kind_j) && kind_j->valuestring[0]) ? kind_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 20;

   cJSON *resp = db2_kb_service_memory_list_json(tier, kind, limit);
   return kb_reply_or_error(fd, resp, "failed to list memories");
}

static int kb_handle_session_briefing_section(int fd, cJSON *req, cJSON *(*fn)(int limit),
                                              const char *err_msg)
{
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 0;
   cJSON *resp = fn(limit);
   if (!resp)
      return kb_send_error(fd, err_msg);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_memory_prospective_list(int fd, cJSON *req)
{
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *state =
       (cJSON_IsString(state_j) && state_j->valuestring[0]) ? state_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 50;
   cJSON *resp = db2_kb_service_memory_prospective_list_json(state, limit);
   return kb_reply_or_error(fd, resp, "failed to list prospective memories");
}

int kb_handle_memory_prospective_create(int fd, cJSON *req)
{
   cJSON *t = cJSON_GetObjectItemCaseSensitive(req, "trigger_text");
   cJSON *a = cJSON_GetObjectItemCaseSensitive(req, "action_text");
   if (!cJSON_IsString(t) || !t->valuestring[0])
      return kb_send_error(fd, "missing trigger_text");
   if (!cJSON_IsString(a) || !a->valuestring[0])
      return kb_send_error(fd, "missing action_text");
   cJSON *ae = cJSON_GetObjectItemCaseSensitive(req, "anchor_entity");
   cJSON *af = cJSON_GetObjectItemCaseSensitive(req, "anchor_file");
   cJSON *re = cJSON_GetObjectItemCaseSensitive(req, "recurrence");
   cJSON *vu = cJSON_GetObjectItemCaseSensitive(req, "valid_until");
   const char *ae_s = cJSON_IsString(ae) ? ae->valuestring : "";
   const char *af_s = cJSON_IsString(af) ? af->valuestring : "";
   const char *re_s = cJSON_IsString(re) ? re->valuestring : NULL;
   const char *vu_s = cJSON_IsString(vu) ? vu->valuestring : "";
   cJSON *resp = db2_kb_service_memory_prospective_create_json(t->valuestring, a->valuestring, ae_s,
                                                               af_s, re_s, vu_s);
   return kb_reply_or_error(fd, resp, "failed to create prospective memory");
}

int kb_handle_directive_create(int fd, cJSON *req)
{
   cJSON *q = cJSON_GetObjectItemCaseSensitive(req, "question");
   cJSON *topic = cJSON_GetObjectItemCaseSensitive(req, "topic");
   cJSON *entity = cJSON_GetObjectItemCaseSensitive(req, "entity");
   cJSON *file = cJSON_GetObjectItemCaseSensitive(req, "file");
   cJSON *cause = cJSON_GetObjectItemCaseSensitive(req, "cause");
   cJSON *pri = cJSON_GetObjectItemCaseSensitive(req, "priority");
   cJSON *session = cJSON_GetObjectItemCaseSensitive(req, "session");
   cJSON *valid_until = cJSON_GetObjectItemCaseSensitive(req, "valid_until");
   if (!cJSON_IsString(q) || !q->valuestring[0])
      return kb_send_error(fd, "missing question");

   const char *question = q->valuestring;
   const char *topic_s = (cJSON_IsString(topic) && topic->valuestring[0]) ? topic->valuestring : "";
   const char *entity_s =
       (cJSON_IsString(entity) && entity->valuestring[0]) ? entity->valuestring : "";
   const char *file_s = (cJSON_IsString(file) && file->valuestring[0]) ? file->valuestring : "";
   const char *cause_s = (cJSON_IsString(cause) && cause->valuestring[0])
                             ? cause->valuestring
                             : MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP;
   int priority = cJSON_IsNumber(pri) ? (int)pri->valuedouble : 50;
   const char *session_s =
       (cJSON_IsString(session) && session->valuestring[0]) ? session->valuestring : "";
   const char *valid_s =
       (cJSON_IsString(valid_until) && valid_until->valuestring[0]) ? valid_until->valuestring : "";

   int dedup = 0;
   cJSON *directive = NULL;
   int rc = db2_kb_service_directive_create(question, topic_s, entity_s, file_s, cause_s, priority,
                                            0, 0, "", session_s, valid_s, &dedup, &directive);
   if (rc != 0)
      return kb_send_error(fd, "memory directive create failed");

   cJSON *resp = jo_ok();
   cJSON_AddBoolToObject(resp, "dedup", dedup ? 1 : 0);
   if (directive)
      cJSON_AddItemToObject(resp, "directive", directive);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_directive_resolve(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *mem_j = cJSON_GetObjectItemCaseSensitive(req, "with_memory");
   cJSON *note_j = cJSON_GetObjectItemCaseSensitive(req, "note");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   int64_t id = (int64_t)id_j->valuedouble;
   int64_t with_memory = cJSON_IsNumber(mem_j) ? (int64_t)mem_j->valuedouble : 0;
   const char *note =
       (cJSON_IsString(note_j) && note_j->valuestring[0]) ? note_j->valuestring : NULL;

   int rc = db2_kb_service_directive_resolve(id, with_memory, note);

   if (rc != 0)
      return kb_send_error(fd, "could not resolve directive (not open or missing)");

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "id", (double)id);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_directive_suppress(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   int64_t id = (int64_t)id_j->valuedouble;

   int rc = db2_kb_service_directive_suppress(id);

   if (rc != 0)
      return kb_send_error(fd, "could not suppress directive (not open or missing)");

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "id", (double)id);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_directive_sweep_expired(int fd, cJSON *req)
{
   (void)req;
   int n = db2_kb_service_directive_sweep_expired();
   if (n < 0)
      return kb_send_error(fd, "failed to sweep expired directives");

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "expired", n);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_directive_list(int fd, cJSON *req)
{
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   cJSON *cause_j = cJSON_GetObjectItemCaseSensitive(req, "cause");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *state =
       (cJSON_IsString(state_j) && state_j->valuestring[0]) ? state_j->valuestring : NULL;
   const char *cause =
       (cJSON_IsString(cause_j) && cause_j->valuestring[0]) ? cause_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 50;
   if (limit < 1)
      limit = 1;
   if (limit > 256)
      limit = 256;

   cJSON *resp = db2_kb_service_directive_list_json(state, cause, limit);
   return kb_reply_or_error(fd, resp, "failed to list directives");
}

int kb_handle_memory_episode_card_generate(int fd, cJSON *req)
{
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "source_session");
   if (!cJSON_IsString(sid_j) || !sid_j->valuestring[0])
      return kb_send_error(fd, "missing source_session");
   cJSON *resp = db2_kb_service_memory_episode_card_generate_json(sid_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to generate episode card");
}

int kb_handle_memory_scope_visibility_rank(int fd, cJSON *req)
{
   cJSON *ids_j = cJSON_GetObjectItemCaseSensitive(req, "ids");
   cJSON *ws_j = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   cJSON *pr_j = cJSON_GetObjectItemCaseSensitive(req, "project");
   if (!cJSON_IsArray(ids_j))
      return kb_send_error(fd, "missing ids array");
   int n = cJSON_GetArraySize(ids_j);
   if (n < 0)
      n = 0;
   if (n > 256)
      n = 256;
   int64_t *ids = n > 0 ? calloc((size_t)n, sizeof(int64_t)) : NULL;
   for (int i = 0; i < n; i++)
   {
      cJSON *it = cJSON_GetArrayItem(ids_j, i);
      ids[i] = (int64_t)(cJSON_IsNumber(it) ? it->valuedouble : 0);
   }
   const char *ws = (cJSON_IsString(ws_j) && ws_j->valuestring[0]) ? ws_j->valuestring : NULL;
   const char *pr = (cJSON_IsString(pr_j) && pr_j->valuestring[0]) ? pr_j->valuestring : NULL;
   cJSON *resp = db2_kb_service_memory_scope_visibility_rank_json(ids, n, ws, pr);
   free(ids);
   return kb_reply_or_error(fd, resp, "failed to compute scope visibility ranks");
}

int kb_handle_memory_tag_workspace(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   cJSON *ws_j = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(ws_j))
      return kb_send_error(fd, "missing memory_id or workspace");
   cJSON *resp =
       db2_kb_service_memory_tag_workspace_json((int64_t)id_j->valuedouble, ws_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to tag workspace");
}

int kb_handle_memory_tag_scope(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   cJSON *st_j = cJSON_GetObjectItemCaseSensitive(req, "scope_type");
   cJSON *sv_j = cJSON_GetObjectItemCaseSensitive(req, "scope_value");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(st_j) || !cJSON_IsString(sv_j))
      return kb_send_error(fd, "missing memory_id, scope_type or scope_value");
   cJSON *resp = db2_kb_service_memory_tag_scope_json((int64_t)id_j->valuedouble, st_j->valuestring,
                                                      sv_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to tag scope");
}

int kb_handle_memory_get_provenance(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing memory_id");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : MAX_PROVENANCE_ENTRIES;
   cJSON *resp = db2_kb_service_memory_get_provenance_json((int64_t)id_j->valuedouble, max);
   return kb_reply_or_error(fd, resp, "failed to get provenance");
}

int kb_handle_memory_prospective_match(int fd, cJSON *req)
{
   cJSON *t = cJSON_GetObjectItemCaseSensitive(req, "turn_text");
   cJSON *ae = cJSON_GetObjectItemCaseSensitive(req, "active_entity");
   cJSON *af = cJSON_GetObjectItemCaseSensitive(req, "active_file");
   cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "max");
   const char *t_s = cJSON_IsString(t) ? t->valuestring : NULL;
   const char *ae_s = cJSON_IsString(ae) ? ae->valuestring : NULL;
   const char *af_s = cJSON_IsString(af) ? af->valuestring : NULL;
   int max = cJSON_IsNumber(m) ? (int)m->valuedouble : 3;
   cJSON *resp = db2_kb_service_memory_prospective_match_json(t_s, ae_s, af_s, max);
   return kb_reply_or_error(fd, resp, "failed to match prospective memories");
}

int kb_handle_memory_prospective_mark_triggered(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   cJSON *resp = db2_kb_service_memory_prospective_mark_triggered_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to mark prospective triggered");
}

int kb_handle_memory_list_conflicts(int fd, cJSON *req)
{
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 64;
   cJSON *resp = db2_kb_service_memory_list_conflicts_json(max);
   return kb_reply_or_error(fd, resp, "failed to list memory conflicts");
}

int kb_handle_memory_query_health(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_memory_query_health_json();
   return kb_reply_or_error(fd, resp, "failed to query memory health");
}

int kb_handle_memory_diagnose_scoped(int fd, cJSON *req)
{
   cJSON *q = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *st = cJSON_GetObjectItemCaseSensitive(req, "scope_type");
   cJSON *sv = cJSON_GetObjectItemCaseSensitive(req, "scope_value");
   cJSON *l = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(q))
      return kb_send_error(fd, "missing query");
   const char *st_s = (cJSON_IsString(st) && st->valuestring[0]) ? st->valuestring : NULL;
   const char *sv_s = (cJSON_IsString(sv) && sv->valuestring[0]) ? sv->valuestring : NULL;
   int limit = cJSON_IsNumber(l) ? (int)l->valuedouble : 10;
   cJSON *resp = db2_kb_service_memory_diagnose_scoped_json(q->valuestring, st_s, sv_s, limit);
   return kb_reply_or_error(fd, resp, "failed to diagnose memory");
}

int kb_handle_memory_explain_match(int fd, cJSON *req)
{
   cJSON *q = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   if (!cJSON_IsString(q) || !cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing query/memory_id");
   cJSON *resp =
       db2_kb_service_memory_explain_match_json(q->valuestring, (int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to explain match");
}

int kb_handle_memory_find_facts_visible(int fd, cJSON *req)
{
   cJSON *q = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *ws = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   cJSON *pr = cJSON_GetObjectItemCaseSensitive(req, "project");
   cJSON *l = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(q))
      return kb_send_error(fd, "missing query");
   const char *workspace = (cJSON_IsString(ws) && ws->valuestring[0]) ? ws->valuestring : NULL;
   const char *project = (cJSON_IsString(pr) && pr->valuestring[0]) ? pr->valuestring : NULL;
   int limit = cJSON_IsNumber(l) ? (int)l->valuedouble : 20;
   cJSON *resp =
       db2_kb_service_memory_find_facts_visible_json(q->valuestring, workspace, project, limit);
   return kb_reply_or_error(fd, resp, "failed to find visible facts");
}

int kb_handle_memory_find_facts_scoped(int fd, cJSON *req)
{
   cJSON *q = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *st = cJSON_GetObjectItemCaseSensitive(req, "scope_type");
   cJSON *sv = cJSON_GetObjectItemCaseSensitive(req, "scope_value");
   cJSON *l = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(q))
      return kb_send_error(fd, "missing query");
   const char *scope_type = cJSON_IsString(st) ? st->valuestring : "";
   const char *scope_value = cJSON_IsString(sv) ? sv->valuestring : "";
   int limit = cJSON_IsNumber(l) ? (int)l->valuedouble : 20;
   /* Honour graph_code_fusion_state for the duration of this recall (see
    * kb_handle_memory_find_facts). Thread-local; cleared after. */
   cJSON *fusion_j = cJSON_GetObjectItemCaseSensitive(req, "graph_code_fusion_state");
   memory_fusion_state_set(cJSON_IsString(fusion_j) ? fusion_j->valuestring : NULL);
   cJSON *resp =
       db2_kb_service_memory_find_facts_scoped_json(q->valuestring, scope_type, scope_value, limit);
   memory_fusion_state_clear();
   return kb_reply_or_error(fd, resp, "failed to find scoped facts");
}

int kb_handle_memory_export_jsonl(int fd, cJSON *req)
{
   cJSON *path_j = cJSON_GetObjectItemCaseSensitive(req, "path");
   if (!cJSON_IsString(path_j) || !path_j->valuestring[0])
      return kb_send_error(fd, "missing path");
   cJSON *resp = db2_kb_service_memory_export_jsonl_json(path_j->valuestring);
   return kb_reply_or_error(fd, resp, "memory export failed");
}

int kb_handle_memory_decisions_export_jsonl(int fd, cJSON *req)
{
   cJSON *path_j = cJSON_GetObjectItemCaseSensitive(req, "path");
   if (!cJSON_IsString(path_j) || !path_j->valuestring[0])
      return kb_send_error(fd, "missing path");
   cJSON *resp = db2_kb_service_memory_decisions_export_jsonl_json(path_j->valuestring);
   return kb_reply_or_error(fd, resp, "decisions export failed");
}

int kb_handle_memory_key_exists(int fd, cJSON *req)
{
   cJSON *key_j = cJSON_GetObjectItemCaseSensitive(req, "key");
   if (!cJSON_IsString(key_j))
      return kb_send_error(fd, "missing key");
   cJSON *resp = db2_kb_service_memory_key_exists_json(key_j->valuestring);
   return kb_reply_or_error(fd, resp, "key_exists check failed");
}

int kb_handle_memory_search(int fd, cJSON *req)
{
   cJSON *clusters_j = cJSON_GetObjectItemCaseSensitive(req, "clusters");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 10;
   cJSON *resp = db2_kb_service_memory_search_json(clusters_j, limit);
   return kb_reply_or_error(fd, resp, "failed to search memory windows");
}

int kb_handle_memory_assemble_context(int fd, cJSON *req)
{
   cJSON *t_j = cJSON_GetObjectItemCaseSensitive(req, "task_hint");
   const char *task = (cJSON_IsString(t_j) && t_j->valuestring[0]) ? t_j->valuestring : NULL;
   cJSON *resp = db2_kb_service_memory_assemble_context_json(task);
   return kb_reply_or_error(fd, resp, "failed to assemble context");
}

int kb_handle_memory_compact_windows(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_memory_compact_windows_json();
   return kb_reply_or_error(fd, resp, "failed to compact windows");
}

int kb_handle_memory_query_edges(int fd, cJSON *req)
{
   cJSON *ent_j = cJSON_GetObjectItemCaseSensitive(req, "entity");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   if (!cJSON_IsString(ent_j))
      return kb_send_error(fd, "missing entity");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 128;
   cJSON *resp = db2_kb_service_memory_query_edges_json(ent_j->valuestring, max);
   return kb_reply_or_error(fd, resp, "failed to query memory edges");
}

int kb_handle_memory_effectiveness_stats(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_memory_effectiveness_stats_json();
   return kb_reply_or_error(fd, resp, "failed to compute effectiveness stats");
}

int kb_handle_memory_delete(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   cJSON *resp = db2_kb_service_memory_delete_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to delete memory");
}

int kb_handle_memory_touch(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   cJSON *resp = db2_kb_service_memory_touch_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to touch memory");
}

int kb_handle_memory_update(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *content_j = cJSON_GetObjectItemCaseSensitive(req, "content");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(content_j))
      return kb_send_error(fd, "missing id or content");
   cJSON *resp =
       db2_kb_service_memory_update_json((int64_t)id_j->valuedouble, content_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to update memory");
}

int kb_handle_memory_reject(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   cJSON *reason_j = cJSON_GetObjectItemCaseSensitive(req, "reason");
   const char *reason = cJSON_IsString(reason_j) ? reason_j->valuestring : NULL;
   cJSON *resp = db2_kb_service_memory_reject_json((int64_t)id_j->valuedouble, reason);
   return kb_reply_or_error(fd, resp, "failed to reject memory");
}

int kb_handle_memory_stats(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_memory_stats_json();
   return kb_reply_or_error(fd, resp, "failed to compute memory stats");
}

int kb_handle_memory_link_create(int fd, cJSON *req)
{
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(req, "source_id");
   cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(req, "target_id");
   cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(req, "relation");
   if (!cJSON_IsNumber(src_j) || !cJSON_IsNumber(tgt_j) || !cJSON_IsString(rel_j))
      return kb_send_error(fd, "missing source_id/target_id/relation");
   cJSON *resp = db2_kb_service_memory_link_create_json(
       (int64_t)src_j->valuedouble, (int64_t)tgt_j->valuedouble, rel_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to create memory link");
}

int kb_handle_memory_link_query(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing memory_id");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 32;
   cJSON *resp = db2_kb_service_memory_link_query_json((int64_t)id_j->valuedouble, max);
   return kb_reply_or_error(fd, resp, "failed to query memory links");
}

int kb_handle_memory_link_delete(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "link_id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing link_id");
   cJSON *resp = db2_kb_service_memory_link_delete_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to delete memory link");
}

int kb_handle_memory_upsert_workflow(int fd, cJSON *req)
{
   cJSON *ws_j = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   cJSON *sig_j = cJSON_GetObjectItemCaseSensitive(req, "signal_type");
   cJSON *rule_j = cJSON_GetObjectItemCaseSensitive(req, "rule");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(req, "observed_confidence");
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   if (!cJSON_IsString(ws_j) || !ws_j->valuestring[0])
      return kb_send_error(fd, "missing workspace");
   if (!cJSON_IsString(sig_j) || !sig_j->valuestring[0])
      return kb_send_error(fd, "missing signal_type");
   if (!cJSON_IsString(rule_j) || !rule_j->valuestring[0])
      return kb_send_error(fd, "missing rule");
   double conf = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 0.6;
   const char *sid = cJSON_IsString(sid_j) ? sid_j->valuestring : "";
   cJSON *resp = db2_kb_service_memory_upsert_workflow_json(ws_j->valuestring, sig_j->valuestring,
                                                            rule_j->valuestring, conf, sid);
   return kb_reply_or_error(fd, resp, "failed to upsert workflow memory");
}

int kb_handle_memory_alerts(int fd, cJSON *req)
{
   cJSON *since_j = cJSON_GetObjectItemCaseSensitive(req, "since");
   const char *since =
       (cJSON_IsString(since_j) && since_j->valuestring[0]) ? since_j->valuestring : NULL;
   cJSON *resp = db2_kb_service_memory_alerts_json(since);
   return kb_reply_or_error(fd, resp, "failed to render memory alerts");
}

int kb_handle_memory_recall(int fd, cJSON *req)
{
   cJSON *task_j = cJSON_GetObjectItemCaseSensitive(req, "task_hint");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit_tokens");
   cJSON *start_j = cJSON_GetObjectItemCaseSensitive(req, "session_start");
   const char *task_hint =
       (cJSON_IsString(task_j) && task_j->valuestring[0]) ? task_j->valuestring : NULL;
   int limit_tokens = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 0;
   int session_start = cJSON_IsBool(start_j) ? (cJSON_IsTrue(start_j) ? 1 : 0) : 0;
   /* Honour graph_code_fusion_state across the recall assembly (its fact
    * sections retrieve through the fusion-aware ranking path). Thread-local. */
   cJSON *fusion_j = cJSON_GetObjectItemCaseSensitive(req, "graph_code_fusion_state");
   memory_fusion_state_set(cJSON_IsString(fusion_j) ? fusion_j->valuestring : NULL);
   cJSON *resp = db2_kb_service_memory_recall_json(task_hint, limit_tokens, session_start);
   memory_fusion_state_clear();
   return kb_reply_or_error(fd, resp, "failed to render memory recall");
}

int kb_handle_memory_prospective_sweep_expired(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_memory_prospective_sweep_expired_json();
   return kb_reply_or_error(fd, resp, "failed to sweep prospective memories");
}

int kb_handle_memory_maintenance_run(int fd, cJSON *req)
{
   cJSON *modes_j = cJSON_GetObjectItemCaseSensitive(req, "modes");
   cJSON *force_j = cJSON_GetObjectItemCaseSensitive(req, "force");
   cJSON *dry_j = cJSON_GetObjectItemCaseSensitive(req, "dry_run");
   unsigned int modes = cJSON_IsNumber(modes_j) ? (unsigned int)modes_j->valuedouble : 0;
   int force = cJSON_IsBool(force_j) ? (cJSON_IsTrue(force_j) ? 1 : 0) : 0;
   int dry_run = cJSON_IsBool(dry_j) ? (cJSON_IsTrue(dry_j) ? 1 : 0) : 0;
   cJSON *resp = db2_kb_service_memory_maintenance_run_json(modes, force, dry_run);
   return kb_reply_or_error(fd, resp, "failed to run memory maintenance");
}

int kb_handle_memory_lint(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_memory_lint_json();
   return kb_reply_or_error(fd, resp, "failed to run memory lint");
}

int kb_handle_memory_prospective_complete(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   cJSON *resp = db2_kb_service_memory_prospective_complete_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to complete prospective memory");
}

int kb_handle_session_briefing_commitments(int fd, cJSON *req)
{
   return kb_handle_session_briefing_section(fd, req,
                                             db2_kb_service_session_briefing_commitments_json,
                                             "failed to render session-briefing commitments");
}

int kb_handle_session_briefing_directives(int fd, cJSON *req)
{
   return kb_handle_session_briefing_section(fd, req,
                                             db2_kb_service_session_briefing_directives_json,
                                             "failed to render session-briefing directives");
}

int kb_handle_memory_top_l2_facts(int fd, cJSON *req)
{
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 5;
   cJSON *resp = db2_kb_service_memory_top_l2_facts_json(max);
   return kb_reply_or_error(fd, resp, "failed to load top L2 facts");
}

int kb_handle_memory_load_eval_corpus(int fd, cJSON *req)
{
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 100;
   cJSON *resp = db2_kb_service_memory_load_eval_corpus_json(max);
   return kb_reply_or_error(fd, resp, "failed to load eval corpus");
}

int kb_handle_memory_get(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "memory.get requires id");

   cJSON *resp = db2_kb_service_memory_get_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to get memory");
}

int kb_handle_memory_briefing(int fd, cJSON *req)
{
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit_tokens");
   int limit_tokens = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 0;

   cJSON *resp = db2_kb_service_memory_briefing_json(limit_tokens);
   return kb_reply_or_error(fd, resp, "failed to build memory briefing");
}

int kb_handle_memory_context_block(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *block_j = cJSON_GetObjectItemCaseSensitive(req, "block_type");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "memory.context_block requires query");
   const char *block_type = cJSON_IsString(block_j) ? block_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 5;

   cJSON *resp = db2_kb_service_memory_context_block_json(query_j->valuestring, block_type, limit);
   return kb_reply_or_error(fd, resp, "failed to build context block");
}

int kb_handle_memory_entity_profile(int fd, cJSON *req)
{
   cJSON *entity_j = cJSON_GetObjectItemCaseSensitive(req, "entity");
   if (!cJSON_IsString(entity_j))
      return kb_send_error(fd, "memory.entity_profile requires entity");

   cJSON *resp = db2_kb_service_memory_entity_profile_json(entity_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to fetch entity profile");
}

int kb_handle_memory_entity_edges(int fd, cJSON *req)
{
   cJSON *entity_j = cJSON_GetObjectItemCaseSensitive(req, "entity");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(entity_j))
      return kb_send_error(fd, "memory.entity_edges requires entity");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 10;

   cJSON *resp = db2_kb_service_memory_entity_edges_json(entity_j->valuestring, limit);
   return kb_reply_or_error(fd, resp, "failed to fetch entity edges");
}

int kb_handle_memory_search_graph(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "memory.search_graph requires query");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 10;

   cJSON *resp = db2_kb_service_memory_search_graph_json(query_j->valuestring, limit);
   return kb_reply_or_error(fd, resp, "failed to search memory graph");
}

int kb_handle_memory_search_graph_as_of(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *as_of_j = cJSON_GetObjectItemCaseSensitive(req, "as_of");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "memory.search_graph_as_of requires query");
   if (!cJSON_IsString(as_of_j))
      return kb_send_error(fd, "memory.search_graph_as_of requires as_of");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 10;

   cJSON *resp = db2_kb_service_memory_search_graph_as_of_json(query_j->valuestring,
                                                               as_of_j->valuestring, limit);
   return kb_reply_or_error(fd, resp, "failed to search memory graph as-of");
}

int kb_handle_memory_get_episode(int fd, cJSON *req)
{
   cJSON *key_j = cJSON_GetObjectItemCaseSensitive(req, "episode_key");
   if (!cJSON_IsString(key_j))
      return kb_send_error(fd, "memory.get_episode requires episode_key");

   cJSON *resp = db2_kb_service_memory_get_episode_json(key_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to fetch episode");
}

int kb_handle_memory_ask(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   cJSON *st_j = cJSON_GetObjectItemCaseSensitive(req, "scope_type");
   cJSON *sv_j = cJSON_GetObjectItemCaseSensitive(req, "scope_value");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "memory.ask requires query");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 5;
   const char *st = (cJSON_IsString(st_j) && st_j->valuestring[0]) ? st_j->valuestring : NULL;
   const char *sv = (cJSON_IsString(sv_j) && sv_j->valuestring[0]) ? sv_j->valuestring : NULL;

   cJSON *resp = db2_kb_service_memory_ask_json(query_j->valuestring, st, sv, limit);
   return kb_reply_or_error(fd, resp, "failed to answer query");
}

int kb_handle_memory_find_id_by_key_kind(int fd, cJSON *req)
{
   cJSON *key_j = cJSON_GetObjectItemCaseSensitive(req, "key");
   cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(req, "kind");
   if (!cJSON_IsString(key_j) || !cJSON_IsString(kind_j))
      return kb_send_error(fd, "memory.find_id_by_key_kind requires key and kind");

   cJSON *resp =
       db2_kb_service_memory_find_id_by_key_kind_json(key_j->valuestring, kind_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to look up memory id");
}

int kb_handle_memory_search_facts_patterns_by_keyword(int fd, cJSON *req)
{
   cJSON *kw_j = cJSON_GetObjectItemCaseSensitive(req, "keyword");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   if (!cJSON_IsString(kw_j))
      return kb_send_error(fd, "memory.search_facts_patterns_by_keyword requires keyword");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 5;

   cJSON *resp =
       db2_kb_service_memory_search_facts_patterns_by_keyword_json(kw_j->valuestring, max);
   return kb_reply_or_error(fd, resp, "failed to search facts/patterns");
}

int kb_handle_memory_supersede(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "old_id");
   cJSON *content_j = cJSON_GetObjectItemCaseSensitive(req, "new_content");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(req, "confidence");
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(content_j))
      return kb_send_error(fd, "memory.supersede requires old_id and new_content");
   double conf = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 1.0;
   const char *sid = cJSON_IsString(sid_j) ? sid_j->valuestring : "";

   cJSON *resp = db2_kb_service_memory_supersede_json((int64_t)id_j->valuedouble,
                                                      content_j->valuestring, conf, sid);
   return kb_reply_or_error(fd, resp, "failed to supersede memory");
}

int kb_handle_memory_fact_history(int fd, cJSON *req)
{
   cJSON *key_j = cJSON_GetObjectItemCaseSensitive(req, "key");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   if (!cJSON_IsString(key_j))
      return kb_send_error(fd, "memory.fact_history requires key");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 16;

   cJSON *resp = db2_kb_service_memory_fact_history_json(key_j->valuestring, max);
   return kb_reply_or_error(fd, resp, "failed to fetch fact history");
}

int kb_handle_memory_list_session_scope_priority(int fd, cJSON *req)
{
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 24;

   cJSON *resp = db2_kb_service_memory_list_session_scope_priority_json(max);
   return kb_reply_or_error(fd, resp, "failed to list session-scope memories");
}

int kb_handle_memory_list_low_effectiveness(int fd, cJSON *req)
{
   cJSON *th_j = cJSON_GetObjectItemCaseSensitive(req, "threshold");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   double threshold = cJSON_IsNumber(th_j) ? th_j->valuedouble : 0.5;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 50;

   cJSON *resp = db2_kb_service_memory_list_low_effectiveness_json(threshold, limit);
   return kb_reply_or_error(fd, resp, "failed to list low-effectiveness memories");
}

int kb_handle_memory_list_unused_l2(int fd, cJSON *req)
{
   cJSON *days_j = cJSON_GetObjectItemCaseSensitive(req, "days");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   int days = cJSON_IsNumber(days_j) ? (int)days_j->valuedouble : 14;
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 64;

   cJSON *resp = db2_kb_service_memory_list_unused_l2_json(days, max);
   return kb_reply_or_error(fd, resp, "failed to list unused L2 memories");
}

int kb_handle_memory_list_superseded_keys(int fd, cJSON *req)
{
   cJSON *mv_j = cJSON_GetObjectItemCaseSensitive(req, "min_versions");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   int mv = cJSON_IsNumber(mv_j) ? (int)mv_j->valuedouble : 3;
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 64;

   cJSON *resp = db2_kb_service_memory_list_superseded_keys_json(mv, max);
   return kb_reply_or_error(fd, resp, "failed to list superseded memory keys");
}

int kb_handle_memory_set_artifact(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   cJSON *type_j = cJSON_GetObjectItemCaseSensitive(req, "artifact_type");
   cJSON *ref_j = cJSON_GetObjectItemCaseSensitive(req, "artifact_ref");
   cJSON *hash_j = cJSON_GetObjectItemCaseSensitive(req, "artifact_hash");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(type_j) || !cJSON_IsString(ref_j))
      return kb_send_error(fd,
                           "memory.set_artifact requires memory_id, artifact_type, artifact_ref");
   const char *hash =
       (cJSON_IsString(hash_j) && hash_j->valuestring[0]) ? hash_j->valuestring : NULL;

   cJSON *resp = db2_kb_service_memory_set_artifact_json(
       (int64_t)id_j->valuedouble, type_j->valuestring, ref_j->valuestring, hash);
   return kb_reply_or_error(fd, resp, "failed to set artifact");
}

int kb_handle_memory_list_session_scope_priority_like(int fd, cJSON *req)
{
   cJSON *pat_j = cJSON_GetObjectItemCaseSensitive(req, "pattern");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   if (!cJSON_IsString(pat_j))
      return kb_send_error(fd, "memory.list_session_scope_priority_like requires pattern");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 5;

   cJSON *resp =
       db2_kb_service_memory_list_session_scope_priority_like_json(pat_j->valuestring, max);
   return kb_reply_or_error(fd, resp, "failed to list session-scope memories (like)");
}

int kb_handle_memory_check_drift(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "task_id");
   cJSON *path_j = cJSON_GetObjectItemCaseSensitive(req, "file_path");
   cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(req, "command");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "memory.check_drift requires task_id");
   const char *path = cJSON_IsString(path_j) ? path_j->valuestring : "";
   const char *cmd = cJSON_IsString(cmd_j) ? cmd_j->valuestring : "";

   cJSON *resp = db2_kb_service_memory_check_drift_json((int64_t)id_j->valuedouble, path, cmd);
   return kb_reply_or_error(fd, resp, "failed to check drift");
}

int kb_handle_task_create(int fd, cJSON *req)
{
   cJSON *title_j = cJSON_GetObjectItemCaseSensitive(req, "title");
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *parent_j = cJSON_GetObjectItemCaseSensitive(req, "parent_id");
   if (!cJSON_IsString(title_j))
      return kb_send_error(fd, "task.create requires title");
   const char *sid = cJSON_IsString(sid_j) ? sid_j->valuestring : "";
   int64_t parent = cJSON_IsNumber(parent_j) ? (int64_t)parent_j->valuedouble : 0;

   cJSON *resp = db2_kb_service_task_create_json(title_j->valuestring, sid, parent);
   return kb_reply_or_error(fd, resp, "failed to create task");
}

int kb_handle_task_update_state(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(state_j))
      return kb_send_error(fd, "task.update_state requires id and state");

   cJSON *resp =
       db2_kb_service_task_update_state_json((int64_t)id_j->valuedouble, state_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to update task state");
}

int kb_handle_task_delete(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "task.delete requires id");

   cJSON *resp = db2_kb_service_task_delete_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to delete task");
}

int kb_handle_task_add_edge(int fd, cJSON *req)
{
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(req, "source");
   cJSON *dst_j = cJSON_GetObjectItemCaseSensitive(req, "target");
   cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(req, "relation");
   if (!cJSON_IsNumber(src_j) || !cJSON_IsNumber(dst_j))
      return kb_send_error(fd, "task.add_edge requires source and target");
   const char *relation = cJSON_IsString(rel_j) ? rel_j->valuestring : "depends_on";

   cJSON *resp = db2_kb_service_task_add_edge_json((int64_t)src_j->valuedouble,
                                                   (int64_t)dst_j->valuedouble, relation);
   return kb_reply_or_error(fd, resp, "failed to add task edge");
}

int kb_handle_task_get_edges(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "task_id");
   cJSON *max_j = cJSON_GetObjectItemCaseSensitive(req, "max");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "task.get_edges requires task_id");
   int max = cJSON_IsNumber(max_j) ? (int)max_j->valuedouble : 16;

   cJSON *resp = db2_kb_service_task_get_edges_json((int64_t)id_j->valuedouble, max);
   return kb_reply_or_error(fd, resp, "failed to fetch task edges");
}

int kb_handle_task_list(int fd, cJSON *req)
{
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *state = cJSON_IsString(state_j) ? state_j->valuestring : NULL;
   const char *sid = cJSON_IsString(sid_j) ? sid_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 16;

   cJSON *resp = db2_kb_service_task_list_json(state, sid, limit);
   return kb_reply_or_error(fd, resp, "failed to list tasks");
}

int kb_handle_memory_store(int fd, cJSON *req)
{
   cJSON *key_j = cJSON_GetObjectItemCaseSensitive(req, "key");
   cJSON *content_j = cJSON_GetObjectItemCaseSensitive(req, "content");
   if (!cJSON_IsString(key_j) || !cJSON_IsString(content_j))
      return kb_send_error(fd, "memory.store requires key and content");

   cJSON *tier_j = cJSON_GetObjectItemCaseSensitive(req, "tier");
   cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(req, "kind");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(req, "confidence");
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *use_cases_j = cJSON_GetObjectItemCaseSensitive(req, "use_cases");
   const char *tier = cJSON_IsString(tier_j) ? tier_j->valuestring : TIER_L0;
   const char *kind = cJSON_IsString(kind_j) ? kind_j->valuestring : KIND_FACT;
   double confidence = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 1.0;
   const char *session_id = cJSON_IsString(sid_j) ? sid_j->valuestring : "";
   const char *use_cases = cJSON_IsString(use_cases_j) ? use_cases_j->valuestring : "";

   cJSON *resp = db2_kb_service_memory_insert_ex_json(
       tier, kind, key_j->valuestring, content_j->valuestring, use_cases, confidence, session_id);
   return kb_reply_or_error(fd, resp, "failed to store memory");
}
