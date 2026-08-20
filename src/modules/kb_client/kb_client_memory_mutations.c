/* kb_client_memory_mutations.c: mutation/bookkeeping wrappers for memory.* RPCs */
#include "kb_client_memory_internal.h"
#include "memory_query.h"
#include "tasks.h"

#include "kb_client.h" /* kb_client_memory_audit_note (defined in kb_client_memory_audit.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kb_client_memory_supersede(int64_t old_id, const char *new_content, double confidence,
                               const char *session_id, memory_t *out)
{
   if (old_id <= 0 || !new_content)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "old_id", (double)old_id);
   cJSON_AddStringToObject(req, "new_content", new_content);
   cJSON_AddNumberToObject(req, "confidence", confidence);
   if (session_id && session_id[0])
      cJSON_AddStringToObject(req, "session_id", session_id);
   char *json = kb_v1_action_request("memory.supersede", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   int64_t new_id = old_id;
   if (ok)
   {
      cJSON *mem = cJSON_GetObjectItemCaseSensitive(resp, "memory");
      if (cJSON_IsObject(mem))
      {
         if (out)
            kbc_memory_row_from_json(mem, out);
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(mem, "id");
         if (cJSON_IsNumber(id_j))
            new_id = (int64_t)id_j->valuedouble;
      }
      else if (out)
      {
         memset(out, 0, sizeof(*out));
      }
   }
   cJSON_Delete(resp);
   /* supersede rewrites an existing memory's content — a server-initiated
    * mutation, audited like the others (id-only; no kind/key, never content). */
   kb_client_memory_audit_note("memory.supersede", new_id, NULL, NULL, NULL, confidence, session_id,
                               ok);
   return ok ? 0 : -1;
}

/* Typed-fact §4 retraction. Reports the number of edges affected via
 * *out_retracted (0 is a legitimate success: nothing current matched), and
 * distinguishes a refusal on an immutable relation from a generic failure so the
 * caller can say which happened. */
int kb_client_facts_retract(const char *source, const char *relation, const char *target,
                            const char *authority, int *out_retracted, int *out_immutable)
{
   if (out_retracted)
      *out_retracted = 0;
   if (out_immutable)
      *out_immutable = 0;
   if (!source || !source[0] || !relation || !relation[0])
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "source", source);
   cJSON_AddStringToObject(req, "relation", relation);
   if (target && target[0])
      cJSON_AddStringToObject(req, "target", target);
   if (authority && authority[0])
      cJSON_AddStringToObject(req, "authority", authority);
   char *json = kb_v1_action_request("facts.retract", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (ok)
   {
      cJSON *n = cJSON_GetObjectItemCaseSensitive(resp, "retracted");
      if (cJSON_IsNumber(n) && out_retracted)
         *out_retracted = (int)n->valuedouble;
   }
   else if (out_immutable)
   {
      cJSON *reason = cJSON_GetObjectItemCaseSensitive(resp, "reason");
      if (cJSON_IsString(reason) && strcmp(reason->valuestring, "immutable") == 0)
         *out_immutable = 1;
   }
   cJSON_Delete(resp);
   /* A retraction removes a belief from recall — as much a mutation as a store,
    * and audited on the same terms (identifiers only, never fact content). */
   kb_client_memory_audit_note("facts.retract", 0, NULL, relation, NULL, 0.0, NULL, ok);
   return ok ? 0 : -1;
}

int kb_client_entities_merge(int64_t from_id, int64_t into_id, int64_t *out_merge_id)
{
   if (out_merge_id)
      *out_merge_id = 0;
   if (from_id <= 0 || into_id <= 0)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "from_id", (double)from_id);
   cJSON_AddNumberToObject(req, "into_id", (double)into_id);
   char *json = kb_v1_action_request("entities.merge", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (ok && out_merge_id)
   {
      cJSON *mid = cJSON_GetObjectItemCaseSensitive(resp, "merge_id");
      if (cJSON_IsNumber(mid))
         *out_merge_id = (int64_t)mid->valuedouble;
   }
   cJSON_Delete(resp);
   kb_client_memory_audit_note("entities.merge", from_id, NULL, NULL, NULL, 0.0, NULL, ok);
   return ok ? 0 : -1;
}

int kb_client_entities_unmerge(int64_t merge_id)
{
   if (merge_id <= 0)
      return -1;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "merge_id", (double)merge_id);
   char *json = kb_v1_action_request("entities.unmerge", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   cJSON_Delete(resp);
   kb_client_memory_audit_note("entities.unmerge", merge_id, NULL, NULL, NULL, 0.0, NULL, ok);
   return ok ? 0 : -1;
}

int kb_client_memory_fact_history(const char *key, memory_t *out, int max)
{
   if (!key || !out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "key", key);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.fact_history", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "history");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_list_session_scope_priority(memory_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(req);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.list_session_scope_priority", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_list_session_scope_priority_like(const char *pattern, memory_t *out, int max)
{
   if (!pattern || !out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(req);
   cJSON_AddStringToObject(req, "pattern", pattern);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.list_session_scope_priority_like", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_check_drift(int64_t task_id, const char *file_path, const char *command,
                                 drift_result_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "task_id", (double)task_id);
   if (file_path && file_path[0])
      cJSON_AddStringToObject(req, "file_path", file_path);
   if (command && command[0])
      cJSON_AddStringToObject(req, "command", command);
   char *json = kb_v1_action_request("memory.check_drift", req);
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
   cJSON *drifted_j = cJSON_GetObjectItemCaseSensitive(resp, "drifted");
   cJSON *tid_j = cJSON_GetObjectItemCaseSensitive(resp, "task_id");
   cJSON *title_j = cJSON_GetObjectItemCaseSensitive(resp, "task_title");
   cJSON *msg_j = cJSON_GetObjectItemCaseSensitive(resp, "message");
   if (cJSON_IsBool(drifted_j))
      out->drifted = cJSON_IsTrue(drifted_j) ? 1 : 0;
   if (cJSON_IsNumber(tid_j))
      out->task_id = (int64_t)tid_j->valuedouble;
   if (cJSON_IsString(title_j))
      snprintf(out->task_title, sizeof(out->task_title), "%s", title_j->valuestring);
   if (cJSON_IsString(msg_j))
      snprintf(out->message, sizeof(out->message), "%s", msg_j->valuestring);
   cJSON_Delete(resp);
   return 0;
}

int64_t kb_client_memory_find_id_by_key_kind(const char *key, const char *kind)
{
   if (!key || !kind)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "key", key);
   cJSON_AddStringToObject(req, "kind", kind);
   char *json = kb_v1_action_request("memory.find_id_by_key_kind", req);
   if (!json)
      return 0;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(resp, "id");
   int64_t id = 0;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(id_j))
      id = (int64_t)id_j->valuedouble;
   cJSON_Delete(resp);
   return id;
}

int kb_client_memory_search_facts_patterns_by_keyword(const char *keyword, memory_t *out, int max)
{
   if (!keyword || !out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(req);
   cJSON_AddStringToObject(req, "keyword", keyword);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.search_facts_patterns_by_keyword", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         kbc_memory_row_from_json(r, &out[n++]);
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_insert(const char *tier, const char *kind, const char *key,
                            const char *content, double confidence, const char *session_id,
                            memory_t *out)
{
   return kb_client_memory_insert_ex(tier, kind, key, content, "", confidence, session_id, out);
}

int kb_client_memory_insert_ex(const char *tier, const char *kind, const char *key,
                               const char *content, const char *use_cases, double confidence,
                               const char *session_id, memory_t *out)
{
   if (!key || !content)
      return -1;

   cJSON *req = cJSON_CreateObject();
   if (tier && tier[0])
      cJSON_AddStringToObject(req, "tier", tier);
   if (kind && kind[0])
      cJSON_AddStringToObject(req, "kind", kind);
   cJSON_AddStringToObject(req, "key", key);
   cJSON_AddStringToObject(req, "content", content);
   if (use_cases && use_cases[0])
      cJSON_AddStringToObject(req, "use_cases", use_cases);
   cJSON_AddNumberToObject(req, "confidence", confidence);
   if (session_id && session_id[0])
      cJSON_AddStringToObject(req, "session_id", session_id);
   char *json = kb_v1_action_request("memory.store", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;

   int64_t rec_id = 0;
   if (ok && out)
   {
      memset(out, 0, sizeof(*out));
      cJSON *mem_j = cJSON_GetObjectItemCaseSensitive(resp, "memory");
      if (cJSON_IsObject(mem_j))
      {
         kbc_memory_row_from_json(mem_j, out);
      }
      else
      {
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(resp, "id");
         if (cJSON_IsNumber(id_j))
            out->id = (int64_t)id_j->valuedouble;
      }
      rec_id = out->id;
   }
   else if (ok)
   {
      /* Extract the id for the audit note even when the caller wants no row back. */
      cJSON *mem_j = cJSON_GetObjectItemCaseSensitive(resp, "memory");
      cJSON *id_j = cJSON_IsObject(mem_j) ? cJSON_GetObjectItemCaseSensitive(mem_j, "id")
                                          : cJSON_GetObjectItemCaseSensitive(resp, "id");
      if (cJSON_IsNumber(id_j))
         rec_id = (int64_t)id_j->valuedouble;
   }
   cJSON_Delete(resp);
   /* NON-CONTENT audit: identity + confidence + session only, never the content.
    * Fires on the kb's verdict (a kb-rejected insert is recorded as ok=0), so the
    * trail records rejected stores too — symmetric with update/delete/reject. */
   kb_client_memory_audit_note("memory.insert", rec_id, tier, kind, key, confidence, session_id,
                               ok);
   return ok ? 0 : -1;
}

int kb_client_memory_list_low_effectiveness(double threshold, int limit,
                                            db2_memory_low_eff_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "threshold", threshold);
   cJSON_AddNumberToObject(req, "limit", limit > 0 ? limit : max);
   char *json = kb_v1_action_request("memory.list_low_effectiveness", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "rows");
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
         cJSON *tier_j = cJSON_GetObjectItemCaseSensitive(r, "tier");
         cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(r, "kind");
         cJSON *key_j = cJSON_GetObjectItemCaseSensitive(r, "key");
         cJSON *eff_j = cJSON_GetObjectItemCaseSensitive(r, "effectiveness");
         cJSON *uc_j = cJSON_GetObjectItemCaseSensitive(r, "use_count");
         if (cJSON_IsNumber(id_j))
            out[n].id = (int64_t)id_j->valuedouble;
         if (cJSON_IsString(tier_j))
            snprintf(out[n].tier, sizeof(out[n].tier), "%s", tier_j->valuestring);
         if (cJSON_IsString(kind_j))
            snprintf(out[n].kind, sizeof(out[n].kind), "%s", kind_j->valuestring);
         if (cJSON_IsString(key_j))
            snprintf(out[n].key, sizeof(out[n].key), "%s", key_j->valuestring);
         if (cJSON_IsNumber(eff_j))
            out[n].effectiveness = eff_j->valuedouble;
         if (cJSON_IsNumber(uc_j))
            out[n].use_count = (int)uc_j->valuedouble;
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_list_unused_l2(int days, db2_memory_unused_l2_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "days", days);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.list_unused_l2", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "rows");
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
         cJSON *key_j = cJSON_GetObjectItemCaseSensitive(r, "key");
         cJSON *tier_j = cJSON_GetObjectItemCaseSensitive(r, "tier");
         cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(r, "kind");
         cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(r, "confidence");
         if (cJSON_IsNumber(id_j))
            out[n].id = (int64_t)id_j->valuedouble;
         if (cJSON_IsString(key_j))
            snprintf(out[n].key, sizeof(out[n].key), "%s", key_j->valuestring);
         if (cJSON_IsString(tier_j))
            snprintf(out[n].tier, sizeof(out[n].tier), "%s", tier_j->valuestring);
         if (cJSON_IsString(kind_j))
            snprintf(out[n].kind, sizeof(out[n].kind), "%s", kind_j->valuestring);
         if (cJSON_IsNumber(conf_j))
            out[n].confidence = conf_j->valuedouble;
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_list_superseded_keys(int min_versions, db2_memory_superseded_row_t *out,
                                          int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "min_versions", min_versions);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.list_superseded_keys", req);
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
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "rows");
   int n = 0;
   if (cJSON_IsArray(rows))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, rows)
      {
         if (n >= max)
            break;
         memset(&out[n], 0, sizeof(out[n]));
         cJSON *bk_j = cJSON_GetObjectItemCaseSensitive(r, "base_key");
         cJSON *v_j = cJSON_GetObjectItemCaseSensitive(r, "versions");
         if (cJSON_IsString(bk_j))
            snprintf(out[n].base_key, sizeof(out[n].base_key), "%s", bk_j->valuestring);
         if (cJSON_IsNumber(v_j))
            out[n].versions = (int)v_j->valuedouble;
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_set_artifact(int64_t memory_id, const char *artifact_type,
                                  const char *artifact_ref, const char *artifact_hash)
{
   if (!artifact_type || !artifact_ref)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   cJSON_AddStringToObject(req, "artifact_type", artifact_type);
   cJSON_AddStringToObject(req, "artifact_ref", artifact_ref);
   if (artifact_hash && artifact_hash[0])
      cJSON_AddStringToObject(req, "artifact_hash", artifact_hash);
   char *json = kb_v1_action_request("memory.set_artifact", req);
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

int kb_client_memory_touch(int64_t id)
{
   if (id <= 0)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   char *json = kb_v1_action_request("memory.touch", req);
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

int kb_client_memory_update_as(int64_t id, const char *content, memory_authority_t authority,
                               int64_t *new_id_out)
{
   if (new_id_out)
      *new_id_out = 0;
   if (id <= 0 || !content || !content[0])
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   cJSON_AddStringToObject(req, "content", content);
   if (authority == MEMORY_AUTHORITY_USER)
      cJSON_AddStringToObject(req, "authority", "user");
   char *json = kb_v1_action_request("memory.update", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   if (rc == 0 && new_id_out)
   {
      cJSON *nid = cJSON_GetObjectItemCaseSensitive(resp, "id");
      *new_id_out = cJSON_IsNumber(nid) ? (int64_t)nid->valuedouble : id;
   }
   cJSON_Delete(resp);
   kb_client_memory_audit_note("memory.update", id, NULL, NULL, NULL, 0.0, NULL, rc == 0);
   return rc;
}

int kb_client_memory_update(int64_t id, const char *content)
{
   return kb_client_memory_update_as(id, content, MEMORY_AUTHORITY_USER, NULL);
}

int kb_client_memory_reject(int64_t id, const char *reason)
{
   if (id <= 0)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   if (reason && reason[0])
      cJSON_AddStringToObject(req, "reason", reason);
   char *json = kb_v1_action_request("memory.reject", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   kb_client_memory_audit_note("memory.reject", id, NULL, NULL, NULL, 0.0, NULL, rc == 0);
   return rc;
}
