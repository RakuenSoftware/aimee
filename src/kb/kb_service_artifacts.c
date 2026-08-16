/* kb_service_artifacts.c: aimee-kb dispatch handlers for the artifacts.*
 * RPC family (list_proposed, set_state).  Split out of kb_service.c so
 * the file stays under the per-file line cap. */

#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include "config.h"
#include "modules/db2/c/artifacts.h"
#include "kb_reasoning.h"
#include "kb_service_artifacts.h"
#include "log.h"

/* Defined in kb_service.c; non-static so this file can call them. */
int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);

/* Enforcing contradiction check (Phase 3).
 * For each "contradicts" link on the just-committed artifact, run the
 * Datalog structural check.  If it fails, demote the artifact back to
 * proposed+flagged_for_review so a human reviewer sees it. */
static void reasoning_enforce_contradiction_check(const char *id)
{
   if (!config_reasoning_datalog_command()[0])
      return;

#define ENFORCE_LINK_MAX 8
   db2_artifact_link_row_t links[ENFORCE_LINK_MAX];
   int n = db2_artifact_links_read(id, links, ENFORCE_LINK_MAX);
   for (int i = 0; i < n; i++)
   {
      if (strcmp(links[i].link_kind, "contradicts") != 0)
         continue;
      int result = kb_reasoning_contradiction_check(id, links[i].to_id);
      if (result == 0)
      {
         db2_artifact_flag_review(id, "datalog_structural_check_failed");
         aimee_log(LOG_WARN, "reasoning.enforce",
                   "contradiction_check_failed: artifact=%s contradicts=%s"
                   " — demoted to proposed+flagged_for_review",
                   id, links[i].to_id);
      }
      else if (result == 1)
         aimee_log(LOG_DEBUG, "reasoning.enforce",
                   "contradiction_check_ok: artifact=%s contradicts=%s", id, links[i].to_id);
   }
#undef ENFORCE_LINK_MAX
}

int kb_handle_artifacts_list_proposed(int fd, cJSON *req)
{
   cJSON *surf_j = cJSON_GetObjectItemCaseSensitive(req, "target_surface");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *surf =
       (cJSON_IsString(surf_j) && surf_j->valuestring[0]) ? surf_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 20;
   if (limit <= 0 || limit > 200)
      limit = 20;

#define ALP_MAX 64
   db2_artifact_proposed_t rows[ALP_MAX];
   int n = db2_artifact_list_proposed(surf, limit, rows, ALP_MAX);
   if (n < 0)
      return kb_send_error(fd, "failed to query proposed artifacts");

   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "artifacts");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "id", rows[i].id);
      cJSON_AddStringToObject(obj, "kind", rows[i].kind);
      cJSON_AddStringToObject(obj, "target_surface", rows[i].target_surface);
      cJSON_AddNumberToObject(obj, "confidence", rows[i].confidence);
      cJSON_AddStringToObject(obj, "created_at", rows[i].created_at);
      cJSON *payload = cJSON_Parse(rows[i].payload_json[0] ? rows[i].payload_json : "{}");
      cJSON_AddItemToObject(obj, "payload", payload ? payload : cJSON_CreateObject());
      cJSON_AddStringToObject(obj, "payload_json",
                              rows[i].payload_json[0] ? rows[i].payload_json : "{}");
      if (rows[i].citation_ids[0])
         cJSON_AddStringToObject(obj, "citation_ids", rows[i].citation_ids);
      cJSON_AddItemToArray(arr, obj);
   }
   kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return 0;
#undef ALP_MAX
}

int kb_handle_artifacts_set_state(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "new_state");
   if (!cJSON_IsString(id_j) || !id_j->valuestring[0])
      return kb_send_error(fd, "missing id");
   if (!cJSON_IsString(state_j) || !state_j->valuestring[0])
      return kb_send_error(fd, "missing new_state");

   const char *id = id_j->valuestring;
   const char *new_state = state_j->valuestring;

   int rc;
   if (strcmp(new_state, "rejected") == 0)
   {
      cJSON *vt_j = cJSON_GetObjectItemCaseSensitive(req, "verdict_tag");
      cJSON *vs_j = cJSON_GetObjectItemCaseSensitive(req, "verdict_scope");
      cJSON *ce_j = cJSON_GetObjectItemCaseSensitive(req, "counter_example");
      cJSON *bj_j = cJSON_GetObjectItemCaseSensitive(req, "before_json");
      rc = db2_artifact_reject(id, cJSON_IsString(vt_j) ? vt_j->valuestring : NULL,
                               cJSON_IsString(vs_j) ? vs_j->valuestring : NULL,
                               cJSON_IsString(ce_j) ? ce_j->valuestring : NULL,
                               cJSON_IsString(bj_j) ? bj_j->valuestring : NULL);
   }
   else if (strcmp(new_state, "committed") == 0)
   {
      rc = db2_artifact_set_state(id, "committed");
      if (rc == 0)
      {
         char audit_id[37];
         db2_artifact_gen_id(audit_id, sizeof(audit_id));
         char after_buf[64];
         snprintf(after_buf, sizeof(after_buf), "{\"state\":\"committed\"}");
         db2_audit_event_write(audit_id, id, "", "", "", "user", "", 1.0, 0, "{}", after_buf);
         reasoning_enforce_contradiction_check(id);
      }
   }
   else
   {
      rc = db2_artifact_set_state(id, new_state);
   }

   if (rc != 0)
      return kb_send_error(fd, "failed to update artifact state");

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "id", id);
   cJSON_AddStringToObject(resp, "new_state", new_state);
   kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return 0;
}
