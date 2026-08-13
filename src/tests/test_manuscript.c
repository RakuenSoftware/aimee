/* test_manuscript.c: unit tests for the pure manuscript helpers. */
#include "manuscript.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void write_file(const char *dir, const char *name, const char *content)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(content, f);
   fclose(f);
}

int main(void)
{
   printf("manuscript: ");

   /* --- manuscript_count_words --- */
   assert(manuscript_count_words(NULL) == 0);
   assert(manuscript_count_words("") == 0);
   assert(manuscript_count_words("   \n\t ") == 0);
   assert(manuscript_count_words("one") == 1);
   assert(manuscript_count_words("one two three") == 3);
   assert(manuscript_count_words("  leading and   trailing  ") == 3);
   assert(manuscript_count_words("line one\nline two\n") == 4);

   /* --- manuscript_is_prose_file --- */
   assert(manuscript_is_prose_file("ch01.md") == 1);
   assert(manuscript_is_prose_file("scene.txt") == 1);
   assert(manuscript_is_prose_file("notes.MD") == 0); /* case-sensitive ext */
   assert(manuscript_is_prose_file("code.c") == 0);
   assert(manuscript_is_prose_file(".hidden.md") == 0);
   assert(manuscript_is_prose_file(".aimee-rules") == 0);
   assert(manuscript_is_prose_file(NULL) == 0);

   /* --- manuscript_continuity_failed --- */
   assert(manuscript_continuity_failed("...\nCONTINUITY: PASS\n") == 0);
   assert(manuscript_continuity_failed("...\nCONTINUITY: FAIL\n") == 1);
   assert(manuscript_continuity_failed("No issues found.") == 0);
   assert(manuscript_continuity_failed(NULL) == 0);

   /* --- manuscript_scan over a fixture dir --- */
   {
      char tmpl[256];
      snprintf(tmpl, sizeof tmpl, "%s/aimee-manuscript-XXXXXX", platform_tmpdir());
      char *dir = mkdtemp(tmpl);
      assert(dir);

      write_file(dir, "02-second.md", "alpha beta gamma");     /* 3 words */
      write_file(dir, "01-first.md", "one two");               /* 2 words */
      write_file(dir, "notes.txt", "a b c d e");               /* 5 words */
      write_file(dir, ".aimee-rules", "# bible\nnot counted"); /* excluded */
      write_file(dir, "outline.json", "ignored");              /* not prose */

      manuscript_entry_t files[16];
      long total = 0;
      int n = manuscript_scan(dir, files, 16, &total);
      assert(n == 3);
      assert(total == 10);
      /* Sorted by name: 01-first.md, 02-second.md, notes.txt */
      assert(strcmp(files[0].name, "01-first.md") == 0 && files[0].words == 2);
      assert(strcmp(files[1].name, "02-second.md") == 0 && files[1].words == 3);
      assert(strcmp(files[2].name, "notes.txt") == 0 && files[2].words == 5);

      /* Cleanup. */
      char rm[1100];
      snprintf(rm, sizeof(rm), "rm -rf '%s'", dir);
      (void)(system(rm) + 1);
   }

   /* --- empty / unreadable dir --- */
   {
      manuscript_entry_t files[4];
      long total = 7;
      int n = manuscript_scan("/nonexistent/aimee/dir", files, 4, &total);
      assert(n == 0);
      assert(total == 0);
   }

   printf("OK\n");
   return 0;
}
