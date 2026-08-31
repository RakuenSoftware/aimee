/* test_git_verify_contract.c: the structured (format=json) verify verdict
 * contract — autonomous-dev-execution-substrate §1.
 *
 * Exercises the REAL builder (verify_build_verdict), not a stub, with synthetic
 * step contexts, asserting the machine-stable shape the autonomous driver
 * consumes: verdict/reason, per-step {name,tier,status,exit,seconds[,log]}, the
 * mechanical-tier default, and the cancelled->unavailable mapping that keeps an
 * unfinished run DISTINCT from a real pass. */

#include "modules/git/git_verify.h"
#include "modules/git/git_verify_internal.h"

#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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

/* "no Makefile found" was asserted for five different causes and never named the
 * directory it searched. In this very repository -- which has src/Makefile, a case
 * find_makefile_subdir handles explicitly -- `aimee git verify` reported exactly
 * that, because verify_root resolved to a different checkout via the session's
 * worktree mapping. The Makefile was never missing.
 *
 * The fix is not a better guess, it is naming the path. Assert the root appears in
 * every branch, so a wrong root stays diagnosable from the message alone. */
static void test_unavailable_reason_names_the_root(void)
{
   char why[768];

   /* A directory with no Makefile: say so, and say where. */
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-verify-reason-XXXXXX", platform_tmpdir());
   char *dir = mkdtemp(tmpl);
   assert(dir != NULL);
   verify_config_unavailable_reason(dir, why, sizeof(why));
   assert(strstr(why, dir) != NULL);
   assert(strstr(why, "no Makefile") != NULL);
   /* And point at the real cause of the observed failure, which is not the file. */
   assert(strstr(why, "worktree mapping") != NULL);

   /* A Makefile under src/ is FOUND, so the message must not claim otherwise --
    * this is the exact shape of this repo, and the case that was misreported. */
   char sub[512];
   snprintf(sub, sizeof(sub), "%s/src", dir);
   assert(mkdir(sub, 0755) == 0);
   char mk[600];
   snprintf(mk, sizeof(mk), "%s/Makefile", sub);
   FILE *f = fopen(mk, "w");
   assert(f != NULL);
   fputs("nothing-aimee-knows:\n\t@true\n", f);
   fclose(f);

   verify_config_unavailable_reason(dir, why, sizeof(why));
   assert(strstr(why, "no Makefile") == NULL); /* it exists; do not lie about it */
   assert(strstr(why, dir) != NULL);
   assert(strstr(why, "src") != NULL);
   assert(strstr(why, "recognise") != NULL);

   unlink(mk);
   rmdir(sub);
   rmdir(dir);

   /* No resolvable root at all: distinct from "no Makefile", because there was
    * nowhere to look rather than nothing to find. */
   verify_config_unavailable_reason("", why, sizeof(why));
   assert(strstr(why, "no repository root") != NULL);
   verify_config_unavailable_reason(NULL, why, sizeof(why));
   assert(strstr(why, "no repository root") != NULL);

   printf("  test_unavailable_reason_names_the_root: PASS\n");
}

/* Verify picks its target by asking which candidate is actually a repository.
 * That question only has value if "no" is a possible answer.
 *
 * resolve_verify_root falls back to the directory itself, and ultimately to
 * getcwd(), so it answers "yes, here" for ANY directory. Relying on it to choose
 * a root is how `aimee git verify` came to resolve /var/lib/aimee -- aimee-server's
 * own home, not a repository -- and then report no Makefile there. Passing an
 * explicit path=<repo> did not help, because the argument was never read.
 *
 * Pin the discriminator: a real repo resolves, a plain directory does NOT. */
static void test_git_toplevel_rejects_a_non_repo(void)
{
   char out[1024];

   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-verify-root-XXXXXX", platform_tmpdir());
   char *dir = mkdtemp(tmpl);
   assert(dir != NULL);

   /* A directory that is not a repository must fail, not answer with itself. */
   out[0] = '\0';
   assert(verify_git_toplevel(dir, out, sizeof(out)) != 0);

   /* The same directory, once it IS a repository, resolves to its toplevel. */
   char cmd[1200];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q 2>/dev/null", dir);
   assert(system(cmd) == 0);
   out[0] = '\0';
   assert(verify_git_toplevel(dir, out, sizeof(out)) == 0);
   assert(out[0] == '/');
   /* mkdtemp under /tmp may be a symlink (macOS /tmp -> /private/tmp), so compare
    * on the leaf rather than the full path. */
   const char *leaf = strrchr(dir, '/');
   assert(leaf && strstr(out, leaf + 1) != NULL);

   /* A subdirectory resolves to the repository ROOT, not to itself. */
   char sub[1100];
   snprintf(sub, sizeof(sub), "%s/nested/deeper", dir);
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", sub);
   assert(system(cmd) == 0);
   char sub_out[1024];
   assert(verify_git_toplevel(sub, sub_out, sizeof(sub_out)) == 0);
   assert(strcmp(sub_out, out) == 0);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
   (void)system(cmd);
   printf("  test_git_toplevel_rejects_a_non_repo: PASS\n");
}

int main(void)
{
   printf("git_verify_contract:\n");
   test_git_toplevel_rejects_a_non_repo();
   test_all_pass();
   test_one_fail();
   test_skip();
   test_cancelled_unavailable();
   test_unavailable_reason_names_the_root();
   printf("ok\n");
   return 0;
}
