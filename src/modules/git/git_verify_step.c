#include "git_verify_internal.h"

#include "dstr.h"
#include "platform_process.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VERIFY_STEP_TIMEOUT_MS_DEFAULT (30 * 60 * 1000)

static int verify_step_cancelled(void *arg)
{
   volatile int *cancel_requested = (volatile int *)arg;
   return cancel_requested && *cancel_requested;
}

static int verify_step_timeout_ms(void)
{
   const char *env = getenv("AIMEE_VERIFY_STEP_TIMEOUT_MS");
   if (env && env[0])
   {
      char *end = NULL;
      long v = strtol(env, &end, 10);
      if (end != env && v > 0 && v <= 24L * 60L * 60L * 1000L)
         return (int)v;
   }
   return VERIFY_STEP_TIMEOUT_MS_DEFAULT;
}

static char *verify_replace_all_literal(const char *src, const char *needle,
                                        const char *replacement)
{
   if (!src)
      return safe_strdup("");
   if (!needle || !needle[0] || !replacement)
      return safe_strdup(src);

   const char *first = strstr(src, needle);
   if (!first)
      return safe_strdup(src);

   dstr_t out;
   dstr_init(&out);
   const char *p = src;
   const size_t needle_len = strlen(needle);
   while (1)
   {
      const char *hit = strstr(p, needle);
      if (!hit)
      {
         dstr_append_str(&out, p);
         break;
      }
      dstr_append(&out, p, (size_t)(hit - p));
      dstr_append_str(&out, replacement);
      p = hit + needle_len;
   }
   return dstr_steal(&out);
}

static int verify_step_is_test_like(const verify_thread_ctx_t *ctx, const char *run)
{
   const char *name = (ctx && ctx->step) ? ctx->step->name : "";
   if (strcmp(name, "unit-tests") == 0 || strcmp(name, "test") == 0 ||
       strcmp(name, "verify-local") == 0)
      return 1;
   return run && (strstr(run, "unit-tests") || strstr(run, "verify-local"));
}

static char *verify_normalize_step_command(const verify_thread_ctx_t *ctx, const char *run)
{
   char *cmd = verify_replace_all_literal(run, "make -j$(nproc | awk '{print ($1>8)?8:$1}')",
                                          "make -j${AIMEE_VERIFY_MAKE_JOBS}");
   if (!cmd)
      return safe_strdup(run);

   dstr_t out;
   dstr_init(&out);
   dstr_append_str(
       &out, "AIMEE_VERIFY_MAKE_JOBS=${AIMEE_VERIFY_MAKE_JOBS:-2}; "
             "export AIMEE_VERIFY_MAKE_JOBS; "
             "CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-$AIMEE_VERIFY_MAKE_JOBS}; "
             "export CMAKE_BUILD_PARALLEL_LEVEL; ");
   if (verify_step_is_test_like(ctx, cmd))
      dstr_append_str(&out, "TEST_RUN_JOBS=${TEST_RUN_JOBS:-${AIMEE_VERIFY_TEST_JOBS:-1}}; "
                            "export TEST_RUN_JOBS; ");
   if (ctx && ctx->step && ctx->step->scope_changed)
   {
      char *changed_file = shell_escape(ctx->changed_files_path);
      char *baseline = shell_escape(ctx->baseline_ref);
      char *matched = shell_escape(ctx->changed_matched);
      dstr_appendf(&out,
                   "AIMEE_VERIFY_CHANGED_FILES_FILE='%s'; export "
                   "AIMEE_VERIFY_CHANGED_FILES_FILE; ",
                   changed_file ? changed_file : "");
      dstr_appendf(&out, "AIMEE_VERIFY_BASELINE_REF='%s'; export AIMEE_VERIFY_BASELINE_REF; ",
                   baseline ? baseline : "");
      dstr_appendf(&out,
                   "AIMEE_VERIFY_CHANGED_MATCHED='%s'; export "
                   "AIMEE_VERIFY_CHANGED_MATCHED; ",
                   matched ? matched : "");
      dstr_appendf(&out, "AIMEE_VERIFY_CHANGED_ALL=%d; export AIMEE_VERIFY_CHANGED_ALL; ",
                   ctx->changed_all ? 1 : 0);
      free(changed_file);
      free(baseline);
      free(matched);
   }
   dstr_append_str(&out, cmd);
   free(cmd);
   return dstr_steal(&out);
}

static char *verify_build_step_command(const verify_thread_ctx_t *ctx)
{
   const char *run = (ctx && ctx->step) ? ctx->step->run : "";
   char *normalized = verify_normalize_step_command(ctx, run);
   if (!ctx || !ctx->project_root[0])
      return normalized;

   char *esc = shell_escape(ctx->project_root);
   size_t len = strlen(esc) + strlen(normalized) + 16;
   char *cmd = malloc(len);
   if (!cmd)
   {
      free(esc);
      return normalized;
   }
   snprintf(cmd, len, "cd '%s' && %s", esc, normalized);
   free(esc);
   free(normalized);
   return cmd;
}

void verify_run_step(verify_thread_ctx_t *ctx)
{
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   char *cmd = verify_build_step_command(ctx);
   size_t out_len = 0;
   int timeout_ms = verify_step_timeout_ms();
   if (ctx->cancel_requested && *ctx->cancel_requested)
   {
      ctx->rc = -1;
      ctx->output = safe_strdup("verify: step cancelled before start\n");
   }
   else
   {
      ctx->rc =
          platform_exec_capture_cancellable(cmd, &ctx->output, &out_len, timeout_ms,
                                            verify_step_cancelled, (void *)ctx->cancel_requested);
   }
   free(cmd);

   clock_gettime(CLOCK_MONOTONIC, &t1);
   ctx->elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

   if (!ctx->output)
      ctx->output = safe_strdup("");
   if (ctx->cancel_requested && *ctx->cancel_requested)
   {
      size_t cur = strlen(ctx->output);
      const char *msg = "\nverify: step cancelled because the owning session closed\n";
      char *merged = malloc(cur + strlen(msg) + 1);
      if (merged)
      {
         memcpy(merged, ctx->output, cur);
         strcpy(merged + cur, msg);
         free(ctx->output);
         ctx->output = merged;
      }
   }

   if (ctx->rc == -1 && timeout_ms > 0 && ctx->elapsed * 1000.0 >= (double)timeout_ms * 0.95)
   {
      dstr_t msg;
      dstr_init(&msg);
      if (ctx->output && ctx->output[0])
         dstr_append_str(&msg, ctx->output);
      dstr_appendf(&msg, "\nverify: step timed out after %.1fs\n", (double)timeout_ms / 1000.0);
      free(ctx->output);
      ctx->output = dstr_steal(&msg);
   }
}
