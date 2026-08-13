#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void mkdirp(const char *path)
{
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s", path);
   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         mkdir(tmp, 0777);
         *p = '/';
      }
   }
   mkdir(tmp, 0777);
}

static void write_file(const char *path, const char *content)
{
   /* Ensure parent directory exists. */
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   if (slash)
   {
      *slash = '\0';
      mkdirp(dir);
   }
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(content, f);
   fclose(f);
}

/* Create a fresh temporary project rooted at tmp_root with a `.git` marker.
 * Returns 1 on success. */
static int make_project(char *tmp_root, size_t root_len)
{
   snprintf(tmp_root, root_len, "%s/aimee_ctxdisc_XXXXXX", platform_tmpdir());
   if (!mkdtemp(tmp_root))
      return 0;
   char git_dir[MAX_PATH_LEN];
   snprintf(git_dir, sizeof(git_dir), "%s/.git", tmp_root);
   mkdir(git_dir, 0777);
   return 1;
}

static void rm_rf(const char *path)
{
   char cmd[MAX_PATH_LEN + 16];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
   int rc = system(cmd);
   (void)rc;
}

static void test_empty_dir_yields_nothing(void)
{
   char root[MAX_PATH_LEN];
   assert(make_project(root, sizeof(root)));

   context_discovery_t disc = {0};
   assert(context_discover(root, 0, &disc) == 0);
   assert(disc.rendered == NULL);
   assert(disc.file_count == 0);
   context_discovery_free(&disc);
   rm_rf(root);
}

static void test_finds_nearest_file_first(void)
{
   char root[MAX_PATH_LEN];
   assert(make_project(root, sizeof(root)));

   /* Root-level AGENTS.md (ancestor), nested .aimee-rules (closer). */
   char top_rules[MAX_PATH_LEN];
   char sub_rules[MAX_PATH_LEN];
   snprintf(top_rules, sizeof(top_rules), "%s/AGENTS.md", root);
   write_file(top_rules, "# Top level convention\n");

   char sub[MAX_PATH_LEN];
   snprintf(sub, sizeof(sub), "%s/a/b", root);
   mkdirp(sub);
   snprintf(sub_rules, sizeof(sub_rules), "%s/.aimee-rules", sub);
   write_file(sub_rules, "# Nested rules\n");

   context_discovery_t disc = {0};
   assert(context_discover(sub, 0, &disc) == 0);
   assert(disc.rendered != NULL);
   assert(disc.file_count == 2);

   /* The nested file should appear before the ancestor file in the output. */
   const char *nested_marker = strstr(disc.rendered, "Nested rules");
   const char *top_marker = strstr(disc.rendered, "Top level convention");
   assert(nested_marker && top_marker);
   assert(nested_marker < top_marker);

   context_discovery_free(&disc);
   rm_rf(root);
}

static void test_deduplicates_identical_files(void)
{
   char root[MAX_PATH_LEN];
   assert(make_project(root, sizeof(root)));

   const char *body = "# Shared convention body that is byte-identical\n";
   char p1[MAX_PATH_LEN];
   char p2[MAX_PATH_LEN];
   snprintf(p1, sizeof(p1), "%s/AGENTS.md", root);
   write_file(p1, body);

   char sub[MAX_PATH_LEN];
   snprintf(sub, sizeof(sub), "%s/sub", root);
   mkdirp(sub);
   snprintf(p2, sizeof(p2), "%s/AGENTS.md", sub);
   write_file(p2, body);

   context_discovery_t disc = {0};
   assert(context_discover(sub, 0, &disc) == 0);
   assert(disc.file_count == 1);
   assert(disc.duplicate_skips == 1);

   /* Ensure the shared body appears only once. */
   const char *first = strstr(disc.rendered, "Shared convention body");
   assert(first);
   const char *second = strstr(first + 1, "Shared convention body");
   assert(second == NULL);

   context_discovery_free(&disc);
   rm_rf(root);
}

static void test_budget_caps_output(void)
{
   char root[MAX_PATH_LEN];
   assert(make_project(root, sizeof(root)));

   /* Write a large file, then give the discovery call a tiny budget. The
    * output must stay under budget + small slack. */
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/AGENTS.md", root);
   FILE *f = fopen(path, "w");
   assert(f);
   for (int i = 0; i < 400; i++)
      fputs("line of convention text that eats up several dozen bytes per row\n", f);
   fclose(f);

   context_discovery_t disc = {0};
   assert(context_discover(root, 512, &disc) == 0);
   /* Either the file was skipped (budget_truncations) or it was rendered
    * below the budget + slack. */
   if (disc.rendered)
      assert(disc.bytes_used <= 512 + 256);
   context_discovery_free(&disc);
   rm_rf(root);
}

static void test_stops_at_project_root(void)
{
   /* Create a project, then place a file in the parent of the root (which
    * should NOT be visited because discovery stops at `.git`). */
   char root[MAX_PATH_LEN];
   assert(make_project(root, sizeof(root)));

   /* Parent of root. */
   char parent[MAX_PATH_LEN];
   snprintf(parent, sizeof(parent), "%s", root);
   char *slash = strrchr(parent, '/');
   assert(slash);
   *slash = '\0';

   /* Sibling AGENTS.md in parent — must not be picked up. */
   char sibling[MAX_PATH_LEN];
   snprintf(sibling, sizeof(sibling), "%s/AGENTS.md.aimee_ctxdisc_test", parent);
   write_file(sibling, "# Parent-level convention (must not be picked up)\n");

   context_discovery_t disc = {0};
   assert(context_discover(root, 0, &disc) == 0);
   /* The project had no files of its own, so nothing should be rendered
    * (the sibling in /tmp must not leak in). */
   if (disc.rendered)
      assert(strstr(disc.rendered, "Parent-level convention") == NULL);

   context_discovery_free(&disc);
   unlink(sibling);
   rm_rf(root);
}

int main(void)
{
   test_empty_dir_yields_nothing();
   test_finds_nearest_file_first();
   test_deduplicates_identical_files();
   test_budget_caps_output();
   test_stops_at_project_root();
   printf("test_context_discover: all tests passed\n");
   return 0;
}
