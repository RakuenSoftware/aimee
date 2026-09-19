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
#include "kb_client_pii.h"
#include "db1_client/caches.h"
#include "headers/module_commands.h"
#include "db1_optional.h"
#include "cJSON.h"
#include "config.h"
#include "memory_query.h" /* db2_memory_low_eff_row_t etc. */
#include "tasks.h"
#include "integrity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread int s_kbc_memory_scope_active;
static __thread int s_kbc_memory_scope_all;
static __thread char s_kbc_memory_workspace[512];
static __thread char s_kbc_memory_project[512];

#define KBC_MEMORY_ACTIVATION_MAX_ROWS 256

typedef struct
{
   int64_t memory_id;
   int64_t last_turn;
} kbc_memory_activation_row_t;

typedef struct
{
   kbc_memory_activation_row_t rows[KBC_MEMORY_ACTIVATION_MAX_ROWS];
   int count;
   int64_t current_turn;
   int loaded;
} kbc_memory_activation_t;

static void kbc_memory_activation_load(kbc_memory_activation_t *activation)
{
   memset(activation, 0, sizeof(*activation));
   const char *conversation_id = session_id();
   if (!conversation_id || !conversation_id[0] || !db1_context_snapshot_activation)
      return;

   char rows[KBC_MEMORY_ACTIVATION_MAX_ROWS][DB1_CONTEXT_ACTIVATION_ROW_LEN];
   int n = db1_context_snapshot_activation(conversation_id, rows, KBC_MEMORY_ACTIVATION_MAX_ROWS);
   if (n < 0)
      return;

   for (int i = 0; i < n && activation->count < KBC_MEMORY_ACTIVATION_MAX_ROWS; i++)
   {
      char *end = NULL;
      long long memory_id = strtoll(rows[i], &end, 10);
      if (memory_id < 0 || !end)
         continue;
      long long turn = strtoll(end, NULL, 10);
      if (turn < 0)
         continue;
      if (memory_id == 0)
      {
         activation->current_turn = (int64_t)turn;
         continue;
      }
      activation->rows[activation->count].memory_id = (int64_t)memory_id;
      activation->rows[activation->count].last_turn = (int64_t)turn;
      activation->count++;
   }
   activation->loaded = activation->current_turn > 0;
}

static void kbc_memory_activation_add(cJSON *req, kbc_memory_activation_t *activation)
{
   kbc_memory_activation_load(activation);
   if (!req || !activation->loaded)
      return;
   cJSON *obj = cJSON_AddObjectToObject(req, "activation");
   cJSON *rows = obj ? cJSON_AddArrayToObject(obj, "rows") : NULL;
   if (!obj || !rows)
      return;
   cJSON_AddNumberToObject(obj, "current_turn", (double)activation->current_turn);
   for (int i = 0; i < activation->count; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)activation->rows[i].memory_id);
      cJSON_AddNumberToObject(row, "last_turn", (double)activation->rows[i].last_turn);
      cJSON_AddItemToArray(rows, row);
   }
}

static void kbc_memory_activation_record_recall(const cJSON *response,
                                                const kbc_memory_activation_t *activation)
{
   static const char *sections[] = {"identity", "preferences", "active_context", "open_commitments",
                                    NULL};
   const cJSON *recall = cJSON_GetObjectItemCaseSensitive(response, "recall");
   int64_t seen[64];
   int seen_n = 0;
   for (int s = 0; cJSON_IsObject(recall) && sections[s]; s++)
   {
      const cJSON *arr = cJSON_GetObjectItemCaseSensitive(recall, sections[s]);
      const cJSON *item = NULL;
      cJSON_ArrayForEach(item, arr)
      {
         const cJSON *managed = cJSON_GetObjectItemCaseSensitive(item, "activation_managed");
         if (!cJSON_IsTrue(managed))
            continue;
         const cJSON *mid = cJSON_GetObjectItemCaseSensitive(item, "memory_id");
         if (!cJSON_IsNumber(mid) || mid->valuedouble <= 0.0)
            continue;
         int64_t id = (int64_t)mid->valuedouble;
         int duplicate = 0;
         for (int i = 0; i < seen_n; i++)
            if (seen[i] == id)
               duplicate = 1;
         if (duplicate)
            continue;
         if (seen_n < (int)(sizeof(seen) / sizeof(seen[0])))
            seen[seen_n++] = id;
         const char *conversation_id = session_id();
         if (activation->loaded && activation->current_turn > 0 && conversation_id &&
             conversation_id[0] && db1_context_snapshot_insert_turn)
            (void)db1_context_snapshot_insert_turn(conversation_id, id, 0.0,
                                                   activation->current_turn);
      }
   }
}

void kb_client_memory_scope_context_set(const char *workspace, const char *project, int include_all)
{
   s_kbc_memory_scope_active = 1;
   s_kbc_memory_scope_all = include_all ? 1 : 0;
   snprintf(s_kbc_memory_workspace, sizeof(s_kbc_memory_workspace), "%s",
            workspace ? workspace : "");
   snprintf(s_kbc_memory_project, sizeof(s_kbc_memory_project), "%s", project ? project : "");
}

void kb_client_memory_scope_context_clear(void)
{
   s_kbc_memory_scope_active = 0;
   s_kbc_memory_scope_all = 0;
   s_kbc_memory_workspace[0] = '\0';
   s_kbc_memory_project[0] = '\0';
}

void kb_client_memory_scope_context_apply(cJSON *req)
{
   if (!req || !s_kbc_memory_scope_active)
      return;
   /* Preserve an explicit per-operation value.  Besides making this helper
    * idempotent, that keeps exact caller scope from becoming an ambiguous
    * duplicate JSON key when an ambient agent request context also exists. */
   if (!cJSON_GetObjectItemCaseSensitive(req, "scope_context"))
      cJSON_AddBoolToObject(req, "scope_context", 1);
   if (!cJSON_GetObjectItemCaseSensitive(req, "include_all"))
      cJSON_AddBoolToObject(req, "include_all", s_kbc_memory_scope_all);
   if (s_kbc_memory_workspace[0] && !cJSON_GetObjectItemCaseSensitive(req, "workspace"))
      cJSON_AddStringToObject(req, "workspace", s_kbc_memory_workspace);
   if (s_kbc_memory_project[0] && !cJSON_GetObjectItemCaseSensitive(req, "project"))
      cJSON_AddStringToObject(req, "project", s_kbc_memory_project);
}

static void kbc_memory_add_scope_context(cJSON *req)
{
   kb_client_memory_scope_context_apply(req);
}

/* Single-record readers use 1 for a valid miss and -1 for an unavailable or
 * malformed result. Keeping those outcomes distinct prevents callers from
 * rendering a stale thread-local dependency status as a genuine not-found. */
static int kbc_memory_single_miss(const cJSON *resp)
{
   const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
   return (cJSON_IsString(status) && (strcmp(status->valuestring, "empty") == 0 ||
                                      strcmp(status->valuestring, "not_found") == 0)) ||
          (cJSON_IsString(message) && strstr(message->valuestring, "not found") != NULL);
}

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
   kbc_memory_add_scope_context(req);
   cJSON_AddStringToObject(req, "query", query);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   /* Fusion policy belongs to the receiving instance. */
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
   kbc_memory_add_scope_context(req);
   cJSON_AddStringToObject(req, "query", query);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   /* The legacy fusion argument is retained for ABI compatibility only. */
   (void)graph_code_fusion_state; /* legacy argument; instance policy always wins */
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
   kbc_memory_add_scope_context(req);
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
      PICK(hybrid_total);
      PICK(blended_total);
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
   kbc_memory_add_scope_context(req);
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
   (void)graph_code_fusion_state; /* legacy argument; instance policy always wins */
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
   kbc_memory_add_scope_context(req);
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
   kbc_memory_add_scope_context(req);
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

char *kb_client_memory_assemble_typed_context(const char *query)
{
   if (!query || !query[0])
      return NULL;

   cJSON *req = cJSON_CreateObject();
   kbc_memory_add_scope_context(req);
   cJSON_AddStringToObject(req, "query", query);
   char *json = kb_v1_action_request("memory.assemble_typed_context", req);
   if (!json)
      return NULL;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *used = cJSON_GetObjectItemCaseSensitive(resp, "used_tokens");
   cJSON *body = cJSON_GetObjectItemCaseSensitive(resp, "rendered_context");
   char *out = NULL;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(used) &&
       used->valuedouble > 0 && cJSON_IsString(body) && body->valuestring[0])
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

int kb_client_memory_delete_as(int64_t id, memory_authority_t authority)
{
   cJSON *req = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(req);
   cJSON_AddNumberToObject(req, "id", (double)id);
   /* Only ever sent for a user/operator caller. The KB treats an absent field as
    * model authority, so a stale client cannot destroy anything. */
   if (authority == MEMORY_AUTHORITY_USER)
      cJSON_AddStringToObject(req, "authority", "user");
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
   kb_client_memory_audit_note(authority == MEMORY_AUTHORITY_USER ? "memory.delete"
                                                                  : "memory.retire",
                               id, NULL, NULL, NULL, 0.0, NULL, rc == 0);
   return rc;
}

int kb_client_memory_delete(int64_t id)
{
   return kb_client_memory_delete_as(id, MEMORY_AUTHORITY_USER);
}

/* The native host transports the scoped shared bundle to its local Go memory
 * owner. Selection, collision precedence and final budgeting belong to Go. */
static char *memory_recall_json(const char *task_hint, int limit_tokens, int session_start,
                                const char *graph_code_fusion_state, int include_user)
{
   cJSON *req = cJSON_CreateObject();
   kbc_memory_add_scope_context(req);
   kbc_memory_activation_t activation;
   kbc_memory_activation_add(req, &activation);
   if (task_hint && task_hint[0])
      cJSON_AddStringToObject(req, "task_hint", task_hint);
   if (limit_tokens > 0)
      cJSON_AddNumberToObject(req, "limit_tokens", limit_tokens);
   if (session_start)
      cJSON_AddBoolToObject(req, "session_start", 1);
   (void)graph_code_fusion_state; /* legacy argument; instance policy always wins */
   char *j = kb_v1_action_request("memory.recall", req);
   if (!j)
      return NULL;

   /* Every recall consumer shares this seam. Reclassify legacy/future rows at
    * materialization before they can become prompt authority. */
   integrity_result_t recall_gate;
   if (integrity_ingress_decide(j, INTEGRITY_SOURCE_AGENT_MESSAGE, "recall", 1, &recall_gate))
   {
      free(j);
      return strdup("{\"status\":\"quarantined\",\"recall\":{},"
                    "\"integrity_verdict\":\"quarantine\"}");
   }

   if (include_user)
   {
      cJSON *request = cJSON_CreateObject();
      if (!request)
      {
         free(j);
         return NULL;
      }
      cJSON_AddStringToObject(request, "operation", "compose-recall");
      cJSON_AddStringToObject(request, "shared_json", j);
      cJSON_AddNumberToObject(request, "limit_tokens", limit_tokens);
      cJSON_AddBoolToObject(request, "session_start", session_start != 0);
      cJSON *reply = NULL;
      int rc = aimee_module_commands_dispatch_internal("memory.runtime", request, &reply);
      cJSON_Delete(request);
      free(j);
      const char *body =
          rc == 1 ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "json")) : NULL;
      j = body ? strdup(body) : NULL;
      cJSON_Delete(reply);
      if (!j)
         return NULL;
   }
   cJSON *response = cJSON_Parse(j);
   if (response)
   {
      kbc_memory_activation_record_recall(response, &activation);
      cJSON_Delete(response);
   }
   return j;
}

/* Explicit shared-store reads must never merge personal rows into the result. */
char *kb_client_memory_recall_shared_json(const char *task_hint, int limit_tokens,
                                          int session_start)
{
   return memory_recall_json(task_hint, limit_tokens, session_start, "on", 0);
}

/* Preserve the combined bundle for existing composed-context consumers. */
char *kb_client_memory_recall_json_ex(const char *task_hint, int limit_tokens, int session_start,
                                      const char *graph_code_fusion_state)
{
   return memory_recall_json(task_hint, limit_tokens, session_start, graph_code_fusion_state, 1);
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
   kbc_memory_add_scope_context(req);
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
   return kb_client_memory_get_as_of(id, NULL, out, NULL);
}

int kb_client_memory_get_json_as_of(int64_t id, const char *as_of, cJSON **out,
                                    kb_valid_at_t *verdict)
{
   if (verdict)
      *verdict = KB_VALID_AT_UNASKED;
   if (!out)
      return -1;
   *out = NULL;

   cJSON *req = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(req);
   cJSON_AddNumberToObject(req, "id", (double)id);
   /* Only sent when asked. aimee-kb emits as_of/valid_at exactly when it
    * receives a non-empty as_of, so an empty one here would be indistinguishable
    * from not asking -- and this omission is what left `memory get --as-of`
    * marshalling the flag correctly and then dropping it on this hop. */
   if (as_of && as_of[0])
      cJSON_AddStringToObject(req, "as_of", as_of);
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
      int miss = kbc_memory_single_miss(resp);
      cJSON_Delete(resp);
      return miss ? 1 : -1;
   }

   cJSON *mem_j = cJSON_GetObjectItemCaseSensitive(resp, "memory");
   if (!cJSON_IsObject(mem_j))
   {
      cJSON_Delete(resp);
      return -1;
   }
   /* aimee-kb answers with a bool, or the string "unknown" when it could not
    * tell. A missing key means it was not asked. */
   if (verdict)
   {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "valid_at");
      if (cJSON_IsString(v))
         *verdict = KB_VALID_AT_UNKNOWN;
      else if (cJSON_IsBool(v))
         *verdict = cJSON_IsTrue(v) ? KB_VALID_AT_YES : KB_VALID_AT_NO;
   }
   *out = cJSON_DetachItemFromObjectCaseSensitive(resp, "memory");
   cJSON_Delete(resp);
   return *out ? 0 : -1;
}

int kb_client_memory_get_as_of(int64_t id, const char *as_of, memory_t *out, kb_valid_at_t *verdict)
{
   if (!out)
      return -1;
   cJSON *mem_j = NULL;
   int rc = kb_client_memory_get_json_as_of(id, as_of, &mem_j, verdict);
   if (rc != 0)
      return rc;
   kbc_memory_row_from_json(mem_j, out);
   cJSON_Delete(mem_j);
   return 0;
}

int kb_client_evidence_emit_retrieval_event_ex(const char *turn_id, const char *role,
                                               const char *query_fingerprint, const int64_t *ids,
                                               int n_ids, char *event_id_out, size_t event_id_len)
{
   if (event_id_out && event_id_len > 0)
      event_id_out[0] = '\0';
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
   if (ok && event_id_out && event_id_len > 0)
   {
      cJSON *ev = cJSON_GetObjectItemCaseSensitive(resp, "retrieval_event_id");
      if (cJSON_IsString(ev) && ev->valuestring[0])
         snprintf(event_id_out, event_id_len, "%s", ev->valuestring);
   }
   cJSON_Delete(resp);
   return ok ? 0 : -1;
}

int kb_client_evidence_emit_retrieval_event(const char *turn_id, const char *role,
                                            const char *query_fingerprint, const int64_t *ids,
                                            int n_ids)
{
   return kb_client_evidence_emit_retrieval_event_ex(turn_id, role, query_fingerprint, ids, n_ids,
                                                     NULL, 0);
}

/* Record retrieval outcomes for surfaced rows against an event, as one batch.
 * `surface` selects the KB-service method: "memory" -> memory.record_retrieval_outcome
 * (writes retrieval_attribution), "ranker" -> ranker.record_outcome (writes
 * ranker_outcome). Returns 0 on an ok response, -1 otherwise. */
int kb_client_record_retrieval_outcome(const char *surface, const char *event_id,
                                       const int64_t *ids, int n, const char *verdict)
{
   if (!event_id || !event_id[0] || !verdict || !verdict[0] || n <= 0 || !ids)
      return -1;
   const char *method = (surface && strcmp(surface, "ranker") == 0)
                            ? "ranker.record_outcome"
                            : "memory.record_retrieval_outcome";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "retrieval_event_id", event_id);
   cJSON *rows = cJSON_AddArrayToObject(req, "rows");
   for (int i = 0; rows && i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      cJSON_AddNumberToObject(row, "id", (double)ids[i]);
      cJSON_AddStringToObject(row, "verdict", verdict);
      cJSON_AddItemToArray(rows, row);
   }
   char *json = kb_v1_action_request(method, req);
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

int kb_client_ranker_emit_event(const int64_t *doc_ids, int n, const char *query_fingerprint,
                                char *event_id_out, size_t event_id_len)
{
   if (event_id_out && event_id_len > 0)
      event_id_out[0] = '\0';
   if (!doc_ids || n <= 0)
      return -1;
   cJSON *req = cJSON_CreateObject();
   if (query_fingerprint && query_fingerprint[0])
      cJSON_AddStringToObject(req, "query_fingerprint", query_fingerprint);
   cJSON *arr = cJSON_AddArrayToObject(req, "doc_ids");
   for (int i = 0; arr && i < n; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)doc_ids[i]));
   char *json = kb_v1_action_request("ranker.emit_event", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *ev = cJSON_GetObjectItemCaseSensitive(resp, "retrieval_event_id");
   int ok = cJSON_IsString(ev) && ev->valuestring[0];
   if (ok && event_id_out && event_id_len > 0)
      snprintf(event_id_out, event_id_len, "%s", ev->valuestring);
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

/* Host-owned file bytes travel unchanged; parsing, case selection, retrieval,
 * scoring and report construction belong to the Go memory command. Consumes req. */
char *kb_client_memory_benchmark_json(cJSON *req, const char *corpus_path)
{
   if (!req)
      return NULL;
   if (corpus_path)
   {
      FILE *fp = fopen(corpus_path, "rb");
      char *input = fp ? malloc(1048578) : NULL;
      if (!fp || !input)
      {
         if (fp)
            fclose(fp);
         cJSON_Delete(req);
         return NULL;
      }
      size_t count = fread(input, 1, 1048577, fp);
      int failed = ferror(fp);
      fclose(fp);
      if (failed || count > 1048576 || memchr(input, '\0', count))
      {
         free(input);
         cJSON_Delete(req);
         return NULL;
      }
      input[count] = '\0';
      if (!cJSON_AddStringToObject(req, "corpus_json", input))
      {
         free(input);
         cJSON_Delete(req);
         return NULL;
      }
      free(input);
   }
   return kb_v1_action_request_with_timeout("memory.benchmark", req, 120000);
}
