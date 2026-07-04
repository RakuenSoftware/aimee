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

   /* ---- tc_recognize (S2) ---- */
#define RECO(line) tc_recognize(line)
   {
      tc_reco_result_t r;
      /* plain recognized commands + subcommand extraction */
      r = RECO("git status");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "git") && !strcmp(r.sub, "status"));
      r = RECO("cargo test --all");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "cargo") && !strcmp(r.sub, "test"));
      r = RECO("pytest tests/");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "pytest"));
      /* subcommand skips leading options */
      r = RECO("git -C /repo status");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.sub, "status"));

      /* wrapper unwrapping */
      r = RECO("npx tsc --noEmit");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "tsc"));
      r = RECO("uv run pytest -q");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "pytest"));
      r = RECO("poetry run pytest");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "pytest"));
      r = RECO("time cargo build");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "cargo"));
      r = RECO("env FOO=bar RUST_LOG=info cargo test");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "cargo"));
      r = RECO("FOO=bar cargo test");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "cargo"));
      r = RECO("sudo -u ci npm run build");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "npm") && !strcmp(r.sub, "run"));
      r = RECO("pnpm exec eslint .");
      assert(r.outcome == TC_RECOGNIZED && !strcmp(r.cmd, "eslint"));

      /* a path-prefixed invocation is OPAQUE even if the basename is a known command —
       * `/tmp/git` / `./git` must NOT inherit the git family filter (masquerade guard). */
      r = RECO("/usr/bin/git log");
      assert(r.outcome == TC_OPAQUE);
      r = RECO("./git status");
      assert(r.outcome == TC_OPAQUE);
      r = RECO("/tmp/cargo test");
      assert(r.outcome == TC_OPAQUE);

      /* OPAQUE: multiplexers, make, scripts, interpreters */
      r = RECO("make test");
      assert(r.outcome == TC_OPAQUE && !strcmp(r.cmd, "make"));
      r = RECO("xargs rm");
      assert(r.outcome == TC_OPAQUE);
      r = RECO("./run.sh --ci");
      assert(r.outcome == TC_OPAQUE);
      r = RECO("bash scripts/build.sh");
      assert(r.outcome == TC_OPAQUE);
      r = RECO("/opt/tools/custom_runner");
      assert(r.outcome == TC_OPAQUE);

      /* UNRECOGNIZED: unknown command + any compound/piped/substituted line */
      r = RECO("frobnicate --now");
      assert(r.outcome == TC_UNRECOGNIZED);
      r = RECO("cargo test | grep FAIL");
      assert(r.outcome == TC_UNRECOGNIZED); /* pipe -> passthrough */
      r = RECO("git status && cargo build");
      assert(r.outcome == TC_UNRECOGNIZED); /* chain -> passthrough */
      r = RECO("echo $(git rev-parse HEAD)");
      assert(r.outcome == TC_UNRECOGNIZED); /* substitution -> passthrough */
      r = RECO("");
      assert(r.outcome == TC_UNRECOGNIZED);
      r = RECO(NULL);
      assert(r.outcome == TC_UNRECOGNIZED);
   }

   printf("ok\n");
   return 0;
}
