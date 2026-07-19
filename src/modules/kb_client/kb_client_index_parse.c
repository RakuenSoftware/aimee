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
   }
   return 0;
}
