/* kb_client_memory.c: kb_client wrappers for the memory.* RPC family
 * (find_facts, list, get, insert, briefing, context_block, ask,
 * entity_profile, entity_edges, search_graph, get_episode).  Split
 * out of kb_client.c so the file stays under the per-file line cap.
 *
 * All wrappers go through kb_v1_action_request (defined in kb_client.c)
 * — a NULL kb response always means "kb is unreachable", error
 * payloads from the kb side land in the parsed response.
 *
 * Count-returning readers (list, search, find_facts and friends) return a
 * NEGATIVE value when no answer could be obtained -- kb unreachable, an
 * unparseable response, or a non-"ok" envelope -- and a row count >= 0 only
 * on a successful "ok" response. Callers MUST treat <0 as "service
 * unavailable" rather than "empty"; collapsing the two silently masks an
 * outage as an empty store (the original bug this discipline fixes). */

#include "kb_client.h"
#include "kb_client_memory_internal.h"
#include "cJSON.h"
#include "memory_query.h" /* db2_memory_low_eff_row_t etc. */
#include "tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kbc_memory_row_from_json(cJSON *f, memory_t *m)
{
   memset(m, 0, sizeof(*m));
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(f, "id");
   cJSON *tier_j = cJSON_GetObjectItemCaseSensitive(f, "tier");
   cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(f, "kind");
   cJSON *key_j = cJSON_GetObjectItemCaseSensitive(f, "key");
   cJSON *headline_j = cJSON_GetObjectItemCaseSensitive(f, "headline");
   cJSON *content_j = cJSON_GetObjectItemCaseSensitive(f, "content");
   cJSON *use_cases_j = cJSON_GetObjectItemCaseSensitive(f, "use_cases");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(f, "confidence");
   cJSON *uses_j = cJSON_GetObjectItemCaseSensitive(f, "use_count");
   cJSON *last_j = cJSON_GetObjectItemCaseSensitive(f, "last_used_at");
   cJSON *created_j = cJSON_GetObjectItemCaseSensitive(f, "created_at");
   cJSON *updated_j = cJSON_GetObjectItemCaseSensitive(f, "updated_at");
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(f, "source_session");
   if (cJSON_IsNumber(id_j))
      m->id = (int64_t)id_j->valuedouble;
   if (cJSON_IsString(tier_j))
      snprintf(m->tier, sizeof(m->tier), "%s", tier_j->valuestring);
   if (cJSON_IsString(kind_j))
      snprintf(m->kind, sizeof(m->kind), "%s", kind_j->valuestring);
   if (cJSON_IsString(key_j))
      snprintf(m->key, sizeof(m->key), "%s", key_j->valuestring);
   if (cJSON_IsString(headline_j))
      snprintf(m->headline, sizeof(m->headline), "%s", headline_j->valuestring);
   if (cJSON_IsString(content_j))
      snprintf(m->content, sizeof(m->content), "%s", content_j->valuestring);
   if (cJSON_IsString(use_cases_j))
      snprintf(m->use_cases, sizeof(m->use_cases), "%s", use_cases_j->valuestring);
   if (cJSON_IsNumber(conf_j))
      m->confidence = conf_j->valuedouble;
   if (cJSON_IsNumber(uses_j))
      m->use_count = (int)uses_j->valuedouble;
   if (cJSON_IsString(last_j))
      snprintf(m->last_used_at, sizeof(m->last_used_at), "%s", last_j->valuestring);
   if (cJSON_IsString(created_j))
      snprintf(m->created_at, sizeof(m->created_at), "%s", created_j->valuestring);
   if (cJSON_IsString(updated_j))
      snprintf(m->updated_at, sizeof(m->updated_at), "%s", updated_j->valuestring);
   if (cJSON_IsString(src_j))
      snprintf(m->source_session, sizeof(m->source_session), "%s", src_j->valuestring);
}

int kb_client_memory_find_facts(const char *query, int limit, memory_t *out, int max)
{
   if (!query || !out || max <= 0)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   /* Graph-code fusion is always on for recall; the eval/benchmark harness is the
    * only path that forwards a different graph_code_fusion_state (via the _ex
    * variants) to measure on-vs-off. */
   cJSON_AddStringToObject(req, "graph_code_fusion_state", "on");
   char *json = kb_v1_action_request("memory.find_facts", req);
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

   cJSON *facts = cJSON_GetObjectItemCaseSensitive(resp, "facts");
   int n = 0;
   if (cJSON_IsArray(facts))
   {
      cJSON *f;
      cJSON_ArrayForEach(f, facts)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(f, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_find_facts_ex(const char *query, int limit, memory_t *out, int max,
                                   const char *graph_code_fusion_state)
{
   if (!query || !out || max <= 0)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   /* graph_code_fusion_state ("off" | "shadow" | "on") is consumed by
    * kb_handle_memory_find_facts, which runs the graph-code fusion rerank in the
    * recall path when "on". Default to "off" when the caller passes NULL. */
   cJSON_AddStringToObject(
       req, "graph_code_fusion_state",
       (graph_code_fusion_state && graph_code_fusion_state[0]) ? graph_code_fusion_state : "off");
   char *json = kb_v1_action_request("memory.find_facts", req);
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

   cJSON *facts = cJSON_GetObjectItemCaseSensitive(resp, "facts");
   int n = 0;
   if (cJSON_IsArray(facts))
   {
      cJSON *f;
      cJSON_ArrayForEach(f, facts)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(f, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_list(const char *tier, const char *kind, int limit, memory_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   if (tier && tier[0])
      cJSON_AddStringToObject(req, "tier", tier);
   if (kind && kind[0])
      cJSON_AddStringToObject(req, "kind", kind);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.list", req);
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

   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *f;
      cJSON_ArrayForEach(f, rows)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(f, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

static char *kb_client_v1_session_briefing_section(const char *method, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request(method, req);
   if (!json)
      return NULL;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *body = cJSON_GetObjectItemCaseSensitive(resp, "body");
   char *out = NULL;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsString(body) &&
       body->valuestring[0])
      out = strdup(body->valuestring);
   cJSON_Delete(resp);
   return out;
}

char *kb_client_memory_maintenance_run_json(unsigned int modes, int force, int dry_run)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "modes", (double)modes);
   cJSON_AddBoolToObject(req, "force", force ? 1 : 0);
   cJSON_AddBoolToObject(req, "dry_run", dry_run ? 1 : 0);
   return kb_v1_action_request("memory.maintenance_run", req);
}

char *kb_client_memory_alerts_json(const char *since)
{
   cJSON *req = cJSON_CreateObject();
   if (since && since[0])
      cJSON_AddStringToObject(req, "since", since);
   return kb_v1_action_request("memory.alerts", req);
}

int kb_client_memory_list_conflicts(conflict_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.list_conflicts", req);
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
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "conflicts");
   int n = 0;
   if (cJSON_IsArray(arr))
   {
      cJSON *c;
      cJSON_ArrayForEach(c, arr)
      {
         if (n >= max)
            break;
         memset(&out[n], 0, sizeof(out[n]));
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(c, "id");
         cJSON *a_j = cJSON_GetObjectItemCaseSensitive(c, "memory_a");
         cJSON *b_j = cJSON_GetObjectItemCaseSensitive(c, "memory_b");
         cJSON *det_j = cJSON_GetObjectItemCaseSensitive(c, "detected_at");
         cJSON *res_j = cJSON_GetObjectItemCaseSensitive(c, "resolved");
         cJSON *resn_j = cJSON_GetObjectItemCaseSensitive(c, "resolution");
         if (cJSON_IsNumber(id_j))
            out[n].id = (int64_t)id_j->valuedouble;
         if (cJSON_IsNumber(a_j))
            out[n].memory_a = (int64_t)a_j->valuedouble;
         if (cJSON_IsNumber(b_j))
            out[n].memory_b = (int64_t)b_j->valuedouble;
         if (cJSON_IsString(det_j))
            snprintf(out[n].detected_at, sizeof(out[n].detected_at), "%s", det_j->valuestring);
         if (cJSON_IsNumber(res_j))
            out[n].resolved = (int)res_j->valuedouble;
         if (cJSON_IsString(resn_j))
            snprintf(out[n].resolution, sizeof(out[n].resolution), "%s", resn_j->valuestring);
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

static int kbc_facts_array_from_envelope(const char *json, memory_t *out, int max)
{
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }
   cJSON *facts = cJSON_GetObjectItemCaseSensitive(resp, "facts");
   int n = 0;
   if (cJSON_IsArray(facts))
   {
      cJSON *f;
      cJSON_ArrayForEach(f, facts)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(f, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

static void kbc_memory_diagnostic_from_json(cJSON *j, memory_diagnostic_t *out)
{
   memset(out, 0, sizeof(*out));
   cJSON *mem = cJSON_GetObjectItemCaseSensitive(j, "memory");
   if (cJSON_IsObject(mem))
      kbc_memory_row_from_json(mem, &out->memory);
   cJSON *parts = cJSON_GetObjectItemCaseSensitive(j, "parts");
   if (cJSON_IsObject(parts))
   {
#define PICK(field)                                                                                \
   do                                                                                              \
   {                                                                                               \
      cJSON *v = cJSON_GetObjectItemCaseSensitive(parts, #field);                                  \
      if (cJSON_IsNumber(v))                                                                       \
         out->parts.field = v->valuedouble;                                                        \
   } while (0)
      PICK(lexical);
      PICK(coverage);
      PICK(entity);
      PICK(temporal);
      PICK(evidence);
      PICK(semantic);
      PICK(state);
      PICK(intent);
      PICK(confidence);
      PICK(salience);
      PICK(surprise);
      PICK(pagerank);
      PICK(cross_encoder);
      PICK(hybrid_total);
      PICK(blended_total);
      PICK(rerank_mix);
      PICK(total);
#undef PICK
   }
}

int kb_client_memory_diagnose_scoped(const char *query, const char *scope_type,
                                     const char *scope_value, int limit, memory_diagnostic_t *out,
                                     int max)
{
   if (!query || !out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   if (scope_type && scope_type[0])
      cJSON_AddStringToObject(req, "scope_type", scope_type);
   if (scope_value && scope_value[0])
      cJSON_AddStringToObject(req, "scope_value", scope_value);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.diagnose_scoped", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "rows");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         kbc_memory_diagnostic_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max)
{
   return kb_client_memory_diagnose_scoped(query, NULL, NULL, limit, out, max);
}

int kb_client_memory_explain_match(const char *query, int64_t memory_id, memory_diagnostic_t *out)
{
   if (!query || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   char *json = kb_v1_action_request("memory.explain_match", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *row = cJSON_GetObjectItemCaseSensitive(resp, "row");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsObject(row))
   {
      cJSON_Delete(resp);
      return -1;
   }
   kbc_memory_diagnostic_from_json(row, out);
   cJSON_Delete(resp);
   return 0;
}

int kb_client_memory_find_facts_visible(const char *query, const char *workspace,
                                        const char *project, int limit, memory_t *out, int max)
{
   if (!query || !out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   if (workspace && workspace[0])
      cJSON_AddStringToObject(req, "workspace", workspace);
   if (project && project[0])
      cJSON_AddStringToObject(req, "project", project);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.find_facts_visible", req);
   int n = kbc_facts_array_from_envelope(json, out, max);
   free(json);
   return n;
}

int kb_client_memory_find_facts_scoped_ex(const char *query, const char *scope_type,
                                          const char *scope_value, int limit, memory_t *out,
                                          int max, const char *graph_code_fusion_state)
{
   if (!query || !out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   if (scope_type && scope_type[0])
      cJSON_AddStringToObject(req, "scope_type", scope_type);
   if (scope_value && scope_value[0])
      cJSON_AddStringToObject(req, "scope_value", scope_value);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   cJSON_AddStringToObject(
       req, "graph_code_fusion_state",
       (graph_code_fusion_state && graph_code_fusion_state[0]) ? graph_code_fusion_state : "off");
   char *json = kb_v1_action_request("memory.find_facts_scoped", req);
   int n = kbc_facts_array_from_envelope(json, out, max);
   free(json);
   return n;
}

int kb_client_memory_find_facts_scoped(const char *query, const char *scope_type,
                                       const char *scope_value, int limit, memory_t *out, int max)
{
   return kb_client_memory_find_facts_scoped_ex(query, scope_type, scope_value, limit, out, max,
                                                "on");
}

int kb_client_memory_search(char **clusters, int cluster_count, int limit, search_result_t *out,
                            int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(req, "clusters");
   for (int i = 0; i < cluster_count; i++)
      if (clusters[i] && clusters[i][0])
         cJSON_AddItemToArray(arr, cJSON_CreateString(clusters[i]));
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.search", req);
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
   cJSON *results = cJSON_GetObjectItemCaseSensitive(resp, "results");
   int n = 0;
   if (cJSON_IsArray(results))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, results)
      {
         if (n >= max)
            break;
         memset(&out[n], 0, sizeof(out[n]));
         cJSON *sid = cJSON_GetObjectItemCaseSensitive(r, "session_id");
         cJSON *seq = cJSON_GetObjectItemCaseSensitive(r, "seq");
         cJSON *fp = cJSON_GetObjectItemCaseSensitive(r, "file_path");
         cJSON *sl = cJSON_GetObjectItemCaseSensitive(r, "start_line");
         cJSON *el = cJSON_GetObjectItemCaseSensitive(r, "end_line");
         cJSON *sm = cJSON_GetObjectItemCaseSensitive(r, "summary");
         cJSON *sc = cJSON_GetObjectItemCaseSensitive(r, "score");
         cJSON *files = cJSON_GetObjectItemCaseSensitive(r, "files");
         if (cJSON_IsString(sid))
            snprintf(out[n].session_id, sizeof(out[n].session_id), "%s", sid->valuestring);
         if (cJSON_IsNumber(seq))
            out[n].seq = (int)seq->valuedouble;
         if (cJSON_IsString(fp))
            snprintf(out[n].file_path, sizeof(out[n].file_path), "%s", fp->valuestring);
         if (cJSON_IsNumber(sl))
            out[n].start_line = (int)sl->valuedouble;
         if (cJSON_IsNumber(el))
            out[n].end_line = (int)el->valuedouble;
         if (cJSON_IsString(sm))
            snprintf(out[n].summary, sizeof(out[n].summary), "%s", sm->valuestring);
         if (cJSON_IsNumber(sc))
            out[n].score = sc->valuedouble;
         if (cJSON_IsArray(files))
         {
            int fc = 0;
            cJSON *f;
            cJSON_ArrayForEach(f, files)
            {
               if (fc >= 32)
                  break;
               if (cJSON_IsString(f))
                  snprintf(out[n].files[fc++], sizeof(out[n].files[0]), "%s", f->valuestring);
            }
            out[n].file_count = fc;
         }
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

char *kb_client_memory_assemble_context(const char *task_hint)
{
   cJSON *req = cJSON_CreateObject();
   if (task_hint && task_hint[0])
      cJSON_AddStringToObject(req, "task_hint", task_hint);
   char *json = kb_v1_action_request("memory.assemble_context", req);
   if (!json)
      return NULL;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *body = cJSON_GetObjectItemCaseSensitive(resp, "context");
   char *out = NULL;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsString(body))
      out = strdup(body->valuestring);
   cJSON_Delete(resp);
   return out;
}

int kb_client_memory_compact_windows(int *summary_count, int *fact_count)
{
   if (summary_count)
      *summary_count = 0;
   if (fact_count)
      *fact_count = 0;
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("memory.compact_windows", req);
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
   cJSON *s_j = cJSON_GetObjectItemCaseSensitive(resp, "summaries");
   cJSON *f_j = cJSON_GetObjectItemCaseSensitive(resp, "facts");
   if (summary_count && cJSON_IsNumber(s_j))
      *summary_count = (int)s_j->valuedouble;
   if (fact_count && cJSON_IsNumber(f_j))
      *fact_count = (int)f_j->valuedouble;
   cJSON_Delete(resp);
   return 0;
}

int kb_client_memory_query_edges(const char *entity, edge_t *out, int max)
{
   if (!entity || !out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "entity", entity);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.query_edges", req);
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
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "edges");
   int n = 0;
   if (cJSON_IsArray(arr))
   {
      cJSON *e;
      cJSON_ArrayForEach(e, arr)
      {
         if (n >= max)
            break;
         memset(&out[n], 0, sizeof(out[n]));
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(e, "id");
         cJSON *src_j = cJSON_GetObjectItemCaseSensitive(e, "source");
         cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(e, "relation");
         cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(e, "target");
         cJSON *w_j = cJSON_GetObjectItemCaseSensitive(e, "weight");
         if (cJSON_IsNumber(id_j))
            out[n].id = (int64_t)id_j->valuedouble;
         if (cJSON_IsString(src_j))
            snprintf(out[n].source, sizeof(out[n].source), "%s", src_j->valuestring);
         if (cJSON_IsString(rel_j))
            snprintf(out[n].relation, sizeof(out[n].relation), "%s", rel_j->valuestring);
         if (cJSON_IsString(tgt_j))
            snprintf(out[n].target, sizeof(out[n].target), "%s", tgt_j->valuestring);
         if (cJSON_IsNumber(w_j))
            out[n].weight = (int)w_j->valuedouble;
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_effectiveness_stats(effectiveness_stats_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("memory.effectiveness_stats", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *stats = cJSON_GetObjectItemCaseSensitive(resp, "stats");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsObject(stats))
   {
      cJSON_Delete(resp);
      return -1;
   }
   cJSON *avg = cJSON_GetObjectItemCaseSensitive(stats, "avg_effectiveness");
   cJSON *low = cJSON_GetObjectItemCaseSensitive(stats, "low_effectiveness_count");
   cJSON *hi = cJSON_GetObjectItemCaseSensitive(stats, "high_impact_count");
   cJSON *ns = cJSON_GetObjectItemCaseSensitive(stats, "never_surfaced_l2");
   if (cJSON_IsNumber(avg))
      out->avg_effectiveness = avg->valuedouble;
   if (cJSON_IsNumber(low))
      out->low_effectiveness_count = (int)low->valuedouble;
   if (cJSON_IsNumber(hi))
      out->high_impact_count = (int)hi->valuedouble;
   if (cJSON_IsNumber(ns))
      out->never_surfaced_l2 = (int)ns->valuedouble;
   cJSON_Delete(resp);
   return 0;
}

int kb_client_memory_query_health(memory_health_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("memory.query_health", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *health = cJSON_GetObjectItemCaseSensitive(resp, "health");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsObject(health))
   {
      cJSON_Delete(resp);
      return -1;
   }
   cJSON *cy = cJSON_GetObjectItemCaseSensitive(health, "cycles");
   cJSON *cr = cJSON_GetObjectItemCaseSensitive(health, "contradiction_rate");
   cJSON *pr = cJSON_GetObjectItemCaseSensitive(health, "promotion_rate");
   cJSON *dr = cJSON_GetObjectItemCaseSensitive(health, "demotion_rate");
   cJSON *st = cJSON_GetObjectItemCaseSensitive(health, "staleness");
   cJSON *tc = cJSON_GetObjectItemCaseSensitive(health, "total_contradictions");
   cJSON *tp = cJSON_GetObjectItemCaseSensitive(health, "total_promotions");
   cJSON *td = cJSON_GetObjectItemCaseSensitive(health, "total_demotions");
   cJSON *te = cJSON_GetObjectItemCaseSensitive(health, "total_expirations");
   if (cJSON_IsNumber(cy))
      out->cycles = (int)cy->valuedouble;
   if (cJSON_IsNumber(cr))
      out->contradiction_rate = cr->valuedouble;
   if (cJSON_IsNumber(pr))
      out->promotion_rate = pr->valuedouble;
   if (cJSON_IsNumber(dr))
      out->demotion_rate = dr->valuedouble;
   if (cJSON_IsNumber(st))
      out->staleness = st->valuedouble;
   if (cJSON_IsNumber(tc))
      out->total_contradictions = (int)tc->valuedouble;
   if (cJSON_IsNumber(tp))
      out->total_promotions = (int)tp->valuedouble;
   if (cJSON_IsNumber(td))
      out->total_demotions = (int)td->valuedouble;
   if (cJSON_IsNumber(te))
      out->total_expirations = (int)te->valuedouble;
   cJSON_Delete(resp);
   return 0;
}

int kb_client_memory_delete(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   char *json = kb_v1_action_request("memory.delete", req);
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

int kb_client_memory_stats(memory_stats_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("memory.stats", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *stats = cJSON_GetObjectItemCaseSensitive(resp, "stats");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsObject(stats))
   {
      cJSON_Delete(resp);
      return -1;
   }
   cJSON *total_j = cJSON_GetObjectItemCaseSensitive(stats, "total");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(stats, "conflicts");
   cJSON *pr_last = cJSON_GetObjectItemCaseSensitive(stats, "pagerank_last_ms");
   cJSON *pr_avg = cJSON_GetObjectItemCaseSensitive(stats, "pagerank_avg_ms");
   cJSON *pr_max = cJSON_GetObjectItemCaseSensitive(stats, "pagerank_max_ms");
   cJSON *pr_samp = cJSON_GetObjectItemCaseSensitive(stats, "pagerank_samples");
   cJSON *pr_lc = cJSON_GetObjectItemCaseSensitive(stats, "pagerank_last_candidates");
   cJSON *pr_le = cJSON_GetObjectItemCaseSensitive(stats, "pagerank_last_edges");
   cJSON *tiers = cJSON_GetObjectItemCaseSensitive(stats, "tier_counts");
   cJSON *kinds = cJSON_GetObjectItemCaseSensitive(stats, "kind_counts");
   if (cJSON_IsNumber(total_j))
      out->total = (int)total_j->valuedouble;
   if (cJSON_IsNumber(conf_j))
      out->conflicts = (int)conf_j->valuedouble;
   if (cJSON_IsNumber(pr_last))
      out->pagerank_last_ms = pr_last->valuedouble;
   if (cJSON_IsNumber(pr_avg))
      out->pagerank_avg_ms = pr_avg->valuedouble;
   if (cJSON_IsNumber(pr_max))
      out->pagerank_max_ms = pr_max->valuedouble;
   if (cJSON_IsNumber(pr_samp))
      out->pagerank_samples = (int)pr_samp->valuedouble;
   if (cJSON_IsNumber(pr_lc))
      out->pagerank_last_candidates = (int)pr_lc->valuedouble;
   if (cJSON_IsNumber(pr_le))
      out->pagerank_last_edges = (int)pr_le->valuedouble;
   if (cJSON_IsArray(tiers))
   {
      int n = cJSON_GetArraySize(tiers);
      for (int i = 0; i < n && i < 6; i++)
      {
         cJSON *v = cJSON_GetArrayItem(tiers, i);
         if (cJSON_IsNumber(v))
            out->tier_counts[i] = (int)v->valuedouble;
      }
   }
   if (cJSON_IsArray(kinds))
   {
      int n = cJSON_GetArraySize(kinds);
      for (int i = 0; i < n && i < KIND_COUNT; i++)
      {
         cJSON *v = cJSON_GetArrayItem(kinds, i);
         if (cJSON_IsNumber(v))
            out->kind_counts[i] = (int)v->valuedouble;
      }
   }
   cJSON_Delete(resp);
   return 0;
}

/* Raw-JSON variant of the stats reader: returns the kb "stats" object as a
 * detached JSON string (caller frees), or NULL when the kb is unreachable or
 * returns a non-"ok" envelope. Lets handle_memory_stats forward the payload
 * verbatim without re-marshalling the memory_stats_t struct. */
char *kb_client_memory_stats_json(void)
{
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("memory.stats", req);
   if (!json)
      return NULL;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *stats = cJSON_GetObjectItemCaseSensitive(resp, "stats");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsObject(stats))
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON *detached = cJSON_DetachItemViaPointer(resp, stats);
   char *out = detached ? cJSON_PrintUnformatted(detached) : NULL;
   cJSON_Delete(detached);
   cJSON_Delete(resp);
   return out;
}

int kb_client_memory_link_create(int64_t source_id, int64_t target_id, const char *relation)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "source_id", (double)source_id);
   cJSON_AddNumberToObject(req, "target_id", (double)target_id);
   cJSON_AddStringToObject(req, "relation", relation ? relation : "");
   char *json = kb_v1_action_request("memory.link_create", req);
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

int kb_client_memory_link_query(int64_t memory_id, memory_link_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.link_query", req);
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
   cJSON *links = cJSON_GetObjectItemCaseSensitive(resp, "links");
   int n = 0;
   if (cJSON_IsArray(links))
   {
      cJSON *l;
      cJSON_ArrayForEach(l, links)
      {
         if (n >= max)
            break;
         memset(&out[n], 0, sizeof(out[n]));
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(l, "id");
         cJSON *src_j = cJSON_GetObjectItemCaseSensitive(l, "source_id");
         cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(l, "target_id");
         cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(l, "relation");
         cJSON *cre_j = cJSON_GetObjectItemCaseSensitive(l, "created_at");
         if (cJSON_IsNumber(id_j))
            out[n].id = (int64_t)id_j->valuedouble;
         if (cJSON_IsNumber(src_j))
            out[n].source_id = (int64_t)src_j->valuedouble;
         if (cJSON_IsNumber(tgt_j))
            out[n].target_id = (int64_t)tgt_j->valuedouble;
         if (cJSON_IsString(rel_j))
            snprintf(out[n].relation, sizeof(out[n].relation), "%s", rel_j->valuestring);
         if (cJSON_IsString(cre_j))
            snprintf(out[n].created_at, sizeof(out[n].created_at), "%s", cre_j->valuestring);
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_link_delete(int64_t link_id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "link_id", (double)link_id);
   char *json = kb_v1_action_request("memory.link_delete", req);
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

int64_t kb_client_memory_upsert_workflow(const char *workspace, const char *signal_type,
                                         const char *rule, double observed_confidence,
                                         const char *session_id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "workspace", workspace ? workspace : "");
   cJSON_AddStringToObject(req, "signal_type", signal_type ? signal_type : "");
   cJSON_AddStringToObject(req, "rule", rule ? rule : "");
   cJSON_AddNumberToObject(req, "observed_confidence", observed_confidence);
   if (session_id && session_id[0])
      cJSON_AddStringToObject(req, "session_id", session_id);
   char *json = kb_v1_action_request("memory.upsert_workflow", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(resp, "id");
   int64_t id = -1;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(id_j))
      id = (int64_t)id_j->valuedouble;
   cJSON_Delete(resp);
   return id;
}

char *kb_client_memory_recall_json_ex(const char *task_hint, int limit_tokens, int session_start,
                                      const char *graph_code_fusion_state)
{
   cJSON *req = cJSON_CreateObject();
   if (task_hint && task_hint[0])
      cJSON_AddStringToObject(req, "task_hint", task_hint);
   if (limit_tokens > 0)
      cJSON_AddNumberToObject(req, "limit_tokens", limit_tokens);
   if (session_start)
      cJSON_AddBoolToObject(req, "session_start", 1);
   cJSON_AddStringToObject(
       req, "graph_code_fusion_state",
       (graph_code_fusion_state && graph_code_fusion_state[0]) ? graph_code_fusion_state : "off");
   return kb_v1_action_request("memory.recall", req);
}

char *kb_client_memory_recall_json(const char *task_hint, int limit_tokens, int session_start)
{
   return kb_client_memory_recall_json_ex(task_hint, limit_tokens, session_start, "on");
}

char *kb_client_session_briefing_commitments(int limit)
{
   return kb_client_v1_session_briefing_section("session_briefing.commitments", limit);
}

char *kb_client_session_briefing_directives(int limit)
{
   return kb_client_v1_session_briefing_section("session_briefing.directives", limit);
}

int kb_client_memory_top_l2_facts(memory_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.top_l2_facts", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *f;
      cJSON_ArrayForEach(f, rows)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(f, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_load_eval_corpus(memory_t *out, int max, char *label_out, size_t label_len)
{
   if (label_out && label_len > 0)
      label_out[0] = '\0';
   if (!out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.load_eval_corpus", req);
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

   cJSON *label_j = cJSON_GetObjectItemCaseSensitive(resp, "label");
   if (label_out && label_len > 0 && cJSON_IsString(label_j))
      snprintf(label_out, label_len, "%s", label_j->valuestring);

   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *f;
      cJSON_ArrayForEach(f, rows)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(f, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_get(int64_t id, memory_t *out)
{
   if (!out)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   char *json = kb_v1_action_request("memory.get", req);
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

   cJSON *mem_j = cJSON_GetObjectItemCaseSensitive(resp, "memory");
   if (!cJSON_IsObject(mem_j))
   {
      cJSON_Delete(resp);
      return -1;
   }
   kbc_memory_row_from_json(mem_j, out);
   cJSON_Delete(resp);
   return 0;
}

cJSON *kb_client_memory_briefing(int limit_tokens)
{
   cJSON *req = cJSON_CreateObject();
   if (limit_tokens > 0)
      cJSON_AddNumberToObject(req, "limit_tokens", limit_tokens);
   char *json = kb_v1_action_request("memory.briefing", req);
   if (!json)
      return NULL;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   cJSON *briefing = cJSON_GetObjectItemCaseSensitive(resp, "briefing");
   cJSON *detached = briefing ? cJSON_DetachItemViaPointer(resp, briefing) : NULL;
   cJSON_Delete(resp);
   return detached;
}

static void kbc_memory_relation_from_json(cJSON *r, memory_relation_t *out)
{
   memset(out, 0, sizeof(*out));
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
   cJSON *mid_j = cJSON_GetObjectItemCaseSensitive(r, "memory_id");
   cJSON *eid_j = cJSON_GetObjectItemCaseSensitive(r, "episode_id");
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(r, "src_entity");
   cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(r, "relation");
   cJSON *dst_j = cJSON_GetObjectItemCaseSensitive(r, "dst_entity");
   cJSON *fact_j = cJSON_GetObjectItemCaseSensitive(r, "fact_text");
   cJSON *valid_j = cJSON_GetObjectItemCaseSensitive(r, "valid_at");
   cJSON *invalid_j = cJSON_GetObjectItemCaseSensitive(r, "invalid_at");
   cJSON *weight_j = cJSON_GetObjectItemCaseSensitive(r, "weight");
   cJSON *created_j = cJSON_GetObjectItemCaseSensitive(r, "created_at");
   if (cJSON_IsNumber(id_j))
      out->id = (int64_t)id_j->valuedouble;
   if (cJSON_IsNumber(mid_j))
      out->memory_id = (int64_t)mid_j->valuedouble;
   if (cJSON_IsNumber(eid_j))
      out->episode_id = (int64_t)eid_j->valuedouble;
   if (cJSON_IsString(src_j))
      snprintf(out->src_entity, sizeof(out->src_entity), "%s", src_j->valuestring);
   if (cJSON_IsString(rel_j))
      snprintf(out->relation, sizeof(out->relation), "%s", rel_j->valuestring);
   if (cJSON_IsString(dst_j))
      snprintf(out->dst_entity, sizeof(out->dst_entity), "%s", dst_j->valuestring);
   if (cJSON_IsString(fact_j))
      snprintf(out->fact_text, sizeof(out->fact_text), "%s", fact_j->valuestring);
   if (cJSON_IsString(valid_j))
      snprintf(out->valid_at, sizeof(out->valid_at), "%s", valid_j->valuestring);
   if (cJSON_IsString(invalid_j))
      snprintf(out->invalid_at, sizeof(out->invalid_at), "%s", invalid_j->valuestring);
   if (cJSON_IsNumber(weight_j))
      out->weight = weight_j->valuedouble;
   if (cJSON_IsString(created_j))
      snprintf(out->created_at, sizeof(out->created_at), "%s", created_j->valuestring);
}

int kb_client_memory_get_entity_profile(const char *entity, memory_entity_profile_t *out)
{
   if (!entity || !out)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "entity", entity);
   char *json = kb_v1_action_request("memory.entity_profile", req);
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
   cJSON *prof = cJSON_GetObjectItemCaseSensitive(resp, "profile");
   if (!cJSON_IsObject(prof))
   {
      cJSON_Delete(resp);
      return -1;
   }
   memset(out, 0, sizeof(*out));
   cJSON *ent_j = cJSON_GetObjectItemCaseSensitive(prof, "entity");
   cJSON *mc_j = cJSON_GetObjectItemCaseSensitive(prof, "mention_count");
   cJSON *rc_j = cJSON_GetObjectItemCaseSensitive(prof, "relation_count");
   cJSON *le_j = cJSON_GetObjectItemCaseSensitive(prof, "latest_episode");
   cJSON *sm_j = cJSON_GetObjectItemCaseSensitive(prof, "summary");
   if (cJSON_IsString(ent_j))
      snprintf(out->entity, sizeof(out->entity), "%s", ent_j->valuestring);
   if (cJSON_IsNumber(mc_j))
      out->mention_count = (int)mc_j->valuedouble;
   if (cJSON_IsNumber(rc_j))
      out->relation_count = (int)rc_j->valuedouble;
   if (cJSON_IsString(le_j))
      snprintf(out->latest_episode, sizeof(out->latest_episode), "%s", le_j->valuestring);
   if (cJSON_IsString(sm_j))
      snprintf(out->summary, sizeof(out->summary), "%s", sm_j->valuestring);
   cJSON_Delete(resp);
   return 0;
}

int kb_client_memory_get_entity_edges(const char *entity, int limit, memory_relation_t *out,
                                      int max)
{
   if (!entity || !out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "entity", entity);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.entity_edges", req);
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
   cJSON *edges = cJSON_GetObjectItemCaseSensitive(resp, "edges");
   int n = 0;
   if (cJSON_IsArray(edges))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, edges)
      {
         if (n >= max)
            break;
         kbc_memory_relation_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_search_graph(const char *query, int limit, memory_relation_t *out, int max)
{
   if (!query || !out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.search_graph", req);
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
   cJSON *rels = cJSON_GetObjectItemCaseSensitive(resp, "relations");
   int n = 0;
   if (cJSON_IsArray(rels))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rels)
      {
         if (n >= max)
            break;
         kbc_memory_relation_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_search_graph_as_of(const char *query, const char *as_of, int limit,
                                        memory_relation_t *out, int max)
{
   if (!query || !as_of || !out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   cJSON_AddStringToObject(req, "as_of", as_of);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.search_graph_as_of", req);
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
   cJSON *rels = cJSON_GetObjectItemCaseSensitive(resp, "relations");
   int n = 0;
   if (cJSON_IsArray(rels))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rels)
      {
         if (n >= max)
            break;
         kbc_memory_relation_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_ask(const char *query, const char *scope_type, const char *scope_value,
                         int limit, memory_answer_result_t *out)
{
   if (!query || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   if (scope_type && scope_type[0])
      cJSON_AddStringToObject(req, "scope_type", scope_type);
   if (scope_value && scope_value[0])
      cJSON_AddStringToObject(req, "scope_value", scope_value);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.ask", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *msg_j = cJSON_GetObjectItemCaseSensitive(resp, "message");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      if (cJSON_IsString(msg_j))
         snprintf(out->error, sizeof(out->error), "%s", msg_j->valuestring);
      cJSON_Delete(resp);
      return -1;
   }
   cJSON *answer_j = cJSON_GetObjectItemCaseSensitive(resp, "answer");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(resp, "confidence");
   cJSON *mode_j = cJSON_GetObjectItemCaseSensitive(resp, "evidence_mode");
   cJSON *no_j = cJSON_GetObjectItemCaseSensitive(resp, "no_answer");
   cJSON *low_j = cJSON_GetObjectItemCaseSensitive(resp, "low_confidence");
   cJSON *retr_j = cJSON_GetObjectItemCaseSensitive(resp, "retrieval_count");
   cJSON *cits = cJSON_GetObjectItemCaseSensitive(resp, "citation_ids");
   cJSON *trace = cJSON_GetObjectItemCaseSensitive(resp, "evidence_trace");
   if (cJSON_IsString(answer_j))
      snprintf(out->answer, sizeof(out->answer), "%s", answer_j->valuestring);
   if (cJSON_IsNumber(conf_j))
      out->confidence = conf_j->valuedouble;
   if (cJSON_IsString(mode_j))
      snprintf(out->evidence_mode, sizeof(out->evidence_mode), "%s", mode_j->valuestring);
   if (cJSON_IsBool(no_j))
      out->no_answer = cJSON_IsTrue(no_j) ? 1 : 0;
   if (cJSON_IsBool(low_j))
      out->low_confidence = cJSON_IsTrue(low_j) ? 1 : 0;
   if (cJSON_IsNumber(retr_j))
      out->retrieval_count = (int)retr_j->valuedouble;
   if (cJSON_IsArray(cits))
   {
      cJSON *id_j;
      cJSON_ArrayForEach(id_j, cits)
      {
         if (out->citation_count >= MEMORY_ANSWER_MAX_CITATIONS)
            break;
         if (cJSON_IsNumber(id_j))
            out->citation_ids[out->citation_count++] = (int64_t)id_j->valuedouble;
      }
   }
   if (cJSON_IsObject(trace))
   {
      cJSON *decision = cJSON_GetObjectItemCaseSensitive(trace, "decision");
      cJSON *reason = cJSON_GetObjectItemCaseSensitive(trace, "reason");
      if (cJSON_IsString(decision))
      {
         if (strcmp(decision->valuestring, "answerable") == 0)
            out->evidence.decision = MEMORY_ANSWER_DECISION_ANSWERABLE;
         else if (strcmp(decision->valuestring, "exempt") == 0)
            out->evidence.decision = MEMORY_ANSWER_DECISION_EXEMPT;
         else
            out->evidence.decision = MEMORY_ANSWER_DECISION_ABSTAIN;
      }
      if (cJSON_IsString(reason))
      {
         if (strcmp(reason->valuestring, "structural_empty") == 0)
            out->evidence.reason = MEMORY_ANSWER_REASON_STRUCTURAL_EMPTY;
         else if (strcmp(reason->valuestring, "structural_no_extract") == 0)
            out->evidence.reason = MEMORY_ANSWER_REASON_STRUCTURAL_NO_EXTRACT;
         else if (strcmp(reason->valuestring, "citation_required") == 0)
            out->evidence.reason = MEMORY_ANSWER_REASON_CITATION_REQUIRED;
         else if (strcmp(reason->valuestring, "grounding_low") == 0)
            out->evidence.reason = MEMORY_ANSWER_REASON_GROUNDING_LOW;
         else if (strcmp(reason->valuestring, "chunk_floor") == 0)
            out->evidence.reason = MEMORY_ANSWER_REASON_CHUNK_FLOOR;
         else if (strcmp(reason->valuestring, "curated_exempt") == 0)
            out->evidence.reason = MEMORY_ANSWER_REASON_CURATED_EXEMPT;
         else if (strcmp(reason->valuestring, "db_unavailable") == 0)
            out->evidence.reason = MEMORY_ANSWER_REASON_DB_UNAVAILABLE;
         else
            out->evidence.reason = MEMORY_ANSWER_REASON_OK;
      }
      cJSON *ids = cJSON_GetObjectItemCaseSensitive(trace, "candidate_ids");
      if (cJSON_IsArray(ids))
      {
         cJSON *id_j;
         cJSON_ArrayForEach(id_j, ids)
         {
            if (out->evidence.candidate_id_count >= MEMORY_ANSWER_TRACE_MAX_IDS)
               break;
            if (cJSON_IsNumber(id_j))
               out->evidence.candidate_ids[out->evidence.candidate_id_count++] =
                   (int64_t)id_j->valuedouble;
         }
      }
      cJSON *n = cJSON_GetObjectItemCaseSensitive(trace, "ranked_count");
      if (cJSON_IsNumber(n))
         out->evidence.ranked_count = (int)n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(trace, "anchor_id");
      if (cJSON_IsNumber(n))
         out->evidence.anchor_id = (int64_t)n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(trace, "anchor_rank");
      if (cJSON_IsNumber(n))
         out->evidence.anchor_rank = (int)n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(trace, "topk_grounding");
      if (cJSON_IsNumber(n))
         out->evidence.topk_grounding = n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(trace, "anchor_coverage");
      if (cJSON_IsNumber(n))
         out->evidence.anchor_coverage = n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(trace, "cluster_coverage");
      if (cJSON_IsNumber(n))
         out->evidence.cluster_coverage = n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(trace, "threshold");
      if (cJSON_IsNumber(n))
         out->evidence.threshold = n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(trace, "chunk_floor");
      if (cJSON_IsNumber(n))
         out->evidence.chunk_floor = n->valuedouble;
      cJSON *b = cJSON_GetObjectItemCaseSensitive(trace, "structural");
      if (cJSON_IsBool(b))
         out->evidence.structural = cJSON_IsTrue(b) ? 1 : 0;
      b = cJSON_GetObjectItemCaseSensitive(trace, "exempt");
      if (cJSON_IsBool(b))
         out->evidence.exempt = cJSON_IsTrue(b) ? 1 : 0;
      b = cJSON_GetObjectItemCaseSensitive(trace, "trace_truncated");
      if (cJSON_IsBool(b))
         out->evidence.trace_truncated = cJSON_IsTrue(b) ? 1 : 0;
   }
   cJSON_Delete(resp);
   return 0;
}

int kb_client_memory_get_episode(const char *episode_key, memory_episode_t *out)
{
   if (!episode_key || !out)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "episode_key", episode_key);
   char *json = kb_v1_action_request("memory.get_episode", req);
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
   cJSON *ep = cJSON_GetObjectItemCaseSensitive(resp, "episode");
   if (!cJSON_IsObject(ep))
   {
      cJSON_Delete(resp);
      return -1;
   }
   memset(out, 0, sizeof(*out));
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(ep, "id");
   cJSON *mid_j = cJSON_GetObjectItemCaseSensitive(ep, "memory_id");
   cJSON *key_j = cJSON_GetObjectItemCaseSensitive(ep, "episode_key");
   cJSON *txt_j = cJSON_GetObjectItemCaseSensitive(ep, "episode_text");
   cJSON *src_j = cJSON_GetObjectItemCaseSensitive(ep, "source_session");
   cJSON *ref_j = cJSON_GetObjectItemCaseSensitive(ep, "reference_time");
   cJSON *cre_j = cJSON_GetObjectItemCaseSensitive(ep, "created_at");
   if (cJSON_IsNumber(id_j))
      out->id = (int64_t)id_j->valuedouble;
   if (cJSON_IsNumber(mid_j))
      out->memory_id = (int64_t)mid_j->valuedouble;
   if (cJSON_IsString(key_j))
      snprintf(out->episode_key, sizeof(out->episode_key), "%s", key_j->valuestring);
   if (cJSON_IsString(txt_j))
      snprintf(out->episode_text, sizeof(out->episode_text), "%s", txt_j->valuestring);
   if (cJSON_IsString(src_j))
      snprintf(out->source_session, sizeof(out->source_session), "%s", src_j->valuestring);
   if (cJSON_IsString(ref_j))
      snprintf(out->reference_time, sizeof(out->reference_time), "%s", ref_j->valuestring);
   if (cJSON_IsString(cre_j))
      snprintf(out->created_at, sizeof(out->created_at), "%s", cre_j->valuestring);
   cJSON_Delete(resp);
   return 0;
}

char *kb_client_memory_context_block(const char *query, const char *block_type, int limit)
{
   if (!query)
      return NULL;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   if (block_type && block_type[0])
      cJSON_AddStringToObject(req, "block_type", block_type);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("memory.context_block", req);
   if (!json)
      return NULL;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *block = cJSON_GetObjectItemCaseSensitive(resp, "block");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsString(block))
   {
      cJSON_Delete(resp);
      return NULL;
   }
   char *out = strdup(block->valuestring);
   cJSON_Delete(resp);
   return out;
}

char *kb_client_memory_facts(const char *query)
{
   if (!query || !query[0])
      return NULL;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query);
   char *json = kb_v1_action_request("memory.facts", req);
   if (!json)
      return NULL;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *facts = cJSON_GetObjectItemCaseSensitive(resp, "facts");
   char *out = NULL;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsString(facts) &&
       facts->valuestring[0])
      out = strdup(facts->valuestring);
   cJSON_Delete(resp);
   return out;
}

int kb_client_evidence_emit_retrieval_event(const char *turn_id, const char *role,
                                            const char *query_fingerprint, const int64_t *ids,
                                            int n_ids)
{
   if (!turn_id || !turn_id[0])
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "turn_id", turn_id);
   if (role && role[0])
      cJSON_AddStringToObject(req, "role", role);
   if (query_fingerprint && query_fingerprint[0])
      cJSON_AddStringToObject(req, "query_fingerprint", query_fingerprint);
   cJSON *arr = cJSON_AddArrayToObject(req, "surfaced_ids");
   for (int i = 0; arr && ids && i < n_ids; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)ids[i]));

   char *json = kb_v1_action_request("evidence.emit_retrieval_event", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   cJSON_Delete(resp);
   return ok ? 0 : -1;
}

int kb_client_evidence_merge_retrieval_event(const char *turn_id, const char *role,
                                             const char *query_fingerprint,
                                             const char *const *types, const char *const *refs,
                                             const char *const *versions, int n)
{
   if (!turn_id || !turn_id[0])
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "turn_id", turn_id);
   if (role && role[0])
      cJSON_AddStringToObject(req, "role", role);
   if (query_fingerprint && query_fingerprint[0])
      cJSON_AddStringToObject(req, "query_fingerprint", query_fingerprint);
   cJSON *arr = cJSON_AddArrayToObject(req, "surfaced_refs");
   for (int i = 0; arr && types && refs && i < n; i++)
   {
      if (!types[i] || !types[i][0] || !refs[i] || !refs[i][0])
         continue;
      cJSON *e = cJSON_CreateObject();
      if (!e)
         continue;
      cJSON_AddStringToObject(e, "type", types[i]);
      cJSON_AddStringToObject(e, "ref", refs[i]);
      const char *v = versions ? versions[i] : NULL;
      if (v && v[0])
         cJSON_AddStringToObject(e, "v", v);
      cJSON_AddItemToArray(arr, e);
   }

   char *json = kb_v1_action_request("evidence.merge_retrieval_event", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   cJSON_Delete(resp);
   return ok ? 0 : -1;
}

char *kb_client_evidence_trace_retrieval_event(const char *turn_id)
{
   if (!turn_id || !turn_id[0])
      return NULL;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "turn_id", turn_id);
   /* Pass the KB action's response through verbatim (status + four-state
    * trace_status + retrieval_event_id + event). kb_v1_action_request owns req. */
   return kb_v1_action_request("evidence.trace_retrieval_event", req);
}

char *kb_client_evidence_provenance_retrieval_event(const char *turn_id)
{
   if (!turn_id || !turn_id[0])
      return NULL;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "turn_id", turn_id);
   /* Pass the KB action's response through verbatim (status + provenance_status +
    * retrieval_event_id + sources[]). kb_v1_action_request owns req. */
   return kb_v1_action_request("evidence.provenance_retrieval_event", req);
}

char *kb_client_evidence_fidelity_retrieval_event(const char *turn_id)
{
   if (!turn_id || !turn_id[0])
      return NULL;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "turn_id", turn_id);
   /* Pass the KB action's response through verbatim (status + fidelity_status +
    * report + attribution_count). kb_v1_action_request owns req. */
   return kb_v1_action_request("evidence.fidelity_retrieval_event", req);
}

char *kb_client_memory_lint_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("memory.lint", req);
}
