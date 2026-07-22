/* cmd_kb_export.c: export/import subcommands for aimee kb. */
#include "cmd_kb_export.h"
#include "aimee.h"
#include "headers/util.h"
#include "kb_client_memory_internal.h"
#include "kb_export_obsidian.h"
#include "kb_export_json.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *read_json_file_or_fatal(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      fatal("kb import: cannot open %s: %s", path, strerror(errno));

   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      fatal("kb import: cannot seek %s: %s", path, strerror(errno));
   }

   long sz = ftell(f);
   if (sz < 0)
   {
      fclose(f);
      fatal("kb import: cannot size %s: %s", path, strerror(errno));
   }
   rewind(f);

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      fatal("kb import: out of memory reading %s", path);
   }

   size_t got = fread(buf, 1, (size_t)sz, f);
   if (got != (size_t)sz)
   {
      int read_errno = errno;
      int had_error = ferror(f);
      free(buf);
      fclose(f);
      fatal("kb import: failed to read %s: %s", path,
            had_error ? strerror(read_errno) : "short read");
   }
   buf[sz] = '\0';

   if (fclose(f) != 0)
   {
      free(buf);
      fatal("kb import: failed to close %s: %s", path, strerror(errno));
   }

   return buf;
}

static void validate_import_envelope_or_fatal(cJSON *import_obj, const char *path)
{
   if (!cJSON_IsObject(import_obj))
      fatal("kb import: invalid JSON envelope in %s", path);

   cJSON *schema = cJSON_GetObjectItemCaseSensitive(import_obj, "schema_version");
   if (!cJSON_IsString(schema))
      fatal("kb import: missing schema_version in %s", path);
   if (strcmp(schema->valuestring, "1") != 0)
      fatal("kb import: unsupported schema_version '%s' in %s (expected '1')", schema->valuestring,
            path);

   if (kb_export_json_validate_import(import_obj) != 0)
      fatal("kb import: invalid JSON envelope in %s", path);
}

/* ------------------------------------------------------------------ */
/* kb export                                                            */
/* ------------------------------------------------------------------ */

void kb_cmd_export(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {"include-pruned", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *format = opt_get(&opts, "format");
   const char *out = opt_get(&opts, "out");
   const char *workspace = opt_get(&opts, "workspace");
   const char *kind = opt_get(&opts, "kind");
   const char *since = opt_get(&opts, "since");
   int include_pruned = opt_get_flag(&opts, "include-pruned");

   if (!format || !format[0])
      format = "obsidian";

   /* Build request */
   cJSON *req = cJSON_CreateObject();
   if (workspace)
      cJSON_AddStringToObject(req, "workspace", workspace);
   if (kind)
      cJSON_AddStringToObject(req, "kind", kind);
   if (since)
      cJSON_AddStringToObject(req, "since", since);
   cJSON_AddBoolToObject(req, "include_archived", include_pruned ? 1 : 0);

   char *resp_json = kb_v1_action_request("kb.export", req);
   cJSON *resp_obj = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   if (!resp_obj)
      fatal("kb export: failed to parse response");

   /* Check status */
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp_obj, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      const char *message = "kb export failed";
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp_obj, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         message = msg->valuestring;
      cJSON_Delete(resp_obj);
      fatal("kb export: %s", message);
   }

   /* JSON output mode: print the stable export envelope */
   if (ctx->json_output)
   {
      cJSON *envelope = kb_export_json_build_envelope(resp_obj);
      if (!envelope)
      {
         cJSON_Delete(resp_obj);
         fatal("kb export: invalid response envelope");
      }
      char *json = cJSON_PrintUnformatted(envelope);
      cJSON_Delete(envelope);
      if (!json)
      {
         cJSON_Delete(resp_obj);
         fatal("kb export: failed to serialize JSON");
      }
      printf("%s\n", json);
      free(json);
      cJSON_Delete(resp_obj);
      return;
   }

   /* Get count for summary */
   int count = 0;
   cJSON *count_node = cJSON_GetObjectItemCaseSensitive(resp_obj, "count");
   if (cJSON_IsNumber(count_node))
      count = (int)count_node->valuedouble;

   if (strcmp(format, "json") == 0)
   {
      char out_file[512];
      if (out && out[0])
         snprintf(out_file, sizeof(out_file), "%s", out);
      else
         snprintf(out_file, sizeof(out_file), "kb-export-%ld.json", (long)time(NULL));

      if (kb_export_json_render(resp_obj, out_file) != 0)
      {
         cJSON_Delete(resp_obj);
         fatal("kb export: failed to write %s", out_file);
      }
      printf("Exported %d memories to %s\n", count, out_file);
   }
   else
   {
      char out_dir[512];
      if (out && out[0])
         snprintf(out_dir, sizeof(out_dir), "%s", out);
      else
         snprintf(out_dir, sizeof(out_dir), "kb-export-%ld", (long)time(NULL));

      if (kb_export_obsidian_render(resp_obj, out_dir) != 0)
      {
         cJSON_Delete(resp_obj);
         fatal("kb export: failed to write %s", out_dir);
      }
      printf("Exported %d memories to %s/\n", count, out_dir);
   }

   cJSON_Delete(resp_obj);
}

/* ------------------------------------------------------------------ */
/* kb import                                                            */
/* ------------------------------------------------------------------ */

void kb_cmd_import(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {"dry-run", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *path = opt_pos(&opts, 0);
   if (!path)
      fatal("kb import: <path> is required");

   const char *workspace = opt_get(&opts, "workspace");
   int dry_run = opt_get_flag(&opts, "dry-run");

   /* Read the JSON file */
   char *buf = read_json_file_or_fatal(path);
   cJSON *import_obj = cJSON_Parse(buf);
   free(buf);
   if (!import_obj)
      fatal("kb import: failed to parse %s", path);

   validate_import_envelope_or_fatal(import_obj, path);

   /* Extract memories array */
   cJSON *memories_arr = cJSON_GetObjectItemCaseSensitive(import_obj, "memories");
   cJSON *memories_copy = cJSON_Duplicate(memories_arr, 1);
   if (!memories_copy)
   {
      cJSON_Delete(import_obj);
      fatal("kb import: failed to copy memories from %s", path);
   }

   /* Build request */
   cJSON *req = cJSON_CreateObject();
   if (!req)
   {
      cJSON_Delete(memories_copy);
      cJSON_Delete(import_obj);
      fatal("kb import: failed to build import request");
   }
   if (!cJSON_AddItemToObject(req, "memories", memories_copy))
   {
      cJSON_Delete(memories_copy);
      cJSON_Delete(req);
      cJSON_Delete(import_obj);
      fatal("kb import: failed to build import request");
   }
   memories_copy = NULL;
   if (workspace && !cJSON_AddStringToObject(req, "workspace", workspace))
   {
      cJSON_Delete(req);
      cJSON_Delete(import_obj);
      fatal("kb import: failed to build import request");
   }
   if (!cJSON_AddBoolToObject(req, "dry_run", dry_run ? 1 : 0))
   {
      cJSON_Delete(req);
      cJSON_Delete(import_obj);
      fatal("kb import: failed to build import request");
   }
   cJSON_Delete(import_obj);

   char *resp_json = kb_v1_action_request("kb.import", req);
   cJSON *resp_obj = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   if (!resp_obj)
      fatal("kb import: failed to parse response");

   /* Check status */
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp_obj, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      const char *message = "kb import failed";
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp_obj, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         message = msg->valuestring;
      cJSON_Delete(resp_obj);
      fatal("kb import: %s", message);
   }

   /* Get imported count */
   int imported = 0;
   cJSON *imported_node = cJSON_GetObjectItemCaseSensitive(resp_obj, "imported");
   if (cJSON_IsNumber(imported_node))
      imported = (int)imported_node->valuedouble;

   if (dry_run)
      printf("Dry run: would import %d memories\n", imported);
   else
      printf("Imported %d memories\n", imported);

   cJSON_Delete(resp_obj);
}
