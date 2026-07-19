/* kb_client_prospective.c: kb_client wrappers for the
 * memory.prospective_* RPC family (list, create, complete, match,
 * mark_triggered, sweep_expired) and the closely related memory.tag_*
 * / memory.get_provenance helpers.  Split out of kb_client_memory.c
 * so the file stays under the per-file line cap. */

#include "kb_client.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in kb_client.c. */
char *kb_v1_action_request(const char *method, cJSON *req);

char *kb_client_memory_prospective_list_json(const char *state, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(req, "state", state);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("memory.prospective_list", req);
}

char *kb_client_memory_prospective_create_json(const char *trigger_text, const char *action_text,
                                               const char *anchor_entity, const char *anchor_file,
                                               const char *recurrence, const char *valid_until)
{
   cJSON *req = cJSON_CreateObject();
   if (trigger_text)
      cJSON_AddStringToObject(req, "trigger_text", trigger_text);
   if (action_text)
      cJSON_AddStringToObject(req, "action_text", action_text);
   if (anchor_entity && anchor_entity[0])
      cJSON_AddStringToObject(req, "anchor_entity", anchor_entity);
   if (anchor_file && anchor_file[0])
      cJSON_AddStringToObject(req, "anchor_file", anchor_file);
   if (recurrence && recurrence[0])
      cJSON_AddStringToObject(req, "recurrence", recurrence);
   if (valid_until && valid_until[0])
      cJSON_AddStringToObject(req, "valid_until", valid_until);
   return kb_v1_action_request("memory.prospective_create", req);
}

char *kb_client_memory_prospective_complete_json(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   return kb_v1_action_request("memory.prospective_complete", req);
}

static void kbcp_parse_prospective_row(cJSON *r, memory_prospective_t *out)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
   cJSON *trig = cJSON_GetObjectItemCaseSensitive(r, "trigger_text");
   cJSON *act = cJSON_GetObjectItemCaseSensitive(r, "action_text");
   cJSON *recr = cJSON_GetObjectItemCaseSensitive(r, "recurrence");
   cJSON *valid = cJSON_GetObjectItemCaseSensitive(r, "valid_until");
   cJSON *ae = cJSON_GetObjectItemCaseSensitive(r, "anchor_entity");
   cJSON *af = cJSON_GetObjectItemCaseSensitive(r, "anchor_file");
   cJSON *st = cJSON_GetObjectItemCaseSensitive(r, "state");
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)(cJSON_IsNumber(id_j) ? id_j->valuedouble : 0);
   if (cJSON_IsString(trig))
      snprintf(out->trigger_text, sizeof(out->trigger_text), "%s", trig->valuestring);
   if (cJSON_IsString(act))
      snprintf(out->action_text, sizeof(out->action_text), "%s", act->valuestring);
   if (cJSON_IsString(recr))
      snprintf(out->recurrence, sizeof(out->recurrence), "%s", recr->valuestring);
   if (cJSON_IsString(valid))
      snprintf(out->valid_until, sizeof(out->valid_until), "%s", valid->valuestring);
   if (cJSON_IsString(ae))
      snprintf(out->anchor_entity, sizeof(out->anchor_entity), "%s", ae->valuestring);
   if (cJSON_IsString(af))
      snprintf(out->anchor_file, sizeof(out->anchor_file), "%s", af->valuestring);
   if (cJSON_IsString(st))
      snprintf(out->state, sizeof(out->state), "%s", st->valuestring);
}

int kb_client_memory_prospective_match(const char *turn_text, const char *active_entity,
                                       const char *active_file, memory_prospective_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   if (turn_text && turn_text[0])
      cJSON_AddStringToObject(req, "turn_text", turn_text);
   if (active_entity && active_entity[0])
      cJSON_AddStringToObject(req, "active_entity", active_entity);
   if (active_file && active_file[0])
      cJSON_AddStringToObject(req, "active_file", active_file);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.prospective_match", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "matches");
   int count = 0;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsArray(arr))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, arr)
      {
         if (count >= max)
            break;
         kbcp_parse_prospective_row(r, &out[count]);
         count++;
      }
   }
   cJSON_Delete(resp);
   return count;
}

int kb_client_memory_prospective_mark_triggered(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   char *json = kb_v1_action_request("memory.prospective_mark_triggered", req);
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

int kb_client_memory_prospective_sweep_expired(void)
{
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("memory.prospective_sweep_expired", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *exp_j = cJSON_GetObjectItemCaseSensitive(resp, "expired");
   int n = 0;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(exp_j))
      n = (int)exp_j->valuedouble;
   cJSON_Delete(resp);
   return n;
}

int64_t kb_client_memory_episode_card_generate(const char *source_session)
{
   if (!source_session || !source_session[0])
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "source_session", source_session);
   char *json = kb_v1_action_request("memory.episode_card_generate", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *uid_j = cJSON_GetObjectItemCaseSensitive(resp, "memory_unit_id");
   int64_t uid = 0;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(uid_j))
      uid = (int64_t)uid_j->valuedouble;
   cJSON_Delete(resp);
   return uid;
}

int kb_client_memory_scope_visibility_rank(const int64_t *ids, int id_count, const char *workspace,
                                           const char *project, int *out_ranks)
{
   if (!ids || id_count <= 0 || !out_ranks)
      return 0;
   for (int i = 0; i < id_count; i++)
      out_ranks[i] = 0;
   cJSON *req = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(req, "ids");
   for (int i = 0; i < id_count; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)ids[i]));
   if (workspace && workspace[0])
      cJSON_AddStringToObject(req, "workspace", workspace);
   if (project && project[0])
      cJSON_AddStringToObject(req, "project", project);
   char *json = kb_v1_action_request("memory.scope_visibility_rank", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *ranks = cJSON_GetObjectItemCaseSensitive(resp, "ranks");
   int n = 0;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsArray(ranks))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, ranks)
      {
         if (n >= id_count)
            break;
         out_ranks[n] = cJSON_IsNumber(r) ? (int)r->valuedouble : 0;
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

int kb_client_memory_tag_workspace(int64_t memory_id, const char *workspace)
{
   if (memory_id <= 0 || !workspace || !workspace[0])
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   cJSON_AddStringToObject(req, "workspace", workspace);
   char *json = kb_v1_action_request("memory.tag_workspace", req);
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

int kb_client_memory_tag_scope(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   if (memory_id <= 0 || !scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   cJSON_AddStringToObject(req, "scope_type", scope_type);
   cJSON_AddStringToObject(req, "scope_value", scope_value);
   char *json = kb_v1_action_request("memory.tag_scope", req);
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

int kb_client_memory_get_provenance(int64_t memory_id, provenance_entry_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("memory.get_provenance", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "entries");
   int count = 0;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsArray(arr))
   {
      cJSON *r;
      cJSON_ArrayForEach(r, arr)
      {
         if (count >= max)
            break;
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
         cJSON *mid = cJSON_GetObjectItemCaseSensitive(r, "memory_id");
         cJSON *sid = cJSON_GetObjectItemCaseSensitive(r, "session_id");
         cJSON *act = cJSON_GetObjectItemCaseSensitive(r, "action");
         cJSON *det = cJSON_GetObjectItemCaseSensitive(r, "details");
         cJSON *ca = cJSON_GetObjectItemCaseSensitive(r, "created_at");
         out[count].id = (int64_t)(cJSON_IsNumber(id_j) ? id_j->valuedouble : 0);
         out[count].memory_id = (int64_t)(cJSON_IsNumber(mid) ? mid->valuedouble : 0);
         if (cJSON_IsString(sid))
            snprintf(out[count].session_id, sizeof(out[count].session_id), "%s", sid->valuestring);
         if (cJSON_IsString(act))
            snprintf(out[count].action, sizeof(out[count].action), "%s", act->valuestring);
         if (cJSON_IsString(det))
            snprintf(out[count].details, sizeof(out[count].details), "%s", det->valuestring);
         if (cJSON_IsString(ca))
            snprintf(out[count].created_at, sizeof(out[count].created_at), "%s", ca->valuestring);
         count++;
      }
   }
   cJSON_Delete(resp);
   return count;
}
