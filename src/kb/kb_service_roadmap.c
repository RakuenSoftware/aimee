/* kb_service_roadmap.c: aimee-kb dispatch handlers for the roadmap.*
 * RPC family (create_from_decomposition, validate, show, list, rebuild). */

#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include "roadmap.h"
#include "kb_service_roadmap.h"

/* Defined in kb_service.c; non-static so this file can call them. */
int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);

int kb_handle_roadmap_create_from_decomposition(int fd, cJSON *req)
{
   cJSON *decomp_j = cJSON_GetObjectItemCaseSensitive(req, "decomposition");
   if (!cJSON_IsString(decomp_j) || !decomp_j->valuestring[0])
      return kb_send_error(fd, "missing decomposition");

   char out_id[37] = {0};
   int rc = roadmap_create_from_decomposition(decomp_j->valuestring, out_id, sizeof(out_id));
   if (rc != 0)
      return kb_send_error(fd, "failed to create roadmap from decomposition");

   roadmap_projections_write(out_id);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "roadmap_id", out_id);
   kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return 0;
}

int kb_handle_roadmap_validate(int fd, cJSON *req)
{
   cJSON *decomp_j = cJSON_GetObjectItemCaseSensitive(req, "decomposition");
   if (!cJSON_IsString(decomp_j) || !decomp_j->valuestring[0])
      return kb_send_error(fd, "missing decomposition");

   char err[256] = {0};
   int rc = roadmap_validate_decomposition(decomp_j->valuestring, err, sizeof(err));

   cJSON *resp = jo_ok();
   cJSON_AddBoolToObject(resp, "valid", rc == 0);
   cJSON_AddStringToObject(resp, "reason", err);
   kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return 0;
}

int kb_handle_roadmap_show(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "roadmap_id");
   if (!cJSON_IsString(id_j) || !id_j->valuestring[0])
      return kb_send_error(fd, "missing roadmap_id");

   char *doc = NULL;
   if (roadmap_show_json(id_j->valuestring, &doc) != 0 || !doc)
      return kb_send_error(fd, "roadmap not found");

   cJSON *parsed = cJSON_Parse(doc);
   free(doc);
   if (!parsed)
      return kb_send_error(fd, "roadmap not found");

   cJSON_AddStringToObject(parsed, "status", "ok");
   kb_send_response(fd, parsed);
   cJSON_Delete(parsed);
   return 0;
}

int kb_handle_roadmap_list(int fd, cJSON *req)
{
   (void)req;

   char *doc = NULL;
   if (roadmap_list_json(&doc) != 0 || !doc)
      return kb_send_error(fd, "failed to list roadmaps");

   cJSON *parsed = cJSON_Parse(doc);
   free(doc);
   if (!parsed)
      return kb_send_error(fd, "failed to list roadmaps");

   cJSON_AddStringToObject(parsed, "status", "ok");
   kb_send_response(fd, parsed);
   cJSON_Delete(parsed);
   return 0;
}

int kb_handle_roadmap_rebuild(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "roadmap_id");
   if (!cJSON_IsString(id_j) || !id_j->valuestring[0])
      return kb_send_error(fd, "missing roadmap_id");

   if (roadmap_projections_rebuild(id_j->valuestring) != 0)
      return kb_send_error(fd, "failed to rebuild projections");

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "roadmap_id", id_j->valuestring);
   kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return 0;
}

int kb_handle_roadmap_report(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "roadmap_id");
   if (!cJSON_IsString(id_j) || !id_j->valuestring[0])
      return kb_send_error(fd, "missing roadmap_id");

   cJSON *path_j = cJSON_GetObjectItemCaseSensitive(req, "output_path");
   const char *output_path =
       (cJSON_IsString(path_j) && path_j->valuestring[0]) ? path_j->valuestring : NULL;

   if (roadmap_report_html(id_j->valuestring, output_path) != 0)
      return kb_send_error(fd, "failed to generate HTML report");

   /* Return the output path used so the CLI can print it. */
   char default_path[512];
   if (!output_path)
   {
      snprintf(default_path, sizeof(default_path), ".aimee/roadmap/reports/%.60s.html",
               id_j->valuestring);
      output_path = default_path;
   }

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "roadmap_id", id_j->valuestring);
   cJSON_AddStringToObject(resp, "report_path", output_path);
   kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return 0;
}