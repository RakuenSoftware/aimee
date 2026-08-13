/* test_delegate_context_shed.c: unit tests for delegate prompt context shedding
 * and named-file drift detection */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "cmd_agent_delegate_impl.h"
#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/delegate_role.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* ---- helpers ---- */

static char g_tmpdir[256];

static void setup_tmpdir(void)
{
   snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/test_dcs_XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_tmpdir) != NULL);
}

static void cleanup_tmpdir(void)
{
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
   (void)system(cmd);
}

static void make_file(const char *relpath)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/%s", g_tmpdir, relpath);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "/* test file */\n");
   fclose(f);
}

/* ---- delegate_check_named_file_drift (pre-flight) ---- */

/* A drift provider the tests drive directly, which also RECORDS what it was
 * asked.
 *
 * What a missing file means, whether an unmodified one is drift or context, and
 * how hard to fail are the module's rules, pinned against the module
 * (server-go/modules/delegates/namedfiledrift_test.go) -- the fixtures that used
 * to prove them here were ported case for case, by name.
 *
 * What only this side can test is the half it still owns: that the FACTS it
 * gathers from a real filesystem are the right ones, and that it honours the
 * verdict it gets back. So this decodes the request into something the tests can
 * assert on, and answers with whatever the test asked for. */
typedef struct
{
   char path[512];
   unsigned flags;
   int hit_count;
} drift_seen_path_t;

static struct
{
   char prompt[1024];
   char response[1024];
   char worktree[512];
   unsigned flags;
   int path_count;
   drift_seen_path_t paths[8];
   int calls;
} g_drift_seen;

static unsigned g_drift_severity;
static const char *g_drift_message = "";

static int shed_test_drift(const uint8_t *request, size_t request_len, unsigned *severity,
                           char *message, size_t message_cap)
{
   memset(&g_drift_seen, 0, sizeof(g_drift_seen));

   aimee_delegates_rd_t r;
   r.buf = request;
   r.len = request_len;
   r.at = 0;
   r.bad = 0;
   if (aimee_delegates_rd_u32(&r) != AIMEE_DELEGATES_DRIFT_REQUEST_MAGIC ||
       aimee_delegates_rd_u32(&r) != (uint32_t)AIMEE_DELEGATES_WIRE_VERSION)
      return -1;

   g_drift_seen.flags = aimee_delegates_rd_u32(&r);
   aimee_delegates_rd_str(&r, g_drift_seen.prompt, sizeof(g_drift_seen.prompt));
   aimee_delegates_rd_str(&r, g_drift_seen.response, sizeof(g_drift_seen.response));
   aimee_delegates_rd_str(&r, g_drift_seen.worktree, sizeof(g_drift_seen.worktree));

   uint32_t count = aimee_delegates_rd_u32(&r);
   for (uint32_t i = 0; i < count && !r.bad; i++)
   {
      drift_seen_path_t seen;
      memset(&seen, 0, sizeof(seen));
      aimee_delegates_rd_str(&r, seen.path, sizeof(seen.path));
      seen.flags = aimee_delegates_rd_u32(&r);
      uint32_t hits = aimee_delegates_rd_u32(&r);
      seen.hit_count = (int)hits;
      for (uint32_t h = 0; h < hits && !r.bad; h++)
      {
         char scratch[512];
         aimee_delegates_rd_str(&r, scratch, sizeof(scratch));
      }
      if (g_drift_seen.path_count < 8)
         g_drift_seen.paths[g_drift_seen.path_count++] = seen;
   }
   if (r.bad)
      return -1;

   g_drift_seen.calls++;
   if (severity)
      *severity = g_drift_severity;
   if (message && message_cap)
      snprintf(message, message_cap, "%s", g_drift_message);
   return 0;
}

static const drift_seen_path_t *drift_seen(const char *path)
{
   for (int i = 0; i < g_drift_seen.path_count; i++)
      if (strcmp(g_drift_seen.paths[i].path, path) == 0)
         return &g_drift_seen.paths[i];
   return NULL;
}

/* The existence flag comes from the filesystem, resolved against the worktree
 * for a relative path. This is the fact the whole check is built on. */
static void test_drift_sends_existence_from_the_filesystem(void)
{
   setup_tmpdir();
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/present.c");

   char errbuf[512] = {0};
   const char *paths[] = {"src/present.c", "src/absent.c"};
   g_drift_severity = AIMEE_DELEGATES_DRIFT_NONE;
   g_drift_message = "";
   (void)delegate_check_named_file_drift(paths, 2, "Edit the files.", NULL, g_tmpdir, 1, errbuf,
                                         sizeof(errbuf));

   assert(g_drift_seen.calls == 1);
   assert(g_drift_seen.path_count == 2);
   const drift_seen_path_t *present = drift_seen("src/present.c");
   const drift_seen_path_t *absent = drift_seen("src/absent.c");
   assert(present && (present->flags & AIMEE_DELEGATES_DRIFT_PATH_EXISTS));
   assert(absent && !(absent->flags & AIMEE_DELEGATES_DRIFT_PATH_EXISTS));

   cleanup_tmpdir();
   printf("  drift_sends_existence_from_the_filesystem: ok\n");
}

/* The brief, the reply, the worktree and the role all travel with the question.
 * A module that never learns the role cannot tell a write delegate that failed
 * from a reviewer that was never going to write. */
static void test_drift_sends_the_context_it_owns(void)
{
   char errbuf[512] = {0};
   const char *paths[] = {"src/a.c"};
   g_drift_severity = AIMEE_DELEGATES_DRIFT_NONE;

   (void)delegate_check_named_file_drift(paths, 1, "the brief", "the reply", "/some/worktree", 1,
                                         errbuf, sizeof(errbuf));
   assert(strcmp(g_drift_seen.prompt, "the brief") == 0);
   assert(strcmp(g_drift_seen.response, "the reply") == 0);
   assert(strcmp(g_drift_seen.worktree, "/some/worktree") == 0);
   assert(g_drift_seen.flags & AIMEE_DELEGATES_DRIFT_WRITES_ALLOWED);

   (void)delegate_check_named_file_drift(paths, 1, "the brief", NULL, NULL, 0, errbuf,
                                         sizeof(errbuf));
   /* Pre-flight: no reply yet, and a read role. */
   assert(g_drift_seen.response[0] == '\0');
   assert(!(g_drift_seen.flags & AIMEE_DELEGATES_DRIFT_WRITES_ALLOWED));

   printf("  drift_sends_the_context_it_owns: ok\n");
}

/* An empty index-hit list is SENT, not treated as a reason to drop the path.
 * Empty means "the index could not say", which the module reads as ambiguous;
 * skipping the path instead would silently excuse every missing file whenever
 * the index is down. */
static void test_drift_sends_paths_even_when_the_index_says_nothing(void)
{
   char errbuf[512] = {0};
   const char *paths[] = {"src/definitely_absent_xyz.c"};
   g_drift_severity = AIMEE_DELEGATES_DRIFT_NONE;

   (void)delegate_check_named_file_drift(paths, 1, "Edit src/definitely_absent_xyz.c.", NULL, NULL,
                                         1, errbuf, sizeof(errbuf));
   assert(g_drift_seen.path_count == 1);
   assert(strcmp(g_drift_seen.paths[0].path, "src/definitely_absent_xyz.c") == 0);
   printf("  drift_sends_paths_even_when_the_index_says_nothing: ok\n");
}

/* The verdict is the module's; this side only translates it into the return
 * code its callers branch on, and surfaces the wording verbatim. */
static void test_drift_honours_the_verdict(void)
{
   char errbuf[512] = {0};
   const char *paths[] = {"src/a.c"};

   g_drift_severity = AIMEE_DELEGATES_DRIFT_HARD;
   g_drift_message = "the wording the module chose";
   assert(delegate_check_named_file_drift(paths, 1, "p", NULL, NULL, 1, errbuf, sizeof(errbuf)) ==
          -1);
   assert(strcmp(errbuf, "the wording the module chose") == 0);

   errbuf[0] = '\0';
   g_drift_severity = AIMEE_DELEGATES_DRIFT_SOFT;
   g_drift_message = "a softer complaint";
   assert(delegate_check_named_file_drift(paths, 1, "p", NULL, NULL, 1, errbuf, sizeof(errbuf)) ==
          1);
   assert(strcmp(errbuf, "a softer complaint") == 0);

   errbuf[0] = '\0';
   g_drift_severity = AIMEE_DELEGATES_DRIFT_NONE;
   g_drift_message = "";
   assert(delegate_check_named_file_drift(paths, 1, "p", NULL, NULL, 1, errbuf, sizeof(errbuf)) ==
          0);
   assert(errbuf[0] == '\0');

   printf("  drift_honours_the_verdict: ok\n");
}

/* A role-policy provider the test drives, so the parent-diff question has a
 * settable answer without this harness deciding which roles want one. */
static int g_needs_parent_diff;

static int shed_test_role_policy(int op, const char *role, int a, int b, int c, int *out)
{
   (void)role;
   (void)a;
   (void)b;
   (void)c;
   if (op != DELEGATE_ROLE_OP_PARENT_DIFF || !out)
      return -1;
   *out = g_needs_parent_diff;
   return 0;
}

/* The live parent-diff evidence path, which had NO test before this: the
 * fixtures that pinned "which roles get a bundle" were driving
 * delegate_maybe_append_validation_bundle, a wrapper whose only caller ships in
 * no binary. This exercises the one server_compute.c actually calls.
 *
 * The ROLE list is the module's and is pinned there; what is asserted here is
 * the composition C still owns -- a delegate that may write is never handed the
 * parent's diff, however the module answers. */
static void test_parent_diff_evidence_respects_write_capability(void)
{
   setup_tmpdir();
   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' init -q && "
            "git -C '%s' config user.email test@example.com && "
            "git -C '%s' config user.name Test",
            g_tmpdir, g_tmpdir, g_tmpdir);
   if (system(cmd) != 0)
   {
      cleanup_tmpdir();
      printf("  parent_diff_evidence_respects_write_capability: skipped (git unavailable)\n");
      return;
   }
   make_file("changed.txt");

   g_needs_parent_diff = 1;

   /* A read-only inspection role gets the parent's diff prepended. */
   char *grounded =
       delegate_prepend_parent_diff_evidence("review this", "review", 0, g_tmpdir, "deleg-test");
   assert(grounded != NULL);
   assert(strstr(grounded, "Parent Worktree Diff Evidence") != NULL);
   assert(strncmp(grounded, "review this", strlen("review this")) == 0);
   free(grounded);

   /* The same role that may WRITE gets nothing: it is producing the diff, not
    * reviewing one. */
   char *writer =
       delegate_prepend_parent_diff_evidence("review this", "review", 1, g_tmpdir, "deleg-test");
   assert(writer == NULL);

   /* And when the module says the role has no inspection duty, nothing either. */
   g_needs_parent_diff = 0;
   char *unrelated =
       delegate_prepend_parent_diff_evidence("do it", "code", 0, g_tmpdir, "deleg-test");
   assert(unrelated == NULL);

   cleanup_tmpdir();
   printf("  parent_diff_evidence_respects_write_capability: ok\n");
}

static void test_validation_bundle_identifies_source_worktree(void)
{
   setup_tmpdir();
   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' init -q && "
            "git -C '%s' config user.email test@example.com && "
            "git -C '%s' config user.name Test",
            g_tmpdir, g_tmpdir, g_tmpdir);
   if (system(cmd) != 0)
   {
      cleanup_tmpdir();
      printf("  validation_bundle_identifies_source_worktree: skipped (git unavailable)\n");
      return;
   }

   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/context.c");
   make_file("src/Makefile");
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' add src/context.c src/Makefile && git -C '%s' commit -m add -q", g_tmpdir,
            g_tmpdir);
   assert(system(cmd) == 0);

   char path[512];
   snprintf(path, sizeof(path), "%s/src/context.c", g_tmpdir);
   FILE *f = fopen(path, "a");
   assert(f != NULL);
   fprintf(f, "/* changed */\n");
   fclose(f);

   char *bundle = delegate_build_validation_bundle(g_tmpdir);
   assert(bundle != NULL);
   assert(strstr(bundle, "Validation Evidence Bundle") != NULL);
   assert(strstr(bundle, "worktree_path: ") != NULL);
   assert(strstr(bundle, g_tmpdir) != NULL);
   assert(strstr(bundle, "repo_evidence:") != NULL);
   assert(strstr(bundle, "build_files_present: src/Makefile") != NULL);
   assert(strstr(bundle, "build_files_absent: Makefile, CMakeLists.txt") != NULL);
   assert(strstr(bundle, "verification_hint_from_files: make -C src ...") != NULL);
   assert(strstr(bundle, "diff_source: uncommitted changes in worktree_path") != NULL);
   assert(strstr(bundle, "diff_command: git -C <worktree_path> diff --no-ext-diff") != NULL);
   assert(strstr(bundle, "branch_diff_source: committed branch delta against origin/main") != NULL);
   assert(strstr(bundle, "branch_diff_base: (unavailable)") != NULL);
   assert(strstr(bundle, "changed_file_count: 1") != NULL);
   assert(strstr(bundle, "src/context.c") != NULL);
   assert(strstr(bundle, "+/* changed */") != NULL);
   assert(strstr(bundle, "Do not assert a build system, symbol, struct field") != NULL);
   free(bundle);
   cleanup_tmpdir();
   printf("  validation_bundle_identifies_source_worktree: ok\n");
}

static void test_validation_bundle_keeps_large_diff_handlers(void)
{
   setup_tmpdir();
   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' init -q && "
            "git -C '%s' config user.email test@example.com && "
            "git -C '%s' config user.name Test",
            g_tmpdir, g_tmpdir, g_tmpdir);
   if (system(cmd) != 0)
   {
      cleanup_tmpdir();
      printf("  validation_bundle_keeps_large_diff_handlers: skipped (git unavailable)\n");
      return;
   }

   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/large.c");
   snprintf(cmd, sizeof(cmd), "git -C '%s' add src/large.c && git -C '%s' commit -m add -q",
            g_tmpdir, g_tmpdir);
   assert(system(cmd) == 0);

   char path[512];
   snprintf(path, sizeof(path), "%s/src/large.c", g_tmpdir);
   FILE *f = fopen(path, "a");
   assert(f != NULL);
   for (int i = 0; i < 650; i++)
      fprintf(f, "int filler_%03d(void) { return %d; }\n", i, i);
   fprintf(f, "static int late_handler(void) { return 42; }\n");
   fprintf(f, "int route_call(void) { return late_handler(); }\n");
   fclose(f);

   char *bundle = delegate_build_validation_bundle(g_tmpdir);
   assert(bundle != NULL);
   assert(strstr(bundle, "--unified=12") != NULL);
   assert(strstr(bundle, "filler_600") != NULL);
   assert(strstr(bundle, "late_handler") != NULL);
   assert(strstr(bundle, "route_call") != NULL);
   free(bundle);
   cleanup_tmpdir();
   printf("  validation_bundle_keeps_large_diff_handlers: ok\n");
}

/* ---- delegate_check_named_file_drift (post-run) ---- */

/* Post-run with a worktree, `git diff` is the ground truth, and whether it
 * mentions a path is a fact this side looks up. */
static void test_drift_sends_diff_membership_post_run(void)
{
   setup_tmpdir();
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q", g_tmpdir);
   if (system(cmd) != 0)
   {
      cleanup_tmpdir();
      printf("  drift_sends_diff_membership_post_run: skipped (git unavailable)\n");
      return;
   }
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/touched.c");
   make_file("src/untouched.c");
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' add -A && git -C '%s' -c user.email=t@t "
            "-c user.name=t commit -qm base",
            g_tmpdir, g_tmpdir);
   if (system(cmd) != 0)
   {
      cleanup_tmpdir();
      printf("  drift_sends_diff_membership_post_run: skipped (git unavailable)\n");
      return;
   }

   char path[512];
   snprintf(path, sizeof(path), "%s/src/touched.c", g_tmpdir);
   FILE *f = fopen(path, "a");
   assert(f != NULL);
   fputs("/* changed */\n", f);
   fclose(f);

   char errbuf[512] = {0};
   const char *paths[] = {"src/touched.c", "src/untouched.c"};
   g_drift_severity = AIMEE_DELEGATES_DRIFT_NONE;
   (void)delegate_check_named_file_drift(paths, 2, "p", "a reply", g_tmpdir, 1, errbuf,
                                         sizeof(errbuf));

   const drift_seen_path_t *touched = drift_seen("src/touched.c");
   const drift_seen_path_t *untouched = drift_seen("src/untouched.c");
   assert(touched && (touched->flags & AIMEE_DELEGATES_DRIFT_PATH_IN_DIFF));
   assert(untouched && !(untouched->flags & AIMEE_DELEGATES_DRIFT_PATH_IN_DIFF));

   cleanup_tmpdir();
   printf("  drift_sends_diff_membership_post_run: ok\n");
}

static void test_null_inputs(void)
{
   char errbuf[512] = {0};
   assert(delegate_extract_named_paths(NULL, NULL, 0) == 0);
   assert(delegate_check_named_file_drift(NULL, 0, NULL, NULL, NULL, 1, errbuf, sizeof(errbuf)) ==
          0);
   assert(delegate_check_named_file_drift(NULL, 5, "prompt", "response", NULL, 1, errbuf,
                                          sizeof(errbuf)) == 0);
   printf("  null_inputs: ok\n");
}

int kb_client_index_find(const char *identifier, term_hit_t *out, int max)
{
   (void)identifier;
   (void)out;
   (void)max;
   return 0;
}

int main(void)
{
   printf("test_delegate_context_shed:\n");

   delegate_register_drift_provider(shed_test_drift);
   delegate_register_role_policy_provider(shed_test_role_policy);

   test_drift_sends_existence_from_the_filesystem();
   test_drift_sends_the_context_it_owns();
   test_drift_sends_paths_even_when_the_index_says_nothing();
   test_drift_honours_the_verdict();
   test_drift_sends_diff_membership_post_run();
   test_parent_diff_evidence_respects_write_capability();
   test_validation_bundle_identifies_source_worktree();
   test_validation_bundle_keeps_large_diff_handlers();

   test_null_inputs();

   printf("All delegate_context_shed tests passed.\n");
   return 0;
}
