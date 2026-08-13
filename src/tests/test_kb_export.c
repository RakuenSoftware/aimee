#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "../db2/kb_service_backend_export.h"
#include "../kb_export_json.h"
#include "../kb_export_obsidian.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static cJSON *sample_export(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "status", "ok");
   cJSON_AddStringToObject(root, "schema_version", "1");
   cJSON_AddStringToObject(root, "exported_at", "2026-05-05T00:00:00Z");
   cJSON_AddNumberToObject(root, "count", 2);

   cJSON *memories = cJSON_AddArrayToObject(root, "memories");

   cJSON *target = cJSON_CreateObject();
   cJSON_AddNumberToObject(target, "id", 100);
   cJSON_AddStringToObject(target, "tier", "L2");
   cJSON_AddStringToObject(target, "kind", "entity");
   cJSON_AddStringToObject(target, "key", "Auth/[Middleware]");
   cJSON_AddStringToObject(target, "content", "Handles token validation.");
   cJSON_AddNumberToObject(target, "confidence", 0.87);
   cJSON_AddStringToObject(target, "lifecycle_state", "active");
   cJSON_AddStringToObject(target, "created_at", "2026-05-05T00:00:00Z");
   cJSON_AddItemToArray(memories, target);

   cJSON *source = cJSON_CreateObject();
   cJSON_AddNumberToObject(source, "id", 101);
   cJSON_AddStringToObject(source, "tier", "L2");
   cJSON_AddStringToObject(source, "kind", "claim");
   cJSON_AddStringToObject(source, "key", "JWT claim extraction");
   cJSON_AddStringToObject(source, "content",
                           "Extracted from `cmd_agent.c`; depends on Auth/[Middleware].");
   cJSON_AddNumberToObject(source, "confidence", 0.91);
   cJSON_AddStringToObject(source, "lifecycle_state", "active");
   cJSON_AddStringToObject(source, "created_at", "2026-05-05T00:00:00Z");
   cJSON *links = cJSON_AddArrayToObject(source, "links");
   cJSON *link = cJSON_CreateObject();
   cJSON_AddStringToObject(link, "relation", "depends_on");
   cJSON_AddStringToObject(link, "target_key", "Auth/[Middleware]");
   cJSON_AddItemToArray(links, link);
   cJSON_AddItemToArray(memories, source);

   return root;
}

static void make_tmp_dir(char *out, size_t out_len, const char *prefix)
{
   char tmpl[256];
   snprintf(tmpl, sizeof(tmpl), "%s/%s-XXXXXX", platform_tmpdir(), prefix);
   char *dir = mkdtemp(tmpl);
   assert(dir);
   snprintf(out, out_len, "%s", dir);
}

static char *read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   assert(f);
   assert(fseek(f, 0, SEEK_END) == 0);
   long size = ftell(f);
   assert(size >= 0);
   rewind(f);
   char *buf = (char *)calloc((size_t)size + 1, 1);
   assert(buf);
   assert(fread(buf, 1, (size_t)size, f) == (size_t)size);
   fclose(f);
   return buf;
}

static void test_obsidian_frontmatter_links_and_sanitation(void)
{
   char dir[256];
   make_tmp_dir(dir, sizeof(dir), "aimee-kb-export-obsidian");

   cJSON *export_obj = sample_export();
   assert(kb_export_obsidian_render(export_obj, dir) == 0);

   char path[512];
   snprintf(path, sizeof(path), "%s/Auth_Middleware.md", dir);
   struct stat st;
   assert(stat(path, &st) == 0);

   char *target_md = read_file(path);
   assert(strncmp(target_md, "---\n", 4) == 0);
   assert(strstr(target_md, "kind: entity\n") != NULL);
   assert(strstr(target_md, "confidence: 0.8700\n") != NULL);
   assert(strstr(target_md, "created_at: \"2026-05-05T00:00:00Z\"\n") != NULL);
   free(target_md);

   snprintf(path, sizeof(path), "%s/JWT claim extraction.md", dir);
   char *source_md = read_file(path);
   assert(strstr(source_md, "[[Auth_Middleware]]") != NULL);
   assert(strstr(source_md, "[[Auth/[Middleware]]]") == NULL);
   free(source_md);

   snprintf(path, sizeof(path), "%s/Entities.md", dir);
   char *entities_md = read_file(path);
   assert(strstr(entities_md, "[[Auth_Middleware]]") != NULL);
   free(entities_md);

   cJSON_Delete(export_obj);
   printf("  obsidian_frontmatter_links_and_sanitation: ok\n");
}

static void test_json_render_parseable_with_schema_version(void)
{
   char dir[256];
   make_tmp_dir(dir, sizeof(dir), "aimee-kb-export-json");

   char path[512];
   snprintf(path, sizeof(path), "%s/export.json", dir);

   cJSON *export_obj = sample_export();
   assert(kb_export_json_render(export_obj, path) == 0);

   char *json = read_file(path);
   cJSON *parsed = cJSON_Parse(json);
   assert(parsed);
   cJSON *schema = cJSON_GetObjectItemCaseSensitive(parsed, "schema_version");
   cJSON *memories = cJSON_GetObjectItemCaseSensitive(parsed, "memories");
   assert(cJSON_IsString(schema));
   assert(strcmp(schema->valuestring, "1") == 0);
   assert(cJSON_IsArray(memories));
   assert(cJSON_GetArraySize(memories) == 2);

   cJSON_Delete(parsed);
   cJSON_Delete(export_obj);
   free(json);
   printf("  json_render_parseable_with_schema_version: ok\n");
}

static void test_import_dry_run_parse_contract(void)
{
   char dir[256];
   make_tmp_dir(dir, sizeof(dir), "aimee-kb-import-dry-run");

   char path[512];
   snprintf(path, sizeof(path), "%s/import.json", dir);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs("{\"schema_version\":\"1\",\"memories\":[{\"tier\":\"L2\",\"kind\":\"fact\","
         "\"key\":\"portable-memory\",\"content\":\"portable content\","
         "\"confidence\":0.75,\"source_session\":\"test\"}]}\n",
         f);
   fclose(f);

   char *json = read_file(path);
   cJSON *parsed = cJSON_Parse(json);
   assert(parsed);
   cJSON *schema = cJSON_GetObjectItemCaseSensitive(parsed, "schema_version");
   cJSON *memories = cJSON_GetObjectItemCaseSensitive(parsed, "memories");
   assert(cJSON_IsString(schema));
   assert(strcmp(schema->valuestring, "1") == 0);
   assert(cJSON_IsArray(memories));

   int imported = -1;
   assert(db2_kb_service_memory_import_json(memories, "import-ws", 1, &imported) == 0);
   assert(imported == 1);

   cJSON_Delete(parsed);
   free(json);
   printf("  import_dry_run_parse_contract: ok\n");
}

int main(void)
{
   printf("kb_export:\n");
   test_obsidian_frontmatter_links_and_sanitation();
   test_json_render_parseable_with_schema_version();
   test_import_dry_run_parse_contract();
   printf("All kb_export tests passed.\n");
   return 0;
}
