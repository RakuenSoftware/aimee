/* Wiki file transport: full Go-rendered text, scope forwarding and failures. */
#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "wiki_render.h"
#include "platform_test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *forced;
static int unavailable;
static char *long_text;
static const char *names[] = {"concepts.md", "rules.md", "preferences.md", "episodes.md",
                              "facts.md",    "log.md",   "index.md"};
void kb_client_memory_scope_context_apply(cJSON *args)
{
   cJSON_AddBoolToObject(args, "scope_context", 1);
   cJSON_AddStringToObject(args, "project", "wiki-project");
}
char *kb_v1_action_request(const char *method, cJSON *args)
{
   assert(!strcmp(method, "memory.list") && !strcmp(jo_cstr(args, "format"), "wiki"));
   assert(jo_bool(args, "scope_context", 0) && !strcmp(jo_cstr(args, "project"), "wiki-project"));
   cJSON_Delete(args);
   if (unavailable)
      return NULL;
   if (forced)
      return strdup(forced);
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "status", "ok");
   cJSON *files = cJSON_AddArrayToObject(result, "files");
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
   {
      cJSON *file = cJSON_CreateObject();
      cJSON_AddStringToObject(file, "name", names[i]);
      cJSON_AddStringToObject(file, "text", i == 0 ? long_text : "Go-owned page\n");
      if (i == 5)
         cJSON_AddBoolToObject(file, "preserve_existing", 1);
      cJSON_AddItemToArray(files, file);
   }
   char *raw = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);
   return raw;
}
static void write_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   assert(fputs(text, f) >= 0);
   assert(fclose(f) == 0);
}
static void expect_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "rb");
   assert(f);
   size_t n = strlen(text);
   char *got = calloc(n + 2, 1);
   assert(got);
   assert(fread(got, 1, n + 1, f) == n);
   assert(!strcmp(got, text));
   free(got);
   assert(fclose(f) == 0);
}
int main(void)
{
   char dir[512], path[1024], logpath[1024];
   snprintf(dir, sizeof(dir), "%s/aimee-wiki-test-XXXXXX", platform_tmpdir());
   assert(mkdtemp(dir));
   long_text = malloc(30032);
   assert(long_text);
   for (int i = 0; i < 10000; i++)
      memcpy(long_text + 3 * i, "界", 3);
   strcpy(long_text + 30000, " tail-marker\n");
   assert(wiki_render(dir) == 0);
   snprintf(path, sizeof(path), "%s/concepts.md", dir);
   expect_text(path, long_text);
   snprintf(logpath, sizeof(logpath), "%s/log.md", dir);
   write_text(logpath, "existing history\n");
   assert(wiki_render(dir) == 0);
   expect_text(logpath, "existing history\n");
   write_text(path, "existing concepts\n");
   const char *bad[] = {"bad-json",
                        "{\"status\":\"error\",\"files\":[]}",
                        "{\"status\":\"ok\"}",
                        "{\"status\":\"ok\",\"files\":[]}",
                        "{\"status\":\"ok\",\"files\":[{\"name\":\"concepts.md\",\"text\":"
                        "\"changed\"},{\"name\":\"../escape.md\",\"text\":\"bad\"}]}",
                        "{\"status\":\"ok\",\"files\":[{\"name\":\"concepts.md\",\"text\":7}]}",
                        "{\"status\":\"ok\",\"files\":[{\"name\":\"concepts.md\",\"text\":"
                        "\"changed\"},{\"name\":\"concepts.md\",\"text\":\"duplicate\"}]}",
                        "{\"status\":\"ok\",\"files\":[{\"name\":\"concepts.md\",\"text\":"
                        "\"changed\",\"preserve_existing\":\"yes\"}]}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      forced = bad[i];
      assert(wiki_render(dir) == -1);
      expect_text(path, "existing concepts\n");
   }
   forced = NULL;
   unavailable = 1;
   assert(wiki_render(dir) == -1);
   expect_text(path, "existing concepts\n");
   unavailable = 0;
   assert(wiki_render(NULL) == -1 && wiki_render("") == -1);
   assert(wiki_render(path) == -1); // Output directory is a regular file.
   assert(unlink(path) == 0);
   assert(mkdir(path, 0700) == 0);
   assert(wiki_render(dir) == -1);
   assert(rmdir(path) == 0);
   // A buffered write can fail only at close; it must still fail the command.
   assert(symlink("/dev/full", path) == 0);
   char *saved_text = long_text;
   long_text = "short buffered write\n";
   assert(wiki_render(dir) == -1);
   long_text = saved_text;
   assert(unlink(path) == 0);
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
   {
      snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
      unlink(path);
   }
   assert(rmdir(dir) == 0);
   free(long_text);
   puts("wiki file transport: ok");
   return 0;
}
