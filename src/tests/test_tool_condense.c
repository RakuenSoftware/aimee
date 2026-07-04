/* test_tool_condense: the deterministic tool-output condensation primitives (S1). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "tool_condense.h"

static void eq(const char *got, const char *want, const char *label)
{
   if (!got || strcmp(got, want) != 0)
   {
      fprintf(stderr, "\nFAIL %s:\n  got : %s\n  want: %s\n", label, got ? got : "(null)", want);
      abort();
   }
}

int main(void)
{
   printf("tool-condense: ");

   /* ---- enable gate ---- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof cfg);
      assert(tool_condense_enabled(&cfg) == 0);
      cfg.reduce_command_filter = 1;
      assert(tool_condense_enabled(&cfg) == 1);
      assert(tool_condense_enabled(NULL) == 0);
   }

   /* ---- tc_strip_noise: ANSI escapes removed ---- */
   {
      char *r = tc_strip_noise("\x1b[31mred\x1b[0m text");
      eq(r, "red text", "strip ansi");
      free(r);
   }
   /* CR redraw: keep text after the last '\r' */
   {
      char *r = tc_strip_noise("Downloading 10%\rDownloading 80%\rDone");
      eq(r, "Done", "strip cr-redraw");
      free(r);
   }
   /* collapse a run of blank lines to one, preserve content */
   {
      char *r = tc_strip_noise("a\n\n\n\nb");
      eq(r, "a\n\nb", "collapse blanks");
      free(r);
   }
   /* a line with only ANSI becomes blank and participates in blank-collapse */
   {
      char *r = tc_strip_noise("a\n\x1b[2K\n\nb");
      eq(r, "a\n\nb", "ansi-only line -> blank");
      free(r);
   }

   /* ---- tc_dedup_lines ---- */
   {
      char *r = tc_dedup_lines("x\nx\nx\ny\nx");
      eq(r, "x  (x3)\ny\nx", "dedup consecutive");
      free(r);
   }
   {
      char *r = tc_dedup_lines("only");
      eq(r, "only", "dedup single");
      free(r);
   }
   {
      char *r = tc_dedup_lines("a\nb\nc");
      eq(r, "a\nb\nc", "dedup no-repeats");
      free(r);
   }

   /* ---- tc_truncate_with_signal ---- */
   /* head=1 tail=1, keep FAIL lines in the middle */
   {
      char *r = tc_truncate_with_signal("L0\nL1\nFAIL x\nL3\nL4\nL5", 1, 1, "FAIL");
      eq(r, "L0\n... 1 lines elided ...\nFAIL x\n... 2 lines elided ...\nL5", "truncate+signal");
      free(r);
   }
   /* already fits (head+tail >= lines) -> verbatim */
   {
      char *r = tc_truncate_with_signal("a\nb\nc", 2, 2, NULL);
      eq(r, "a\nb\nc", "truncate fits");
      free(r);
   }
   /* no signal, pure head/tail with a middle elision */
   {
      char *r = tc_truncate_with_signal("a\nb\nc\nd\ne", 1, 1, NULL);
      eq(r, "a\n... 3 lines elided ...\ne", "truncate no-signal");
      free(r);
   }
   /* signal matches nothing -> just head/tail */
   {
      char *r = tc_truncate_with_signal("a\nb\nc\nd", 1, 1, "ZZZ");
      eq(r, "a\n... 2 lines elided ...\nd", "truncate signal-miss");
      free(r);
   }

   /* ---- empty input round-trips (never NULL-as-OOM) ---- */
   {
      char *r = tc_strip_noise("");
      assert(r && r[0] == '\0');
      free(r);
      r = tc_dedup_lines("");
      assert(r && r[0] == '\0');
      free(r);
   }

   printf("ok\n");
   return 0;
}
