/* test_git_verify_contract.c: the structured (format=json) verify verdict
 * contract — autonomous-dev-execution-substrate §1.
 *
 * Exercises the REAL builder (verify_build_verdict), not a stub, with synthetic
 * step contexts, asserting the machine-stable shape the autonomous driver
 * consumes: verdict/reason, per-step {name,tier,status,exit,seconds[,log]}, the
 * mechanical-tier default, and the cancelled->unavailable mapping that keeps an
 * unfinished run DISTINCT from a real pass. */

#include "git_verify.h"
#include "git_verify_internal.h"

#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static verify_step_t mk_step(const char *name, const char *tier)
{
   verify_step_t s;
   memset(&s, 0, sizeof s);
   snprintf(s.name, sizeof s.name, "%s", name);
   if (tier)
      snprintf(s.tier, sizeof s.tier, "%s", tier);
   return s;
}

static const char *step_status(const cJSON *root, int idx)
{
   const cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
   const cJSON *s = cJSON_GetArrayItem(steps, idx);
   return cJSON_GetObjectItemCaseSensitive(s, "status")->valuestring;
}

static const char *step_str(const cJSON *root, int idx, const char *key)
{
   const cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
   const cJSON *s = cJSON_GetArrayItem(steps, idx);
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(s, key);
   return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* All steps pass -> verdict passed, reason ok; tier defaults to mechanical when
 * the step left it empty, and echoes an explicit tier verbatim. */
static void test_all_pass(void)
{
   verify_step_t s0 = mk_step("build", NULL);        /* empty tier -> default */
   verify_step_t s1 = mk_step("e2e", "integration"); /* explicit tier */
   verify_thread_ctx_t ctxs[2];
   memset(ctxs, 0, sizeof ctxs);
   ctxs[0].step = &s0;
   ctxs[0].rc = 0;
   ctxs[0].elapsed = 1.5;
   ctxs[1].step = &s1;
   ctxs[1].rc = 0;
   ctxs[1].elapsed = 2.0;

   cJSON *v = verify_build_verdict(ctxs, 2, 0, 0);
   assert(cJSON_GetObjectItemCaseSensitive(v, "schema_version")->valueint == 1);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(v, "verdict")->valuestring, "passed") == 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(v, "reason")->valuestring, "ok") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(v, "total")->valueint == 2);
   assert(cJSON_GetObjectItemCaseSensitive(v, "passed")->valueint == 2);
   assert(cJSON_GetObjectItemCaseSensitive(v, "failed")->valueint == 0);
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(v, "has_uncommitted_changes")));
   assert(strcmp(step_status(v, 0), "pass") == 0);
   assert(strcmp(step_str(v, 0, "tier"), "mechanical") == 0); /* default */
   assert(strcmp(step_str(v, 1, "tier"), "integration") == 0);
   assert(step_str(v, 0, "log") == NULL); /* no log on a passing step */
   cJSON_Delete(v);
   printf("  PASS: all-pass verdict + tier default/echo\n");
}

/* A failing step -> verdict failed, reason steps-failed, the failed step carries
 * its exit code + a tail log; the uncommitted-changes flag rides through. */
static void test_one_fail(void)
{
   verify_step_t s0 = mk_step("lint", NULL);
   verify_step_t s1 = mk_step("unit", NULL);
   verify_thread_ctx_t ctxs[2];
   memset(ctxs, 0, sizeof ctxs);
   ctxs[0].step = &s0;
   ctxs[0].rc = 0;
   ctxs[1].step = &s1;
   ctxs[1].rc = 2;
   ctxs[1].elapsed = 0.3;
   ctxs[1].output = strdup("assertion failed: foo != bar");

   cJSON *v = verify_build_verdict(ctxs, 2, 0, 1);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(v, "verdict")->valuestring, "failed") == 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(v, "reason")->valuestring, "steps-failed") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(v, "failed")->valueint == 1);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "has_uncommitted_changes")));
   assert(strcmp(step_status(v, 1), "fail") == 0);
   const cJSON *steps = cJSON_GetObjectItemCaseSensitive(v, "steps");
   assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(steps, 1), "exit")->valueint == 2);
   assert(step_str(v, 1, "log") != NULL && strstr(step_str(v, 1, "log"), "assertion failed"));
   cJSON_Delete(v);
   free(ctxs[1].output);
   printf("  PASS: one-fail verdict + exit + log tail\n");
}

/* A skipped (cached) step reports status "skip", carrying its skip_reason. */
static void test_skip(void)
{
   verify_step_t s0 = mk_step("build", NULL);
   verify_thread_ctx_t ctxs[1];
   memset(ctxs, 0, sizeof ctxs);
   ctxs[0].step = &s0;
   ctxs[0].rc = 0;
   ctxs[0].skipped = 1;
   snprintf(ctxs[0].skip_reason, sizeof ctxs[0].skip_reason, "unchanged paths");

   cJSON *v = verify_build_verdict(ctxs, 1, 0, 0);
   assert(strcmp(step_status(v, 0), "skip") == 0);
   assert(strcmp(step_str(v, 0, "skip_reason"), "unchanged paths") == 0);
   /* a skip still counts as passed for the aggregate verdict */
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(v, "verdict")->valuestring, "passed") == 0);
   cJSON_Delete(v);
   printf("  PASS: skip status + reason\n");
}

/* Cancelled -> verdict "unavailable" (NOT passed), reason cancelled: the driver
 * must never read an aborted run as a verified pass. */
static void test_cancelled_unavailable(void)
{
   verify_step_t s0 = mk_step("build", NULL);
   verify_thread_ctx_t ctxs[1];
   memset(ctxs, 0, sizeof ctxs);
   ctxs[0].step = &s0;
   ctxs[0].rc = 0; /* even with a passing step, cancellation -> unavailable */

   cJSON *v = verify_build_verdict(ctxs, 1, 1 /*cancelled*/, 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(v, "verdict")->valuestring, "unavailable") == 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(v, "reason")->valuestring, "cancelled") == 0);
   cJSON_Delete(v);
   printf("  PASS: cancelled -> unavailable (distinct from passed)\n");
}

int main(void)
{
   printf("git_verify_contract:\n");
   test_all_pass();
   test_one_fail();
   test_skip();
   test_cancelled_unavailable();
   printf("ok\n");
   return 0;
}
