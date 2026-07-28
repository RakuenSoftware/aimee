/* kb_client_index_parse.c: response-shape adapter for index.scan.
 *
 * Extracted from kb_client_index.c so the wire contract — particularly
 * "status:error must surface, never be silently flattened to projects:0" —
 * can be unit-tested without linking the cli transport. The bug this
 * guards: an error response from aimee-kb used to be parsed into a
 * successful empty scan, hiding real DB2/canonical-index failures. */
#include "cJSON.h"
#include "json_fluent.h"
#include "kb_client.h"

#include <stdio.h>
#include <string.h>

/* Build the wire-level response object for an index.scan call from the
 * kb_client_index_scan result. Caller takes ownership of the returned
 * cJSON. Centralised so the dispatch path and tests share one truth. */
void *kb_client_index_scan_format_response(int kb_rc, const kb_client_index_scan_result_t *res)
{
   cJSON *resp;
   if (kb_rc != 0)
   {
      const char *msg = "knowledge service unavailable";
      if (res && strcmp(res->reason, "error") == 0 && res->message[0])
         msg = res->message;
      resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", msg);
      return resp;
   }

   resp = jo_ok();
   if (res)
   {
      cJSON_AddNumberToObject(resp, "projects", res->projects);
      cJSON_AddNumberToObject(resp, "files", res->files);
      if (res->inspected > 0)
         cJSON_AddNumberToObject(resp, "inspected", res->inspected);
      cJSON_AddBoolToObject(resp, "skipped", res->skipped);
      if (res->reason[0])
         jo_add_str(resp, "reason", res->reason);
      if (res->retry_after > 0)
         cJSON_AddNumberToObject(resp, "retry_after", (double)res->retry_after);
      if (res->generation_id > 0)
         cJSON_AddNumberToObject(resp, "generation_id", (double)res->generation_id);
      if (res->model_subjects > 0)
         cJSON_AddNumberToObject(resp, "model_subjects", res->model_subjects);
      if (res->repository_key[0])
         jo_add_str(resp, "repository_key", res->repository_key);
      if (res->source_ref[0])
         jo_add_str(resp, "source_ref", res->source_ref);
      if (res->commit_sha[0])
         jo_add_str(resp, "commit_sha", res->commit_sha);
      if (res->tree_sha[0])
         jo_add_str(resp, "tree_sha", res->tree_sha);
      if (res->generation_state[0])
         jo_add_str(resp, "generation_state", res->generation_state);
      if (res->physical_project[0])
         jo_add_str(resp, "physical_project", res->physical_project);
      if (res->reused_snapshot)
         cJSON_AddBoolToObject(resp, "reused_snapshot", 1);
      if (res->already_current)
         cJSON_AddBoolToObject(resp, "already_current", 1);
   }
   return resp;
}

int kb_client_index_scan_apply_response(const void *resp_v, kb_client_index_scan_result_t *out)
{
   const cJSON *resp = (const cJSON *)resp_v;
   if (out)
      memset(out, 0, sizeof(*out));

   if (!resp)
   {
      if (out)
      {
         out->skipped = 1;
         snprintf(out->reason, sizeof(out->reason), "no_kb");
      }
      return -1;
   }

   const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      if (out)
      {
         out->skipped = 1;
         snprintf(out->reason, sizeof(out->reason), "error");
         const cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(msg) && msg->valuestring[0])
            snprintf(out->message, sizeof(out->message), "%s", msg->valuestring);
      }
      return -1;
   }

   if (out)
   {
      const cJSON *skipped = cJSON_GetObjectItemCaseSensitive(resp, "skipped");
      const cJSON *reason = cJSON_GetObjectItemCaseSensitive(resp, "reason");
      const cJSON *retry = cJSON_GetObjectItemCaseSensitive(resp, "retry_after");
      const cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
      const cJSON *files = cJSON_GetObjectItemCaseSensitive(resp, "files");
      const cJSON *inspected = cJSON_GetObjectItemCaseSensitive(resp, "inspected");
      const cJSON *generation_id = cJSON_GetObjectItemCaseSensitive(resp, "generation_id");
      const cJSON *model_subjects = cJSON_GetObjectItemCaseSensitive(resp, "model_subjects");
      const cJSON *reused = cJSON_GetObjectItemCaseSensitive(resp, "reused_snapshot");
      const cJSON *current = cJSON_GetObjectItemCaseSensitive(resp, "already_current");
      out->skipped = cJSON_IsTrue(skipped) ? 1 : 0;
      if (cJSON_IsString(reason))
         snprintf(out->reason, sizeof(out->reason), "%s", reason->valuestring);
      if (cJSON_IsNumber(retry))
         out->retry_after = (long)retry->valuedouble;
      if (cJSON_IsNumber(projects))
         out->projects = (int)projects->valuedouble;
      if (cJSON_IsNumber(files))
         out->files = (int)files->valuedouble;
      if (cJSON_IsNumber(inspected))
         out->inspected = (int)inspected->valuedouble;
      if (cJSON_IsNumber(generation_id))
         out->generation_id = (int64_t)generation_id->valuedouble;
      if (cJSON_IsNumber(model_subjects))
         out->model_subjects = (int)model_subjects->valuedouble;
      out->reused_snapshot = cJSON_IsTrue(reused) ? 1 : 0;
      out->already_current = cJSON_IsTrue(current) ? 1 : 0;
      const struct
      {
         const char *key;
         char *dst;
         size_t cap;
      } strings[] = {
          {"repository_key", out->repository_key, sizeof(out->repository_key)},
          {"source_ref", out->source_ref, sizeof(out->source_ref)},
          {"commit_sha", out->commit_sha, sizeof(out->commit_sha)},
          {"tree_sha", out->tree_sha, sizeof(out->tree_sha)},
          {"generation_state", out->generation_state, sizeof(out->generation_state)},
          {"project", out->physical_project, sizeof(out->physical_project)},
      };
      for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++)
      {
         const cJSON *value = cJSON_GetObjectItemCaseSensitive(resp, strings[i].key);
         if (cJSON_IsString(value))
            snprintf(strings[i].dst, strings[i].cap, "%s", value->valuestring);
      }
   }
   return 0;
}
