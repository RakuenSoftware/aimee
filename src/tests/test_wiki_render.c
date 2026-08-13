#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "db.h"
#include "db1.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "wiki_render.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char s_tmpdir[256];

static int file_exists(const char *dir, const char *name)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   struct stat st;
   return stat(path, &st) == 0;
}

static void test_render_creates_files(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-wiki-test-XXXXXX", platform_tmpdir());
   char *dir = mkdtemp(tmpl);
   assert(dir);
   snprintf(s_tmpdir, sizeof(s_tmpdir), "%s", dir);

   /* wiki_render calls kb_client_memory_list, which returns 0 items in test env */
   int rc = wiki_render(s_tmpdir);
   assert(rc == 0);

   assert(file_exists(s_tmpdir, "index.md"));
   assert(file_exists(s_tmpdir, "concepts.md"));
   assert(file_exists(s_tmpdir, "rules.md"));
   assert(file_exists(s_tmpdir, "preferences.md"));
   assert(file_exists(s_tmpdir, "episodes.md"));
   assert(file_exists(s_tmpdir, "facts.md"));
   assert(file_exists(s_tmpdir, "log.md"));
   printf("  render_creates_files: ok\n");
}

static void test_render_index_contains_links(void)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/index.md", s_tmpdir);
   FILE *f = fopen(path, "r");
   assert(f);
   char buf[4096] = "";
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   assert(strstr(buf, "# Memory Wiki") != NULL);
   assert(strstr(buf, "concepts.md") != NULL);
   assert(strstr(buf, "rules.md") != NULL);
   assert(strstr(buf, "log.md") != NULL);
   printf("  render_index_contains_links: ok\n");
}

static void test_render_null_dir(void)
{
   int rc = wiki_render(NULL);
   assert(rc == -1);
   printf("  render_null_dir: ok\n");
}

int main(void)
{
   printf("wiki_render:\n");

   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();

   test_render_creates_files();
   test_render_index_contains_links();
   test_render_null_dir();

   printf("All wiki_render tests passed.\n");
   return 0;
}
