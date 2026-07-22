/* test_cmd_core.c: top-level command argument handling tests.
 * Tests MCP handler argument validation and subcmd dispatch. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "aimee.h"
#include "commands.h"
#include "mcp_git.h"
#include "cJSON.h"
#include "platform_test_util.h"

/* --- Helpers --- */

static char *get_mcp_text(cJSON *resp)
{
   if (!resp || !cJSON_IsArray(resp))
      return NULL;
   cJSON *item = cJSON_GetArrayItem(resp, 0);
   if (!item)
      return NULL;
   cJSON *text = cJSON_GetObjectItem(item, "text");
   if (!cJSON_IsString(text))
      return NULL;
   return text->valuestring;
}

/* --- Test handle_git_status in a real temp git repo --- */

static void test_handle_git_status_in_repo(void)
{
   char tmpdir[PATH_MAX];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-git-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo hello > file.txt && "
            "git add file.txt && git commit -q -m 'init'",
            tmpdir);
   assert(system(cmd) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_status(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "branch:") != NULL);
   assert(strstr(text, "clean") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Create an untracked file */
   snprintf(cmd, sizeof(cmd), "touch '%s/untracked.txt'", tmpdir);
   system(cmd);

   args = cJSON_CreateObject();
   resp = handle_git_status(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "untracked") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test handle_git_commit parameter validation --- */

static void test_handle_git_commit_missing_message(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "message") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_branch parameter validation --- */

static void test_handle_git_branch_missing_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "action") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_handle_git_branch_missing_name(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "name") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_clone parameter validation --- */

static void test_handle_git_clone_missing_url(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_clone(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "url") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_pr parameter validation --- */

static void test_handle_git_pr_missing_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "action") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_handle_git_pr_create_missing_title(void)
{
   /* Run in an isolated temp repo so merged-PR and verify gates don't
    * interfere with the missing-title error we're testing for. */
   char tmpdir[PATH_MAX];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-pr-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo x > f.txt && "
            "git add f.txt && git commit -q -m 'init'",
            tmpdir);
   assert(system(cmd) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "title") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test handle_git_commit in repo --- */

static void test_handle_git_commit_in_repo(void)
{
   char tmpdir[PATH_MAX];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-commit-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo hello > file.txt && "
            "git add file.txt && git commit -q -m 'init'",
            tmpdir);
   assert(system(cmd) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   /* Switch to a feature branch — commits on main are blocked */
   system("git checkout -q -b test-feature");

   /* Modify and stage */
   FILE *fp = fopen("file.txt", "w");
   fputs("modified\n", fp);
   fclose(fp);
   system("git add file.txt");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "test commit");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* A local fixture rather than a real command's table. These two cases test the
 * generic dispatcher, not any particular command — they used to borrow
 * get_work_subcmds(), which quietly coupled them to the work queue and broke
 * when it was removed. A fixture keeps them honest and linkable. */
static void fixture_subcmd_noop(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;
}

static const subcmd_t fixture_subcmds[] = {
    {"alpha", "First fixture subcommand", fixture_subcmd_noop},
    {"beta", "Second fixture subcommand", fixture_subcmd_noop},
    {NULL, NULL, NULL}};

/* --- Test subcmd_dispatch returns -1 for unknown subcommand --- */

static void test_subcmd_dispatch_unknown(void)
{
   const subcmd_t *table = fixture_subcmds;
   assert(table != NULL);

   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));

   int rc = subcmd_dispatch(table, "zzz_does_not_exist", &ctx, 0, NULL);
   assert(rc == -1);
}

/* --- Test subcmd_usage does not crash --- */

static void test_subcmd_usage_no_crash(void)
{
   const subcmd_t *table = fixture_subcmds;

   /* Redirect stdout to /dev/null */
   fflush(stdout);
   int saved = dup(STDOUT_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   if (dev_null >= 0)
   {
      dup2(dev_null, STDOUT_FILENO);
      close(dev_null);
   }

   subcmd_usage("fixture", table);

   dup2(saved, STDOUT_FILENO);
   close(saved);
}

/* --- Stack detection tests --- */

static char *make_tmpdir(void)
{
   static char tmpdir[PATH_MAX];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-stack-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   return tmpdir;
}

static void touch(const char *dir, const char *name)
{
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fclose(f);
}

static void rmdir_r(const char *dir)
{
   char cmd[PATH_MAX + 16];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
   system(cmd);
}

static void test_detect_stacks_rust(void)
{
   char *dir = make_tmpdir();
   touch(dir, "Cargo.toml");

   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);

   assert(n == 1);
   assert(strcmp(stacks[0].name, "Rust") == 0);
   assert(strcmp(stacks[0].build_cmd, "cargo build") == 0);
   assert(strcmp(stacks[0].test_cmd, "cargo test") == 0);

   rmdir_r(dir);
}

static void test_detect_stacks_go(void)
{
   char *dir = make_tmpdir();
   touch(dir, "go.mod");

   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);

   assert(n == 1);
   assert(strcmp(stacks[0].name, "Go") == 0);

   rmdir_r(dir);
}

static void test_detect_stacks_typescript(void)
{
   char *dir = make_tmpdir();
   touch(dir, "package.json");
   touch(dir, "tsconfig.json");

   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);

   assert(n == 1);
   assert(strcmp(stacks[0].name, "TypeScript") == 0);

   rmdir_r(dir);
}

static void test_detect_stacks_node(void)
{
   char *dir = make_tmpdir();
   touch(dir, "package.json");

   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);

   assert(n == 1);
   assert(strcmp(stacks[0].name, "Node.js") == 0);

   rmdir_r(dir);
}

static void test_detect_stacks_python(void)
{
   char *dir = make_tmpdir();
   touch(dir, "pyproject.toml");

   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);

   assert(n == 1);
   assert(strcmp(stacks[0].name, "Python") == 0);

   rmdir_r(dir);
}

static void test_detect_stacks_c(void)
{
   char *dir = make_tmpdir();
   touch(dir, "Makefile");
   touch(dir, "main.c");

   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);

   assert(n == 1);
   assert(strcmp(stacks[0].name, "C") == 0);
   assert(strcmp(stacks[0].build_cmd, "make") == 0);

   rmdir_r(dir);
}

static void test_detect_stacks_cmake(void)
{
   char *dir = make_tmpdir();
   touch(dir, "CMakeLists.txt");

   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);

   assert(n == 1);
   assert(strcmp(stacks[0].name, "CMake/C++") == 0);

   rmdir_r(dir);
}

static void test_detect_stacks_none(void)
{
   char *dir = make_tmpdir();

   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);

   assert(n == 0);

   rmdir_r(dir);
}

static void test_write_aimee_rules_basic(void)
{
   char *dir = make_tmpdir();
   stack_info_t stacks[1];
   stacks[0].name = "Rust";
   stacks[0].build_cmd = "cargo build";
   stacks[0].test_cmd = "cargo test";

   int rc = write_aimee_rules_file(dir, stacks, 1);
   assert(rc == 0);

   /* Verify file was created */
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/.aimee-rules", dir);
   FILE *f = fopen(path, "r");
   assert(f != NULL);

   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';

   assert(strstr(buf, "Rust") != NULL);
   assert(strstr(buf, "cargo build") != NULL);
   assert(strstr(buf, "cargo test") != NULL);
   assert(strstr(buf, "Working agreement") != NULL);

   rmdir_r(dir);
}

static void test_write_aimee_rules_idempotent(void)
{
   char *dir = make_tmpdir();

   /* Write once */
   int rc = write_aimee_rules_file(dir, NULL, 0);
   assert(rc == 0);

   /* Write again: should return 1 (already exists) without overwriting */
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/.aimee-rules", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("sentinel", f);
   fclose(f);

   rc = write_aimee_rules_file(dir, NULL, 0);
   assert(rc == 1);

   /* Verify content is still the sentinel */
   f = fopen(path, "r");
   assert(f != NULL);
   char buf[64];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   assert(strcmp(buf, "sentinel") == 0);

   rmdir_r(dir);
}

static void test_write_novel_rules_basic(void)
{
   char *dir = make_tmpdir();

   int rc = write_novel_rules_file(dir);
   assert(rc == 0);

   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/.aimee-rules", dir);
   FILE *f = fopen(path, "r");
   assert(f != NULL);

   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';

   /* Story-bible scaffold, not the engineering stack doc. */
   assert(strstr(buf, "Story Bible") != NULL);
   assert(strstr(buf, "Premise") != NULL);
   assert(strstr(buf, "Voice & POV") != NULL);
   assert(strstr(buf, "Timeline & Canon") != NULL);
   assert(strstr(buf, "Detected stack") == NULL);

   /* Idempotent: second call leaves a sentinel intact. */
   f = fopen(path, "w");
   assert(f != NULL);
   fputs("sentinel", f);
   fclose(f);
   assert(write_novel_rules_file(dir) == 1);
   f = fopen(path, "r");
   assert(f != NULL);
   char sb[64];
   size_t m = fread(sb, 1, sizeof(sb) - 1, f);
   fclose(f);
   sb[m] = '\0';
   assert(strcmp(sb, "sentinel") == 0);

   rmdir_r(dir);
}

static void test_write_songbook_rules_basic(void)
{
   char *dir = make_tmpdir();

   int rc = write_songbook_rules_file(dir);
   assert(rc == 0);

   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/.aimee-rules", dir);
   FILE *f = fopen(path, "r");
   assert(f != NULL);

   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';

   /* Songbook scaffold, not story bible or engineering stack. */
   assert(strstr(buf, "Songbook") != NULL);
   assert(strstr(buf, "Concept") != NULL);
   assert(strstr(buf, "Structure") != NULL);
   assert(strstr(buf, "Hook") != NULL);
   assert(strstr(buf, "Detected stack") == NULL);
   assert(strstr(buf, "Story Bible") == NULL);

   /* Idempotent. */
   assert(write_songbook_rules_file(dir) == 1);

   rmdir_r(dir);
}

static void test_write_aimee_rules_no_stack(void)
{
   char *dir = make_tmpdir();

   int rc = write_aimee_rules_file(dir, NULL, 0);
   assert(rc == 0);

   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/.aimee-rules", dir);
   FILE *f = fopen(path, "r");
   assert(f != NULL);
   char buf[1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';

   assert(strstr(buf, "none detected") != NULL);

   rmdir_r(dir);
}

static void test_ensure_mcp_json_rewrites_server_command(void)
{
   char tmpdir[PATH_MAX];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-mcp-json-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char home_dir[PATH_MAX];
   char bin_dir[PATH_MAX];
   char workspace_dir[PATH_MAX];
   char fake_aimee_client[PATH_MAX];
   char mcp_path[PATH_MAX];
   snprintf(home_dir, sizeof(home_dir), "%s/home", tmpdir);
   snprintf(bin_dir, sizeof(bin_dir), "%s/.local/bin", home_dir);
   snprintf(workspace_dir, sizeof(workspace_dir), "%s/workspace", tmpdir);
   snprintf(fake_aimee_client, sizeof(fake_aimee_client), "%s/aimee", bin_dir);
   snprintf(mcp_path, sizeof(mcp_path), "%s/.mcp.json", workspace_dir);

   char cmd[PATH_MAX * 2];
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s'", bin_dir, workspace_dir);
   assert(system(cmd) == 0);

   FILE *fp = fopen(fake_aimee_client, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\n", fp);
   fclose(fp);

   fp = fopen(mcp_path, "w");
   assert(fp != NULL);
   fputs("{\n"
         "  \"mcpServers\": {\n"
         "    \"aimee\": {\n"
         "      \"command\": \"aimee-server\",\n"
         "      \"args\": [\"mcp-serve\"]\n"
         "    }\n"
         "  }\n"
         "}\n",
         fp);
   fclose(fp);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", home_dir);

   ensure_mcp_json(workspace_dir);

   fp = fopen(mcp_path, "r");
   assert(fp != NULL);
   char buf[2048];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   assert(strstr(buf, fake_aimee_client) != NULL);
   assert(strstr(buf, "\"args\": [\"mcp-serve\"]") != NULL);
   assert(strstr(buf, "\"command\": \"aimee-server\"") == NULL);
   assert(strstr(buf, "\"command\": \"aimee\"") == NULL);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }

   rmdir_r(tmpdir);
}

/* --- Bootstrap banner helpers --- */

/* Simulates the webchat bootstrap flow: detect stacks, check for missing rules,
 * write rules if absent. Mirrors what the /api/chat/bootstrap-status and
 * /api/chat/init-rules endpoints do. */
static void test_webchat_bootstrap_no_rules(void)
{
   char *dir = make_tmpdir();
   touch(dir, "Cargo.toml");

   /* Workspace has no .aimee-rules yet */
   char rules_path[PATH_MAX];
   snprintf(rules_path, sizeof(rules_path), "%s/.aimee-rules", dir);
   assert(access(rules_path, F_OK) != 0);

   /* Detect stacks (as bootstrap-status would) */
   stack_info_t stacks[MAX_DETECTED_STACKS];
   int n = detect_project_stacks(dir, stacks, MAX_DETECTED_STACKS);
   assert(n == 1);
   assert(strcmp(stacks[0].name, "Rust") == 0);

   /* Generate rules (as init-rules would) */
   int rc = write_aimee_rules_file(dir, stacks, n);
   assert(rc == 0);
   assert(access(rules_path, F_OK) == 0);

   /* Calling again is idempotent */
   rc = write_aimee_rules_file(dir, stacks, n);
   assert(rc == 1);

   rmdir_r(dir);
}

static void test_webchat_bootstrap_rules_exist(void)
{
   char *dir = make_tmpdir();

   /* Pre-create .aimee-rules */
   char rules_path[PATH_MAX];
   snprintf(rules_path, sizeof(rules_path), "%s/.aimee-rules", dir);
   FILE *f = fopen(rules_path, "w");
   assert(f != NULL);
   fputs("# existing rules\n", f);
   fclose(f);

   /* has_rules check: file exists, no stacks needed */
   assert(access(rules_path, F_OK) == 0);

   /* write_aimee_rules_file returns 1 (already exists) */
   int rc = write_aimee_rules_file(dir, NULL, 0);
   assert(rc == 1);

   /* Content is preserved */
   f = fopen(rules_path, "r");
   assert(f != NULL);
   char buf[64];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   assert(strcmp(buf, "# existing rules\n") == 0);

   rmdir_r(dir);
}

int main(void)
{
   printf("cmd_core: ");

   test_handle_git_status_in_repo();
   test_handle_git_commit_missing_message();
   test_handle_git_branch_missing_action();
   test_handle_git_branch_missing_name();
   test_handle_git_clone_missing_url();
   test_handle_git_pr_missing_action();
   test_handle_git_pr_create_missing_title();
   test_handle_git_commit_in_repo();
   test_subcmd_dispatch_unknown();
   test_subcmd_usage_no_crash();
   test_detect_stacks_rust();
   test_detect_stacks_go();
   test_detect_stacks_typescript();
   test_detect_stacks_node();
   test_detect_stacks_python();
   test_detect_stacks_c();
   test_detect_stacks_cmake();
   test_detect_stacks_none();
   test_write_aimee_rules_basic();
   test_write_aimee_rules_idempotent();
   test_write_novel_rules_basic();
   test_write_songbook_rules_basic();
   test_write_aimee_rules_no_stack();
   test_ensure_mcp_json_rewrites_server_command();
   test_webchat_bootstrap_no_rules();
   test_webchat_bootstrap_rules_exist();

   printf("all tests passed\n");
   return 0;
}
