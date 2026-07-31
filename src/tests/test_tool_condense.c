/* test_tool_condense: the deterministic tool-output condensation primitives (S1). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* tool_condense_enabled reads live config now (via econ_preset_current). Publish
 * the values through the config SNAPSHOT rather than a config file: writing YAML
 * would drag the whole parse closure (config_sections, config_memory, yaml.o, ...)
 * into what is deliberately a minimal-link test, whereas config_snapshot_init is
 * the supported publish path and touches no disk. The tier resolution table is
 * covered separately, without I/O, in test_config_economizer; what matters here
 * is that the lever tracks econ_preset's command_filter. */
static void publish_econ(int mode, int module_economizer)
{
   static config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.economizer_mode = mode;
   cfg.module_economizer = module_economizer;
   config_snapshot_init(&cfg);
}

int main(void)
{
   printf("tool-condense: ");

   /* ---- enable gate ---- */
   {
      publish_econ(ECON_MODE_OFF, -1);
      assert(tool_condense_enabled() == 0); /* off tier -> lever off */
      publish_econ(ECON_MODE_SAFE, -1);
      assert(tool_condense_enabled() == 0); /* SAFE is lossless only */
      publish_econ(ECON_MODE_AGGRESSIVE, -1);
      assert(tool_condense_enabled() == 1); /* AGGRESSIVE turns the lever on */
      publish_econ(ECON_MODE_AGGRESSIVE, 0);
      assert(tool_condense_enabled() == 0); /* modules.economizer:false hard-kills it */
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

      /* NO-OVER-REDUCTION (P1a): the failure DETAIL (a separate line from its `--- FAIL:`
       * marker, `go test` style) must survive in the CONDENSED output, not just the spill.
       * 60 passes + a failure whose message is the line ABOVE the marker. */
      char gt[8192];
      size_t o = 0;
      for (int k = 0; k < 60; k++)
      {
         o += (size_t)snprintf(gt + o, sizeof gt - o, "=== RUN   TestPass%02d\n", k);
         o += (size_t)snprintf(gt + o, sizeof gt - o, "--- PASS: TestPass%02d (0.00s)\n", k);
      }
      o += (size_t)snprintf(gt + o, sizeof gt - o, "=== RUN   TestBoom\n");
      o += (size_t)snprintf(gt + o, sizeof gt - o, "    x_test.go:63: boom: expected 5 got 4\n");
      snprintf(gt + o, sizeof gt - o, "--- FAIL: TestBoom (0.00s)\nFAIL\n");
      char *r3 = tc_family_test_runner(1, gt);
      assert(r3);
      assert(strstr(r3, "--- FAIL: TestBoom"));     /* the marker */
      assert(strstr(r3, "boom: expected 5 got 4")); /* the DETAIL — must NOT be elided */
      assert(!strstr(r3, "TestPass30"));            /* middle passes still dropped */
      assert(strlen(r3) < strlen(gt));              /* still materially shrank */
      free(r3);

      /* multi-line detail block (pytest-style traceback BELOW the failure line) — every
       * line of the block survives, not just the first. */
      char py[8192];
      size_t p = 0;
      for (int k = 0; k < 50; k++)
         p += (size_t)snprintf(py + p, sizeof py - p, "test_mod.py::test_ok%02d PASSED\n", k);
      snprintf(py + p, sizeof py - p,
               "test_mod.py::test_div FAILED\n"
               "=================== FAILURES ===================\n"
               "    def test_div():\n"
               ">       assert divide(1, 0) == 1\n"
               "E       ZeroDivisionError: division by zero\n"
               "test_mod.py:7: ZeroDivisionError\n"
               "=========== 1 failed, 50 passed ===========\n");
      char *r4 = tc_family_test_runner(1, py);
      assert(r4);
      assert(strstr(r4, "test_div FAILED"));
      assert(strstr(r4, "ZeroDivisionError: division by zero")); /* the assertion cause */
      assert(strstr(r4, "assert divide(1, 0)"));                 /* the asserting line */
      assert(!strstr(r4, "test_ok20 PASSED"));                   /* passes dropped */
      free(r4);
   }

   /* ---- live application is aggressive-only ---- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof cfg);
      cfg.module_economizer = -1;
      cfg.economizer_mode = ECON_MODE_SAFE;
      assert(tool_condense_apply(&cfg, "pytest -q", 0, "one test passed\n", "/tmp", NULL) == NULL);
      cfg.economizer_mode = ECON_MODE_AGGRESSIVE;
      char *condensed =
          tool_condense_apply(&cfg, "pytest -q", 0, "one test passed\n", "/tmp", NULL);
      free(condensed);
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

   printf("ok\n");
   return 0;
}
