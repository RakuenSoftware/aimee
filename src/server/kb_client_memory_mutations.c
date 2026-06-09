/* kb_client_memory_mutations.c: mutation/bookkeeping wrappers for memory.* RPCs */
#include "kb_client_memory_internal.h"
#include "memory_query.h"
#include "tasks.h"

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
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }
   if (out)
   {
      cJSON *mem = cJSON_GetObjectItemCaseSensitive(resp, "memory");
      if (cJSON_IsObject(mem))
         kbc_memory_row_from_json(mem, out);
      else
         memset(out, 0, sizeof(*out));
   }
   cJSON_Delete(resp);
   return 0;
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
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }

   if (out)
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
   }
   cJSON_Delete(resp);
   return 0;
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

int kb_client_memory_update(int64_t id, const char *content)
{
   if (id <= 0 || !content || !content[0])
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   cJSON_AddStringToObject(req, "content", content);
   char *json = kb_v1_action_request("memory.update", req);
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
   return rc;
}
