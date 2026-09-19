/* kb_client_memory_mutations.c: mutation/bookkeeping wrappers for memory.* RPCs */
#include "kb_client_memory_internal.h"
#include "kb_client_pii.h"
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

   char *sup_red = NULL;
   if (kb_client_pii_screen(new_content, &sup_red) != 0)
   {
      kb_client_memory_audit_note("memory.supersede.withheld_pii", old_id, "", "", "", confidence,
                                  session_id, 0);
      return KB_CLIENT_MEMORY_WITHHELD_PII;
   }

   cJSON *req = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(req);
   cJSON_AddNumberToObject(req, "old_id", (double)old_id);
   cJSON_AddStringToObject(req, "new_content", sup_red ? sup_red : new_content);
   free(sup_red);
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
   kb_client_memory_scope_context_apply(req);
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
   return kb_client_memory_insert_as(tier, kind, key, content, use_cases, confidence, session_id,
                                     MEMORY_AUTHORITY_MODEL, out);
}

int kb_client_memory_insert_as(const char *tier, const char *kind, const char *key,
                               const char *content, const char *use_cases, double confidence,
                               const char *session_id, memory_authority_t authority, memory_t *out)
{
   if (!key || !content)
      return -1;

   char *content_red = NULL;
   if (kb_client_pii_identifier_sensitive(key) || kb_client_pii_screen(content, &content_red) != 0)
   {
      free(content_red);
      /* Deliberately no key in this note: the key may be what was sensitive. */
      kb_client_memory_audit_note("memory.insert.withheld_pii", 0, tier, kind, "", confidence,
                                  session_id, 0);
      return KB_CLIENT_MEMORY_WITHHELD_PII;
   }

   cJSON *req = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(req);
   if (tier && tier[0])
      cJSON_AddStringToObject(req, "tier", tier);
   if (kind && kind[0])
      cJSON_AddStringToObject(req, "kind", kind);
   cJSON_AddStringToObject(req, "key", key);
   cJSON_AddStringToObject(req, "content", content_red ? content_red : content);
   free(content_red);
   if (use_cases && use_cases[0])
      cJSON_AddStringToObject(req, "use_cases", use_cases);
   cJSON_AddNumberToObject(req, "confidence", confidence);
   if (session_id && session_id[0])
      cJSON_AddStringToObject(req, "session_id", session_id);
   /* Asks for the user provenance; the kb grants it only if this request
    * authenticated as a person (kb_handle_memory_store). Omitted otherwise, so a
    * caller that never thought about it records the agent provenance. */
   if (authority == MEMORY_AUTHORITY_USER)
      cJSON_AddStringToObject(req, "authority", "user");
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
   kb_client_memory_scope_context_apply(req);
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
   char *upd_red = NULL;
   if (kb_client_pii_screen(content, &upd_red) != 0)
   {
      kb_client_memory_audit_note("memory.update.withheld_pii", id, NULL, NULL, NULL, 0.0, NULL, 0);
      return KB_CLIENT_MEMORY_WITHHELD_PII;
   }
   cJSON *req = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(req);
   cJSON_AddNumberToObject(req, "id", (double)id);
   cJSON_AddStringToObject(req, "content", upd_red ? upd_red : content);
   free(upd_red);
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
   kb_client_memory_scope_context_apply(req);
   cJSON_AddNumberToObject(req, "id", (double)id);
   /* The rejection reason is free prose that aimee-kb keeps alongside the
    * record, so it is screened like any other persisted text. */
   if (kb_client_pii_add_string(req, "reason", reason) != 0)
   {
      cJSON_Delete(req);
      kb_client_memory_audit_note("memory.reject.withheld_pii", id, NULL, NULL, NULL, 0.0, NULL, 0);
      return KB_CLIENT_WITHHELD_PII;
   }
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
