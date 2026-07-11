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

/* ---- helpers ---- */

static char g_tmpdir[256];

static void setup_tmpdir(void)
{
   snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/test_dcs_XXXXXX");
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

/* ---- delegate_extract_named_paths ---- */

static void test_extract_no_paths(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n = delegate_extract_named_paths("Fix the bug in the authentication module.", paths,
                                        DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 0);
   printf("  extract_no_paths: ok\n");
}

static void test_extract_single_path(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n = delegate_extract_named_paths("Edit src/config.c to fix the parsing bug.", paths,
                                        DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 1);
   assert(strcmp(paths[0], "src/config.c") == 0);
   printf("  extract_single_path: ok\n");
}

static void test_extract_trims_terminal_period(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n =
       delegate_extract_named_paths("Edit only /tmp/aimee-fixture/remaining.c. The tests in "
                                    "/tmp/aimee-fixture/test_remaining.c expect a clamped result.",
                                    paths, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 2);
   assert(strcmp(paths[0], "/tmp/aimee-fixture/remaining.c") == 0);
   assert(strcmp(paths[1], "/tmp/aimee-fixture/test_remaining.c") == 0);
   printf("  extract_trims_terminal_period: ok\n");
}

static void test_extract_multiple_paths(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n = delegate_extract_named_paths("Update src/foo.c and src/bar.h to add the new interface.",
                                        paths, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 2);
   printf("  extract_multiple_paths: ok\n");
}

static void test_extract_deduplication(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n = delegate_extract_named_paths("Edit src/config.c carefully; do not break src/config.c.",
                                        paths, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 1);
   assert(strcmp(paths[0], "src/config.c") == 0);
   printf("  extract_deduplication: ok\n");
}

static void test_extract_no_slash_skipped(void)
{
   /* bare filenames without '/' are not repo-relative paths */
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n =
       delegate_extract_named_paths("Fix config.c and util.h.", paths, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 0);
   printf("  extract_no_slash_skipped: ok\n");
}

static void test_extract_skips_negated_references(void)
{
   /* "Do NOT touch / Don't / skip" are common in briefs that name
    * off-limits files. Treating those as required outputs leads to
    * spurious "named file was not created" warnings. */
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   const char *prompt = "Implement src/db1/agent_jobs.c with the new schema\n"
                        "Do NOT touch src/cmd_cron.c — follow-up task\n"
                        "Skip src/scheduler.c — owned by another delegate\n"
                        "Don't modify src/headers/legacy.h either\n";
   int n = delegate_extract_named_paths(prompt, paths, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 1);
   assert(strcmp(paths[0], "src/db1/agent_jobs.c") == 0);
   printf("  extract_skips_negated_references: ok\n");
}

static void test_extract_skips_json_example_values(void)
{
   /* Briefs often embed an illustrative JSON schema to show the delegate the
    * shape of its output. Quoted paths in JSON value position (after [ , :) are
    * documentation, not output targets — scraping them caused spurious
    * "named file 'src/x.c' was not created" failures. Prose instructions that
    * happen to quote a real path ("create \"src/real.c\"") must still count. */
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   const char *prompt = "Implement the decomposer. The schema is:\n"
                        "{\"units\":[{\"local_id\":\"t1\",\"owned_files\":[\"src/x.c\"],"
                        "\"read_context\":[\"src/y.h\"]}]}\n"
                        "Now create \"src/roadmap_decompose.c\" with that logic.\n";
   int n = delegate_extract_named_paths(prompt, paths, DELEGATE_DRIFT_MAX_PATHS);
   /* src/x.c and src/y.h are JSON example values → dropped; the prose-quoted
    * src/roadmap_decompose.c (quote preceded by the word "create ") survives. */
   assert(n == 1);
   assert(strcmp(paths[0], "src/roadmap_decompose.c") == 0);
   printf("  extract_skips_json_example_values: ok\n");
}

static void test_extract_unknown_extension_skipped(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n = delegate_extract_named_paths("Edit src/binary.exe to fix it.", paths,
                                        DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 0);
   printf("  extract_unknown_extension_skipped: ok\n");
}

static void test_extract_various_extensions(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n = delegate_extract_named_paths("Update src/module.py and config/settings.yaml", paths,
                                        DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 2);
   printf("  extract_various_extensions: ok\n");
}

static void test_extract_ignores_prompt_file_attachment(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   const char *prompt = "Review this patch for regressions.\n\n"
                        "# Prompt File\n"
                        "diff --git a/src/foo.c b/src/foo.c\n"
                        "--- a/src/foo.c\n"
                        "+++ b/src/foo.c\n"
                        "+   const char *paths[] = {\"isolated/context.c\"};\n";
   int n = delegate_extract_named_paths(prompt, paths, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 0);
   printf("  extract_ignores_prompt_file_attachment: ok\n");
}

static void test_extract_ignores_validation_evidence_bundle(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   const char *prompt =
       "Slot probe A. Reply exactly: minimax-slot-A-ok\n\n"
       "---\n"
       "## Parent Worktree Diff Evidence\n"
       "Use this bundle as the source of truth.\n\n"
       "---\n"
       "## Validation Evidence Bundle\n"
       "repo_evidence:\n"
       "- build_files_absent: ./tests/test_install_noninteractive.sh, CMakeLists.txt\n"
       "branch_changed_files:\n"
       "src/delegate_prompt.c\n";
   int n = delegate_extract_named_paths(prompt, paths, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 0);
   printf("  extract_ignores_validation_evidence_bundle: ok\n");
}

static void test_extract_normalizes_git_diff_prefixes(void)
{
   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   const char *prompt = "Review findings for diff --git a/src/foo.c b/src/foo.c and "
                        "the hunk header +++ b/src/bar.h.";
   int n = delegate_extract_named_paths(prompt, paths, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 2);
   assert(strcmp(paths[0], "src/foo.c") == 0);
   assert(strcmp(paths[1], "src/bar.h") == 0);
   printf("  extract_normalizes_git_diff_prefixes: ok\n");
}

/* ---- delegate_check_named_file_drift (pre-flight) ---- */

static void test_preflight_nonexistent_no_create_intent(void)
{
   char errbuf[512] = {0};
   const char *paths[] = {"src/nonexistent.c"};
   int rc = delegate_check_named_file_drift(paths, 1, "Edit src/nonexistent.c to fix the bug.",
                                            NULL, NULL, errbuf, sizeof(errbuf));
   assert(rc == -1);
   assert(strstr(errbuf, "nonexistent.c") != NULL);
   assert(strstr(errbuf, "create intent") != NULL);
   printf("  preflight_nonexistent_no_create_intent: ok\n");
}

static void test_preflight_nonexistent_with_create_intent(void)
{
   char errbuf[512] = {0};
   const char *paths[] = {"src/newfile.c"};
   int rc =
       delegate_check_named_file_drift(paths, 1, "Create a new file src/newfile.c with the module.",
                                       NULL, NULL, errbuf, sizeof(errbuf));
   assert(rc == 0);
   printf("  preflight_nonexistent_with_create_intent: ok\n");
}

static void test_preflight_readonly_missing_file_as_context(void)
{
   char errbuf[512] = {0};
   const char *paths[] = {"src/newfile.c"};
   int rc = delegate_check_named_file_drift(paths, 1,
                                            "Read-only review of src/newfile.c. Do not edit files.",
                                            NULL, NULL, errbuf, sizeof(errbuf));
   assert(rc == 0);
   printf("  preflight_readonly_missing_file_as_context: ok\n");
}

static void test_preflight_existing_file_no_error(void)
{
   setup_tmpdir();
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/config.c");

   char full_path[512];
   snprintf(full_path, sizeof(full_path), "%s/src/config.c", g_tmpdir);

   char errbuf[512] = {0};
   const char *paths[] = {full_path};
   int rc =
       delegate_check_named_file_drift(paths, 1, "Edit the config parser to handle edge cases.",
                                       NULL, NULL, errbuf, sizeof(errbuf));
   /* Existing file: pre-flight passes regardless */
   assert(rc == 0);
   cleanup_tmpdir();
   printf("  preflight_existing_file_no_error: ok\n");
}

static void test_preflight_relative_existing_file_with_base(void)
{
   setup_tmpdir();
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/isolated", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("isolated/context.c");

   char errbuf[512] = {0};
   const char *paths[] = {"isolated/context.c"};
   int rc = delegate_check_named_file_drift(
       paths, 1, "Read isolated/context.c and summarize it without edits.", NULL, g_tmpdir, errbuf,
       sizeof(errbuf));
   assert(rc == 0);
   cleanup_tmpdir();
   printf("  preflight_relative_existing_file_with_base: ok\n");
}

static void test_preflight_remote_scp_path_skipped(void)
{
   /* An ops/deploy brief that names a remote scp target must NOT hard-fail the
    * delegate: the tokenizer strips the `user@host:` prefix and extracts the bare
    * absolute host path, which does not resolve under the worktree. */
   const char *prompt =
       "Update aimee-server on the host. The server config lives at "
       "admin@192.168.1.254:/mnt/media/.plugins/aimee-server/server/home/aimee.yaml; "
       "read it over SSH and confirm the bearer token.";

   char extracted[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int n = delegate_extract_named_paths(prompt, extracted, DELEGATE_DRIFT_MAX_PATHS);
   assert(n == 1);
   assert(strcmp(extracted[0], "/mnt/media/.plugins/aimee-server/server/home/aimee.yaml") == 0);

   char errbuf[512] = {0};
   const char *paths[] = {extracted[0]};
   int rc = delegate_check_named_file_drift(paths, 1, prompt, NULL, "/home/virant/dev/aimee",
                                            errbuf, sizeof(errbuf));
   assert(rc == 0);
   assert(errbuf[0] == '\0');
   printf("  preflight_remote_scp_path_skipped: ok\n");
}

static void test_preflight_absolute_outside_worktree_skipped(void)
{
   /* A bare absolute path outside the worktree root is a referenced external
    * file, not an in-repo create target — pre-flight must not hard-fail. */
   char errbuf[512] = {0};
   const char *paths[] = {"/mnt/media/other/thing.c"};
   int rc = delegate_check_named_file_drift(paths, 1,
                                            "Update /mnt/media/other/thing.c on the remote host.",
                                            NULL, "/home/virant/dev/aimee", errbuf, sizeof(errbuf));
   assert(rc == 0);
   printf("  preflight_absolute_outside_worktree_skipped: ok\n");
}

static void test_preflight_relative_under_worktree_still_fails(void)
{
   /* Regression guard: a relative, in-worktree path that doesn't exist and has no
    * create verb MUST still hard-fail — the external-path skip must not weaken
    * the guard for real in-repo targets. */
   char errbuf[512] = {0};
   const char *paths[] = {"src/nonexistent_xyz.c"};
   int rc = delegate_check_named_file_drift(paths, 1, "Edit src/nonexistent_xyz.c to fix the bug.",
                                            NULL, "/home/virant/dev/aimee", errbuf, sizeof(errbuf));
   assert(rc == -1);
   assert(strstr(errbuf, "create intent") != NULL);
   printf("  preflight_relative_under_worktree_still_fails: ok\n");
}

/* ---- delegate_prompt_allows_writes ---- */

static void test_prompt_write_intent(void)
{
   assert(delegate_prompt_allows_writes(NULL) == 1);
   assert(delegate_prompt_allows_writes("") == 1);
   assert(delegate_prompt_allows_writes("Implement src/foo.c and update tests.") == 1);
   assert(delegate_prompt_allows_writes("Add focused tests. Do not edit implementation files.") ==
          1);
   assert(delegate_prompt_allows_writes("Do not edit implementation files.") == 0);
   assert(delegate_prompt_allows_writes("Read-only review of src/foo.c. Do not edit files.") == 0);
   assert(delegate_prompt_allows_writes("Inspect src/foo.c but do not modify anything.") == 0);
   assert(delegate_prompt_allows_writes("Ownership: inspect only for now.") == 0);
   assert(delegate_prompt_allows_writes("Analysis only: identify the lowest-risk split.") == 0);
   printf("  prompt_write_intent: ok\n");
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

static void test_postrun_path_in_response(void)
{
   setup_tmpdir();
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/config.c");

   char full_path[512];
   snprintf(full_path, sizeof(full_path), "%s/src/config.c", g_tmpdir);

   char response[1024];
   snprintf(response, sizeof(response),
            "I edited %s to fix the parsing bug by updating the token loop.", full_path);

   char errbuf[512] = {0};
   const char *paths[] = {full_path};
   int rc = delegate_check_named_file_drift(paths, 1, NULL, response, NULL, errbuf, sizeof(errbuf));
   assert(rc == 0);
   cleanup_tmpdir();
   printf("  postrun_path_in_response: ok\n");
}

static void test_postrun_path_absent_hard_drift(void)
{
   setup_tmpdir();
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/config.c");

   char full_path[512];
   snprintf(full_path, sizeof(full_path), "%s/src/config.c", g_tmpdir);

   const char *response = "I fixed the authentication module by updating the session handler.";

   char errbuf[512] = {0};
   const char *paths[] = {full_path};
   int rc = delegate_check_named_file_drift(paths, 1, NULL, response, NULL, errbuf, sizeof(errbuf));
   assert(rc == -1);
   assert(strstr(errbuf, "drift") != NULL || strstr(errbuf, "not found") != NULL);
   cleanup_tmpdir();
   printf("  postrun_path_absent_hard_drift: ok\n");
}

static void test_postrun_readonly_path_absent_soft_drift(void)
{
   setup_tmpdir();
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/config.c");

   char full_path[512];
   snprintf(full_path, sizeof(full_path), "%s/src/config.c", g_tmpdir);

   const char *prompt = "Review src/config.c. Do not edit files.";
   const char *response = "No blocking findings.";

   char errbuf[512] = {0};
   const char *paths[] = {full_path};
   int rc =
       delegate_check_named_file_drift(paths, 1, prompt, response, NULL, errbuf, sizeof(errbuf));
   assert(rc == 1);
   assert(strstr(errbuf, "read-only context") != NULL);
   cleanup_tmpdir();
   printf("  postrun_readonly_path_absent_soft_drift: ok\n");
}

static void test_postrun_basename_only_soft_drift(void)
{
   setup_tmpdir();
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/config.c");

   char full_path[512];
   snprintf(full_path, sizeof(full_path), "%s/src/config.c", g_tmpdir);

   /* Response mentions "config.c" but not the full path */
   const char *response = "I edited config.c to fix the parsing bug.";

   char errbuf[512] = {0};
   const char *paths[] = {full_path};
   int rc = delegate_check_named_file_drift(paths, 1, NULL, response, NULL, errbuf, sizeof(errbuf));
   assert(rc == 1); /* soft drift — only basename matched */
   assert(strstr(errbuf, "ambiguous") != NULL || strstr(errbuf, "basename") != NULL);
   cleanup_tmpdir();
   printf("  postrun_basename_only_soft_drift: ok\n");
}

static void test_postrun_nonexistent_skipped(void)
{
   /* Nonexistent files are skipped in the post-run check */
   const char *response = "I completed the task without touching that file.";
   char errbuf[512] = {0};
   const char *paths[] = {"/tmp/definitely_nonexistent_file_xyz.c"};
   int rc = delegate_check_named_file_drift(paths, 1, NULL, response, NULL, errbuf, sizeof(errbuf));
   assert(rc == 0);
   printf("  postrun_nonexistent_skipped: ok\n");
}

static void test_postrun_wt_readonly_missing_file_soft_drift(void)
{
   setup_tmpdir();
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q && git -C '%s' commit --allow-empty -m init -q",
            g_tmpdir, g_tmpdir);
   if (system(cmd) != 0)
   {
      cleanup_tmpdir();
      printf("  postrun_wt_readonly_missing_file_soft_drift: skipped (git unavailable)\n");
      return;
   }

   char missing_path[512];
   snprintf(missing_path, sizeof(missing_path), "%s/src/missing.c", g_tmpdir);
   const char *prompt = "Read-only review of src/missing.c. Do not edit files.";
   const char *response = "No blocking findings.";
   char errbuf[512] = {0};
   const char *paths[] = {missing_path};
   int rc = delegate_check_named_file_drift(paths, 1, prompt, response, g_tmpdir, errbuf,
                                            sizeof(errbuf));
   assert(rc == 1);
   assert(strstr(errbuf, "read-only context") != NULL);
   cleanup_tmpdir();
   printf("  postrun_wt_readonly_missing_file_soft_drift: ok\n");
}

/* With a real worktree, an existing context file not touched by the delegate
 * must produce soft drift (rc=1), not hard drift. */
static void test_postrun_wt_context_file_soft_drift(void)
{
   setup_tmpdir();
   /* Create a minimal git repo so git diff --name-only HEAD works. */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q && git -C '%s' commit --allow-empty -m init -q",
            g_tmpdir, g_tmpdir);
   if (system(cmd) != 0)
   {
      cleanup_tmpdir();
      printf("  postrun_wt_context_file_soft_drift: skipped (git unavailable)\n");
      return;
   }

   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/src", g_tmpdir);
   mkdir(subdir, 0755);
   make_file("src/context.c"); /* pre-existing file, staged in git */

   snprintf(cmd, sizeof(cmd), "git -C '%s' add src/context.c && git -C '%s' commit -m add -q",
            g_tmpdir, g_tmpdir);
   (void)system(cmd);

   char full_path[512];
   snprintf(full_path, sizeof(full_path), "%s/src/context.c", g_tmpdir);

   /* Delegate's response doesn't mention the context file — response mentions only new work. */
   const char *response = "Created src/new_module.c with the requested functionality.";
   char errbuf[512] = {0};
   const char *paths[] = {full_path};
   int rc =
       delegate_check_named_file_drift(paths, 1, NULL, response, g_tmpdir, errbuf, sizeof(errbuf));
   /* context file not in git diff, exists on disk → soft drift, not hard */
   assert(rc == 1);
   assert(strstr(errbuf, "context") != NULL || strstr(errbuf, "not modified") != NULL);
   cleanup_tmpdir();
   printf("  postrun_wt_context_file_soft_drift: ok\n");
}

static void test_null_inputs(void)
{
   char errbuf[512] = {0};
   assert(delegate_extract_named_paths(NULL, NULL, 0) == 0);
   assert(delegate_check_named_file_drift(NULL, 0, NULL, NULL, NULL, errbuf, sizeof(errbuf)) == 0);
   assert(delegate_check_named_file_drift(NULL, 5, "prompt", "response", NULL, errbuf,
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

   test_extract_no_paths();
   test_extract_single_path();
   test_extract_trims_terminal_period();
   test_extract_multiple_paths();
   test_extract_deduplication();
   test_extract_no_slash_skipped();
   test_extract_skips_negated_references();
   test_extract_skips_json_example_values();
   test_extract_unknown_extension_skipped();
   test_extract_various_extensions();
   test_extract_ignores_prompt_file_attachment();
   test_extract_ignores_validation_evidence_bundle();
   test_extract_normalizes_git_diff_prefixes();

   test_preflight_nonexistent_no_create_intent();
   test_preflight_nonexistent_with_create_intent();
   test_preflight_readonly_missing_file_as_context();
   test_preflight_existing_file_no_error();
   test_preflight_relative_existing_file_with_base();
   test_preflight_remote_scp_path_skipped();
   test_preflight_absolute_outside_worktree_skipped();
   test_preflight_relative_under_worktree_still_fails();
   test_prompt_write_intent();
   test_validation_bundle_identifies_source_worktree();
   test_validation_bundle_keeps_large_diff_handlers();

   test_postrun_path_in_response();
   test_postrun_path_absent_hard_drift();
   test_postrun_readonly_path_absent_soft_drift();
   test_postrun_basename_only_soft_drift();
   test_postrun_nonexistent_skipped();
   test_postrun_wt_readonly_missing_file_soft_drift();
   test_postrun_wt_context_file_soft_drift();
   test_null_inputs();

   printf("All delegate_context_shed tests passed.\n");
   return 0;
}
