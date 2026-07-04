/* test_tool_condense: the deterministic tool-output condensation primitives (S1). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

   /* ---- tc_family_test_runner (S3) ---- */
   {
      /* build 30 passing lines + a failure + summary */
      char big[4096];
      size_t off = 0;
      off += (size_t)snprintf(big + off, sizeof big - off, "running 32 tests\n");
      for (int k = 0; k < 30; k++)
         off += (size_t)snprintf(big + off, sizeof big - off, "test suite::case_%02d ... ok\n", k);
      off += (size_t)snprintf(big + off, sizeof big - off, "test suite::case_bad ... FAILED\n");
      snprintf(big + off, sizeof big - off,
               "failures:\n    suite::case_bad\ntest result: FAILED. 31 passed; 1 failed\n");

      char *r = tc_family_test_runner(1, big);
      assert(r);
      assert(strstr(r, "FAILED"));          /* failure kept */
      assert(strstr(r, "test result"));     /* summary kept */
      assert(strstr(r, "lines elided"));    /* passes elided */
      assert(strlen(r) < strlen(big));      /* shrank */
      assert(!strstr(r, "case_05 ... ok")); /* a middle passing line dropped */
      free(r);

      /* non-zero exit with NO failure signal -> passthrough (NULL) */
      char *r2 = tc_family_test_runner(1, "building...\nlinking...\nnothing useful here\n");
      assert(r2 == NULL);
   }

   /* ---- tool_condense_apply (S3) ---- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof cfg);

      /* a big passing pytest run (exit 0) */
      char big[8192];
      size_t off = 0;
      off +=
          (size_t)snprintf(big + off, sizeof big - off, "============ test session starts ====\n");
      for (int k = 0; k < 120; k++)
         off += (size_t)snprintf(big + off, sizeof big - off,
                                 "tests/test_mod.py::test_%03d PASSED\n", k);
      snprintf(big + off, sizeof big - off, "==== 120 passed in 3.14s ====\n");

      /* disabled -> passthrough */
      assert(tool_condense_apply(&cfg, "pytest -q", 0, big, "/tmp", NULL) == NULL);

      cfg.reduce_command_filter = 1;
      /* unrecognized command -> passthrough */
      assert(tool_condense_apply(&cfg, "frobnicate", 0, big, "/tmp", NULL) == NULL);
      /* recognized but no spill dir -> passthrough (lossless: never condense without spill) */
      assert(tool_condense_apply(&cfg, "pytest -q", 0, big, NULL, NULL) == NULL);

      /* recognized test runner + a real spill dir -> condensed + spill written */
      char dir[] = "/tmp/tc_test_XXXXXX";
      assert(mkdtemp(dir));
      tc_stats_t st;
      char *r = tool_condense_apply(&cfg, "pytest -q", 0, big, dir, &st);
      assert(r);
      assert(st.recognized && st.spilled && !strcmp(st.family, "test"));
      assert(st.final_bytes < st.raw_bytes);
      assert(strstr(r, "condensed by aimee")); /* recovery pointer present */
      assert(strstr(r, st.spill_ref));         /* references the spill file */
      assert(strstr(r, dir));                  /* the catable full path */
      /* the spill file exists + holds the FULL raw */
      char spath[512];
      snprintf(spath, sizeof spath, "%s/%s.out", dir, st.spill_ref);
      FILE *sf = fopen(spath, "rb");
      assert(sf);
      fseek(sf, 0, SEEK_END);
      long sz = ftell(sf);
      fclose(sf);
      assert(sz == st.raw_bytes);
      /* cleanup */
      unlink(spath);
      rmdir(dir);
      free(r);
   }

   /* ---- tc_family_diagnostics (S5) ---- */
   {
      char big[8192];
      size_t off = 0;
      for (int k = 0; k < 60; k++)
         off += (size_t)snprintf(big + off, sizeof big - off,
                                 "   Compiling crate_%02d v0.1.0 (/w/crate_%02d)\n", k, k);
      off += (size_t)snprintf(big + off, sizeof big - off,
                              "error[E0308]: mismatched types\n --> src/lib.rs:42:9\n");
      snprintf(big + off, sizeof big - off, "error: aborting due to previous error\n");

      char *r = tc_family_diagnostics(1, big);
      assert(r);
      assert(strstr(r, "E0308"));         /* the error kept */
      assert(strstr(r, "src/lib.rs:42")); /* the diagnostic location kept */
      assert(strstr(r, "lines elided"));  /* Compiling… progress elided */
      assert(!strstr(r, "crate_30"));     /* a middle progress line dropped */
      assert(strlen(r) < strlen(big));
      free(r);

      /* warnings on a clean (exit 0) build are still condensed (not gated on failure) */
      char warns[4096];
      off = 0;
      for (int k = 0; k < 40; k++)
         off += (size_t)snprintf(warns + off, sizeof warns - off, "   Compiling pkg_%02d\n", k);
      snprintf(warns + off, sizeof warns - off,
               "warning: unused variable `x`\n --> a.rs:1:5\nwarning: 1 warning emitted\n");
      char *r2 = tc_family_diagnostics(0, warns);
      assert(r2 && strstr(r2, "unused variable") && strstr(r2, "lines elided"));
      free(r2);

      /* SAFETY: a non-zero exit whose failure is in NO recognized format -> passthrough
       * (never hide an unrecognized build failure). */
      char *r3 = tc_family_diagnostics(1, "step one\nstep two\nsomething went sideways\n");
      assert(r3 == NULL);
   }

   /* ---- diagnostics routing through tool_condense_apply (S5) ---- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof cfg);
      cfg.reduce_command_filter = 1;
      char big[8192];
      size_t off = 0;
      for (int k = 0; k < 80; k++)
         off += (size_t)snprintf(big + off, sizeof big - off, "src/mod_%02d.ts building...\n", k);
      snprintf(big + off, sizeof big - off,
               "src/x.ts:10:3 - error TS2322: Type mismatch\nFound 1 error.\n");
      char dir[] = "/tmp/tc_diag_XXXXXX";
      assert(mkdtemp(dir));
      tc_stats_t st;
      char *r = tool_condense_apply(&cfg, "tsc --noEmit", 1, big, dir, &st);
      assert(r);
      assert(st.recognized && st.spilled && !strcmp(st.family, "diag"));
      assert(strstr(r, "TS2322"));
      char spath[512];
      snprintf(spath, sizeof spath, "%s/%s.out", dir, st.spill_ref);
      unlink(spath);
      rmdir(dir);
      free(r);
   }

   printf("ok\n");
   return 0;
}
