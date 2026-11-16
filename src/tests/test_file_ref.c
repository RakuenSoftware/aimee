/* test_file_ref.c: unit tests for resolve_file_reference_context() */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "agent_coord.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* --- helpers --- */

static char g_tmpdir[256];

static void setup_tmpdir(void)
{
   snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/test_file_ref_XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_tmpdir) != NULL);
}

static void cleanup_tmpdir(void)
{
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
   (void)system(cmd);
}

static void write_file(const char *relpath, const char *content)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/%s", g_tmpdir, relpath);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(content, f);
   fclose(f);
}

/* --- tests --- */

static void test_valid_reference_evidence(void)
{
   write_file("foo.c", "int main(void) { return 0; }\n");

   char prompt[256];
   snprintf(prompt, sizeof(prompt), "Update the file @foo.c to add logging");

   char *out = resolve_file_reference_context(prompt, g_tmpdir);
   assert(out != NULL);
   assert(strstr(out, "--- @foo.c ---") != NULL);
   assert(strstr(out, "int main(void)") != NULL);
   assert(strstr(out, "---") != NULL);
   free(out);

   printf("  valid_reference_inlined: ok\n");
}

static void test_no_references_no_evidence(void)
{
   char *out = resolve_file_reference_context("just a plain prompt", g_tmpdir);
   assert(out != NULL);
   assert(!out[0]);
   free(out);

   printf("  no_references_passthrough: ok\n");
}

static void test_nonexistent_file_no_evidence(void)
{
   /* Unresolved task text must not become fabricated file evidence. */
   char *out = resolve_file_reference_context("see @nonexistent.c for details", g_tmpdir);
   assert(out != NULL);
   assert(!out[0]);
   assert(strstr(out, "[file not found") == NULL);
   free(out);

   printf("  nonexistent_file_literal: ok\n");
}

static void test_path_outside_project_no_evidence(void)
{
   /* An absolute path outside the project is left literal and NEVER inlined. */
   char *out = resolve_file_reference_context("read @/etc/passwd please", g_tmpdir);
   assert(out != NULL);
   assert(!out[0]);
   assert(strstr(out, "root:") == NULL); /* content must not leak */
   assert(strstr(out, "[file outside project") == NULL);
   free(out);

   printf("  path_outside_project_literal: ok\n");
}

static void test_traversal_no_evidence(void)
{
   /* A path-traversal @token is left literal and never inlined. */
   char *out = resolve_file_reference_context("read @../../../etc/shadow", g_tmpdir);
   assert(out != NULL);
   assert(!out[0]);
   assert(strstr(out, "root:") == NULL);
   free(out);

   printf("  traversal_literal: ok\n");
}

static void test_diff_has_no_evidence(void)
{
   /* The regression that made roundtables "sandbox-blind": a unified diff in the
    * prompt must pass through unmangled — @@ hunk headers, decorators and emails
    * are not file references. */
   const char *diff = "diff --git a/x.py b/x.py\n"
                      "@@ -1,3 +1,4 @@\n"
                      " @property\n"
                      "-old\n"
                      "+new  # ping a@b.com\n";
   char *out = resolve_file_reference_context(diff, g_tmpdir);
   assert(out != NULL);
   assert(!out[0]); /* Task text stays outside the evidence projection. */

   assert(strstr(out, "[file") == NULL); /* no markers injected */
   free(out);

   printf("  diff_passthrough: ok\n");
}

static void test_max_3_refs_respected(void)
{
   write_file("a.txt", "file A");
   write_file("b.txt", "file B");
   write_file("c.txt", "file C");
   write_file("d.txt", "file D");

   /* Only real files consume the evidence budget; the fourth is explicit. */
   char *out =
       resolve_file_reference_context("read @a.txt and @b.txt and @c.txt and @d.txt", g_tmpdir);
   assert(out != NULL);
   assert(strstr(out, "file A") != NULL);
   assert(strstr(out, "file B") != NULL);
   assert(strstr(out, "file C") != NULL);
   assert(strstr(out, "file D") == NULL); /* 4th not inlined */
   assert(strstr(out, "[file reference limit reached: @d.txt left unresolved]") != NULL);
   free(out);

   printf("  max_3_refs_respected: ok\n");
}

static void test_large_file_truncated(void)
{
   /* Write a file larger than FILE_REF_MAX_SIZE (10 KB) */
   char path[512];
   snprintf(path, sizeof(path), "%s/big.txt", g_tmpdir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   for (int i = 0; i < 11 * 1024; i++)
      fputc('x', f);
   fclose(f);

   char *out = resolve_file_reference_context("check @big.txt", g_tmpdir);
   assert(out != NULL);
   assert(strstr(out, "[TRUNCATED]") != NULL);
   free(out);

   printf("  large_file_truncated: ok\n");
}

static void test_at_without_path_no_evidence(void)
{
   /* Bare '@' with no path chars after it should pass through literally */
   char *out = resolve_file_reference_context("email me @ home", g_tmpdir);
   assert(out != NULL);
   assert(!out[0]);
   free(out);

   printf("  at_without_path_literal: ok\n");
}

static void test_reference_evidence_excludes_original_task(void)
{
   write_file("evidence.txt", "untrusted file content");
   char *evidence = resolve_file_reference_context("TASK_AUTHORITY @evidence.txt suffix", g_tmpdir);
   assert(evidence && strstr(evidence, "--- @evidence.txt ---"));
   assert(strstr(evidence, "untrusted file content"));
   assert(!strstr(evidence, "TASK_AUTHORITY") && !strstr(evidence, "suffix"));
   free(evidence);
   evidence = resolve_file_reference_context("TASK_AUTHORITY @missing.txt @ @@", g_tmpdir);
   assert(evidence && !evidence[0]);
   free(evidence);
   printf("  reference_evidence_excludes_original_task: ok\n");
}

int main(void)
{
   printf("test_file_ref:\n");

   setup_tmpdir();

   test_valid_reference_evidence();
   test_reference_evidence_excludes_original_task();
   test_no_references_no_evidence();
   test_nonexistent_file_no_evidence();
   test_path_outside_project_no_evidence();
   test_traversal_no_evidence();
   test_diff_has_no_evidence();
   test_max_3_refs_respected();
   test_large_file_truncated();
   test_at_without_path_no_evidence();

   cleanup_tmpdir();

   printf("all file_ref tests passed.\n");
   return 0;
}
