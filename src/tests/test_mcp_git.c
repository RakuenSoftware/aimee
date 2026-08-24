/* test_mcp_git.c: MCP git tool handler tests for mcp_git_* modules. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "modules/git/mcp_git.h"
#include "session_worktree_key.h"
#include "modules/workspace/workspace_provider.h"
#include "modules/git/git_verify.h"
#include "branch_ownership.h"
#include "tests/support/git_pr_api_stub.h"
#include "cJSON.h"
#include "db1_client/db1.h"
#include "db1_client/git_ownership.h" /* ownership_get_owner -- the per-repo claim assertion */
#include "../modules/db2/c/db2.h"
#include "../modules/db2/c/db2_internal.h"
#include "support/git_module_fixture.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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

static char g_tmpdir[256];
static char g_saved_cwd[4096];
static void verify_test_setup_repo(char *tmpdir, size_t tmpdir_len, const char *prefix);
static void verify_test_write_yaml(const char *tmpdir, char *fake_home, size_t fake_home_len,
                                   const char *yaml);
static void verify_test_teardown(const char *tmpdir, const char *fake_home);
static void setup_ownership_db(void);
static void teardown_ownership_db(void);
static void init_nested_git_repo(const char *path, const char *label);

static void setup_git_repo(void)
{
   snprintf(g_tmpdir, sizeof g_tmpdir, "%s/aimee-test-mcp-git-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo 'hello world' > file.txt && "
            "git add file.txt && git commit -q -m 'initial commit'",
            g_tmpdir);
   assert(system(cmd) == 0);

   assert(getcwd(g_saved_cwd, sizeof(g_saved_cwd)) != NULL);
   assert(chdir(g_tmpdir) == 0);
}

static void teardown_git_repo(void)
{
   assert(chdir(g_saved_cwd) == 0);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
   system(cmd);
}

/* --- Test handle_git_status in a clean repo --- */

static void test_git_status_clean(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_status(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "branch:") != NULL);
   assert(strstr(text, "clean") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Test handle_git_status with modifications --- */

static void test_git_status_modified(void)
{
   setup_git_repo();

   FILE *fp = fopen("file.txt", "w");
   assert(fp != NULL);
   fputs("modified content\n", fp);
   fclose(fp);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_status(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "modified") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_mcp_chdir_uses_cwd_argument(void)
{
   setup_git_repo();

   char repo[sizeof(g_tmpdir)];
   snprintf(repo, sizeof(repo), "%s", g_tmpdir);
   assert(chdir("/tmp") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "cwd", repo);
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   const char *resolved = run_cmd_get_cwd();
   assert(resolved != NULL);
   assert(strcmp(resolved, repo) == 0);

   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   teardown_git_repo();
}

static void test_mcp_chdir_session_cwd_precedes_proxy_cwd(void)
{
   setup_git_repo();

   char proxy_repo[sizeof(g_tmpdir)];
   snprintf(proxy_repo, sizeof(proxy_repo), "%s", g_tmpdir);

   char tracked_repo[256];
   snprintf(tracked_repo, sizeof tracked_repo, "%s/aimee-test-mcp-git-tracked-XXXXXX",
            platform_tmpdir());
   assert(mkdtemp(tracked_repo) != NULL);
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "cd '%s' && git init -q", tracked_repo);
   assert(system(cmd) == 0);

   session_id_set_override("test-session-cwd");

   char cwd_path[MAX_PATH_LEN];
   snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(), session_id());
   FILE *fp = fopen(cwd_path, "w");
   assert(fp != NULL);
   fputs(tracked_repo, fp);
   fclose(fp);

   assert(chdir("/tmp") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "cwd", proxy_repo);
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   const char *resolved = run_cmd_get_cwd();
   assert(resolved != NULL);
   assert(strcmp(resolved, tracked_repo) == 0);

   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   unlink(cwd_path);
   session_id_clear_override();

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tracked_repo);
   system(cmd);
   teardown_git_repo();
}

static void test_explicit_path_is_authoritative_and_live(void)
{
   setup_git_repo();
   setup_ownership_db();

   assert(system("printf 'requested\\n' >> file.txt && git commit -q -am requested-head") == 0);

   char stale[MAX_PATH_LEN];
   snprintf(stale, sizeof(stale), "/tmp/aimee-test-mcp-git-stale-%d", (int)getpid());
   init_nested_git_repo(stale, "stale-head");

   const char *sid = "explicit-path-session";
   session_id_set_override(sid);
   session_state_t state;
   memset(&state, 0, sizeof(state));
   snprintf(state.session_mode, sizeof(state.session_mode), "implement");
   snprintf(state.guardrail_mode, sizeof(state.guardrail_mode), "approve");
   state.worktree_count = 1;
   snprintf(state.worktrees[0].git_root, sizeof(state.worktrees[0].git_root), "%s", g_tmpdir);
   snprintf(state.worktrees[0].worktree_path, sizeof(state.worktrees[0].worktree_path), "%s",
            stale);
   session_state_force_save(&state, sid);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "path", g_tmpdir);
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   cJSON *resp = handle_git_log(args);
   char *result = get_mcp_text(resp);
   assert(result != NULL);
   assert(strstr(result, "requested-head") != NULL);
   assert(strstr(result, "stale-head") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Mutations must use the same authoritative path.  This is the direct
    * regression for branch=create claiming success in an invisible scratch
    * checkout while leaving the named repository untouched. */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "path", g_tmpdir);
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "path-created");
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   resp = handle_git_branch(args);
   result = get_mcp_text(resp);
   assert(result != NULL);
   assert(strstr(result, "created: path-created") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(system("git show-ref --verify --quiet refs/heads/path-created") == 0);

   char stale_check[MAX_PATH_LEN + 96];
   snprintf(stale_check, sizeof(stale_check),
            "git -C '%s' show-ref --verify --quiet refs/heads/path-created", stale);
   assert(system(stale_check) != 0);

   /* Delete the stale mapped checkout, then change the requested checkout.
    * The second call must read the new state live rather than replaying a
    * cached response or holding the stale worktree provider. */
   char cmd[MAX_PATH_LEN + 32];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", stale);
   assert(system(cmd) == 0);
   FILE *fp = fopen("live-only.txt", "w");
   assert(fp != NULL);
   fputs("live\n", fp);
   fclose(fp);

   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "path", g_tmpdir);
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   resp = handle_git_status(args);
   result = get_mcp_text(resp);
   assert(result != NULL);
   assert(strstr(result, "live-only.txt") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "path", "/tmp/aimee-path-that-does-not-exist");
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == -2);
   cJSON_Delete(args);

   run_cmd_set_cwd(NULL);
   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void init_nested_git_repo(const char *path, const char *label)
{
   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "mkdir -p '%s' && git -C '%s' init -q && "
            "git -C '%s' config user.email test@test && "
            "git -C '%s' config user.name test && "
            "echo %s > '%s/file.txt' && "
            "git -C '%s' add file.txt && git -C '%s' commit -q -m %s",
            path, path, path, path, label, path, path, path, label);
   assert(system(cmd) == 0);
}

static void test_mcp_chdir_repairs_stale_delegate_tracked_cwd(void)
{
   setup_git_repo();

   char session_main[MAX_PATH_LEN];
   char stale_delegate[MAX_PATH_LEN];
   /* Derive the key rather than hard-coding it. The session key is a hash of the
    * full id now, not its first 16 characters — a literal path here silently
    * stopped matching what the resolver computes. */
   char skey[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key("102ee97d-session", skey, sizeof(skey));
   snprintf(session_main, sizeof(session_main), "%s/.aimee/worktrees/%s/main", g_tmpdir, skey);
   snprintf(stale_delegate, sizeof(stale_delegate), "%s/.aimee/worktrees/deleg-24/37368447",
            g_tmpdir);

   init_nested_git_repo(session_main, "main");
   init_nested_git_repo(stale_delegate, "stale");

   session_id_set_override("102ee97d-session");

   char cwd_path[MAX_PATH_LEN];
   snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(), session_id());
   FILE *fp = fopen(cwd_path, "w");
   assert(fp != NULL);
   fputs(stale_delegate, fp);
   fclose(fp);

   assert(chdir("/tmp") == 0);

   cJSON *args = cJSON_CreateObject();
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   const char *resolved = run_cmd_get_cwd();
   assert(resolved != NULL);
   assert(strcmp(resolved, session_main) == 0);

   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   unlink(cwd_path);
   session_id_clear_override();
   teardown_git_repo();
}

static void test_mcp_chdir_keeps_stale_delegate_cwd_when_repair_missing(void)
{
   setup_git_repo();

   char stale_delegate[MAX_PATH_LEN];
   snprintf(stale_delegate, sizeof(stale_delegate), "%s/.aimee/worktrees/deleg-24/37368447",
            g_tmpdir);
   init_nested_git_repo(stale_delegate, "stale");

   session_id_set_override("102ee97d-session");

   char cwd_path[MAX_PATH_LEN];
   snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(), session_id());
   FILE *fp = fopen(cwd_path, "w");
   assert(fp != NULL);
   fputs(stale_delegate, fp);
   fclose(fp);

   assert(chdir("/tmp") == 0);

   cJSON *args = cJSON_CreateObject();
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   const char *resolved = run_cmd_get_cwd();
   assert(resolved != NULL);
   assert(strcmp(resolved, stale_delegate) == 0);

   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   unlink(cwd_path);
   session_id_clear_override();
   teardown_git_repo();
}

static void test_mcp_chdir_does_not_repair_delegate_session_cwd(void)
{
   setup_git_repo();

   char session_main[MAX_PATH_LEN];
   char stale_delegate[MAX_PATH_LEN];
   snprintf(session_main, sizeof(session_main), "%s/.aimee/worktrees/deleg-abc/main", g_tmpdir);
   snprintf(stale_delegate, sizeof(stale_delegate), "%s/.aimee/worktrees/deleg-24/37368447",
            g_tmpdir);
   init_nested_git_repo(session_main, "main");
   init_nested_git_repo(stale_delegate, "stale");

   session_id_set_override("deleg-abcdef");

   char cwd_path[MAX_PATH_LEN];
   snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(), session_id());
   FILE *fp = fopen(cwd_path, "w");
   assert(fp != NULL);
   fputs(stale_delegate, fp);
   fclose(fp);

   assert(chdir("/tmp") == 0);

   cJSON *args = cJSON_CreateObject();
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   const char *resolved = run_cmd_get_cwd();
   assert(resolved != NULL);
   assert(strcmp(resolved, stale_delegate) == 0);

   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   unlink(cwd_path);
   session_id_clear_override();
   teardown_git_repo();
}

static void test_mcp_chdir_keeps_explicit_managed_worktree_despite_stale_session_state(void)
{
   setup_git_repo();
   setup_ownership_db();

   char active_worktree[MAX_PATH_LEN];
   char stale_worktree[MAX_PATH_LEN];
   char akey[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key("102ee97d-session", akey, sizeof(akey));
   snprintf(active_worktree, sizeof(active_worktree), "%s/.aimee/worktrees/%s/main", g_tmpdir,
            akey);
   snprintf(stale_worktree, sizeof(stale_worktree), "%s/.aimee/worktrees/6ab82f0e-session/main",
            g_tmpdir);
   init_nested_git_repo(active_worktree, "active");
   init_nested_git_repo(stale_worktree, "stale");

   session_id_set_override("6ab82f0e-session");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   snprintf(state.session_mode, sizeof(state.session_mode), "implement");
   snprintf(state.guardrail_mode, sizeof(state.guardrail_mode), "approve");
   state.worktree_count = 1;
   snprintf(state.worktrees[0].git_root, sizeof(state.worktrees[0].git_root), "%s", g_tmpdir);
   snprintf(state.worktrees[0].worktree_path, sizeof(state.worktrees[0].worktree_path), "%s",
            stale_worktree);
   session_state_force_save(&state, session_id());

   assert(chdir("/tmp") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "cwd", active_worktree);
   char *mismatch_err = NULL;
   assert(mcp_chdir_git_root(NULL, 0, args, &mismatch_err) == 1);
   const char *resolved = run_cmd_get_cwd();
   assert(resolved != NULL);
   assert(strcmp(resolved, active_worktree) == 0);
   assert(mismatch_err == NULL);

   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_mcp_chdir_uses_pwd_fallback(void)
{
   setup_git_repo();

   char repo[sizeof(g_tmpdir)];
   snprintf(repo, sizeof(repo), "%s", g_tmpdir);
   char old_pwd[4096] = "";
   const char *pwd = getenv("PWD");
   if (pwd)
      snprintf(old_pwd, sizeof(old_pwd), "%s", pwd);

   assert(chdir("/tmp") == 0);
   assert(setenv("PWD", repo, 1) == 0);

   cJSON *args = cJSON_CreateObject();
   assert(mcp_chdir_git_root(NULL, 0, args, NULL) == 1);
   const char *resolved = run_cmd_get_cwd();
   assert(resolved != NULL);
   assert(strcmp(resolved, repo) == 0);

   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   if (old_pwd[0])
      assert(setenv("PWD", old_pwd, 1) == 0);
   else
      unsetenv("PWD");
   teardown_git_repo();
}

/* --- Test handle_git_commit parameter validation --- */

static void test_git_commit_missing_message(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "message") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Empty message */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "");
   resp = handle_git_commit(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_commit in repo --- */

static void test_git_commit_success(void)
{
   setup_git_repo();

   /* Must be on a feature branch — commits on main are blocked */
   system("git checkout -q -b test-feature");

   FILE *fp = fopen("new.txt", "w");
   fputs("new file\n", fp);
   fclose(fp);
   system("git add new.txt");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "add new file");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   assert(strstr(text, "add new file") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_git_commit_reads_masked_global_identity(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b global-identity-test && "
                 "git config --unset-all user.name && git config --unset-all user.email") == 0);

   char fake_home[256];
   snprintf(fake_home, sizeof fake_home, "%s/aimee-test-git-identity-XXXXXX", platform_tmpdir());
   assert(mkdtemp(fake_home) != NULL);
   const char *env_names[] = {"HOME",
                              "XDG_CONFIG_HOME",
                              "GIT_CONFIG_NOSYSTEM",
                              "GIT_CONFIG_SYSTEM",
                              "GIT_CONFIG_GLOBAL",
                              "AIMEE_TEST_GIT_IDENTITY_FROM_CONFIG"};
   char *saved[sizeof(env_names) / sizeof(env_names[0])] = {0};
   for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); i++)
   {
      const char *value = getenv(env_names[i]);
      saved[i] = value ? strdup(value) : NULL;
   }

   setenv("HOME", fake_home, 1);
   setenv("XDG_CONFIG_HOME", fake_home, 1);
   unsetenv("GIT_CONFIG_NOSYSTEM");
   unsetenv("GIT_CONFIG_SYSTEM");
   unsetenv("GIT_CONFIG_GLOBAL");
   assert(system("git config --global user.name 'Global Operator' && "
                 "git config --global user.email 'global@example.test'") == 0);
   setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
   setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);
   setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
   setenv("AIMEE_TEST_GIT_IDENTITY_FROM_CONFIG", "1", 1);

   FILE *fp = fopen("global-author.txt", "w");
   assert(fp != NULL);
   fputs("identity\n", fp);
   fclose(fp);
   assert(system("git add global-author.txt") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "use global identity");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(system("test \"$(git log -1 --format='%an <%ae>')\" = "
                 "'Global Operator <global@example.test>'") == 0);

   for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); i++)
   {
      if (saved[i])
      {
         setenv(env_names[i], saved[i], 1);
         free(saved[i]);
      }
      else
      {
         unsetenv(env_names[i]);
      }
   }
   char clean[320];
   snprintf(clean, sizeof(clean), "rm -rf '%s'", fake_home);
   assert(system(clean) == 0);
   teardown_git_repo();
}

/* --- Test handle_git_commit with sensitive file filtering --- */

static void test_git_commit_skips_sensitive(void)
{
   setup_git_repo();

   /* Must be on a feature branch — commits on main are blocked */
   system("git checkout -q -b test-sensitive");

   FILE *fp = fopen("normal.txt", "w");
   fputs("normal\n", fp);
   fclose(fp);

   fp = fopen(".env", "w");
   fputs("SECRET=xyz\n", fp);
   fclose(fp);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "test sensitive");
   cJSON *files = cJSON_CreateArray();
   cJSON_AddItemToArray(files, cJSON_CreateString("normal.txt"));
   cJSON_AddItemToArray(files, cJSON_CreateString(".env"));
   cJSON_AddItemToObject(args, "files", files);

   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* A CONTAINER-sandboxed delegate runs `--network none` on a minimal image with no
 * git binary and no forge credential, so aimee's git tooling must execute on the
 * SERVER against the path-identity bind-mounted worktree — never route into the
 * container. Regression for the delegate-sandbox E2E where every git_* call died
 * with "git: command not found" because mcp_git_run dispatched through the container
 * provider's exec_shell. The spy mimics that no-git failure: if git were still
 * routed into the container the commit would fail and the spy would be hit. */
static int g_container_shell_spy_called;
static char *container_shell_spy(const workspace_provider_t *p, const char *cmd, int *exit_code)
{
   (void)p;
   (void)cmd;
   g_container_shell_spy_called = 1;
   if (exit_code)
      *exit_code = 127; /* as if `git` were absent from the sandbox image */
   return strdup("bash: git: command not found\n");
}

static void test_git_container_provider_runs_on_server(void)
{
   setup_git_repo();
   system("git checkout -q -b test-sandbox");

   FILE *fp = fopen("sbx.txt", "w");
   fputs("sandbox\n", fp);
   fclose(fp);
   system("git add sbx.txt");

   workspace_provider_t container;
   memset(&container, 0, sizeof(container));
   container.kind = WS_PROVIDER_CONTAINER;
   container.exec_shell = container_shell_spy;
   workspace_provider_set_active(&container);
   g_container_shell_spy_called = 0;

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "sandbox commit");
   cJSON *resp = handle_git_commit(args);
   workspace_provider_clear_active();

   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* git ran on the server (the commit really landed), NOT through the container
    * spy — so a no-git sandbox image no longer blocks a delegate's commit. */
   assert(g_container_shell_spy_called == 0);
   assert(strstr(text, "committed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
   printf("  PASS: test_git_container_provider_runs_on_server\n");
}

/* A MIRROR workspace is registered under the CLIENT's path; aimee-server serves it by
 * rebuilding its own worktree from a bare mirror plus the client's diff, and replaces
 * that rebuild whenever the client's tree changes. A commit made there is therefore
 * discarded — and it USED to be reported as a success, returning a SHA that no
 * reachable checkout contained while the client still held every file uncommitted.
 * These pin the refusal, per operation, so the silence cannot come back. */
static void test_mirror_workspace_refuses_local_history_writes(void)
{
   setup_git_repo();
   system("git checkout -q -b mirror-durability");

   workspace_provider_t mirror;
   memset(&mirror, 0, sizeof(mirror));
   mirror.kind = WS_PROVIDER_MIRROR;
   mirror.exec_shell = container_shell_spy; /* must never be reached */
   workspace_provider_set_active(&mirror);
   g_container_shell_spy_called = 0;

   /* commit: the operation that lost work. */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "would be discarded");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "reconstruction") != NULL);
   /* Refused BEFORE running anything: no shell, and nothing reported as committed. */
   assert(strstr(text, "committed") == NULL);
   assert(g_container_shell_spy_called == 0);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* The same rule for every other local-history write, including the ones that reach
    * it by delegation (checkout -> restore, switch -> branch). */
   struct
   {
      const char *label;
      cJSON *(*fn)(cJSON *);
      const char *action;
   } cases[] = {
       {"reset", handle_git_reset, NULL},
       {"add", handle_git_add, NULL},
       {"restore", handle_git_restore, NULL},
       {"merge", handle_git_merge, NULL},
       {"rebase", handle_git_rebase, NULL},
       {"sync", handle_git_sync, NULL},
       {"cherry_pick", handle_git_cherry_pick, NULL},
       {"revert", handle_git_revert, NULL},
       {"pull", handle_git_pull, NULL},
       {"stash", handle_git_stash, "push"},
       {"branch", handle_git_branch, "create"},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      cJSON *a = cJSON_CreateObject();
      if (cases[i].action)
         cJSON_AddStringToObject(a, "action", cases[i].action);
      cJSON_AddStringToObject(a, "name", "some-branch");
      cJSON_AddStringToObject(a, "ref", "HEAD~1");
      cJSON_AddBoolToObject(a, "all", 1);
      cJSON *r = cases[i].fn(a);
      char *t = get_mcp_text(r);
      assert(t != NULL);
      if (!strstr(t, "reconstruction"))
      {
         fprintf(stderr, "mirror durability: %s was not refused: %s\n", cases[i].label, t);
         assert(0);
      }
      cJSON_Delete(r);
      cJSON_Delete(a);
   }
   assert(g_container_shell_spy_called == 0);

   /* READS stay available: the rebuild is faithful, which is the whole reason the
    * write path looked like it worked. */
   cJSON *ls = cJSON_CreateObject();
   cJSON_AddStringToObject(ls, "action", "list");
   cJSON *ls_resp = handle_git_branch(ls);
   char *ls_text = get_mcp_text(ls_resp);
   assert(ls_text != NULL);
   assert(strstr(ls_text, "reconstruction") == NULL);
   cJSON_Delete(ls_resp);
   cJSON_Delete(ls);

   workspace_provider_clear_active();

   /* And with no mirror active the same call is allowed through — the guard keys on
    * the provider, not on the operation being inherently unsafe. */
   cJSON *ok_args = cJSON_CreateObject();
   cJSON_AddStringToObject(ok_args, "message", "lands for real");
   cJSON *ok_resp = handle_git_commit(ok_args);
   char *ok_text = get_mcp_text(ok_resp);
   assert(ok_text != NULL);
   assert(strstr(ok_text, "reconstruction") == NULL);
   cJSON_Delete(ok_resp);
   cJSON_Delete(ok_args);

   teardown_git_repo();
   printf("  PASS: test_mirror_workspace_refuses_local_history_writes\n");
}

/* --- Test handle_git_push in non-git directory --- */

static void test_git_push_requires_branch(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-test-push-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);

   char saved[4096];
   assert(getcwd(saved, sizeof(saved)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_push(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved) == 0);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test handle_git_branch parameter validation --- */

static void test_git_branch_missing_action(void)
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

static void test_git_branch_create_and_list(void)
{
   setup_git_repo();

   /* List branches */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "list");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strlen(text) > 0);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Create a branch */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "test-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "created") != NULL);
   assert(strstr(text, "test-branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Switch back to original branch */
   system("git checkout -q master 2>/dev/null || git checkout -q main 2>/dev/null");

   /* Delete the branch */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "delete");
   cJSON_AddStringToObject(args, "name", "test-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "deleted") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* A repository can carry a hostile/broken remote.<name>.fetch configuration.
 * The MCP fetch operation must override it with a remote-tracking refspec rather
 * than trusting it to choose which local refs -- and which prune namespace --
 * are writable. This reproduces the incident configuration exactly. */
static void test_git_fetch_never_writes_or_prunes_local_branches(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-test-safe-fetch-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root) != NULL);
   char remote[512], seed[512], local[512], cmd[4096];
   snprintf(remote, sizeof(remote), "%s/remote.git", root);
   snprintf(seed, sizeof(seed), "%s/seed", root);
   snprintf(local, sizeof(local), "%s/local", root);

   snprintf(cmd, sizeof(cmd),
            "git init -q --bare '%s' && git init -q -b main '%s' && "
            "git -C '%s' config user.email test@test && git -C '%s' config user.name test && "
            "printf 'base\\n' > '%s/file.txt' && git -C '%s' add file.txt && "
            "git -C '%s' commit -q -m base && git -C '%s' remote add origin '%s' && "
            "git -C '%s' push -q -u origin main && "
            "git -C '%s' checkout -q -b feature && printf 'feature\\n' > '%s/feature.txt' && "
            "git -C '%s' add feature.txt && git -C '%s' commit -q -m feature && "
            "git -C '%s' push -q -u origin feature && "
            "git --git-dir='%s' symbolic-ref HEAD refs/heads/main && git clone -q '%s' '%s'",
            remote, seed, seed, seed, seed, seed, seed, seed, remote, seed, seed, seed, seed, seed,
            seed, remote, remote, local);
   assert(system(cmd) == 0);

   /* Advance origin/main after the clone. A fetch that falls back to the bad
    * configured destination would now try to update refs/heads/main and fail
    * because main is checked out. */
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' checkout -q main && printf 'advanced\\n' >> '%s/file.txt' && "
            "git -C '%s' commit -q -am advanced && git -C '%s' tag fetch-side-effect && "
            "git -C '%s' push -q origin main refs/tags/fetch-side-effect",
            seed, seed, seed, seed, seed);
   assert(system(cmd) == 0);

   char saved[4096];
   assert(getcwd(saved, sizeof(saved)) != NULL);
   assert(chdir(local) == 0);
   assert(system("git config user.email test@test && git config user.name test && "
                 "git config --replace-all remote.origin.fetch "
                 "'+refs/heads/*:refs/heads/*' && "
                 "git config remote.origin.pruneTags true && git tag local-keep-tag") == 0);

   int local_rc = 0;
   char *local_main_before = mcp_git_run("git rev-parse refs/heads/main", &local_rc);
   assert(local_rc == 0);
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse refs/heads/main", seed);
   int expected_rc = 0;
   char *expected_main = run_cmd(cmd, &expected_rc);
   assert(local_main_before != NULL && expected_main != NULL && expected_rc == 0);
   assert(strcmp(local_main_before, expected_main) != 0);

   /* The unsafe configured refspec would try to update checked-out main and
    * fail. The explicit safe refspec must make this fetch succeed. */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "prune", 1);
   cJSON *resp = handle_git_fetch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL && strncmp(text, "error:", 6) != 0);
   char *local_main_after = mcp_git_run("git rev-parse refs/heads/main", &local_rc);
   assert(local_rc == 0);
   char *remote_main_after = mcp_git_run("git rev-parse refs/remotes/origin/main", &local_rc);
   assert(local_rc == 0);
   char *configured_refspec = mcp_git_run("git config --get remote.origin.fetch", &local_rc);
   assert(local_rc == 0);
   assert(local_main_after != NULL && remote_main_after != NULL && configured_refspec != NULL);
   assert(strcmp(local_main_before, local_main_after) == 0);
   assert(strcmp(expected_main, remote_main_after) == 0);
   assert(strcmp(configured_refspec, "+refs/heads/*:refs/heads/*\n") == 0);
   assert(system("! git show-ref --verify --quiet refs/tags/fetch-side-effect") == 0);
   assert(system("git show-ref --verify --quiet refs/tags/local-keep-tag") == 0);
   free(local_main_before);
   free(local_main_after);
   free(remote_main_after);
   free(expected_main);
   free(configured_refspec);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(system("git checkout -q -b local-only && git branch keep-local && "
                 "git update-ref refs/remotes/origin/stale HEAD && "
                 "printf 'dirty\\n' >> file.txt && printf 'staged\\n' > staged.txt && "
                 "git add staged.txt && printf 'untracked\\n' > untracked.txt") == 0);

   char *head_before = mcp_git_run("git rev-parse HEAD", &local_rc);
   char *status_before = mcp_git_run("git status --porcelain=v1 --untracked-files=all", &local_rc);
   char *refs_before = mcp_git_run(
       "git for-each-ref --sort=refname --format='%(refname) %(objectname)' refs/heads", &local_rc);
   assert(head_before != NULL && status_before != NULL && refs_before != NULL);

   args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "prune", 1);
   resp = handle_git_fetch(args);
   text = get_mcp_text(resp);
   assert(text != NULL && strncmp(text, "error:", 6) != 0);

   char *head_after = mcp_git_run("git rev-parse HEAD", &local_rc);
   char *status_after = mcp_git_run("git status --porcelain=v1 --untracked-files=all", &local_rc);
   char *refs_after = mcp_git_run(
       "git for-each-ref --sort=refname --format='%(refname) %(objectname)' refs/heads", &local_rc);
   assert(head_after != NULL && status_after != NULL && refs_after != NULL);
   assert(strcmp(head_before, head_after) == 0);
   assert(strcmp(status_before, status_after) == 0);
   assert(strcmp(refs_before, refs_after) == 0);
   free(head_before);
   free(head_after);
   free(status_before);
   free(status_after);
   free(refs_before);
   free(refs_after);

   assert(system("test \"$(git symbolic-ref --short HEAD)\" = local-only && "
                 "git show-ref --verify --quiet refs/heads/local-only && "
                 "git show-ref --verify --quiet refs/heads/keep-local && "
                 "git show-ref --verify --quiet refs/remotes/origin/main && "
                 "git show-ref --verify --quiet refs/remotes/origin/feature && "
                 "! git show-ref --verify --quiet refs/remotes/origin/stale") == 0);

   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(chdir(saved) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
   assert(system(cmd) == 0);
}

static void test_git_fetch_refuses_unborn_head(void)
{
   setup_git_repo();
   assert(system("git checkout -q --orphan unborn && git rm -q -rf .") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_fetch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "HEAD does not resolve") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_git_fetch_rejects_unsafe_remote_name(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "remote", "-overwrite");
   cJSON *resp = handle_git_fetch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "remote name is invalid") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* Note: handle_git_log is not directly tested here because its internal
 * format string contains git-format % placeholders that conflict with
 * snprintf's format parsing. This is tested indirectly through
 * test_mcp_server and test_cmd_core. */

/* --- Test handle_git_clone parameter validation --- */

static void test_git_clone_missing_url(void)
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

/* --- Test handle_git_stash parameter validation --- */

static void test_git_stash_unknown_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "bogus");
   cJSON *resp = handle_git_stash(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test session-aware stash pop --- */

static void test_git_stash_session_aware_pop(void)
{
   setup_git_repo();

   /* Create a stash tagged with a different session ID */
   assert(system("echo 'other session change' > other.txt && "
                 "git add other.txt && "
                 "git stash push -m 'aimee-autostash-deadbeef' 2>/dev/null") == 0);

   /* Create a stash tagged with our session ID */
   char stash_cmd[512];
   snprintf(stash_cmd, sizeof(stash_cmd),
            "echo 'my change' > mine.txt && "
            "git add mine.txt && "
            "git stash push -m 'aimee-autostash-%.8s' 2>/dev/null",
            session_id());
   assert(system(stash_cmd) == 0);

   /* Create another stash from a third session on top */
   assert(system("echo 'third session' > third.txt && "
                 "git add third.txt && "
                 "git stash push -m 'aimee-autostash-cafebabe' 2>/dev/null") == 0);

   /* Pop should find and pop our session's stash, not the top one */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "pop");
   cJSON *resp = handle_git_stash(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Verify our file was restored, not the other sessions' files */
   struct stat st;
   assert(stat("mine.txt", &st) == 0);
   assert(stat("third.txt", &st) != 0); /* should NOT exist */
   assert(stat("other.txt", &st) != 0); /* should NOT exist */

   teardown_git_repo();
}

/* --- Test handle_git_pr parameter validation --- */

static void test_git_pr_missing_action(void)
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

static void test_git_pr_unknown_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "nonexistent");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_pr_create_missing_title(void)
{
   /* Run in an isolated temp repo so merged-PR and verify gates don't
    * interfere with the missing-title error we're testing for. */
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-test-pr-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[512];
   /* A github origin and an explicit base: this test is about the TITLE, and create
    * now checks the cheap preconditions first (origin, then base), so a fixture missing
    * either would fail before reaching the title path this test exists to pin. */
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo x > f.txt && "
            "git add f.txt && git commit -q -m 'init' && "
            "git remote add origin https://github.com/example/repo.git",
            tmpdir);
   assert(system(cmd) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "base", "master");
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

/* create must resolve the repository through the workspace runner and refuse
 * cleanly when there is no github.com origin.
 *
 * #2386 instead let the API resolve `origin` in aimee-server's own process. That
 * looks identical on a co-located checkout and fails on every remote one -- a
 * DETACHED workspace keeps the filesystem on the client, so the server saw no
 * such path and reported "no origin remote" for repositories that have one. This
 * pins the refusal to the runner-resolved path: the message is about THIS
 * checkout's origin, and it arrives before any API call. */
static void test_git_pr_create_without_github_origin(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-test-pr-noorigin-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);

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
   cJSON_AddStringToObject(args, "title", "a title");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   /* Not "no origin remote": that string is the API's in-process lookup, which
    * this path must no longer reach. */
   assert(strstr(text, "origin") != NULL);
   assert(strstr(text, "no origin remote") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_git_pr_edit_missing_number(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "edit");
   cJSON_AddStringToObject(args, "title", "new title");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "number") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_pr_edit_requires_fields(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "edit");
   cJSON_AddNumberToObject(args, "number", 13);
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "title/body/base") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_pr_checks_missing_number(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "checks");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "number") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_pr_wait_missing_number(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "checks");
   cJSON_AddBoolToObject(args, "wait", 1);
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "number") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_pr_wait_is_rejected_without_running_gh(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-test-gh-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);

   char gh_path[512];
   snprintf(gh_path, sizeof(gh_path), "%s/gh", tmpdir);
   FILE *fp = fopen(gh_path, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\nexit 99\n", fp);
   fclose(fp);
   assert(chmod(gh_path, 0700) == 0);

   const char *old_path = getenv("PATH");
   char saved_path[4096] = "";
   if (old_path)
      snprintf(saved_path, sizeof(saved_path), "%s", old_path);

   char new_path[8192];
   snprintf(new_path, sizeof(new_path), "%s%s%s", tmpdir, old_path ? ":" : "",
            old_path ? old_path : "");
   assert(setenv("PATH", new_path, 1) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "checks");
   cJSON_AddNumberToObject(args, "number", 123);
   cJSON_AddBoolToObject(args, "wait", 1);
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "blocking PR check waits are disabled") != NULL);
   assert(strstr(text, "poll") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   if (saved_path[0])
      assert(setenv("PATH", saved_path, 1) == 0);
   else
      unsetenv("PATH");
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_git_pr_auto_merge_accepts_pending_checks_without_claiming_merge(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-test-gh-auto-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);

   char gh_path[512];
   snprintf(gh_path, sizeof(gh_path), "%s/gh", tmpdir);
   FILE *fp = fopen(gh_path, "w");
   assert(fp != NULL);
   /* Only `gh pr merge --auto` is faked now. The CI gate no longer shells out --
    * it reads the Checks API in-process -- so a faked `gh pr checks` would never
    * be consulted; the verdict comes from the stub below instead. */
   fputs("#!/bin/sh\n"
         "if [ \"$1\" = pr ] && [ \"$2\" = merge ]; then exit 0; fi\n"
         "exit 2\n",
         fp);
   fclose(fp);
   assert(chmod(gh_path, 0700) == 0);

   const char *old_path = getenv("PATH");
   char saved_path[4096] = "";
   if (old_path)
      snprintf(saved_path, sizeof(saved_path), "%s", old_path);
   char new_path[8192];
   snprintf(new_path, sizeof(new_path), "%s%s%s", tmpdir, old_path ? ":" : "",
            old_path ? old_path : "");
   assert(setenv("PATH", new_path, 1) == 0);

   /* This test owns its checkout. It used to run in whatever directory the
    * previous test left behind, which meant it only passed where that happened
    * to be a clone whose origin is a github.com URL -- true in a developer's
    * working copy, false in a checkout cloned from a bundle or a mirror. The
    * merge path resolves the origin slug BEFORE it reaches auto-merge, so the
    * ambient case returned "cannot resolve a github.com origin" and the --auto
    * assertion below failed for a reason that had nothing to do with the
    * behaviour under test. A test must carry its own preconditions. */
   char repo[320];
   snprintf(repo, sizeof(repo), "%s/repo", tmpdir);
   char setup[1024];
   snprintf(setup, sizeof(setup),
            "mkdir -p '%s' && cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo x > f.txt && git add f.txt && "
            "git commit -q -m init && "
            "git remote add origin https://github.com/example/repo.git",
            repo, repo);
   assert(system(setup) == 0);
   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(repo) == 0);

   /* Pending CI is the whole point of auto-merge: branch protection holds the PR
    * until green, so the gate must NOT refuse it here. Without this exemption an
    * auto-merge into a protected branch would refuse itself. */
   git_pr_api_stub_set_ci(GIT_PR_CI_PENDING);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "merge");
   cJSON_AddNumberToObject(args, "number", 123);
   cJSON_AddBoolToObject(args, "auto", 1);
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "--auto") != NULL);
   assert(strstr(text, "\"auto_merge_enabled\":true") != NULL);
   assert(strstr(text, "\"merged\":false") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* The exemption is for PENDING alone. An unreadable forge is not "pending", and
    * "unknown" is never "pass" -- auto or not, that must still refuse. */
   git_pr_api_stub_set_ci(GIT_PR_CI_ERROR);
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "merge");
   cJSON_AddNumberToObject(args, "number", 123);
   cJSON_AddBoolToObject(args, "auto", 1);
   resp = handle_git_pr(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "merge blocked") != NULL);
   assert(strstr(text, "--auto") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* A failed check refuses even with auto, for the same reason. */
   git_pr_api_stub_set_ci(GIT_PR_CI_FAILURE);
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "merge");
   cJSON_AddNumberToObject(args, "number", 123);
   cJSON_AddBoolToObject(args, "auto", 1);
   resp = handle_git_pr(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "merge blocked") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);
   git_pr_api_stub_set_ci(GIT_PR_CI_ERROR); /* leave the fail-closed default in place */

   if (saved_path[0])
      assert(setenv("PATH", saved_path, 1) == 0);
   else
      unsetenv("PATH");
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test handle_git_issue --- */

static void test_git_issue_list_defaults(void)
{
   /* No action — defaults to "list". gh will fail in CI (no gh auth),
    * but we just need the handler to reach gh, not succeed. */
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_issue(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* Either ran gh (any output) or got a gh error — both are valid here */
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_issue_invalid_state(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "list");
   cJSON_AddStringToObject(args, "state", "bogus");
   cJSON *resp = handle_git_issue(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "state") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_issue_unknown_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON *resp = handle_git_issue(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_diff_summary in repo --- */

static void test_git_diff_no_changes(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_diff_summary(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "no changes") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Test handle_git_verify --- */

static void test_git_verify(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 0); /* force sync for test assertions */
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* Should show some output (success or no-config message) */
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_git_verify_creates_state_in_repo_root(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-root");

   char fake_home[256];
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: build\n"
                          "      run: echo ok\n");

   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/subdir", tmpdir);
   assert(mkdir(subdir, 0755) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(subdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 0); /* force sync for test assertions */
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "verified") != NULL);
   assert(strstr(text, "warning: could not record verify state") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   char state_path[512];
   struct stat st;
   snprintf(state_path, sizeof(state_path), "%s/.aimee/.last-verify", tmpdir);
   assert(stat(state_path, &st) == 0);

   snprintf(state_path, sizeof(state_path), "%s/subdir/.aimee/.last-verify", tmpdir);
   assert(stat(state_path, &st) != 0);

   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

static void test_git_verify_dirty_tree_ignores_cached_pass(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-dirty");

   char exclude_cmd[512];
   snprintf(exclude_cmd, sizeof(exclude_cmd), "printf '.aimee/\\n' >> '%s/.git/info/exclude'",
            tmpdir);
   assert(system(exclude_cmd) == 0);

   char counter_path[512];
   snprintf(counter_path, sizeof(counter_path), "%s-counter", tmpdir);

   char yaml[1024];
   snprintf(yaml, sizeof(yaml),
            "verify:\n"
            "  enforce: true\n"
            "  steps:\n"
            "    - name: marker\n"
            "      run: sh -c 'printf x >> %s'\n",
            counter_path);

   char fake_home[256];
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home), yaml);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 0);
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "PASS (cached)") == NULL);
   assert(strstr(text, "verified") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   struct stat st;
   assert(stat(counter_path, &st) == 0);
   assert(st.st_size == 1);

   char state_path[512];
   snprintf(state_path, sizeof(state_path), "%s/.aimee/.last-verify", tmpdir);
   FILE *state_file = fopen(state_path, "r");
   assert(state_file != NULL);
   char state_before[512];
   size_t state_before_len = fread(state_before, 1, sizeof(state_before) - 1, state_file);
   assert(!ferror(state_file));
   state_before[state_before_len] = '\0';
   fclose(state_file);

   FILE *dirty_file = fopen("f", "a");
   assert(dirty_file != NULL);
   fputs("dirty\n", dirty_file);
   fclose(dirty_file);
   sleep(1);

   args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 0);
   resp = handle_git_verify(NULL, args, NULL);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "warning: uncommitted changes") != NULL);
   assert(strstr(text, "PASS (cached)") == NULL);
   assert(strstr(text, "verification state not recorded") != NULL);
   assert(strstr(text, "all 1 steps passed -- verified") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(stat(counter_path, &st) == 0);
   assert(st.st_size == 2);

   state_file = fopen(state_path, "r");
   assert(state_file != NULL);
   char state_after[512];
   size_t state_after_len = fread(state_after, 1, sizeof(state_after) - 1, state_file);
   assert(!ferror(state_file));
   state_after[state_after_len] = '\0';
   fclose(state_file);
   assert(state_before_len == state_after_len);
   assert(strcmp(state_before, state_after) == 0);

   remove(counter_path);
   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

/* --- Test verify_load_config: nested format with enforce flag ---
 *
 * verify_load_config reads ~/.config/aimee/projects/<basename>/project.yaml,
 * keyed by the basename of the project's main repo root. Tests override HOME
 * so the global path resolves under a sandbox dir, then write the test YAML
 * there. The basename used for keying matches the test's tmpdir basename
 * because the test tmpdir is itself a fresh git repo (not a worktree). */

static char g_verify_saved_home[4096];
static int g_verify_home_was_set;
static int g_verify_home_saved;
static char g_verify_saved_aimee_home[4096];
static int g_verify_aimee_home_was_set;
static int g_verify_aimee_home_saved;
static char g_verify_saved_aimee_profile[4096];
static int g_verify_aimee_profile_was_set;
static int g_verify_aimee_profile_saved;
static char g_verify_saved_path[4096];
static int g_verify_path_was_set;
static int g_verify_path_saved;

static void verify_test_setup_repo(char *tmpdir, size_t tmpdir_len, const char *prefix)
{
   snprintf(tmpdir, tmpdir_len, "%s/%s-XXXXXX", platform_tmpdir(), prefix);
   assert(mkdtemp(tmpdir) != NULL);
   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init",
            tmpdir);
   assert(system(cmd) == 0);
}

static void verify_test_set_fake_home(char *fake_home, size_t fake_home_len)
{
   const char *old = getenv("HOME");
   g_verify_home_saved = 1;
   if (old)
   {
      snprintf(g_verify_saved_home, sizeof(g_verify_saved_home), "%s", old);
      g_verify_home_was_set = 1;
   }
   else
   {
      g_verify_home_was_set = 0;
   }

   old = getenv("AIMEE_HOME");
   g_verify_aimee_home_saved = 1;
   if (old)
   {
      snprintf(g_verify_saved_aimee_home, sizeof(g_verify_saved_aimee_home), "%s", old);
      g_verify_aimee_home_was_set = 1;
   }
   else
   {
      g_verify_aimee_home_was_set = 0;
   }

   old = getenv("AIMEE_PROFILE");
   g_verify_aimee_profile_saved = 1;
   if (old)
   {
      snprintf(g_verify_saved_aimee_profile, sizeof(g_verify_saved_aimee_profile), "%s", old);
      g_verify_aimee_profile_was_set = 1;
   }
   else
   {
      g_verify_aimee_profile_was_set = 0;
   }

   snprintf(fake_home, fake_home_len, "%s/aimee-test-home-XXXXXX", platform_tmpdir());
   assert(mkdtemp(fake_home) != NULL);
   setenv("HOME", fake_home, 1);
   unsetenv("AIMEE_HOME");
   unsetenv("AIMEE_PROFILE");
}

static void verify_test_set_fake_path(char *fake_bin_dir, size_t fake_bin_dir_len)
{
   const char *old = getenv("PATH");
   g_verify_path_saved = 1;
   if (old)
   {
      snprintf(g_verify_saved_path, sizeof(g_verify_saved_path), "%s", old);
      g_verify_path_was_set = 1;
   }
   else
   {
      g_verify_path_was_set = 0;
   }

   snprintf(fake_bin_dir, fake_bin_dir_len, "%s/aimee-test-bin-XXXXXX", platform_tmpdir());
   assert(mkdtemp(fake_bin_dir) != NULL);
   {
      char new_path[8192];
      snprintf(new_path, sizeof(new_path), "%s:/usr/bin:/bin", fake_bin_dir);
      setenv("PATH", new_path, 1);
   }
}

static void verify_test_write_fake_gh(const char *fake_bin_dir, const char *body)
{
   char gh_path[1024];
   snprintf(gh_path, sizeof(gh_path), "%s/gh", fake_bin_dir);
   FILE *f = fopen(gh_path, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n", f);
   fputs(body, f);
   fclose(f);
   assert(chmod(gh_path, 0755) == 0);
}

static void verify_test_write_fake_git(const char *fake_bin_dir, const char *body)
{
   char git_path[1024];
   snprintf(git_path, sizeof(git_path), "%s/git", fake_bin_dir);
   FILE *f = fopen(git_path, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n", f);
   fputs(body, f);
   fclose(f);
   assert(chmod(git_path, 0755) == 0);
}

static void verify_test_write_yaml(const char *tmpdir, char *fake_home, size_t fake_home_len,
                                   const char *yaml)
{
   verify_test_set_fake_home(fake_home, fake_home_len);

   const char *base = strrchr(tmpdir, '/');
   base = base ? base + 1 : tmpdir;

   char dir[1024], path[1024], cmd[2048];
   snprintf(dir, sizeof(dir), "%s/.config/aimee/projects/%s", fake_home, base);
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
   assert(system(cmd) == 0);

   snprintf(path, sizeof(path), "%s/project.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(yaml, f);
   fclose(f);
}

static void verify_test_teardown(const char *tmpdir, const char *fake_home)
{
   char cmd[2048];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", tmpdir, fake_home);
   system(cmd);
   if (g_verify_home_saved)
   {
      if (g_verify_home_was_set)
         setenv("HOME", g_verify_saved_home, 1);
      else
         unsetenv("HOME");
      g_verify_home_saved = 0;
   }
   if (g_verify_aimee_home_saved)
   {
      if (g_verify_aimee_home_was_set)
         setenv("AIMEE_HOME", g_verify_saved_aimee_home, 1);
      else
         unsetenv("AIMEE_HOME");
      g_verify_aimee_home_saved = 0;
   }
   if (g_verify_aimee_profile_saved)
   {
      if (g_verify_aimee_profile_was_set)
         setenv("AIMEE_PROFILE", g_verify_saved_aimee_profile, 1);
      else
         unsetenv("AIMEE_PROFILE");
      g_verify_aimee_profile_saved = 0;
   }
   if (g_verify_path_saved)
   {
      if (g_verify_path_was_set)
         setenv("PATH", g_verify_saved_path, 1);
      else
         unsetenv("PATH");
      g_verify_path_saved = 0;
   }
}

static void test_verify_load_config_enforce_true(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: build\n"
                          "      run: echo ok\n");

   verify_config_t cfg;
   int rc = verify_load_config(tmpdir, &cfg);
   assert(rc == 0);
   assert(cfg.enforce == 1);
   assert(cfg.count == 1);
   assert(strcmp(cfg.steps[0].name, "build") == 0);
   assert(strcmp(cfg.steps[0].run, "echo ok") == 0);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_enforce_false(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify2");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: false\n"
                          "  steps:\n"
                          "    - name: lint\n"
                          "      run: echo lint\n");

   verify_config_t cfg;
   int rc = verify_load_config(tmpdir, &cfg);
   assert(rc == 0);
   assert(cfg.enforce == 0);
   assert(cfg.count == 1);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_no_enforce_defaults_false(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify3");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  steps:\n"
                          "    - name: build\n"
                          "      run: make\n");

   verify_config_t cfg;
   int rc = verify_load_config(tmpdir, &cfg);
   assert(rc == 0);
   assert(cfg.enforce == 0); /* defaults to false when not specified */
   assert(cfg.count == 1);

   verify_test_teardown(tmpdir, fake_home);
}

static void verify_test_add_go_module(const char *tmpdir, const char *module)
{
   char dir[512], path[512];
   if (strcmp(module, ".") == 0)
      snprintf(dir, sizeof(dir), "%s", tmpdir);
   else
   {
      snprintf(dir, sizeof(dir), "%s/%s", tmpdir, module);
      assert(mkdir(dir, 0700) == 0);
   }
   snprintf(path, sizeof(path), "%s/go.mod", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("module example.invalid/test\n\ngo 1.25\n", f);
   fclose(f);
}

static void test_verify_load_config_emits_parallel_steps(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-multi");
   verify_test_set_fake_home(fake_home, sizeof(fake_home));

   char makefile_path[512];
   snprintf(makefile_path, sizeof(makefile_path), "%s/Makefile", tmpdir);
   FILE *f = fopen(makefile_path, "w");
   assert(f != NULL);
   fputs(".PHONY: verify-local lint all unit-tests build-integrity\n"
         "verify-local:\n"
         "\t@echo verify-local\n"
         "lint:\n"
         "\t@echo lint\n"
         "all:\n"
         "\t@echo build\n"
         "unit-tests:\n"
         "\t@echo tests\n",
         f);
   fputs("build-integrity:\n"
         "\t@echo build-integrity\n",
         f);
   fclose(f);
   verify_test_add_go_module(tmpdir, "server-go");

   verify_config_t cfg;
   int rc = verify_load_config(tmpdir, &cfg);
   assert(rc == 0);
   /* verify-local is the repo's curated fast local gate; prefer it over
    * auto-splitting Makefile targets and pulling in heavier CI checks. Go
    * modules remain separate so an older curated target cannot hide them. */
   assert(cfg.count == 2);
   assert(strcmp(cfg.steps[0].name, "verify-local") == 0);
   assert(strstr(cfg.steps[0].run, "AIMEE_VERIFY_MAKE_JOBS") != NULL);
   /* Builds can use $(nproc), while tests use a bounded per-verifier default so
    * concurrent workflow verification cannot multiply into host-wide overload. */
   assert(strstr(cfg.steps[0].run, "nproc") != NULL);
   assert(strstr(cfg.steps[0].run, "AIMEE_VERIFY_TEST_JOBS") != NULL);
   assert(strcmp(cfg.steps[1].name, "go-test-server-go") == 0);
   assert(strstr(cfg.steps[1].run, "cd server-go") != NULL);
   assert(strstr(cfg.steps[1].run, "/usr/local/go/bin/go") != NULL);
   assert(strstr(cfg.steps[1].run, "unset AIMEE_WFE_ENGINE AIMEE_WFE_HTTP_SOCKET") != NULL);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_repairs_existing_generated_plan_with_go_modules(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-existing-go");
   verify_test_add_go_module(tmpdir, "zeta-go");
   verify_test_add_go_module(tmpdir, ".");
   verify_test_add_go_module(tmpdir, "alpha-go");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "# Auto-generated by aimee on first verify. Edit freely —\n"
                          "verify:\n"
                          "  enforce: false\n"
                          "  steps:\n"
                          "    - name: verify-local\n"
                          "      run: cd src && make -j${AIMEE_VERIFY_MAKE_JOBS:-$(nproc)} "
                          "AIMEE_VERIFY_TEST_JOBS=${AIMEE_VERIFY_TEST_JOBS:-2} verify-local\n");

   verify_config_t cfg;
   assert(verify_load_config(tmpdir, &cfg) == 0);
   assert(cfg.count == 4);
   assert(strcmp(cfg.steps[0].name, "verify-local") == 0);
   assert(strcmp(cfg.steps[1].name, "go-test-root") == 0);
   assert(strcmp(cfg.steps[2].name, "go-test-alpha-go") == 0);
   assert(strcmp(cfg.steps[3].name, "go-test-zeta-go") == 0);
   for (int i = 1; i < cfg.count; i++)
      assert(strstr(cfg.steps[i].run, "unset AIMEE_WFE_ENGINE AIMEE_WFE_HTTP_SOCKET") != NULL);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_leaves_custom_plan_unchanged(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-custom-go");
   verify_test_add_go_module(tmpdir, "server-go");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: verify-local\n"
                          "      run: ./scripts/project-specific-verify\n");

   verify_config_t cfg;
   assert(verify_load_config(tmpdir, &cfg) == 0);
   assert(cfg.count == 1);
   assert(strcmp(cfg.steps[0].run, "./scripts/project-specific-verify") == 0);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_discovers_go_modules_from_cwd(void)
{
   char tmpdir[256], fake_home[256], saved_cwd[4096];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-cwd-go");
   verify_test_add_go_module(tmpdir, "server-go");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: false\n"
                          "  steps:\n"
                          "    - name: verify-local\n"
                          "      run: make -j${AIMEE_VERIFY_MAKE_JOBS:-2} verify-local\n");
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   verify_config_t cfg;
   assert(verify_load_config(NULL, &cfg) == 0);
   assert(cfg.count == 2);
   assert(strcmp(cfg.steps[1].name, "go-test-server-go") == 0);

   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_collapses_generated_pipeline_to_verify_local(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-generated-fast");

   char makefile_path[512];
   snprintf(makefile_path, sizeof(makefile_path), "%s/Makefile", tmpdir);
   FILE *f = fopen(makefile_path, "w");
   assert(f != NULL);
   fputs(".PHONY: verify-local lint all unit-tests build-integrity\n"
         "verify-local:\n"
         "\t@echo verify-local\n",
         f);
   fclose(f);

   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: lint\n"
                          "      run: make -j$(nproc | awk '{print ($1>8)?8:$1}') lint\n"
                          "    - name: build\n"
                          "      run: make -j$(nproc | awk '{print ($1>8)?8:$1}') all\n"
                          "    - name: unit-tests\n"
                          "      run: make -j$(nproc | awk '{print ($1>8)?8:$1}') unit-tests\n"
                          "      after: build\n"
                          "    - name: build-integrity\n"
                          "      run: make -j$(nproc | awk '{print ($1>8)?8:$1}') build-integrity\n"
                          "      after: unit-tests\n");

   verify_config_t cfg;
   assert(verify_load_config(tmpdir, &cfg) == 0);
   assert(cfg.enforce == 1);
   assert(cfg.count == 1);
   assert(strcmp(cfg.steps[0].name, "verify-local") == 0);
   assert(strstr(cfg.steps[0].run, "verify-local") != NULL);
   /* The stale multi-step `$(nproc | awk ...)` form is NOT carried over; the
    * collapsed step uses the clean AIMEE_VERIFY_* defaults. */
   assert(strstr(cfg.steps[0].run, "awk") == NULL);
   assert(strstr(cfg.steps[0].run, "AIMEE_VERIFY_MAKE_JOBS") != NULL);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_prefers_check_linking_for_build(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-checklink");
   verify_test_set_fake_home(fake_home, sizeof(fake_home));

   char makefile_path[512];
   snprintf(makefile_path, sizeof(makefile_path), "%s/Makefile", tmpdir);
   FILE *f = fopen(makefile_path, "w");
   assert(f != NULL);
   fputs(".PHONY: lint all check-linking unit-tests\n"
         "lint:\n"
         "\t@echo lint\n"
         "all:\n"
         "\t@echo build\n"
         "check-linking:\n"
         "\t@echo check-linking\n"
         "unit-tests:\n"
         "\t@echo tests\n",
         f);
   fclose(f);

   verify_config_t cfg;
   int rc = verify_load_config(tmpdir, &cfg);
   assert(rc == 0);
   assert(cfg.count == 3);
   assert(strcmp(cfg.steps[1].name, "build") == 0);
   /* When both `all` and `check-linking` exist, prefer `check-linking`
    * for the build step so every shippable binary gets linked. */
   assert(strstr(cfg.steps[1].run, "check-linking") != NULL);
   assert(strstr(cfg.steps[1].run, " all") == NULL);

   verify_test_teardown(tmpdir, fake_home);
}

/* A small repository may expose only `make test`. The generated test step must
 * not depend on a build step that was never emitted, or the wave scheduler can
 * never submit it and reports exit -1 before running the repository's tests. */
static void test_verify_load_config_test_only_has_no_missing_build_dependency(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-test-only");
   verify_test_set_fake_home(fake_home, sizeof(fake_home));

   char makefile_path[512];
   snprintf(makefile_path, sizeof(makefile_path), "%s/Makefile", tmpdir);
   FILE *f = fopen(makefile_path, "w");
   assert(f != NULL);
   fputs(".PHONY: test\n"
         "test:\n"
         "\t@echo tests\n",
         f);
   fclose(f);

   verify_config_t cfg;
   assert(verify_load_config(tmpdir, &cfg) == 0);
   assert(cfg.count == 1);
   assert(strcmp(cfg.steps[0].name, "test") == 0);
   assert(cfg.steps[0].after[0] == '\0');

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_normalizes_build_integrity_order(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-order");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: lint\n"
                          "      run: make lint\n"
                          "    - name: build\n"
                          "      run: make all\n"
                          "    - name: unit-tests\n"
                          "      run: make unit-tests\n"
                          "      after: build\n"
                          "    - name: build-integrity\n"
                          "      run: make build-integrity\n"
                          "      after: build\n");

   verify_config_t cfg;
   assert(verify_load_config(tmpdir, &cfg) == 0);
   assert(cfg.count == 4);
   assert(strcmp(cfg.steps[3].name, "build-integrity") == 0);
   assert(strcmp(cfg.steps[3].after, "unit-tests") == 0);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_falls_back_to_verify_local(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-fallback");
   verify_test_set_fake_home(fake_home, sizeof(fake_home));

   char makefile_path[512];
   snprintf(makefile_path, sizeof(makefile_path), "%s/Makefile", tmpdir);
   FILE *f = fopen(makefile_path, "w");
   assert(f != NULL);
   /* Repo defines a verify-local target but no recognisable
    * parallelisable targets; generator must fall back to it. */
   fputs(".PHONY: verify-local\n"
         "verify-local:\n"
         "\t@echo verify-local\n",
         f);
   fclose(f);

   verify_config_t cfg;
   int rc = verify_load_config(tmpdir, &cfg);
   assert(rc == 0);
   assert(cfg.count == 1);
   assert(strcmp(cfg.steps[0].name, "verify-local") == 0);
   assert(strcmp(cfg.steps[0].run,
                 "make -j${AIMEE_VERIFY_MAKE_JOBS:-$(nproc 2>/dev/null || echo 4)} "
                 "AIMEE_VERIFY_TEST_JOBS=${AIMEE_VERIFY_TEST_JOBS:-2} "
                 "verify-local") == 0);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_load_config_old_flat_format_ignored(void)
{
   char tmpdir[256], fake_home[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify4");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  - name: build\n"
                          "    run: make\n");

   verify_config_t cfg;
   int rc = verify_load_config(tmpdir, &cfg);
   /* Old flat format produces no steps and no enforce → returns -1 (no gate) */
   assert(rc == -1);
   assert(cfg.count == 0);
   assert(cfg.enforce == 0);

   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_prepare_pr_blocks_branch_with_merged_pr(void)
{
   char tmpdir[256], fake_home[256], fake_bin_dir[256];
   const char fake_gh_script[] = "if [ \"$1\" = \"pr\" ] && [ \"$2\" = \"list\" ]; then\n"
                                 "  printf '[{\"number\":537,\"headRefOid\":\"merged-head\"}]\\n'\n"
                                 "  exit 0\n"
                                 "fi\n"
                                 "exit 1\n";
   const char fake_git_script[] =
       "if [ \"$1\" = \"rev-parse\" ] && [ \"$2\" = \"--abbrev-ref\" ] && "
       "[ \"$3\" = \"HEAD\" ]; then\n"
       "  printf 'feature-reused\\n'\n"
       "  exit 0\n"
       "fi\n"
       "if [ \"$1\" = \"rev-parse\" ] && [ \"$2\" = \"HEAD\" ]; then\n"
       "  printf 'merged-head\\n'\n"
       "  exit 0\n"
       "fi\n"
       "exit 1\n";
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-merged-pr");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: false\n"
                          "  steps:\n"
                          "    - name: verify-local\n"
                          "      run: echo ok\n");
   verify_test_set_fake_path(fake_bin_dir, sizeof(fake_bin_dir));
   verify_test_write_fake_gh(fake_bin_dir, fake_gh_script);
   verify_test_write_fake_git(fake_bin_dir, fake_git_script);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "prepare-pr");
   cJSON_AddStringToObject(args, "base", "main");
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "Branch Reuse: BLOCKED") != NULL);
   assert(strstr(text, "already has a merged PR") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);

   {
      char cmd[1024];
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fake_bin_dir);
      system(cmd);
   }
   verify_test_teardown(tmpdir, fake_home);
}

static void test_verify_prepare_pr_allows_reused_branch_with_new_head(void)
{
   char tmpdir[256], fake_home[256], fake_bin_dir[256];
   const char fake_gh_script[] = "if [ \"$1\" = \"pr\" ] && [ \"$2\" = \"list\" ]; then\n"
                                 "  printf '[{\"number\":537,\"headRefOid\":\"merged-head\"}]\\n'\n"
                                 "  exit 0\n"
                                 "fi\n"
                                 "exit 1\n";
   const char fake_git_script[] =
       "if [ \"$1\" = \"rev-parse\" ] && [ \"$2\" = \"--abbrev-ref\" ] && "
       "[ \"$3\" = \"HEAD\" ]; then\n"
       "  printf 'testing\\n'\n"
       "  exit 0\n"
       "fi\n"
       "if [ \"$1\" = \"rev-parse\" ] && [ \"$2\" = \"HEAD\" ]; then\n"
       "  printf 'new-testing-head\\n'\n"
       "  exit 0\n"
       "fi\n"
       "exit 1\n";
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-reused-pr-new-head");
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: false\n"
                          "  steps:\n"
                          "    - name: verify-local\n"
                          "      run: echo ok\n");
   verify_test_set_fake_path(fake_bin_dir, sizeof(fake_bin_dir));
   verify_test_write_fake_gh(fake_bin_dir, fake_gh_script);
   verify_test_write_fake_git(fake_bin_dir, fake_git_script);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "prepare-pr");
   cJSON_AddStringToObject(args, "base", "main");
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "Branch Reuse: BLOCKED") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);

   {
      char cmd[1024];
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fake_bin_dir);
      system(cmd);
   }
   verify_test_teardown(tmpdir, fake_home);
}

/* --- Test verify gate enforcement in push/PR --- */

/* Helper: create an isolated git repo on a non-main branch with no upstream.
 * The repo has no .aimee/project.yaml so verify gates don't apply. */
static char g_verifytest_dir[256];
static char g_verifytest_saved_cwd[4096];

static void setup_feature_branch_repo(void)
{
   snprintf(g_verifytest_dir, sizeof g_verifytest_dir, "%s/aimee-test-push-XXXXXX",
            platform_tmpdir());
   assert(mkdtemp(g_verifytest_dir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f.txt && "
            "git add f.txt && git commit -q -m init && "
            "git checkout -q -b feature-branch",
            g_verifytest_dir);
   assert(system(cmd) == 0);

   assert(getcwd(g_verifytest_saved_cwd, sizeof(g_verifytest_saved_cwd)) != NULL);
   assert(chdir(g_verifytest_dir) == 0);
}

static void teardown_feature_branch_repo(void)
{
   chdir(g_verifytest_saved_cwd);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_verifytest_dir);
   system(cmd);
}

static void test_verify_gate_not_enforced_without_enforce_flag(void)
{
   /* With enforce: false in the global project.yaml, push should not be
    * blocked by the verify gate — it fails for a different reason (no
    * remote), not verify. */
   setup_feature_branch_repo();

   char fake_home[256];
   verify_test_write_yaml(g_verifytest_dir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: false\n"
                          "  steps:\n"
                          "    - name: build\n"
                          "      run: make\n");

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_push(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* Must NOT be blocked by verify gate specifically */
   assert(strstr(text, "push blocked") == NULL);
   assert(strstr(text, "verification required") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   char cleanup[2048];
   snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", fake_home);
   system(cleanup);
   if (g_verify_home_was_set)
      setenv("HOME", g_verify_saved_home, 1);
   else
      unsetenv("HOME");

   teardown_feature_branch_repo();
}

static void test_verify_gate_enforced_with_enforce_true_and_stale_verify(void)
{
   /* With enforce: true and no .last-verify record, push should be blocked
    * by the verify gate. */
   setup_feature_branch_repo();

   char fake_home[256];
   verify_test_write_yaml(g_verifytest_dir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: build\n"
                          "      run: make\n");

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_push(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* Must be blocked by the verify gate */
   assert(strstr(text, "push blocked") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   char cleanup[2048];
   snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", fake_home);
   system(cleanup);
   if (g_verify_home_was_set)
      setenv("HOME", g_verify_saved_home, 1);
   else
      unsetenv("HOME");

   teardown_feature_branch_repo();
}

/* --- Branch ownership tests --- */

/* Branch ownership is DB1-local. Each test gets a fresh in-memory DB1 so
 * branch_ownership starts empty. */

static void setup_ownership_db(void)
{
   db1_shutdown();
   assert(db1_init(":memory:") == 0);
}

static void teardown_ownership_db(void)
{
   db1_shutdown();
}

/* The feature branch a session's PRs target lives in DB1, keyed repo+session, for one
 * reason: the PR path runs on aimee-server, and a server serving a DETACHED or MIRROR
 * workspace cannot read anything under the checkout (that is #2386 -- server-side path
 * access against a client-held checkout, which broke every pr create). A file under
 * the worktree would resolve here and nowhere that matters. */
static void test_feature_branch_store_is_session_scoped(void)
{
   setup_git_repo();
   setup_ownership_db();
   session_id_set_override("session-A");

   char branch[256];
   /* Nothing recorded yet: 1, not an error, and not a guessed branch. The PR path
    * treats this as "use the repository default branch" rather than refusing. */
   assert(feature_branch_for_session(branch, sizeof(branch)) == 1);
   assert(branch[0] == '\0');

   assert(feature_branch_set("aimee/feat/operator-choice") == 0);
   assert(feature_branch_for_session(branch, sizeof(branch)) == 0);
   assert(strcmp(branch, "aimee/feat/operator-choice") == 0);

   /* Re-naming the feature replaces it rather than accumulating. */
   assert(feature_branch_set("aimee/feat/second-choice") == 0);
   assert(feature_branch_for_session(branch, sizeof(branch)) == 0);
   assert(strcmp(branch, "aimee/feat/second-choice") == 0);

   /* Another session in the same repo must not inherit it: two concurrent sessions on
    * different features would otherwise aim their PRs at each other's branch. */
   session_id_set_override("session-B");
   assert(feature_branch_for_session(branch, sizeof(branch)) == 1);
   assert(branch[0] == '\0');
   assert(feature_branch_set("aimee/feat/other-feature") == 0);
   assert(feature_branch_for_session(branch, sizeof(branch)) == 0);
   assert(strcmp(branch, "aimee/feat/other-feature") == 0);

   session_id_set_override("session-A");
   assert(feature_branch_for_session(branch, sizeof(branch)) == 0);
   assert(strcmp(branch, "aimee/feat/second-choice") == 0);

   /* Anything outside aimee/feat/<slug> is refused. This is caller-supplied input -- a
    * PR's `base` -- and whatever is stored is what a later PR in this session opens
    * against, so a bad value here retargets real PRs. */
   assert(feature_branch_set(NULL) == -1);
   assert(feature_branch_set("") == -1);
   assert(feature_branch_set("main") == -1);
   assert(feature_branch_set("aimee/feat/") == -1);
   assert(feature_branch_set("aimee/feat/../../main") == -1);
   assert(feature_branch_set("aimee/feat/a/b") == -1);
   assert(feature_branch_set("aimee/feat/Caps") == -1);
   assert(feature_branch_set("aimee/feat/-lead") == -1);
   assert(feature_branch_set("aimee/feat/trail-") == -1);
   assert(feature_branch_set("aimee/feat/has space") == -1);
   assert(feature_branch_set("aimee/feat/x\nmain") == -1);
   assert(feature_branch_set("refs/heads/aimee/feat/x") == -1);

   /* None of those may have displaced the real one. */
   assert(feature_branch_for_session(branch, sizeof(branch)) == 0);
   assert(strcmp(branch, "aimee/feat/second-choice") == 0);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_branch_create_registers_ownership(void)
{
   setup_git_repo();
   setup_ownership_db();
   session_id_set_override("session-A");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "test-branch");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "created: test-branch") != NULL);
   assert(strstr(text, "owner: session-A") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_commit_blocked_by_other_session_ownership(void)
{
   setup_git_repo();
   setup_ownership_db();

   /* Create branch as session-A */
   session_id_set_override("session-A");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "owned-branch");
   cJSON *resp = handle_git_branch(args);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Commit as session-B should be blocked */
   session_id_set_override("session-B");
   system("echo 'change' >> file.txt");
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "test commit");
   resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   assert(strstr(text, "owned by session") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Commit as session-A should succeed */
   session_id_set_override("session-A");
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "test commit");
   resp = handle_git_commit(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed:") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_push_blocked_by_other_session_ownership(void)
{
   setup_git_repo();
   setup_ownership_db();

   /* Create branch as session-A */
   session_id_set_override("session-A");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "push-branch");
   cJSON *resp = handle_git_branch(args);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Push as session-B should be blocked */
   session_id_set_override("session-B");
   args = cJSON_CreateObject();
   resp = handle_git_push(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   assert(strstr(text, "owned by session") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_branch_claim(void)
{
   setup_git_repo();
   setup_ownership_db();

   /* Claim unowned branch as session-A */
   session_id_set_override("session-A");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "claim");
   cJSON_AddStringToObject(args, "name", "main");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* main cannot be claimed */
   assert(strstr(text, "error: cannot claim main") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Claim a regular branch */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "claim");
   cJSON_AddStringToObject(args, "name", "some-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "claimed: some-branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Another session gets an actionable refusal, then may explicitly transfer
    * stale ownership rather than being trapped by the suggested claim. */
   session_id_set_override("session-B");
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "claim");
   cJSON_AddStringToObject(args, "name", "some-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "session-A") != NULL);
   assert(strstr(text, "force=true") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "claim");
   cJSON_AddStringToObject(args, "name", "some-branch");
   cJSON_AddBoolToObject(args, "force", 1);
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "claimed: some-branch") != NULL);
   char owned[256] = "";
   assert(branch_own_get_session_branch(owned, sizeof(owned)) == 0);
   assert(strcmp(owned, "some-branch") == 0);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* A non-owner cannot release silently, but force provides the documented
    * stale-record escape hatch. */
   session_id_set_override("session-A");
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "release");
   cJSON_AddStringToObject(args, "name", "some-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL && strstr(text, "force=true") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "release");
   cJSON_AddStringToObject(args, "name", "some-branch");
   cJSON_AddBoolToObject(args, "force", 1);
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL && strstr(text, "released ownership") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_set_override("session-C");
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "claim");
   cJSON_AddStringToObject(args, "name", "some-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL && strstr(text, "claimed: some-branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Ownership identity is (canonical repository, branch), not branch name
    * alone. The same name in another repository must remain independently
    * claimable without force. */
   char other_repo[MAX_PATH_LEN];
   snprintf(other_repo, sizeof(other_repo), "/tmp/aimee-test-owner-scope-%d", (int)getpid());
   init_nested_git_repo(other_repo, "other-repo");
   run_cmd_set_cwd(other_repo);
   session_id_set_override("session-D");
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "claim");
   cJSON_AddStringToObject(args, "name", "some-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL && strstr(text, "claimed: some-branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   char repo_owner[64] = "";
   char other_owner[64] = "";
   assert(db1_git_ownership_get_owner(g_tmpdir, "some-branch", repo_owner, sizeof(repo_owner)) ==
          1);
   assert(db1_git_ownership_get_owner(other_repo, "some-branch", other_owner,
                                      sizeof(other_owner)) == 1);
   assert(strcmp(repo_owner, "session-C") == 0);
   assert(strcmp(other_owner, "session-D") == 0);
   run_cmd_set_cwd(NULL);
   char clean[MAX_PATH_LEN + 32];
   snprintf(clean, sizeof(clean), "rm -rf '%s'", other_repo);
   assert(system(clean) == 0);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_main_branch_no_ownership(void)
{
   setup_git_repo();
   setup_ownership_db();

   /* session-A owns nothing — commits on main are now blocked by main branch protection */
   session_id_set_override("session-A");
   system("echo 'change' >> file.txt");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "commit on main");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   assert(strstr(text, "main branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

/* --- Main branch protection tests --- */

static void test_main_branch_commit_blocked(void)
{
   setup_git_repo();

   system("echo 'change' >> file.txt");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "should fail");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   assert(strstr(text, "main branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_main_branch_push_blocked(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_push(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   assert(strstr(text, "main branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_main_branch_reset_blocked(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "HEAD~1");
   cJSON_AddStringToObject(args, "mode", "soft");
   cJSON *resp = handle_git_reset(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_main_branch_delete_blocked(void)
{
   setup_git_repo();

   /* Switch away first so delete is theoretically possible */
   system("git checkout -q -b temp-branch");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "delete");
   cJSON_AddStringToObject(args, "name", "main");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- The integration operations: merge / rebase / cherry-pick / revert / sync --- */

/* Two branches with a commit each, both touching file.txt, so a merge of one into
 * the other conflicts. Leaves HEAD on `integration`. */
static void setup_conflicting_branches(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b side && echo side > file.txt && "
                 "git commit -q -am 'side change'") == 0);
   assert(system("git checkout -q -b integration HEAD~1 && echo mine > file.txt && "
                 "git commit -q -am 'my change'") == 0);
}

/* A clean merge reports what moved, not git's prose. */
static void test_merge_clean_reports_the_change(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b side && echo side > side.txt && "
                 "git add side.txt && git commit -q -m 'side commit'") == 0);
   assert(system("git checkout -q -b integration HEAD~1") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "side");
   cJSON *resp = handle_git_merge(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") == NULL);
   assert(strstr(text, "merge side into integration") != NULL);
   assert(strstr(text, "commit(s)") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(system("test -f side.txt") == 0);

   teardown_git_repo();
}

/* The default on conflict: name the conflicted files AND undo the merge, so the
 * caller is never left holding a tree it has to know how to clean up. */
static void test_merge_conflict_aborts_and_names_files(void)
{
   setup_conflicting_branches();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "side");
   cJSON *resp = handle_git_merge(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "conflict") != NULL);
   assert(strstr(text, "file.txt") != NULL);
   assert(strstr(text, "aborted") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* The tree really is clean: no MERGE_HEAD, no conflict markers. */
   assert(system("git rev-parse --verify --quiet MERGE_HEAD >/dev/null 2>&1") != 0);
   assert(system("git diff --name-only --diff-filter=U | grep -q .") != 0);
   assert(system("grep -q '<<<<<<<' file.txt") != 0);

   teardown_git_repo();
}

/* abort_on_conflict=false is the opt-in to resolving in place — and then
 * action=continue finishes it. */
static void test_merge_keep_conflicts_then_continue(void)
{
   setup_conflicting_branches();
   setup_ownership_db();
   session_id_set_override("session-A");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "side");
   cJSON_AddBoolToObject(args, "abort_on_conflict", 0);
   cJSON *resp = handle_git_merge(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "conflict") != NULL);
   assert(strstr(text, "left in progress") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(system("git rev-parse --verify --quiet MERGE_HEAD >/dev/null 2>&1") == 0);

   /* Continuing before resolving must refuse rather than commit the markers. */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "continue");
   resp = handle_git_merge(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "still have conflict markers") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(system("echo resolved > file.txt && git add file.txt") == 0);
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "continue");
   resp = handle_git_merge(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "merge completed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(system("git rev-parse --verify --quiet MERGE_HEAD >/dev/null 2>&1") != 0);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

/* action=abort backs the whole thing out. */
static void test_merge_action_abort(void)
{
   setup_conflicting_branches();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "side");
   cJSON_AddBoolToObject(args, "abort_on_conflict", 0);
   cJSON *resp = handle_git_merge(args);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "abort");
   resp = handle_git_merge(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "merge aborted") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(system("git rev-parse --verify --quiet MERGE_HEAD >/dev/null 2>&1") != 0);

   teardown_git_repo();
}

/* A half-finished operation is reported as such, with the way out, instead of
 * letting a second one start on top of it. */
static void test_second_operation_refused_while_one_is_in_progress(void)
{
   setup_conflicting_branches();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "side");
   cJSON_AddBoolToObject(args, "abort_on_conflict", 0);
   cJSON *resp = handle_git_merge(args);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "base", "side");
   resp = handle_git_rebase(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "merge is already in progress") != NULL);
   assert(strstr(text, "action=continue") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* Rebase writes commits on its FIRST run, not only when continuing after a conflict,
 * so it needs the operator identity then too. It used to get one only on
 * action=continue, and git has no ambient identity to fall back on (git_ops points
 * GIT_CONFIG_GLOBAL/SYSTEM at /dev/null), so a first-run rebase died on git's own
 * "Committer identity unknown ... unable to auto-detect email address". `pr ready`
 * hit this through sync, which is a rebase.
 *
 * With an identity present the rebase completes; with none, the answer is aimee's
 * actionable message rather than git's. */
static void test_rebase_carries_the_identity_on_first_run(void)
{
   setup_git_repo();

   /* Diverge, so the rebase must REPLAY a commit and therefore write one. */
   assert(system("git checkout -q -b behind-branch && echo mine > mine.txt && "
                 "git add mine.txt && git commit -q -m mine") == 0);
   assert(system("git checkout -q master") == 0);
   assert(system("echo theirs > theirs.txt && git add theirs.txt && "
                 "git commit -q -m theirs") == 0);
   assert(system("git checkout -q behind-branch") == 0);
   assert(system("git config --unset-all user.name && git config --unset-all user.email") == 0);

   /* The server's shape: an identity exists in the GLOBAL config, but git children
    * cannot see it because GIT_CONFIG_GLOBAL/SYSTEM are masked. Only aimee's resolver
    * can read it, so the operation works exactly when the flags are passed — which is
    * what makes this discriminating rather than a repeat of the fixture's own config. */
   char fake_home[256];
   snprintf(fake_home, sizeof fake_home, "%s/aimee-test-rebase-identity-XXXXXX", platform_tmpdir());
   assert(mkdtemp(fake_home) != NULL);
   const char *env_names[] = {"HOME",
                              "XDG_CONFIG_HOME",
                              "GIT_CONFIG_NOSYSTEM",
                              "GIT_CONFIG_SYSTEM",
                              "GIT_CONFIG_GLOBAL",
                              "AIMEE_TEST_GIT_IDENTITY_FROM_CONFIG"};
   char *saved[sizeof(env_names) / sizeof(env_names[0])] = {0};
   for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); i++)
   {
      const char *value = getenv(env_names[i]);
      saved[i] = value ? strdup(value) : NULL;
   }

   setenv("HOME", fake_home, 1);
   setenv("XDG_CONFIG_HOME", fake_home, 1);
   unsetenv("GIT_CONFIG_NOSYSTEM");
   unsetenv("GIT_CONFIG_SYSTEM");
   unsetenv("GIT_CONFIG_GLOBAL");
   assert(system("git config --global user.name 'Global Operator' && "
                 "git config --global user.email 'global@example.test'") == 0);
   setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
   setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);
   setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
   setenv("AIMEE_TEST_GIT_IDENTITY_FROM_CONFIG", "1", 1);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "base", "master");
   cJSON *resp = handle_git_rebase(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* Without the identity on the first run this is where git says "Committer identity
    * unknown ... unable to auto-detect email address". */
   assert(strstr(text, "identity") == NULL);
   assert(strstr(text, "error") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* The replayed commit carries the resolved operator, so the identity really was
    * applied rather than the rebase merely being a no-op. */
   assert(system("test \"$(git log -1 --format='%cn <%ce>')\" = "
                 "'Global Operator <global@example.test>'") == 0);

   for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); i++)
   {
      if (saved[i])
      {
         setenv(env_names[i], saved[i], 1);
         free(saved[i]);
      }
      else
         unsetenv(env_names[i]);
   }
   char rm[512];
   snprintf(rm, sizeof rm, "rm -rf '%s'", fake_home);
   (void)system(rm);

   teardown_git_repo();
}

/* An uncommitted tree cannot be cleanly restored after a conflict, so the
 * operation is refused before it starts rather than half-done. */
static void test_integrate_refuses_dirty_tree(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b work && echo dirty >> file.txt") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "master");
   cJSON *resp = handle_git_merge(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "uncommitted changes") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_integrate_blocked_on_main(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "HEAD");
   cJSON *resp = handle_git_merge(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "main branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_integrate_requires_a_ref(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b work") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_cherry_pick(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "cherry-pick") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* cherry-pick and revert run through the same driver, so one round-trip each is
 * enough to show the wiring is right. */
static void test_cherry_pick_and_revert(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b side && echo side > side.txt && "
                 "git add side.txt && git commit -q -m 'side commit'") == 0);
   char sha[64] = "";
   FILE *fp = popen("git rev-parse --short HEAD", "r");
   assert(fp != NULL);
   assert(fgets(sha, sizeof(sha), fp) != NULL);
   pclose(fp);
   sha[strcspn(sha, "\r\n")] = '\0';
   assert(system("git checkout -q -b picker HEAD~1") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", sha);
   cJSON *resp = handle_git_cherry_pick(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") == NULL);
   assert(system("test -f side.txt") == 0);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "HEAD");
   resp = handle_git_revert(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") == NULL);
   assert(system("test -f side.txt") != 0); /* the revert took it back out */
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* sync answers "am I current?" without a follow-up call. */
static void test_sync_reports_already_current(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b work") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "base", "master");
   cJSON *resp = handle_git_sync(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* No origin in this fixture, so the base resolves to the local ref and the
    * answer is the gap: zero behind. */
   assert(strstr(text, "already current") != NULL || strstr(text, "sync work") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_sync_rejects_unknown_mode(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b work") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "mode", "squash");
   cJSON *resp = handle_git_sync(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "mode must be rebase") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- git_add --- */

/* `all` stages new files (which git_commit cannot reach) but not secrets, and the
 * screen runs against the index so a pattern-based add cannot slip one past. */
static void test_add_all_stages_new_files_but_not_secrets(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b staging-test") == 0);

   FILE *fp = fopen(".env", "w");
   assert(fp != NULL);
   fputs("SECRET=xyz\n", fp);
   fclose(fp);
   fp = fopen("normal.txt", "w");
   assert(fp != NULL);
   fputs("normal\n", fp);
   fclose(fp);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "all", 1);
   cJSON *resp = handle_git_add(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "staged") != NULL);
   assert(strstr(text, ".env") != NULL); /* named in the unstaged warning */
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(system("git diff --cached --name-only | grep -qx normal.txt") == 0);
   assert(system("git diff --cached --name-only | grep -qx .env") != 0);

   teardown_git_repo();
}

static void test_add_refuses_only_sensitive_paths(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b staging-test") == 0);
   FILE *fp = fopen(".env", "w");
   assert(fp != NULL);
   fputs("SECRET=xyz\n", fp);
   fclose(fp);

   cJSON *args = cJSON_CreateObject();
   cJSON *files = cJSON_AddArrayToObject(args, "files");
   cJSON_AddItemToArray(files, cJSON_CreateString(".env"));
   cJSON *resp = handle_git_add(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "sensitive") != NULL);
   assert(strstr(text, "nothing was staged") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_add_allows_env_templates(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b env-template-test") == 0);

   FILE *fp = fopen(".env.example", "w");
   assert(fp != NULL);
   fputs("APP_PORT=3000\n", fp);
   fclose(fp);

   cJSON *args = cJSON_CreateObject();
   cJSON *files = cJSON_AddArrayToObject(args, "files");
   cJSON_AddItemToArray(files, cJSON_CreateString(".env.example"));
   cJSON *resp = handle_git_add(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "staged") != NULL);
   assert(strstr(text, "sensitive") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(system("git diff --cached --name-only | grep -qx .env.example") == 0);
   teardown_git_repo();
}

static void test_add_requires_files_or_all(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b staging-test") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_add(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "'all'") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- switch / checkout routing --- */

static void test_switch_routes_to_branch_switch(void)
{
   setup_git_repo();
   setup_ownership_db();
   assert(system("git branch -q target") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "refs/heads/target");
   cJSON *resp = handle_git_switch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "switched to target") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_ownership_db();
   teardown_git_repo();
}

static void test_switch_creates_tracking_branch_from_origin(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-test-tracking-switch-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root) != NULL);
   char remote[512], seed[512], local[512], cmd[4096];
   snprintf(remote, sizeof(remote), "%s/remote.git", root);
   snprintf(seed, sizeof(seed), "%s/seed", root);
   snprintf(local, sizeof(local), "%s/local", root);
   snprintf(cmd, sizeof(cmd),
            "git init -q --bare '%s' && git init -q -b main '%s' && "
            "git -C '%s' config user.email test@test && git -C '%s' config user.name test && "
            "printf 'base\\n' > '%s/file.txt' && git -C '%s' add file.txt && "
            "git -C '%s' commit -q -m base && git -C '%s' checkout -q -b topic && "
            "git -C '%s' push -q '%s' main topic && "
            "git --git-dir='%s' symbolic-ref HEAD refs/heads/main && git clone -q '%s' '%s'",
            remote, seed, seed, seed, seed, seed, seed, seed, seed, remote, remote, remote, local);
   assert(system(cmd) == 0);

   char saved[4096];
   assert(getcwd(saved, sizeof(saved)) != NULL);
   assert(chdir(local) == 0);
   setup_ownership_db();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "refs/remotes/origin/topic");
   cJSON *resp = handle_git_switch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "tracking origin/topic") != NULL);
   assert(system("test \"$(git symbolic-ref --short HEAD)\" = topic && "
                 "test \"$(git rev-parse --abbrev-ref '@{upstream}')\" = origin/topic && "
                 "test \"$(git config branch.topic.remote)\" = origin && "
                 "test \"$(git config branch.topic.merge)\" = refs/heads/topic") == 0);
   char owned_branch[256] = "";
   assert(branch_own_get_session_branch(owned_branch, sizeof(owned_branch)) == 0);
   assert(strcmp(owned_branch, "topic") == 0);

   cJSON_Delete(resp);
   cJSON_Delete(args);
   teardown_ownership_db();
   assert(chdir(saved) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
   assert(system(cmd) == 0);
}

/* A path-scoped checkout is a restore, and routes there rather than moving HEAD. */
static void test_checkout_with_files_restores(void)
{
   setup_git_repo();
   assert(system("git checkout -q -b work && echo changed > file.txt") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON *files = cJSON_AddArrayToObject(args, "files");
   cJSON_AddItemToArray(files, cJSON_CreateString("file.txt"));
   cJSON *resp = handle_git_checkout(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "restored") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(system("grep -q 'hello world' file.txt") == 0);

   teardown_git_repo();
}

static void test_switch_requires_a_ref(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_switch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "'ref'") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- PR title/body derivation --- */

/* One commit: the PR is that commit. No model call, no invented prose. */
static void test_pr_create_derives_title_from_single_commit(void)
{
   setup_git_repo();
   setup_ownership_db();
   session_id_set_override("session-A");
   assert(system("git checkout -q -b feat/derive && echo x > x.txt && git add x.txt && "
                 "git commit -q -m 'feat(thing): make it work' -m 'Because it did not.'") == 0);

   /* No origin remote in the fixture, so create cannot reach the forge — but it
    * must get PAST the title requirement, which is what this covers. The old
    * behaviour was a hard 'title is required'. */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "base", "master");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "'title' parameter is required") == NULL);
   cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(args, "title");
   assert(cJSON_IsString(jtitle));
   assert(strcmp(jtitle->valuestring, "feat(thing): make it work") == 0);
   cJSON *jbody = cJSON_GetObjectItemCaseSensitive(args, "body");
   assert(cJSON_IsString(jbody));
   assert(strstr(jbody->valuestring, "Because it did not.") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

/* Several commits: the shared conventional-commit prefix is kept and the body
 * lists them, so the PR reads like the commits it contains. */
static void test_pr_create_derives_title_from_many_commits(void)
{
   setup_git_repo();
   setup_ownership_db();
   session_id_set_override("session-A");
   assert(system("git checkout -q -b feat/many && echo a > a.txt && git add a.txt && "
                 "git commit -q -m 'feat: first bit' && echo b > b.txt && git add b.txt && "
                 "git commit -q -m 'feat: second bit'") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "base", "master");
   cJSON *resp = handle_git_pr(args);
   cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(args, "title");
   assert(cJSON_IsString(jtitle));
   assert(strstr(jtitle->valuestring, "feat: many") != NULL);
   assert(strstr(jtitle->valuestring, "2 commits") != NULL);
   cJSON *jbody = cJSON_GetObjectItemCaseSensitive(args, "body");
   assert(cJSON_IsString(jbody));
   assert(strstr(jbody->valuestring, "- feat: first bit") != NULL);
   assert(strstr(jbody->valuestring, "- feat: second bit") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

/* Nothing to open a PR for is its own answer, not a derived-title crash. */
static void test_pr_create_with_no_commits_says_so(void)
{
   setup_git_repo();
   setup_ownership_db();
   session_id_set_override("session-A");
   assert(system("git checkout -q -b feat/empty") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "base", "master");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "no commits") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

/* --- pr action=ready --- */

/* ready stops at the FIRST real failure and returns that step's own explanation,
 * so the caller is never told "ready failed" with no idea which part. Here the
 * sync conflicts, so the conflicted file must come back — and nothing must have
 * been pushed. */
static void test_pr_ready_stops_at_the_failing_step(void)
{
   setup_conflicting_branches();
   setup_ownership_db();
   session_id_set_override("session-A");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "ready");
   cJSON_AddStringToObject(args, "base", "side");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* sync's own conflict report, verbatim — not a generic ready failure. */
   assert(strstr(text, "file.txt") != NULL);
   assert(strstr(text, "push:") == NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

/* When the sync succeeds, the push is attempted next and its failure is what
 * comes back (no origin in the fixture) — proving the steps run in order and the
 * report is not fabricated. */
static void test_pr_ready_runs_sync_then_push(void)
{
   setup_git_repo();
   setup_ownership_db();
   session_id_set_override("session-A");
   assert(system("git checkout -q -b feat/ready && echo x > x.txt && git add x.txt && "
                 "git commit -q -m 'feat: work'") == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "ready");
   cJSON_AddStringToObject(args, "base", "master");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* The push is the step that fails here, and its own message says so. */
   assert(strstr(text, "push") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_pr_unknown_action_lists_ready(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "nonsense");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "ready") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_feature_branch_commit_allowed(void)
{
   setup_git_repo();

   system("git checkout -q -b feature-test");
   system("echo 'change' >> file.txt");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "feature commit");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Worktree-awareness tests --- */

static void test_worktree_branch_create_no_switch(void)
{
   setup_git_repo();

   /* Simulate worktree mode */
   mcp_git_set_worktree(1);

   /* Get current branch before create */
   int rc;
   char *before = run_cmd("git rev-parse --abbrev-ref HEAD 2>/dev/null", &rc);
   char *nl = before ? strchr(before, '\n') : NULL;
   if (nl)
      *nl = '\0';

   /* Create a branch — should NOT switch to it */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "wt-test-branch");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "created") != NULL);
   assert(strstr(text, "worktree mode") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Verify we're still on the original branch */
   char *after = run_cmd("git rev-parse --abbrev-ref HEAD 2>/dev/null", &rc);
   nl = after ? strchr(after, '\n') : NULL;
   if (nl)
      *nl = '\0';
   assert(before && after && strcmp(before, after) == 0);
   free(before);
   free(after);

   mcp_git_set_worktree(0);
   teardown_git_repo();
}

static void test_worktree_branch_switch_blocked(void)
{
   setup_git_repo();

   /* Create a branch to switch to */
   system("git branch switch-target 2>/dev/null");

   /* Simulate worktree mode */
   mcp_git_set_worktree(1);

   /* Attempt to switch — should be blocked */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "switch");
   cJSON_AddStringToObject(args, "name", "switch-target");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "not allowed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   mcp_git_set_worktree(0);
   teardown_git_repo();
}

/* test_mcp_git_verify_threads.inc: git_verify multithreading / timeout /
 * cancellation tests split out of test_mcp_git.c to keep that .c under the
 * 2000-line hard limit. Included mid-file (same TU) so the white-box statics
 * and verify_test_setup/teardown helpers above stay in scope. */
/* --- Test git_verify runs multiple configured steps successfully --- */

static void test_git_verify_multithreaded_steps(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-mt");

   /* Write a project.yaml with multiple steps so sync verify exercises
    * aggregate step handling without using elapsed time as a test gate. */
   char fake_home[256];
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: step-a\n"
                          "      run: echo step-a-done\n"
                          "    - name: step-b\n"
                          "      run: echo step-b-done\n");

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 0); /* force sync for test assertions */
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "step-a") != NULL);
   assert(strstr(text, "step-b") != NULL);
   assert(strstr(text, "PASS") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

static void test_git_verify_step_timeout_finishes(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-timeout");

   char fake_home[256];
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: hangs\n"
                          "      run: sleep 2\n");

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);
   assert(setenv("AIMEE_VERIFY_STEP_TIMEOUT_MS", "200", 1) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 0);
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "hangs") != NULL);
   assert(strstr(text, "FAIL") != NULL);
   assert(strstr(text, "timed out") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   unsetenv("AIMEE_VERIFY_STEP_TIMEOUT_MS");
   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

static void marker_job(void *arg)
{
   volatile int *done = (volatile int *)arg;
   *done = 1;
}

static char *git_verify_status_text(int job_id, cJSON **resp_out)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "status");
   cJSON_AddNumberToObject(args, "job_id", job_id);
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   cJSON_Delete(args);
   *resp_out = resp;
   return get_mcp_text(resp);
}

static void test_git_verify_async_does_not_starve_server_pool(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-async-pool");

   char fake_home[256];
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: hangs\n"
                          "      run: sleep 10\n");

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   assert(ctx != NULL);
   assert(compute_pool_init(&ctx->pool, 1) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 1);
   cJSON *resp = handle_git_verify(ctx, args, "sid-async-pool");
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   int job_id = 0;
   assert(sscanf(text, "Started background verification job #%d.", &job_id) == 1);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   volatile int done = 0;
   assert(compute_pool_submit(&ctx->pool, marker_job, (void *)&done) == 0);
   for (int i = 0; i < 50 && !done; i++)
      usleep(10000);
   assert(done == 1);

   int cancelled = 0;
   for (int i = 0; i < 50 && !cancelled; i++)
   {
      usleep(20000);
      cancelled = verify_cancel_session("sid-async-pool");
   }
   assert(cancelled > 0);

   for (int i = 0; i < 100; i++)
   {
      cJSON *status_resp = NULL;
      char *status = git_verify_status_text(job_id, &status_resp);
      assert(status != NULL);
      int finished = strstr(status, "finished") != NULL;
      cJSON_Delete(status_resp);
      if (finished)
         break;
      usleep(20000);
      assert(i < 99);
   }

   compute_pool_shutdown(&ctx->pool);
   free(ctx);
   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

typedef struct
{
   const char *session_id;
   cJSON *resp;
} sync_verify_thread_t;

static void *run_sync_verify_thread(void *arg)
{
   sync_verify_thread_t *state = (sync_verify_thread_t *)arg;
   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 0);
   state->resp = handle_git_verify(NULL, args, state->session_id);
   cJSON_Delete(args);
   return NULL;
}

static void test_git_verify_serializes_across_sessions(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-global-lock");

   char marker[512], overlap[512], yaml[2048];
   snprintf(marker, sizeof(marker), "%s/verify-running", tmpdir);
   snprintf(overlap, sizeof(overlap), "%s/verify-overlapped", tmpdir);
   snprintf(yaml, sizeof(yaml),
            "verify:\n"
            "  enforce: true\n"
            "  steps:\n"
            "    - name: exclusive\n"
            "      run: if mkdir '%s'; then sleep 0.4; rmdir '%s'; else echo overlap > '%s'; "
            "exit 1; fi\n",
            marker, marker, overlap);
   char fake_home[256];
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home), yaml);

   char dirty[512];
   snprintf(dirty, sizeof(dirty), "%s/dirty", tmpdir);
   FILE *f = fopen(dirty, "w");
   assert(f != NULL);
   fputs("force both runs past the clean-tree cache\n", f);
   fclose(f);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   sync_verify_thread_t first = {.session_id = "sid-global-lock-a", .resp = NULL};
   sync_verify_thread_t second = {.session_id = "sid-global-lock-b", .resp = NULL};
   pthread_t first_tid, second_tid;
   assert(pthread_create(&first_tid, NULL, run_sync_verify_thread, &first) == 0);
   struct stat st;
   for (int i = 0; i < 200 && stat(marker, &st) != 0; i++)
      usleep(10000);
   assert(stat(marker, &st) == 0);
   assert(pthread_create(&second_tid, NULL, run_sync_verify_thread, &second) == 0);

   assert(pthread_join(first_tid, NULL) == 0);
   assert(pthread_join(second_tid, NULL) == 0);
   assert(first.resp != NULL && second.resp != NULL);
   assert(strstr(get_mcp_text(first.resp), "PASS") != NULL);
   assert(strstr(get_mcp_text(second.resp), "PASS") != NULL);
   assert(access(overlap, F_OK) != 0);
   cJSON_Delete(first.resp);
   cJSON_Delete(second.resp);

   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

static void test_git_verify_sync_cancelled_by_session_close(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-cancel");

   char fake_home[256];
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home),
                          "verify:\n"
                          "  enforce: true\n"
                          "  steps:\n"
                          "    - name: hangs\n"
                          "      run: sleep 10\n");

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   sync_verify_thread_t state = {.session_id = "sid-sync-cancel", .resp = NULL};
   pthread_t tid;
   assert(pthread_create(&tid, NULL, run_sync_verify_thread, &state) == 0);

   int cancelled = 0;
   for (int i = 0; i < 500 && !cancelled; i++)
   {
      usleep(20000);
      cancelled = verify_cancel_session(state.session_id);
   }
   assert(cancelled > 0);
   assert(pthread_join(tid, NULL) == 0);

   char *text = get_mcp_text(state.resp);
   assert(text != NULL);
   assert(strstr(text, "cancelled: owning session closed") != NULL);
   assert(strstr(text, "verification cancelled; state not recorded") != NULL);
   cJSON_Delete(state.resp);

   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

static int start_async_verify(server_ctx_t *ctx, const char *session_id)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 1);
   cJSON *resp = handle_git_verify(ctx, args, session_id);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   int job_id = 0;
   assert(sscanf(text, "Started background verification job #%d.", &job_id) == 1);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   return job_id;
}

static void wait_async_verify_finished(int job_id)
{
   for (int i = 0; i < 200; i++)
   {
      cJSON *status_resp = NULL;
      char *status = git_verify_status_text(job_id, &status_resp);
      assert(status != NULL);
      int finished = strstr(status, "finished") != NULL;
      cJSON_Delete(status_resp);
      if (finished)
         return;
      usleep(10000);
   }
   assert(0 && "async verify did not finish");
}

static void test_git_verify_async_rejects_same_session_overlap(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-one-per-session");
   char fake_home[256];
   verify_test_write_yaml(
       tmpdir, fake_home, sizeof(fake_home),
       "verify:\n  enforce: true\n  steps:\n    - name: hangs\n      run: sleep 10\n");
   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   assert(ctx != NULL);
   int job_id = start_async_verify(ctx, "sid-one-verify");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 1);
   cJSON *resp = handle_git_verify(ctx, args, "sid-one-verify");
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "session already has a running verification") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(verify_cancel_session("sid-one-verify") > 0);
   wait_async_verify_finished(job_id);
   free(ctx);
   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

static void test_git_verify_async_reaps_finished_jobs(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-reap-jobs");
   char fake_home[256];
   verify_test_write_yaml(
       tmpdir, fake_home, sizeof(fake_home),
       "verify:\n  enforce: true\n  steps:\n    - name: quick\n      run: echo ok\n");
   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   assert(ctx != NULL);
   for (int i = 0; i < 40; i++)
      wait_async_verify_finished(start_async_verify(ctx, "sid-reap-verify"));
   free(ctx);
   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

static void test_git_verify_sync_rejects_same_session_overlap(void)
{
   char tmpdir[256];
   verify_test_setup_repo(tmpdir, sizeof(tmpdir), "aimee-test-verify-sync-overlap");
   char started_path[512];
   snprintf(started_path, sizeof(started_path), "%s-started", tmpdir);
   char yaml[1024];
   snprintf(yaml, sizeof(yaml),
            "verify:\n  enforce: true\n  steps:\n    - name: hangs\n      run: sh -c 'touch %s; "
            "sleep 10'\n",
            started_path);
   char fake_home[256];
   verify_test_write_yaml(tmpdir, fake_home, sizeof(fake_home), yaml);
   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);
   sync_verify_thread_t state = {.session_id = "sid-sync-overlap", .resp = NULL};
   pthread_t tid;
   assert(pthread_create(&tid, NULL, run_sync_verify_thread, &state) == 0);
   struct stat st;
   for (int i = 0; i < 3000 && stat(started_path, &st) != 0; i++)
      usleep(10000);
   assert(stat(started_path, &st) == 0);
   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "async", 0);
   cJSON *resp = handle_git_verify(NULL, args, state.session_id);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "session already has a running verification") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
   assert(verify_cancel_session(state.session_id) > 0);
   assert(pthread_join(tid, NULL) == 0);
   cJSON_Delete(state.resp);
   assert(chdir(saved_cwd) == 0);
   verify_test_teardown(tmpdir, fake_home);
}

/* --- The PR-create mergeability gate ---
 *
 * aimee opening PRs that arrive CONFLICTING is the failure this gate exists to
 * stop: review cannot start, and CI reports on a merge that will never happen.
 * The gate has to be right in both directions -- a false conflict refuses a
 * good PR, and an inconclusive answer must let one through rather than block
 * on a check that could not run. */
static void test_pr_conflict_gate(void)
{
   char tmp[256];
   snprintf(tmp, sizeof tmp, "%s/aimee-test-pr-conflict-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmp) != NULL);
   char saved[4096];
   assert(getcwd(saved, sizeof(saved)) != NULL);

   char cmd[4096];
   /* main has a file; two branches change the same line and a third changes a
    * different file. All local -- the gate falls back to a bare ref when no
    * origin/<base> exists, which is what a test repo has. */
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q -b main && git config user.email t@t && git config user.name t"
            " && printf 'one\n' > f.txt && git add f.txt && git commit -q -m base"
            " && git checkout -q -b clean && printf 'x\n' > other.txt && git add other.txt"
            " && git commit -q -m clean"
            " && git checkout -q main && printf 'two\n' > f.txt && git commit -q -am theirs"
            " && git checkout -q -b conflicting main~1 && printf 'three\n' > f.txt"
            " && git commit -q -am ours",
            tmp);
   assert(system(cmd) == 0);
   assert(chdir(tmp) == 0);

   char files[1024];

   /* A branch touching a different file merges cleanly. */
   assert(system("git checkout -q clean") == 0);
   assert(mcp_git_conflicts_with_base("main", files, sizeof(files)) == 0);

   /* Both sides rewrote f.txt: conflict, and the gate must name the file so the
    * refusal tells the caller what to fix. */
   assert(system("git checkout -q conflicting") == 0);
   assert(mcp_git_conflicts_with_base("main", files, sizeof(files)) == 1);
   assert(strstr(files, "f.txt") != NULL);

   /* Cannot tell => proceed. A base that does not exist, and an empty base, must
    * both return -1 rather than 1: refusing a PR because the check could not run
    * would be worse than the problem the gate solves. */
   assert(mcp_git_conflicts_with_base("no-such-branch", files, sizeof(files)) == -1);
   assert(mcp_git_conflicts_with_base("", files, sizeof(files)) == -1);
   assert(mcp_git_conflicts_with_base(NULL, files, sizeof(files)) == -1);

   assert(chdir(saved) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
   system(cmd);
   printf("pr_conflict_gate ");
}

int main(void)
{
   /* The verify gate reads its ledger from the git module, so the module has
    * to be up or every verify assertion below fails on a correctly closed
    * gate rather than on what it means to test. */
   git_module_fixture_start();

   printf("mcp_git: ");

   test_git_status_clean();
   test_pr_conflict_gate();
   test_git_status_modified();
   test_mcp_chdir_uses_cwd_argument();
   test_mcp_chdir_session_cwd_precedes_proxy_cwd();
   test_explicit_path_is_authoritative_and_live();
   test_mcp_chdir_repairs_stale_delegate_tracked_cwd();
   test_mcp_chdir_keeps_stale_delegate_cwd_when_repair_missing();
   test_mcp_chdir_does_not_repair_delegate_session_cwd();
   test_mcp_chdir_keeps_explicit_managed_worktree_despite_stale_session_state();
   test_mcp_chdir_uses_pwd_fallback();
   test_git_commit_missing_message();
   test_git_commit_success();
   test_git_commit_reads_masked_global_identity();
   test_git_commit_skips_sensitive();
   test_git_container_provider_runs_on_server();
   test_mirror_workspace_refuses_local_history_writes();
   test_git_push_requires_branch();
   test_git_branch_missing_action();
   test_git_branch_create_and_list();
   test_git_fetch_never_writes_or_prunes_local_branches();
   test_git_fetch_refuses_unborn_head();
   test_git_fetch_rejects_unsafe_remote_name();
   /* test_git_log skipped: format string issue in handle_git_log */
   test_git_clone_missing_url();
   test_git_stash_unknown_action();
   test_git_stash_session_aware_pop();
   test_git_pr_missing_action();
   test_git_pr_unknown_action();
   test_git_pr_create_missing_title();
   test_git_pr_create_without_github_origin();
   test_git_pr_edit_missing_number();
   test_git_pr_edit_requires_fields();
   test_git_pr_checks_missing_number();
   test_git_pr_wait_missing_number();
   test_git_pr_wait_is_rejected_without_running_gh();
   test_git_pr_auto_merge_accepts_pending_checks_without_claiming_merge();
   assert(check_branch_has_merged_pr_for(NULL) == 0);
   assert(check_branch_has_merged_pr_for("") == 0);
   test_git_issue_list_defaults();
   test_git_issue_invalid_state();
   test_git_issue_unknown_action();
   test_git_diff_no_changes();
   test_git_verify();
   test_git_verify_creates_state_in_repo_root();
   test_git_verify_dirty_tree_ignores_cached_pass();
   test_git_verify_multithreaded_steps();
   test_git_verify_step_timeout_finishes();
   test_git_verify_async_does_not_starve_server_pool();
   test_git_verify_async_rejects_same_session_overlap();
   test_git_verify_async_reaps_finished_jobs();
   test_git_verify_serializes_across_sessions();
   test_git_verify_sync_cancelled_by_session_close();
   test_git_verify_sync_rejects_same_session_overlap();

   /* verify_load_config tests */
   test_verify_load_config_enforce_true();
   test_verify_load_config_enforce_false();
   test_verify_load_config_no_enforce_defaults_false();
   test_verify_load_config_emits_parallel_steps();
   test_verify_load_config_repairs_existing_generated_plan_with_go_modules();
   test_verify_load_config_leaves_custom_plan_unchanged();
   test_verify_load_config_discovers_go_modules_from_cwd();
   test_verify_load_config_collapses_generated_pipeline_to_verify_local();
   test_verify_load_config_prefers_check_linking_for_build();
   test_verify_load_config_test_only_has_no_missing_build_dependency();
   test_verify_load_config_normalizes_build_integrity_order();
   test_verify_load_config_falls_back_to_verify_local();
   test_verify_load_config_old_flat_format_ignored();
   test_verify_prepare_pr_blocks_branch_with_merged_pr();
   test_verify_prepare_pr_allows_reused_branch_with_new_head();

   /* Verify gate enforcement tests */
   test_verify_gate_not_enforced_without_enforce_flag();
   test_verify_gate_enforced_with_enforce_true_and_stale_verify();

   /* Branch ownership tests */
   test_feature_branch_store_is_session_scoped();
   test_branch_create_registers_ownership();
   test_commit_blocked_by_other_session_ownership();
   test_push_blocked_by_other_session_ownership();
   test_branch_claim();
   test_main_branch_no_ownership();

   /* Worktree-awareness tests */
   test_worktree_branch_create_no_switch();
   test_worktree_branch_switch_blocked();

   /* Main branch protection tests */
   test_main_branch_commit_blocked();
   test_main_branch_push_blocked();
   test_main_branch_reset_blocked();
   test_main_branch_delete_blocked();
   test_feature_branch_commit_allowed();

   /* Integration operations */
   test_merge_clean_reports_the_change();
   test_merge_conflict_aborts_and_names_files();
   test_merge_keep_conflicts_then_continue();
   test_merge_action_abort();
   test_second_operation_refused_while_one_is_in_progress();
   test_rebase_carries_the_identity_on_first_run();
   test_integrate_refuses_dirty_tree();
   test_integrate_blocked_on_main();
   test_integrate_requires_a_ref();
   test_cherry_pick_and_revert();
   test_sync_reports_already_current();
   test_sync_rejects_unknown_mode();

   /* Staging and ref movement */
   test_add_all_stages_new_files_but_not_secrets();
   test_add_refuses_only_sensitive_paths();
   test_add_allows_env_templates();
   test_add_requires_files_or_all();
   test_switch_routes_to_branch_switch();
   test_switch_creates_tracking_branch_from_origin();
   test_checkout_with_files_restores();
   test_switch_requires_a_ref();

   /* PR title/body derivation */
   test_pr_create_derives_title_from_single_commit();
   test_pr_create_derives_title_from_many_commits();
   test_pr_create_with_no_commits_says_so();
   test_pr_ready_stops_at_the_failing_step();
   test_pr_ready_runs_sync_then_push();
   test_pr_unknown_action_lists_ready();

   printf("all tests passed\n");
   return 0;
}
