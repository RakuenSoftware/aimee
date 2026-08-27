#include "kb_http_code.h"
#include "kb_curator_queue.h"
#include "modules/db2/c/canonical_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int code_scan_phase_bool(cJSON *root, const char *key, int default_val)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
   if (!item)
      return default_val;
   return cJSON_IsTrue(item) ? 1 : cJSON_IsFalse(item) ? 0 : default_val;
}

/* Complete remote scans are phased. Staged file bodies are not query-visible;
 * only a successful seal publishes them and reconciles absent paths. A zero
 * return means this is the legacy incremental request shape. */
int code_scan_handle_phase(cJSON *root, const char *project, const char *root_path, cJSON *files_j,
                           char *out_buf, int out_cap)
{
   cJSON *phase_j = cJSON_GetObjectItemCaseSensitive(root, "phase");
   const char *phase = cJSON_IsString(phase_j) ? phase_j->valuestring : "";
   if (!phase[0])
      return 0;

   if (strcmp(phase, "verify") == 0)
   {
      if (!root_path || !root_path[0])
         return code_scan_write_error(out_buf, out_cap, "missing root_path");
      int deep = code_scan_phase_bool(root, "deep", 0);
      canonical_index_verify_result_t verified;
      int rc = canonical_index_verify_project(project, root_path, deep, &verified);
      if (rc != 0)
         return code_scan_write_error(out_buf, out_cap, "index verification failed");
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddStringToObject(resp, "project", project);
      cJSON_AddStringToObject(resp, "index_state", "current");
      cJSON_AddNumberToObject(resp, "index_revision", (double)verified.index_revision);
      cJSON_AddStringToObject(resp, "workspace_state",
                              verified.unavailable
                                  ? "unavailable"
                                  : (verified.modified_files || verified.unindexed_files
                                         ? "modified"
                                         : (verified.missing_files ? "missing" : "matched")));
      cJSON_AddStringToObject(resp, "verification", deep ? "content_hash" : "manifest");
      cJSON_AddNumberToObject(resp, "indexed_files", verified.indexed_files);
      cJSON_AddNumberToObject(resp, "workspace_files", verified.workspace_files);
      cJSON_AddNumberToObject(resp, "modified_files", verified.modified_files);
      cJSON_AddNumberToObject(resp, "missing_files", verified.missing_files);
      cJSON_AddNumberToObject(resp, "unindexed_files", verified.unindexed_files);
      cJSON *examples = cJSON_AddArrayToObject(resp, "examples");
      for (int i = 0; examples && i < verified.example_count; i++)
         cJSON_AddItemToArray(examples, cJSON_CreateString(verified.examples[i]));
      char *json = cJSON_PrintUnformatted(resp);
      cJSON_Delete(resp);
      if (!json || strlen(json) >= (size_t)out_cap)
      {
         free(json);
         return code_scan_write_error(out_buf, out_cap, "verification result too large");
      }
      snprintf(out_buf, (size_t)out_cap, "%s", json);
      free(json);
      return 200;
   }

   cJSON *scan_id_j = cJSON_GetObjectItemCaseSensitive(root, "scan_id");
   const char *scan_id = cJSON_IsString(scan_id_j) ? scan_id_j->valuestring : "";
   if (!scan_id[0])
      return code_scan_write_error(out_buf, out_cap, "missing scan_id");

   if (strcmp(phase, "begin") == 0)
   {
      long long baseline = -1;
      int rc = canonical_index_scan_begin(project, root_path && root_path[0] ? root_path : "remote",
                                          scan_id, &baseline);
      if (rc != 0)
         return code_scan_write_error(out_buf, out_cap, "scan begin failed");
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"ok\",\"phase\":\"begin\",\"project\":\"%s\","
               "\"scan_id\":\"%s\",\"baseline_revision\":%lld}",
               project, scan_id, baseline);
      return 200;
   }

   if (strcmp(phase, "stage") == 0)
   {
      if (!cJSON_IsArray(files_j))
         return code_scan_write_error(out_buf, out_cap, "stage requires files array");
      int n = cJSON_GetArraySize(files_j);
      canonical_index_file_input_t *inputs = calloc((size_t)(n > 0 ? n : 1), sizeof(*inputs));
      if (!inputs)
         return code_scan_write_error(out_buf, out_cap, "out of memory");
      for (int i = 0; i < n; i++)
      {
         cJSON *entry = cJSON_GetArrayItem(files_j, i);
         cJSON *path_j = cJSON_GetObjectItemCaseSensitive(entry, "rel_path");
         cJSON *content_j = cJSON_GetObjectItemCaseSensitive(entry, "content");
         if (!cJSON_IsString(path_j) || !path_j->valuestring[0] || !cJSON_IsString(content_j))
         {
            free(inputs);
            return code_scan_write_error(out_buf, out_cap, "invalid files array");
         }
         inputs[i].rel_path = path_j->valuestring;
         inputs[i].content = content_j->valuestring;
      }
      int accepted = 0;
      int rc = canonical_index_scan_stage(scan_id, inputs, n, &accepted);
      free(inputs);
      if (rc != 0)
         return code_scan_write_error(out_buf, out_cap, "scan stage failed");
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"ok\",\"phase\":\"stage\",\"scan_id\":\"%s\","
               "\"accepted\":%d}",
               scan_id, accepted);
      return 200;
   }

   if (strcmp(phase, "seal") == 0)
   {
      cJSON *expected_j = cJSON_GetObjectItemCaseSensitive(root, "expected_files");
      if (!cJSON_IsNumber(expected_j) || expected_j->valuedouble < 0)
         return code_scan_write_error(out_buf, out_cap, "missing expected_files");
      int expected_files = (int)expected_j->valuedouble;
      canonical_index_seal_result_t sealed;
      int rc = canonical_index_scan_seal(scan_id, expected_files, &sealed);
      if (rc == -2)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"stale or incomplete scan\",\"code\":\"scan_conflict\"}");
         return 409;
      }
      if (rc != 0)
         return code_scan_write_error(out_buf, out_cap, "scan seal failed");
      kb_curator_queue_code_units_for_project(project, root_path);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"ok\",\"phase\":\"seal\",\"skipped\":false,"
               "\"project\":\"%s\",\"files\":%d,\"inspected\":%d,"
               "\"retracted\":%d,\"index_state\":\"current\","
               "\"index_revision\":%lld,\"workspace_state\":\"matched\","
               "\"verification\":\"content_hash\"}",
               project, sealed.files_indexed, expected_files, sealed.files_retracted,
               sealed.revision);
      return 200;
   }

   if (strcmp(phase, "abort") == 0)
   {
      int rc = canonical_index_scan_abort(scan_id);
      if (rc != 0)
         return code_scan_write_error(out_buf, out_cap, "scan abort failed");
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"ok\",\"phase\":\"abort\",\"scan_id\":\"%s\"}", scan_id);
      return 200;
   }

   return code_scan_write_error(out_buf, out_cap, "invalid scan phase");
}

int handle_post_code_scan_route(const char *method, const char *body, char *out_buf, int out_cap)
{
   if (strcmp(method, "POST") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_post_code_scan(body, out_buf, out_cap);
}
