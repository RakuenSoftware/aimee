#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "workspace.h"
#include "platform_test_util.h"
#include "git_verify.h"

/* Per-case in-memory DB2 backing for test bodies that round-trip
 * memory-subsystem state. The shim helper owns the sqlite handle and
 * the db2_init/shutdown ceremony. */
static void guardrails_open_test_sqlite(void)
{
   db2_test_shim_open();
}

static void guardrails_close_test_sqlite(void)
{
   db2_test_shim_close();
}

/* --- Verify config test helpers ---
 *
 * Verify config now lives at ~/.config/aimee/projects/<basename>/project.yaml
 * (keyed by the basename of the project's main repo root). Tests override
 * HOME so the global path resolves under a sandbox dir, then write the test
 * YAML there. The basename is taken from the directory the test passes — for
 * worktree tests, that must be the MAIN repo dir, since worktrees resolve to
 * the main repo's basename. */

static char g_vy_saved_home[4096];
static int g_vy_home_was_set;
static char g_vy_fake_home[256];

static void guardrails_tmp_path(char *buf, size_t buf_size, const char *stem, const char *suffix)
{
   snprintf(buf, buf_size, "/tmp/%s-%d%s", stem, (int)getpid(), suffix);
}

/* Build a unique synthetic session id for a specific test case. DB1 state
 * tests roundtrip through the shared DB1 connection (opened once in main)
 * so each test must pick a non-colliding id. */
static void guardrails_tmp_sid(char *buf, size_t buf_size, const char *tag)
{
   snprintf(buf, buf_size, "test-%s-%d", tag, (int)getpid());
}

static const char *guardrails_test_worktree_cwd = "/tmp/.aimee/worktrees/test/main";

static void vy_set_global_yaml(const char *project_dir, const char *yaml)
{
   if (g_vy_fake_home[0] == '\0')
   {
      const char *old = getenv("HOME");
      if (old)
      {
         snprintf(g_vy_saved_home, sizeof(g_vy_saved_home), "%s", old);
         g_vy_home_was_set = 1;
      }
      else
      {
         g_vy_home_was_set = 0;
      }
      snprintf(g_vy_fake_home, sizeof(g_vy_fake_home), "/tmp/aimee-test-home-XXXXXX");
      assert(mkdtemp(g_vy_fake_home) != NULL);
      setenv("HOME", g_vy_fake_home, 1);
   }

   const char *base = strrchr(project_dir, '/');
   base = base ? base + 1 : project_dir;

   char dir[1024], path[1024], cmd[2048];
   snprintf(dir, sizeof(dir), "%s/.config/aimee/projects/%s", g_vy_fake_home, base);
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
   assert(system(cmd) == 0);

   snprintf(path, sizeof(path), "%s/project.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(yaml, f);
   fclose(f);
}

static void vy_clear_home(void)
{
   if (g_vy_fake_home[0] == '\0')
      return;
   char cmd[2048];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_vy_fake_home);
   system(cmd);
   g_vy_fake_home[0] = '\0';
   if (g_vy_home_was_set)
      setenv("HOME", g_vy_saved_home, 1);
   else
      unsetenv("HOME");
}

static void test_classify_sensitive(void)
{
   classification_t c = classify_path(".env");
   assert(c.severity == SEV_BLOCK);

   c = classify_path("credentials.json");
   assert(c.severity == SEV_BLOCK);

   c = classify_path("id_rsa");
   assert(c.severity == SEV_BLOCK);

   c = classify_path("server.key");
   assert(c.severity == SEV_BLOCK);
}

static void test_classify_database(void)
{
   classification_t c = classify_path("data.db");
   assert(c.severity == SEV_RED);

   c = classify_path("app.sqlite3");
   assert(c.severity == SEV_RED);
}

static void test_classify_safe(void)
{
   classification_t c = classify_path("main.go");
   assert(c.severity == SEV_GREEN);

   c = classify_path("src/handler.c");
   assert(c.severity == SEV_GREEN);
}

static void write_file_text(const char *path, const char *content)
{
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(content, f);
   fclose(f);
}

static void with_temp_policy_path(char *path, size_t path_len)
{
   snprintf(path, path_len, "/tmp/test-guardrails-policy-XXXXXX.json");
   int fd = mkstemps(path, 5);
   assert(fd >= 0);
   close(fd);
   setenv("AIMEE_GUARDRAILS_PATH", path, 1);
   guardrails_policy_reset();
}

static void clear_temp_policy_path(const char *path)
{
   if (path && path[0])
      unlink(path);
   unsetenv("AIMEE_GUARDRAILS_PATH");
   guardrails_policy_reset();
}

static void test_is_write_command(void)
{
   assert(is_write_command("rm -rf /tmp/test") == 1);
   assert(is_write_command("git push origin main") == 1);
   assert(is_write_command("git commit -m 'msg'") == 1);
   assert(is_write_command("echo hello > file.txt") == 1);
   assert(is_write_command("pip install flask") == 1);
   assert(is_write_command("dnf install python3") == 1);
   assert(is_write_command("dnf remove httpd") == 1);
   assert(is_write_command("dnf erase vim") == 1);
   assert(is_write_command("dnf upgrade") == 1);
   assert(is_write_command("dnf update kernel") == 1);
   assert(is_write_command("yum install gcc") == 1);
   assert(is_write_command("yum remove wget") == 1);
   assert(is_write_command("yum erase curl") == 1);
   assert(is_write_command("yum upgrade") == 1);
   assert(is_write_command("yum update bash") == 1);
   assert(is_write_command("rpm -i package.rpm") == 1);
   assert(is_write_command("rpm -e package") == 1);
   assert(is_write_command("rpm -U package.rpm") == 1);
   assert(is_write_command("rpm --install package.rpm") == 1);
   assert(is_write_command("rpm --erase package") == 1);
   assert(is_write_command("rpm --upgrade package.rpm") == 1);

   assert(is_write_command("git log --oneline") == 0);
   assert(is_write_command("ls -la") == 0);
   assert(is_write_command("grep -r pattern .") == 0);
   assert(is_write_command("rpm -q package") == 0);
   assert(is_write_command("rpm --query package") == 0);
   assert(is_write_command("dnf list installed") == 0);
   assert(is_write_command("yum list available") == 0);

   /* '>' inside quoted arguments must not be treated as redirection. */
   assert(is_write_command("grep -n \"stores->shared\" src/foo.c") == 0);
   assert(is_write_command("grep -rn 'a->b' .") == 0);
   assert(is_write_command("echo \"arrow->target\"") == 0);
   /* Redirection after a quoted arg is still a write. */
   assert(is_write_command("echo \"a->b\" > /tmp/out.txt") == 1);
}

static void test_policy_file_overrides_defaults(void)
{
   char policy_path[256];
   char deny_dir[] = "/tmp/test-guardrails-deny-XXXXXX";
   char denied_file[512];
   with_temp_policy_path(policy_path, sizeof(policy_path));
   assert(mkdtemp(deny_dir) != NULL);
   snprintf(denied_file, sizeof(denied_file), "%s/file.txt", deny_dir);
   write_file_text(denied_file, "secret");

   {
      FILE *f = fopen(policy_path, "w");
      assert(f != NULL);
      fprintf(f,
              "{\n"
              "  \"sensitive_exact\": [\"deploy.token\"],\n"
              "  \"sensitive_patterns\": [\"deploy-secret\"],\n"
              "  \"db_extensions\": [\".vaultdb\"],\n"
              "  \"path_denies\": [\"%s/\"],\n"
              "  \"write_commands\": [\"deploy \"],\n"
              "  \"git_write_commands\": [\"git custom-push\"],\n"
              "  \"package_commands\": [\"brew install\"]\n"
              "}\n",
              deny_dir);
      fclose(f);
   }
   guardrails_policy_reset();

   assert(classify_path("deploy.token").severity == SEV_BLOCK);
   assert(classify_path("notes/deploy-secret.txt").severity == SEV_BLOCK);
   assert(classify_path("archive.vaultdb").severity == SEV_RED);
   assert(is_write_command("deploy release") == 1);
   assert(is_write_command("git custom-push origin testing") == 1);
   assert(is_write_command("brew install jq") == 1);

   char resolved[MAX_PATH_LEN];
   assert(guardrails_validate_file_path(denied_file, resolved, sizeof(resolved)) != NULL);

   clear_temp_policy_path(policy_path);
   unlink(denied_file);
   rmdir(deny_dir);
}

static void test_policy_file_reloads_on_change(void)
{
   char policy_path[256];
   with_temp_policy_path(policy_path, sizeof(policy_path));

   write_file_text(policy_path, "{ \"sensitive_exact\": [\"alpha.secret\"] }\n");
   guardrails_policy_reset();
   assert(classify_path("alpha.secret").severity == SEV_BLOCK);
   assert(classify_path("beta.secret").severity == SEV_GREEN);

   sleep(1);
   write_file_text(policy_path,
                   "{ \"sensitive_exact\": [\"beta.secret\"], \"write_commands\": [\"sync \"] }\n");

   assert(classify_path("alpha.secret").severity == SEV_GREEN);
   assert(classify_path("beta.secret").severity == SEV_BLOCK);
   assert(is_write_command("sync now") == 1);

   clear_temp_policy_path(policy_path);
}

static void test_normalize_path(void)
{
   char buf[MAX_PATH_LEN];

   normalize_path("/abs/path", "/cwd", buf, sizeof(buf));
   assert(strcmp(buf, "/abs/path") == 0);

   normalize_path("rel/path", "/cwd", buf, sizeof(buf));
   assert(strcmp(buf, "/cwd/rel/path") == 0);

   normalize_path("~/home/file", "/cwd", buf, sizeof(buf));
   const char *home = getenv("HOME");
   if (home)
   {
      char expected[MAX_PATH_LEN];
      snprintf(expected, sizeof(expected), "%s/home/file", home);
      assert(strcmp(buf, expected) == 0);
   }
   else
   {
      assert(strcmp(buf, "~/home/file") == 0);
   }
}

static void test_plan_mode_blocks_writes(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_PLAN);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];
   int rc = pre_tool_check("Edit", "{\"file_path\":\"/test.c\"}", &state, MODE_APPROVE, "", msg,
                           sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "plan mode") != NULL);

   rc = pre_tool_check("Bash", "{\"command\":\"ls\"}", &state, MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 0); /* read-only command allowed */

   rc = pre_tool_check("execute_script", "{\"language\":\"bash\",\"body\":\"ls\"}", &state,
                       MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "plan mode") != NULL);

   guardrails_close_test_sqlite();
}

static void test_session_id(void)
{
   const char *id = session_id();
   assert(id != NULL);
   assert(id[0] != '\0');
   /* Should be stable within a process */
   assert(strcmp(id, session_id()) == 0);
}

static void test_session_id_override(void)
{
   const char *base = session_id();
   assert(base != NULL);
   assert(base[0] != '\0');

   session_id_set_override("override-session");
   assert(strcmp(session_id(), "override-session") == 0);

   session_id_clear_override();
   assert(strcmp(session_id(), base) == 0);
}

static void test_canonical_tool_names(void)
{
   assert(strcmp(guardrails_canonical_tool_name("bash"), "Bash") == 0);
   assert(strcmp(guardrails_canonical_tool_name("execute_script"), "Bash") == 0);
   assert(strcmp(guardrails_canonical_tool_name("write_file"), "Write") == 0);
   assert(strcmp(guardrails_canonical_tool_name("spawn_agent"), "Subagent") == 0);
   assert(strcmp(guardrails_canonical_tool_name("RemoteTrigger"), "Subagent") == 0);
   assert(strcmp(guardrails_canonical_tool_name("Read"), "Read") == 0);
}

static void test_session_state_worktrees(void)
{
   session_state_t *state = malloc(sizeof(session_state_t));
   memset(state, 0, sizeof(*state));
   strcpy(state->session_mode, MODE_IMPLEMENT);
   strcpy(state->guardrail_mode, MODE_APPROVE);

   /* Add worktree mappings */
   strcpy(state->worktrees[0].git_root, "/root/dev/wol");
   strcpy(state->worktrees[0].worktree_path, "/root/dev/wol-abc12345");
   strcpy(state->worktrees[1].git_root, "/root/dev/acktng");
   strcpy(state->worktrees[1].worktree_path, "/root/dev/acktng-abc12345");
   state->worktree_count = 2;

   /* Save and reload via DB1 */
   char test_sid[128];
   guardrails_tmp_sid(test_sid, sizeof(test_sid), "session-wt");
   session_state_force_save(state, test_sid);

   session_state_t *loaded = malloc(sizeof(session_state_t));
   session_state_load(loaded, test_sid);
   assert(loaded->worktree_count == 2);
   assert(strcmp(loaded->worktrees[0].git_root, "/root/dev/wol") == 0);
   assert(strcmp(loaded->worktrees[0].worktree_path, "/root/dev/wol-abc12345") == 0);
   assert(strcmp(loaded->worktrees[1].git_root, "/root/dev/acktng") == 0);
   assert(strcmp(loaded->worktrees[1].worktree_path, "/root/dev/acktng-abc12345") == 0);

   db1_session_state_delete(test_sid);
   free(state);
   free(loaded);
}

static void test_worktree_for_cwd(void)
{
   session_state_t state;
   memset(&state, 0, sizeof(state));

   /* Set up worktree mapping */
   strcpy(state.worktrees[0].git_root, "/root/dev/aimee");
   strcpy(state.worktrees[0].worktree_path, "/root/dev/aimee/.aimee/worktrees/abc12345/main");
   state.worktree_count = 1;

   /* CWD inside git root should match */
   const char *wt = worktree_for_cwd(&state, "/root/dev/aimee/src/memory.c");
   assert(wt != NULL);
   assert(strcmp(wt, "/root/dev/aimee/.aimee/worktrees/abc12345/main") == 0);

   /* CWD inside worktree should NOT match (already in worktree) */
   wt = worktree_for_cwd(&state, "/root/dev/aimee/.aimee/worktrees/abc12345/main/src/memory.c");
   assert(wt == NULL);

   /* CWD outside git root should not match */
   wt = worktree_for_cwd(&state, "/root/dev/other/file.c");
   assert(wt == NULL);

   /* No worktrees means no match */
   state.worktree_count = 0;
   wt = worktree_for_cwd(&state, "/root/dev/aimee/src/memory.c");
   assert(wt == NULL);
}

static void test_worktree_prefers_specific_git_root(void)
{
   session_state_t state;
   memset(&state, 0, sizeof(state));

   /* Set up worktrees for both parent "dev" and child "dev/aimee" */
   strcpy(state.worktrees[0].git_root, "/root/dev");
   strcpy(state.worktrees[0].worktree_path, "/root/dev/.aimee/worktrees/abc12345/main");
   strcpy(state.worktrees[1].git_root, "/root/dev/aimee");
   strcpy(state.worktrees[1].worktree_path, "/root/dev/aimee/.aimee/worktrees/abc12345/main");
   state.worktree_count = 2;

   /* Path inside /root/dev/aimee should match the aimee worktree, not dev */
   const char *wt = worktree_for_cwd(&state, "/root/dev/aimee/src/main.c");
   assert(wt != NULL);
   assert(strcmp(wt, "/root/dev/aimee/.aimee/worktrees/abc12345/main") == 0);

   /* Path directly inside /root/dev (not a child) should match dev */
   wt = worktree_for_cwd(&state, "/root/dev/other/file.c");
   assert(wt != NULL);
   assert(strcmp(wt, "/root/dev/.aimee/worktrees/abc12345/main") == 0);
}

static void test_worktree_sibling_path(void)
{
   char buf[MAX_PATH_LEN];

   /* The session key is the first 16 chars of the id (WORKTREE_SID_KEY_LEN). */

   /* Without work_name: session-level worktree. */
   int rc = worktree_sibling_path("/root/dev/aimee", "fadc648f-1234-5678", NULL, buf, sizeof(buf));
   assert(rc == 0);
   assert(strcmp(buf, "/root/dev/aimee/.aimee/worktrees/fadc648f-1234-56/main") == 0);

   /* With work_name: per-delegate worktree. */
   rc = worktree_sibling_path("/root/dev/aimee", "fadc648f-1234-5678", "task01", buf, sizeof(buf));
   assert(rc == 0);
   assert(strcmp(buf, "/root/dev/aimee/.aimee/worktrees/fadc648f-1234-56/task01") == 0);

   /* A legacy short (<=16 char) id maps to itself, so worktrees created before
    * the key was widened keep their path. */
   rc = worktree_sibling_path("/root/dev/aimee", "abcd1234", NULL, buf, sizeof(buf));
   assert(rc == 0);
   assert(strcmp(buf, "/root/dev/aimee/.aimee/worktrees/abcd1234/main") == 0);
}

static void test_worktree_detect_base_branch_active(void)
{
   /* A new worktree must be rooted on the repository's DEFAULT branch
    * (origin/HEAD), NOT on whatever feature branch is currently checked out. */
   char tmpdir[] = "/tmp/test_wt_branch_XXXXXX";
   if (mkdtemp(tmpdir) == NULL)
   {
      fprintf(stderr, "test_worktree_detect_base_branch_active: mkdtemp failed, skipping\n");
      return;
   }

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' config user.email 'test@test.com' 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' config user.name 'Test' 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd),
            "touch '%s/x' && git -C '%s' add x && git -C '%s' commit -qm 'init' 2>/dev/null",
            tmpdir, tmpdir, tmpdir);
   (void)system(cmd);
   /* Normalize the default branch name and simulate a configured remote default
    * (origin/HEAD -> origin/main) without needing a network remote. */
   snprintf(cmd, sizeof(cmd), "git -C '%s' branch -m main 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' update-ref refs/remotes/origin/main HEAD 2>/dev/null",
            tmpdir);
   (void)system(cmd);
   snprintf(
       cmd, sizeof(cmd),
       "git -C '%s' symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main 2>/dev/null",
       tmpdir);
   (void)system(cmd);
   /* Check out a distinct feature branch — detection must ignore it. */
   snprintf(cmd, sizeof(cmd), "git -C '%s' checkout -qb 'feat/test-branch' 2>/dev/null", tmpdir);
   (void)system(cmd);

   char branch[64];
   worktree_detect_base_branch(tmpdir, branch, sizeof(branch));
   assert(strcmp(branch, "origin/main") == 0);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   (void)system(cmd);
}

static void test_worktree_detect_base_branch_local_default(void)
{
   /* With no remote default set, fall back to a local default branch (main),
    * still in preference to the checked-out feature branch. */
   char tmpdir[] = "/tmp/test_wt_localdef_XXXXXX";
   if (mkdtemp(tmpdir) == NULL)
   {
      fprintf(stderr, "test_worktree_detect_base_branch_local_default: mkdtemp failed, skipping\n");
      return;
   }

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' config user.email 'test@test.com' 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' config user.name 'Test' 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd),
            "touch '%s/x' && git -C '%s' add x && git -C '%s' commit -qm 'init' 2>/dev/null",
            tmpdir, tmpdir, tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' branch -m main 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' checkout -qb 'feat/test-branch' 2>/dev/null", tmpdir);
   (void)system(cmd);

   char branch[64];
   worktree_detect_base_branch(tmpdir, branch, sizeof(branch));
   assert(strcmp(branch, "main") == 0);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   (void)system(cmd);
}

static void test_worktree_detect_base_branch_fallback(void)
{
   /* Non-existent path: should fall back to "HEAD" */
   char branch[64];
   worktree_detect_base_branch("/tmp/__nonexistent_aimee_test_repo__", branch, sizeof(branch));
   /* Any value is acceptable — just verify it doesn't crash and is non-empty */
   assert(branch[0] != '\0');
}

/* --- session_isolation_target: catches the regression where concurrent
 * claude sessions share a main checkout because the SessionStart hook
 * failed to create/point them at per-session managed worktrees. */

static void session_isolation_make_repo(char *tmpdir, size_t tmpdir_len)
{
   /* mkdtemp template MUST live in caller's stack — strncpy so len is safe */
   char tmpl[] = "/tmp/test_sess_iso_XXXXXX";
   assert(mkdtemp(tmpl) != NULL);
   snprintf(tmpdir, tmpdir_len, "%s", tmpl);

   /* Nest so the repo-local worktree path is inside a realistic repository root. */
   char repo[MAX_PATH_LEN];
   snprintf(repo, sizeof(repo), "%s/aimee", tmpdir);
   assert(mkdir(repo, 0755) == 0);

   char cmd[MAX_PATH_LEN + 256];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q 2>/dev/null", repo);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' config user.email 'test@test.com' 2>/dev/null", repo);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "git -C '%s' config user.name 'Test' 2>/dev/null", repo);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd),
            "touch '%s/x' && git -C '%s' add x && git -C '%s' commit -qm 'init' 2>/dev/null", repo,
            repo, repo);
   (void)system(cmd);
}

static void session_isolation_cleanup(const char *tmpdir)
{
   char cmd[MAX_PATH_LEN + 64];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   (void)system(cmd);

   /* Repo-local worktrees are under tmpdir and removed above. */
}

static void test_session_isolation_creates_and_returns_worktree(void)
{
   char tmpdir[MAX_PATH_LEN];
   session_isolation_make_repo(tmpdir, sizeof(tmpdir));
   char repo[MAX_PATH_LEN];
   snprintf(repo, sizeof(repo), "%s/aimee", tmpdir);

   char target[MAX_PATH_LEN];
   int rc = session_isolation_target(repo, "fadc648f-1234-5678", target, sizeof(target), 1);
   assert(rc == 1);

   char expected[MAX_PATH_LEN];
   snprintf(expected, sizeof(expected), "%s/aimee/.aimee/worktrees/fadc648f-1234-56/main", tmpdir);
   assert(strcmp(target, expected) == 0);

   /* Worktree must exist on disk after create_if_missing */
   struct stat st;
   assert(stat(target, &st) == 0 && S_ISDIR(st.st_mode));
   char git_file[MAX_PATH_LEN];
   snprintf(git_file, sizeof(git_file), "%s/.git", target);
   assert(stat(git_file, &st) == 0);

   session_isolation_cleanup(tmpdir);
}

static void test_session_isolation_sanitizes_malicious_sid(void)
{
   char tmpdir[MAX_PATH_LEN];
   session_isolation_make_repo(tmpdir, sizeof(tmpdir));
   char repo[MAX_PATH_LEN];
   snprintf(repo, sizeof(repo), "%s/aimee", tmpdir);

   /* A traversal-laden session id (now reachable: the webchat git panel/editor
    * pass session_id in the request) must NOT escape the worktrees dir. */
   char target[MAX_PATH_LEN];
   int rc = session_isolation_target(repo, "../../../../etc/x", target, sizeof(target), 1);
   assert(rc == 1);
   char prefix[MAX_PATH_LEN];
   snprintf(prefix, sizeof(prefix), "%s/aimee/.aimee/worktrees/", tmpdir);
   assert(strncmp(target, prefix, strlen(prefix)) == 0); /* stays under worktrees/ */
   assert(strstr(target, "..") == NULL);                 /* no traversal component */
   struct stat st;
   assert(stat(target, &st) == 0 && S_ISDIR(st.st_mode)); /* a real, contained worktree */

   session_isolation_cleanup(tmpdir);
}

static void test_session_isolation_skips_when_already_in_same_session_worktree(void)
{
   char tmpdir[MAX_PATH_LEN];
   session_isolation_make_repo(tmpdir, sizeof(tmpdir));
   char repo[MAX_PATH_LEN];
   snprintf(repo, sizeof(repo), "%s/aimee", tmpdir);

   char target[MAX_PATH_LEN];
   int rc = session_isolation_target(repo, "fadc648f-1234-5678", target, sizeof(target), 1);
   assert(rc == 1);

   char nested[MAX_PATH_LEN];
   snprintf(nested, sizeof(nested), "%s/src", target);
   assert(mkdir(nested, 0755) == 0);

   char same_target[MAX_PATH_LEN];
   rc = session_isolation_target(nested, "fadc648f-1234-5678", same_target, sizeof(same_target), 1);
   assert(rc == 0);
   assert(same_target[0] == '\0');

   session_isolation_cleanup(tmpdir);
}

static void test_session_isolation_creates_new_worktree_from_existing_worktree(void)
{
   char tmpdir[MAX_PATH_LEN];
   session_isolation_make_repo(tmpdir, sizeof(tmpdir));
   char repo[MAX_PATH_LEN];
   snprintf(repo, sizeof(repo), "%s/aimee", tmpdir);

   char first[MAX_PATH_LEN];
   int rc = session_isolation_target(repo, "aaaabbbb-1234-5678", first, sizeof(first), 1);
   assert(rc == 1);

   char nested[MAX_PATH_LEN];
   snprintf(nested, sizeof(nested), "%s/src", first);
   assert(mkdir(nested, 0755) == 0);

   char second[MAX_PATH_LEN];
   rc = session_isolation_target(nested, "ccccdddd-1234-5678", second, sizeof(second), 1);
   assert(rc == 1);
   assert(strcmp(first, second) != 0);

   char expected[MAX_PATH_LEN];
   snprintf(expected, sizeof(expected), "%s/aimee/.aimee/worktrees/ccccdddd-1234-56/main", tmpdir);
   assert(strcmp(second, expected) == 0);

   struct stat st;
   assert(stat(second, &st) == 0 && S_ISDIR(st.st_mode));

   session_isolation_cleanup(tmpdir);
}

static void test_session_isolation_skips_when_not_a_git_repo(void)
{
   char tmpdir[] = "/tmp/test_sess_iso_nogit_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char target[MAX_PATH_LEN];
   int rc = session_isolation_target(tmpdir, "fadc648f-1234-5678", target, sizeof(target), 1);
   assert(rc == 0);
   assert(target[0] == '\0');

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   (void)system(cmd);
}

static void test_session_isolation_idempotent_when_worktree_exists(void)
{
   char tmpdir[MAX_PATH_LEN];
   session_isolation_make_repo(tmpdir, sizeof(tmpdir));
   char repo[MAX_PATH_LEN];
   snprintf(repo, sizeof(repo), "%s/aimee", tmpdir);

   /* First call creates the worktree */
   char target1[MAX_PATH_LEN];
   int rc = session_isolation_target(repo, "fadc648f-1234-5678", target1, sizeof(target1), 1);
   assert(rc == 1);

   /* Second call finds it and returns the same path without error */
   char target2[MAX_PATH_LEN];
   rc = session_isolation_target(repo, "fadc648f-1234-5678", target2, sizeof(target2), 1);
   assert(rc == 1);
   assert(strcmp(target1, target2) == 0);

   /* And with create_if_missing=0 when it already exists */
   char target3[MAX_PATH_LEN];
   rc = session_isolation_target(repo, "fadc648f-1234-5678", target3, sizeof(target3), 0);
   assert(rc == 1);
   assert(strcmp(target1, target3) == 0);

   session_isolation_cleanup(tmpdir);
}

static void test_session_isolation_missing_no_create_returns_zero(void)
{
   char tmpdir[MAX_PATH_LEN];
   session_isolation_make_repo(tmpdir, sizeof(tmpdir));
   char repo[MAX_PATH_LEN];
   snprintf(repo, sizeof(repo), "%s/aimee", tmpdir);

   /* With create_if_missing=0 and no pre-existing worktree, refuse. */
   char target[MAX_PATH_LEN];
   int rc = session_isolation_target(repo, "fadc648f-1234-5678", target, sizeof(target), 0);
   assert(rc == 0);
   assert(target[0] == '\0');

   session_isolation_cleanup(tmpdir);
}

static void test_session_isolation_empty_sid_rejected(void)
{
   char target[MAX_PATH_LEN];
   int rc = session_isolation_target("/home/foo/dev/aimee", "", target, sizeof(target), 1);
   assert(rc == 0);
}

/* --- Deeper edge case tests --- */

static void test_classify_path_traversal(void)
{
   /* Path traversal attempts should still classify based on final component */
   classification_t c = classify_path("../../.env");
   assert(c.severity == SEV_BLOCK);

   c = classify_path("/tmp/../../etc/shadow");
   assert(c.severity >= SEV_GREEN); /* Not in sensitive list, but path is suspicious */

   c = classify_path("foo/.env.local");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(".env.production");
   assert(c.severity == SEV_BLOCK);

   c = classify_path("config/.env.backup");
   assert(c.severity == SEV_BLOCK);
}

static void test_classify_edge_cases(void)
{
   /* Empty path */
   classification_t c = classify_path("");
   assert(c.severity == SEV_GREEN);

   /* Path with only extension */
   c = classify_path(".pem");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(".p12");
   assert(c.severity == SEV_BLOCK);

   /* Deep nested sensitive file */
   c = classify_path("deploy/secrets/production/credentials.json");
   assert(c.severity == SEV_BLOCK);

   /* Safe file with misleading name */
   c = classify_path("env_test.go");
   assert(c.severity == SEV_GREEN);
}

static void test_is_write_command_edge_cases(void)
{
   /* Append redirect */
   assert(is_write_command("echo data >> logfile") == 1);

   /* Pipe to write command */
   assert(is_write_command("cat file | tee output.txt") == 1);

   /* sed in-place */
   assert(is_write_command("sed -i 's/old/new/g' file.txt") == 1);

   /* chmod */
   assert(is_write_command("chmod 755 script.sh") == 1);

   /* mv */
   assert(is_write_command("mv old.txt new.txt") == 1);

   /* cp */
   assert(is_write_command("cp src.txt dst.txt") == 1);

   /* Git write commands */
   assert(is_write_command("git add src/foo.c") == 1);
   assert(is_write_command("git restore --staged file.c") == 1);
   assert(is_write_command("git rm old_file.c") == 1);
   assert(is_write_command("git mv old.c new.c") == 1);

   /* Git read-only commands */
   assert(is_write_command("git status") == 0);
   assert(is_write_command("git log --oneline") == 0);
   assert(is_write_command("git diff") == 0);
   assert(is_write_command("git show HEAD") == 0);

   /* Read-only commands with tricky names */
   assert(is_write_command("cat file.txt") == 0);
   assert(is_write_command("head -20 file.txt") == 0);
   assert(is_write_command("wc -l file.txt") == 0);
   assert(is_write_command("diff a.txt b.txt") == 0);
   assert(is_write_command("find . -name '*.c'") == 0);

   /* fd-to-fd redirections — NOT file writes */
   assert(is_write_command("make 2>&1") == 0);
   assert(is_write_command("gcc -o test test.c 2>&1") == 0);
   assert(is_write_command("./run.sh 2>&1 | head") == 0);
   assert(is_write_command("cmd >&2") == 0);
   assert(is_write_command("cmd 1>&2") == 0);
   assert(is_write_command("git grep pattern -- src 2>/dev/null") == 0);
   assert(is_write_command("cmd 1>/dev/null") == 0);
   assert(is_write_command("cmd 2>/dev/null") == 0);
   assert(is_write_command("rg pattern >/dev/null 2>&1") == 0);
   assert(is_write_command("make >> /dev/null") == 0);

   /* File redirections — ARE writes */
   assert(is_write_command("echo hello > file.txt") == 1);
   assert(is_write_command("echo hi > /tmp/out.txt 2>/dev/null") == 1);
   assert(is_write_command("echo hello 2>err.log") == 1);
   assert(is_write_command("echo hello 1>out.log") == 1);

   /* Empty/null command */
   assert(is_write_command("") == 0);
   assert(is_write_command(NULL) == 0);
}

static void test_normalize_path_edge_cases(void)
{
   char buf[MAX_PATH_LEN];

   /* NULL cwd */
   normalize_path("relative", NULL, buf, sizeof(buf));
   assert(strlen(buf) > 0);

   /* Empty path */
   normalize_path("", "/cwd", buf, sizeof(buf));

   /* Very long path component */
   char longpath[512];
   memset(longpath, 'a', 500);
   longpath[500] = '\0';
   normalize_path(longpath, "/cwd", buf, sizeof(buf));
   assert(strlen(buf) > 0);
}

static void test_plan_mode_allows_reads(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_PLAN);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* Read tools should be allowed in plan mode */
   int rc = pre_tool_check("Read", "{\"file_path\":\"/test.c\"}", &state, MODE_APPROVE, "", msg,
                           sizeof(msg));
   assert(rc == 0);

   rc = pre_tool_check("Glob", "{\"pattern\":\"*.c\"}", &state, MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 0);

   rc = pre_tool_check("Grep", "{\"path\":\".\",\"pattern\":\"test\"}", &state, MODE_APPROVE, "",
                       msg, sizeof(msg));
   assert(rc == 0);

   /* Write tool blocked */
   rc = pre_tool_check("Write", "{\"file_path\":\"/test.c\",\"content\":\"x\"}", &state,
                       MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 2);

   /* MultiEdit blocked */
   rc = pre_tool_check("MultiEdit", "{\"file_path\":\"/test.c\"}", &state, MODE_APPROVE, "", msg,
                       sizeof(msg));
   assert(rc == 2);

   /* Bash write command blocked */
   rc = pre_tool_check("Bash", "{\"command\":\"echo x > file.txt\"}", &state, MODE_APPROVE, "", msg,
                       sizeof(msg));
   assert(rc == 2);

   guardrails_close_test_sqlite();
}

static void test_session_state_save_load_roundtrip(void)
{
   char sid[128];
   guardrails_tmp_sid(sid, sizeof(sid), "state-rt");

   session_state_t *original = malloc(sizeof(session_state_t));
   memset(original, 0, sizeof(*original));
   strcpy(original->session_mode, MODE_PLAN);
   strcpy(original->guardrail_mode, MODE_DENY);
   original->active_task_id = 42;
   original->skill_find_symbols_advisory_sent = 1;
   original->skill_condition_waiting_advisory_sent = 1;
   original->skill_tdd_advisory_sent = 1;
   strcpy(original->seen_paths[0], "/path/to/.env");
   strcpy(original->seen_paths[1], "/another/secret.key");
   original->seen_count = 2;
   strcpy(original->worktrees[0].git_root, "/root/proj");
   strcpy(original->worktrees[0].worktree_path, "/root/proj-abc12345");
   original->worktree_count = 1;

   session_state_force_save(original, sid);

   session_state_t *loaded = malloc(sizeof(session_state_t));
   session_state_load(loaded, sid);

   assert(strcmp(loaded->session_mode, MODE_PLAN) == 0);
   assert(strcmp(loaded->guardrail_mode, MODE_DENY) == 0);
   assert(loaded->active_task_id == 42);
   assert(loaded->skill_find_symbols_advisory_sent == 1);
   assert(loaded->skill_condition_waiting_advisory_sent == 1);
   assert(loaded->skill_tdd_advisory_sent == 1);
   assert(loaded->seen_count == 2);
   assert(strcmp(loaded->seen_paths[0], "/path/to/.env") == 0);
   assert(strcmp(loaded->seen_paths[1], "/another/secret.key") == 0);
   assert(loaded->worktree_count == 1);
   assert(strcmp(loaded->worktrees[0].git_root, "/root/proj") == 0);
   assert(strcmp(loaded->worktrees[0].worktree_path, "/root/proj-abc12345") == 0);

   db1_session_state_delete(sid);
   free(original);
   free(loaded);
}

static void test_worktree_mapping_roundtrip(void)
{
   char sid[128];
   guardrails_tmp_sid(sid, sizeof(sid), "wt-mapping");

   session_state_t *state = malloc(sizeof(session_state_t));
   memset(state, 0, sizeof(*state));
   strcpy(state->session_mode, MODE_IMPLEMENT);
   strcpy(state->guardrail_mode, MODE_APPROVE);

   /* Set up worktree mappings */
   strcpy(state->worktrees[0].git_root, "/home/user/myrepo");
   strcpy(state->worktrees[0].worktree_path, "/home/user/myrepo-abc12345");
   strcpy(state->worktrees[1].git_root, "/home/user/other");
   strcpy(state->worktrees[1].worktree_path, "/home/user/other-abc12345");
   state->worktree_count = 2;

   session_state_force_save(state, sid);

   session_state_t *loaded = malloc(sizeof(session_state_t));
   session_state_load(loaded, sid);
   assert(loaded->worktree_count == 2);
   assert(strcmp(loaded->worktrees[0].git_root, "/home/user/myrepo") == 0);
   assert(strcmp(loaded->worktrees[0].worktree_path, "/home/user/myrepo-abc12345") == 0);
   assert(strcmp(loaded->worktrees[1].git_root, "/home/user/other") == 0);
   assert(strcmp(loaded->worktrees[1].worktree_path, "/home/user/other-abc12345") == 0);

   db1_session_state_delete(sid);
   free(state);
   free(loaded);
}

static void test_app_ctx_cfg_pointer(void)
{
   /* Verify that app_ctx_t can carry a config pointer */
   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   assert(ctx.cfg == NULL);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.provider, sizeof(cfg.provider), "test-provider");
   ctx.cfg = &cfg;

   assert(ctx.cfg != NULL);
   assert(strcmp(ctx.cfg->provider, "test-provider") == 0);
}

static void test_malformed_tool_payloads(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* NULL input_json */
   int rc = pre_tool_check("Edit", NULL, &state, MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 0); /* Should handle gracefully */

   /* Empty JSON object */
   rc = pre_tool_check("Edit", "{}", &state, MODE_APPROVE, "/tmp/.aimee/worktrees/test/main", msg,
                       sizeof(msg));
   assert(rc == 0);

   /* Malformed JSON */
   rc = pre_tool_check("Edit", "{broken", &state, MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 0); /* Should not crash */

   /* Very large file_path */
   char big_json[8192];
   char big_path[4000];
   memset(big_path, 'x', sizeof(big_path) - 1);
   big_path[sizeof(big_path) - 1] = '\0';
   snprintf(big_json, sizeof(big_json), "{\"file_path\":\"%s\"}", big_path);
   rc = pre_tool_check("Edit", big_json, &state, MODE_APPROVE, "/tmp/.aimee/worktrees/test/main",
                       msg, sizeof(msg));
   /* Should handle without crash */

   guardrails_close_test_sqlite();
}

/* Clear the anti_patterns table so each guardrails test starts fresh.
 * Tests share the same db1 connection (opened once in test_guardrails main),
 * so without this they would leak rows into each other. */
static void clear_anti_patterns_for_test(void)
{
   anti_pattern_t aps[64];
   int n = db2_anti_pattern_list(aps, 64);
   for (int i = 0; i < n; i++)
      db2_anti_pattern_delete(aps[i].id);
}

static void test_anti_pattern_in_session_warning(void)
{
   guardrails_open_test_sqlite();
   clear_anti_patterns_for_test();

   /* Insert an anti-pattern that phrase-matches "rm -rf" commands. */
   anti_pattern_t ap;
   int rc = db2_anti_pattern_insert("rm -rf", "Do not use rm -rf on project dirs", "test",
                                    "test-ref", 0.9, &ap);
   assert(rc == 0);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];
   const char *worktree_cwd = "/tmp/.aimee/worktrees/test/main";

   /* First hit: should warn but allow (rc=0), msg should contain WARNING */
   rc = pre_tool_check("Bash", "{\"command\":\"rm -rf /tmp/project\"}", &state, MODE_APPROVE,
                       worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "WARNING") != NULL);
   assert(strstr(msg, "1/3") != NULL);

   /* Second hit: still warning */
   rc = pre_tool_check("Bash", "{\"command\":\"rm -rf /tmp/other\"}", &state, MODE_APPROVE,
                       worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "WARNING") != NULL);
   assert(strstr(msg, "2/3") != NULL);

   rc = pre_tool_check("execute_script", "{\"language\":\"bash\",\"body\":\"rm -rf /tmp/script\"}",
                       &state, MODE_APPROVE, worktree_cwd, msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);

   /* Further hits should stay blocked. */
   rc = pre_tool_check("Bash", "{\"command\":\"rm -rf /tmp/again\"}", &state, MODE_APPROVE,
                       worktree_cwd, msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);

   /* Reset clears session hits so the next call allows through as a 1/3. */
   state.ap_hit_count = 0;
   memset(state.ap_hits, 0, sizeof(state.ap_hits));
   rc = pre_tool_check("Bash", "{\"command\":\"rm -rf /tmp/post-reset\"}", &state, MODE_APPROVE,
                       worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "1/3") != NULL);

   guardrails_close_test_sqlite();
}

static void test_anti_pattern_empty_description_falls_back_to_pattern(void)
{
   guardrails_open_test_sqlite();
   clear_anti_patterns_for_test();

   /* Pattern with no description: previous code produced an empty message
    * after the colon. The fallback must surface the pattern string so the
    * agent can tell which row fired. */
   anti_pattern_t ap;
   int rc = db2_anti_pattern_insert("rm -rf", "", "test", "", 0.9, &ap);
   assert(rc == 0);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];
   rc = pre_tool_check("Bash", "{\"command\":\"rm -rf /tmp/a\"}", &state, MODE_APPROVE,
                       "/tmp/.aimee/worktrees/test/main", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "rm -rf") != NULL);
   /* Must not leave the message ending in a dangling colon. */
   size_t mlen = strlen(msg);
   assert(mlen > 0);
   assert(msg[mlen - 1] != ':');
   assert(!(mlen >= 2 && msg[mlen - 2] == ':' && msg[mlen - 1] == ' '));

   guardrails_close_test_sqlite();
}

static void test_anti_pattern_bypass_env(void)
{
   guardrails_open_test_sqlite();
   clear_anti_patterns_for_test();

   anti_pattern_t ap;
   db2_anti_pattern_insert("rm -rf", "d", "test", "", 0.9, &ap);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Pre-seed hit count to threshold so next call would otherwise block. */
   state.ap_hit_count = 1;
   state.ap_hits[0].pattern_id = ap.id;
   state.ap_hits[0].hits = AP_HIT_BLOCK_THRESHOLD;

   setenv("AIMEE_ANTIPATTERNS_BYPASS", "1", 1);
   char msg[512] = "";
   int rc = pre_tool_check("Bash", "{\"command\":\"rm -rf /tmp/xyz\"}", &state, MODE_APPROVE,
                           "/tmp/.aimee/worktrees/test/main", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "BLOCKED") == NULL);
   unsetenv("AIMEE_ANTIPATTERNS_BYPASS");

   guardrails_close_test_sqlite();
}

static void test_anti_pattern_no_match_no_warning(void)
{
   guardrails_open_test_sqlite();
   clear_anti_patterns_for_test();

   /* Pattern that only matches literal "rm -rf". Must not match "ls". */
   anti_pattern_t ap;
   db2_anti_pattern_insert("rm -rf", "Dangerous deletion", "test", "ref", 0.9, &ap);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];
   int rc = pre_tool_check("Bash", "{\"command\":\"ls /tmp\"}", &state, MODE_APPROVE, "/tmp", msg,
                           sizeof(msg));
   assert(rc == 0);
   /* msg should be empty (no anti-pattern match, no other warning) */

   /* Also: a pattern like "git fetch origin" must not fire on commands that
    * merely share a token with it — the old half-tokens matcher failed here. */
   db2_anti_pattern_insert("git fetch origin", "no network fetches", "test", "", 0.9, &ap);
   rc = pre_tool_check("Bash", "{\"command\":\"echo hello && git status\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "WARNING") == NULL);

   guardrails_close_test_sqlite();
}

static void test_known_subagent_tools_blocked(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   char msg[512];

   /* Claude sub-agent tool should be blocked */
   int rc = pre_tool_check("Agent",
                           "{\"subagent_type\":\"Explore\","
                           "\"prompt\":\"Find the installer code\","
                           "\"description\":\"Find installer\"}",
                           &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "guardrails") != NULL);
   assert(strstr(msg, "aimee delegate") != NULL);

   /* Claude Code's actual sub-agent tool name (Task) should be blocked too */
   rc = pre_tool_check("Task",
                       "{\"subagent_type\":\"Explore\","
                       "\"prompt\":\"Find the installer code\","
                       "\"description\":\"Find installer\"}",
                       &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "aimee delegate") != NULL);

   /* Codex sub-agent tool should also be blocked */
   rc = pre_tool_check("spawn_agent",
                       "{\"agent_type\":\"explorer\","
                       "\"message\":\"Find the installer code\"}",
                       &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "guardrails") != NULL);
   assert(strstr(msg, "aimee delegate") != NULL);

   /* Another provider-native remote delegation surface should also be blocked */
   rc = pre_tool_check("RemoteTrigger", "{\"task\":\"Run tests\"}", &state, MODE_APPROVE, "/tmp",
                       msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "guardrails") != NULL);
   assert(strstr(msg, "aimee delegate") != NULL);

   guardrails_close_test_sqlite();
}

static void test_unknown_subagent_surface_blocked(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   char msg[512];

   int rc = pre_tool_check("launch_remote_agent",
                           "{\"message\":\"Investigate the codebase\","
                           "\"role\":\"explorer\","
                           "\"description\":\"delegate task\"}",
                           &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "guardrails") != NULL);

   /* Similar names without sub-agent-shaped payloads should not be trapped. */
   rc = pre_tool_check("agent_status", "{\"query\":\"last run\"}", &state, MODE_APPROVE, "/tmp",
                       msg, sizeof(msg));
   assert(rc == 0);

   guardrails_close_test_sqlite();
}

/* Note: the new worktree enforcement uses git_repo_root() which requires a real
 * git repo. These tests verify the worktree_for_cwd() lookup logic and
 * worktree_sibling_path computation instead of the full pre_tool_check flow,
 * since pre_tool_check's worktree enforcement depends on git_repo_root which
 * is impractical to mock in unit tests. */

static void test_worktree_for_cwd_edge_cases(void)
{
   session_state_t state;
   memset(&state, 0, sizeof(state));

   /* Set up worktree mapping */
   strcpy(state.worktrees[0].git_root, "/root/dev/aimee");
   strcpy(state.worktrees[0].worktree_path, "/root/dev/aimee/.aimee/worktrees/abc12345/main");
   state.worktree_count = 1;

   /* Exact git root match */
   const char *wt = worktree_for_cwd(&state, "/root/dev/aimee");
   assert(wt != NULL);
   assert(strcmp(wt, "/root/dev/aimee/.aimee/worktrees/abc12345/main") == 0);

   /* Subdirectory of git root */
   wt = worktree_for_cwd(&state, "/root/dev/aimee/src/memory.c");
   assert(wt != NULL);

   /* Already inside worktree: should return NULL. */
   wt = worktree_for_cwd(&state, "/root/dev/aimee/.aimee/worktrees/abc12345/main/src/memory.c");
   assert(wt == NULL);

   /* Partial prefix match should NOT match (e.g. /root/dev/aimee2) */
   wt = worktree_for_cwd(&state, "/root/dev/aimee2/src/foo.c");
   assert(wt == NULL);

   /* NULL state */
   wt = worktree_for_cwd(NULL, "/root/dev/aimee/src/foo.c");
   assert(wt == NULL);
}

static void test_hook_call_count_increments(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   assert(state.hook_call_count == 0);

   char msg[1024] = "";
   pre_tool_check("Read", "{\"file_path\":\"/tmp/foo.c\"}", &state, MODE_APPROVE, "/tmp", msg,
                  sizeof(msg));
   assert(state.hook_call_count == 1);

   pre_tool_check("Read", "{\"file_path\":\"/tmp/bar.c\"}", &state, MODE_APPROVE, "/tmp", msg,
                  sizeof(msg));
   assert(state.hook_call_count == 2);

   /* Verify it roundtrips through DB1 */
   char sid[128];
   guardrails_tmp_sid(sid, sizeof(sid), "hook-count");
   state.dirty = 1;
   session_state_force_save(&state, sid);

   session_state_t loaded;
   session_state_load(&loaded, sid);
   assert(loaded.hook_call_count == 2);

   db1_session_state_delete(sid);
   guardrails_close_test_sqlite();
}

static void test_no_worktree_blocks_writes(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[1024] = "";
   int rc = pre_tool_check("Edit",
                           "{\"file_path\":\"/tmp/test.c\","
                           "\"old_string\":\"old\",\"new_string\":\"new\"}",
                           &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "not running in a worktree") != NULL);

   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"echo x > /tmp/test.c\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "not running in a worktree") != NULL);

   msg[0] = '\0';
   rc =
       pre_tool_check("execute_script", "{\"language\":\"bash\",\"body\":\"echo x > /tmp/test.c\"}",
                      &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "not running in a worktree") != NULL);

   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"git status\"}", &state, MODE_APPROVE, "/tmp", msg,
                       sizeof(msg));
   assert(rc == 0);

   guardrails_close_test_sqlite();
}

static void test_shell_command_targeting_worktree_allows_write(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   const char *worktree = "/tmp/project/.aimee/worktrees/session/main";
   char input[1024];
   char msg[512] = {0};
   snprintf(input, sizeof(input), "{\"command\":\"cd '%s' && git add src/file.c\"}", worktree);
   int rc = pre_tool_check("Bash", input, &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc != 2);
   assert(strstr(msg, "not running in a worktree") == NULL);

   msg[0] = '\0';
   snprintf(input, sizeof(input), "{\"command\":\"git -C '%s' add src/file.c\"}", worktree);
   rc = pre_tool_check("Bash", input, &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc != 2);
   assert(strstr(msg, "not running in a worktree") == NULL);

   guardrails_close_test_sqlite();
}

static void test_write_file_targeting_worktree_allows_stale_cwd(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   const char *worktree_file = "/tmp/project/.aimee/worktrees/deleg-39/abcdef/src/new_file.c";
   char input[1024];
   char msg[512] = {0};
   snprintf(input, sizeof(input), "{\"path\":\"%s\",\"content\":\"int x;\\n\"}", worktree_file);
   int rc = pre_tool_check("write_file", input, &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc != 2);
   assert(strstr(msg, "not running in a worktree") == NULL);

   msg[0] = '\0';
   snprintf(input, sizeof(input), "{\"file_path\":\"%s\",\"content\":\"int x;\\n\"}",
            worktree_file);
   rc = pre_tool_check("Write", input, &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc != 2);
   assert(strstr(msg, "not running in a worktree") == NULL);

   guardrails_close_test_sqlite();
}

static void test_external_feature_checkout_allows_writes(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-external-feature-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && git checkout -q -b feat/hook-state",
            tmpdir);
   assert(system(cmd) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"git add f\"}", &state, MODE_APPROVE, tmpdir, msg,
                           sizeof(msg));
   assert(rc != 2);
   assert(strstr(msg, "not running in a worktree") == NULL);

   guardrails_close_test_sqlite();

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_external_default_checkout_blocks_writes(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-external-main-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && git branch -M main",
            tmpdir);
   assert(system(cmd) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"git add f\"}", &state, MODE_APPROVE, tmpdir, msg,
                           sizeof(msg));
   /* Auto-redirect now fires before the hard block: the write is transparently
    * re-targeted at the per-session worktree (rc=3) rather than blocked (rc=2). */
   assert(rc == 3);
   assert(strstr(msg, tmpdir) != NULL);

   guardrails_close_test_sqlite();

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_shell_in_main_checkout_forced_to_worktree(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-force-main-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && git branch -M main",
            tmpdir);
   assert(system(cmd) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[1024] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"git status\"}", &state, MODE_APPROVE, tmpdir,
                           msg, sizeof(msg));
   assert(rc == 3);
   assert(strstr(msg, "cd ") == msg);
   assert(strstr(msg, "/.aimee/worktrees/") != NULL);
   assert(strstr(msg, "git status") != NULL);

   msg[0] = '\0';
   rc = pre_tool_check("Read", "{\"file_path\":\"f\"}", &state, MODE_APPROVE, tmpdir, msg,
                       sizeof(msg));
   assert(rc == 1);
   assert(strstr(msg, "/.aimee/worktrees/") != NULL);
   assert(strstr(msg, "/f") != NULL);

   msg[0] = '\0';
   rc = pre_tool_check("Glob", "{\"pattern\":\"*\"}", &state, MODE_APPROVE, tmpdir, msg,
                       sizeof(msg));
   assert(rc == 1);
   assert(strstr(msg, "/.aimee/worktrees/") != NULL);

   char input[1024];
   snprintf(input, sizeof(input), "{\"command\":\"git -C '%s' status\"}", tmpdir);
   msg[0] = '\0';
   rc = pre_tool_check("Bash", input, &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "forced to worktree") != NULL);

   guardrails_close_test_sqlite();

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_path_tool_redirect_is_cwd_independent(void)
{
   /* Regression: a path tool carries an absolute, cwd-independent target, so its
    * redirect into the session worktree must fire regardless of the current
    * directory. The redirect used to be gated on the cwd NOT being a worktree,
    * so when the cwd happened to already be a worktree an absolute real-checkout
    * path slipped through unrewritten and the write landed in the source
    * checkout, scattering edits across two trees. */
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-cwdindep-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && git branch -M main",
            tmpdir);
   assert(system(cmd) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* The transient state that used to suppress the redirect: cwd is itself a
    * worktree. */
   char wt_cwd[512];
   snprintf(wt_cwd, sizeof(wt_cwd), "%s/.aimee/worktrees/deadbeef/main", tmpdir);

   /* Absolute path into the *real* checkout. */
   char fp_input[1024];
   snprintf(fp_input, sizeof(fp_input), "{\"file_path\":\"%s/f\"}", tmpdir);

   char msg[1024] = {0};
   int rc = pre_tool_check("Read", fp_input, &state, MODE_APPROVE, wt_cwd, msg, sizeof(msg));
   assert(rc == 1); /* redirected (would have been 0 = allowed-through before the fix) */
   assert(strstr(msg, "/.aimee/worktrees/") != NULL);
   assert(strstr(msg, "/f") != NULL);

   msg[0] = '\0';
   rc = pre_tool_check("Edit", fp_input, &state, MODE_APPROVE, wt_cwd, msg, sizeof(msg));
   assert(rc == 1);
   assert(strstr(msg, "/.aimee/worktrees/") != NULL);

   guardrails_close_test_sqlite();

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_c_source_has_bare_string_newline(void)
{
   /* Clean: escape sequence \n, not a literal newline */
   assert(c_source_has_bare_string_newline("printf(\"hello\\n\");") == 0);
   /* Clean: no strings */
   assert(c_source_has_bare_string_newline("int x = 5;") == 0);
   /* Clean: NULL / empty */
   assert(c_source_has_bare_string_newline(NULL) == 0);
   assert(c_source_has_bare_string_newline("") == 0);
   /* Clean: newline in block comment */
   assert(c_source_has_bare_string_newline("/* comment\nspanning lines */") == 0);
   /* Clean: newline in line comment */
   assert(c_source_has_bare_string_newline("// comment\nint x;") == 0);
   /* Clean: backslash-newline continuation inside string (valid C) */
   assert(c_source_has_bare_string_newline("char *s = \"line1 \\\nline2\";") == 0);
   /* Clean: escaped quote inside string */
   assert(c_source_has_bare_string_newline("char *s = \"say \\\"hi\\\"\";") == 0);

   /* Bug: literal newline inside a string literal */
   assert(c_source_has_bare_string_newline("printf(\"ok\ntext\");") == 1);
   assert(c_source_has_bare_string_newline("char *s = \"hello\nworld\";") == 1);
   /* Bug: newline at start of string */
   assert(c_source_has_bare_string_newline("puts(\"\nhello\");") == 1);

   /* Clean: double-quote inside a character literal must not trigger string mode.
    * e.g. strchr(s, '"') should be clean even though it contains a bare '"'. */
   assert(c_source_has_bare_string_newline("char c = '\"';") == 0);
   assert(c_source_has_bare_string_newline("if (c == '\"') return;") == 0);
   assert(c_source_has_bare_string_newline("strchr(href, '\"');\nif (x) {}") == 0);
   /* The tricky case: char literal with '"' followed by a normal string */
   assert(c_source_has_bare_string_newline("char c = '\"'; char *s = \"hello\";") == 0);
}

static void test_write_c_file_bare_newline_blocked(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* Write a .c file with a bare newline in a string — must be blocked */
   int rc = pre_tool_check("Write",
                           "{\"file_path\":\"/tmp/test.c\","
                           "\"content\":\"printf(\\\"ok\\ntext\\\");\"}",
                           &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "newline") != NULL);

   /* Write a .h file with a bare newline — also blocked */
   rc = pre_tool_check("Write",
                       "{\"file_path\":\"/tmp/test.h\","
                       "\"content\":\"#define MSG \\\"hello\\nworld\\\"\"}",
                       &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);

   /* Write a .c file with a clean escape sequence — allowed */
   rc = pre_tool_check("Write",
                       "{\"file_path\":\"/tmp/clean.c\","
                       "\"content\":\"printf(\\\"hello\\\\n\\\");\"}",
                       &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);

   /* Write a non-C file — bare newlines in strings are not our concern */
   rc = pre_tool_check("Write",
                       "{\"file_path\":\"/tmp/test.py\","
                       "\"content\":\"print(\\\"hello\\nworld\\\")\"}",
                       &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);

   guardrails_close_test_sqlite();
}

static void test_git_commands_allowed_by_default(void)
{
   /* Raw git/gh commands in Bash must not be blocked. Regression guard
    * against reintroducing git command interception in guardrails. */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-git-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_setenv("AIMEE_NO_CACHE", "1");

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   char msg[512] = {0};

   /* Simple git command */
   int rc = pre_tool_check("Bash", "{\"command\":\"git status\"}", &state, MODE_APPROVE, "/tmp",
                           msg, sizeof(msg));
   assert(rc == 0);
   assert(msg[0] == '\0' || strstr(msg, "BLOCKED") == NULL);

   /* gh CLI command */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"gh pr list\"}", &state, MODE_APPROVE, "/tmp", msg,
                       sizeof(msg));
   assert(rc == 0);
   assert(msg[0] == '\0' || strstr(msg, "BLOCKED") == NULL);

   /* Compound command with git */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"echo ok && git log --oneline -5\"}", &state,
                       MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(msg[0] == '\0' || strstr(msg, "BLOCKED") == NULL);

   platform_unsetenv("AIMEE_NO_CACHE");
   guardrails_close_test_sqlite();
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
   (void)system(cmd);
}

static void test_bash_command_guard_warns(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* cat <file> — should trigger advisory */
   msg[0] = '\0';
   int rc = pre_tool_check("Bash", "{\"command\":\"cat foo.c\"}", &state, MODE_APPROVE, "/tmp", msg,
                           sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "Read") != NULL);

   /* head [-n N] <file> — should trigger advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"head -n 10 foo.c\"}", &state, MODE_APPROVE, "/tmp",
                       msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "Read") != NULL);

   /* tail <file> — should trigger advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"tail -n 20 bar.log\"}", &state, MODE_APPROVE, "/tmp",
                       msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "Read") != NULL);

   /* grep pattern path — should trigger advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"grep -r pattern src/\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "Grep") != NULL);

   /* find path -name glob — should trigger advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"find . -name '*.c'\"}", &state, MODE_APPROVE, "/tmp",
                       msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "Glob") != NULL);

   guardrails_close_test_sqlite();
}

static void write_skill_dispatch_advisory_config(int enabled)
{
   const char *home = getenv("HOME");
   assert(home && home[0]);
   char dir[512];
   snprintf(dir, sizeof(dir), "%s/.config/aimee", home);
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
   assert(system(cmd) == 0);

   char path[640];
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   write_file_text(path, enabled ? "skills:\n  dispatch:\n    advisory: true\n"
                                 : "skills:\n  dispatch:\n    advisory: false\n");
}

static void test_skill_dispatch_find_symbols_advisory(void)
{
   guardrails_open_test_sqlite();
   write_skill_dispatch_advisory_config(1);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   for (int i = 0; i < MAX_SEEN_PATHS; i++)
      snprintf(state.seen_paths[i], MAX_SEEN_LEN, "/tmp/seen-%d", i);
   state.seen_count = MAX_SEEN_PATHS;

   char msg[512] = "";
   int rc = pre_tool_check("Bash", "{\"command\":\"rg delegate_credentials_acquire src\"}", &state,
                           MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "find-symbols") != NULL);
   assert(strstr(msg, "aimee index find") != NULL);
   assert(state.skill_find_symbols_advisory_sent == 1);

   /* Per-session dedupe suppresses the capability reminder after the first hit;
    * the older generic grep advisory can still surface, and this must keep
    * working even when seen_paths is full. */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"grep -R delegate_credentials_acquire src\"}", &state,
                       MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "find-symbols") == NULL);

   write_skill_dispatch_advisory_config(0);
   guardrails_close_test_sqlite();
}

static void test_skill_dispatch_trigger_advisories(void)
{
   guardrails_open_test_sqlite();
   write_skill_dispatch_advisory_config(1);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = "";
   int rc = pre_tool_check("Bash", "{\"command\":\"sleep 5\"}", &state, MODE_APPROVE, "/tmp", msg,
                           sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "condition-based-waiting") != NULL);
   assert(state.skill_condition_waiting_advisory_sent == 1);

   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"echo ok\"}", &state, MODE_APPROVE, "/tmp", msg,
                       sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "condition-based-waiting") == NULL);

   msg[0] = '\0';
   rc = pre_tool_check("Write", "{\"file_path\":\"src/foo_test.c\",\"content\":\"x\"}", &state,
                       MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "test-driven-development") != NULL);
   assert(state.skill_tdd_advisory_sent == 1);

   write_skill_dispatch_advisory_config(0);
   guardrails_close_test_sqlite();
}

static void test_bash_command_guard_no_warn_pipelines(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* Pipeline: should NOT trigger advisory */
   msg[0] = '\0';
   int rc = pre_tool_check("Bash", "{\"command\":\"cat foo.c | grep bar\"}", &state, MODE_APPROVE,
                           "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") == NULL);

   /* Compound with &&: should NOT trigger advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"cat foo.c && echo done\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") == NULL);

   /* Semicolon-separated: should NOT trigger advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"grep pattern src/; echo ok\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") == NULL);

   /* cat with flags: should NOT trigger advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Bash", "{\"command\":\"cat -n foo.c\"}", &state, MODE_APPROVE, "/tmp", msg,
                       sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") == NULL);

   /* Non-shell tool: should NOT trigger advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Read", "{\"file_path\":\"/tmp/foo.c\"}", &state, MODE_APPROVE, "/tmp", msg,
                       sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") == NULL);

   guardrails_close_test_sqlite();
}

static void test_orch_discipline_source_edit_warns(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   /* is_delegate = 0 by default: orchestrator session */

   char msg[512];
   /* Editing a .c source file should trigger an orchestrator discipline advisory */
   msg[0] = '\0';
   int rc = pre_tool_check("Edit",
                           "{\"file_path\":\"/project/src/foo.c\",\"old_string\":\"x\","
                           "\"new_string\":\"y\"}",
                           &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "orchestrator") != NULL);
   assert(state.orch_direct_edits == 1);

   /* Editing a .go file should also trigger */
   msg[0] = '\0';
   rc = pre_tool_check("Write",
                       "{\"file_path\":\"/project/handler.go\",\"content\":\"package main\"}",
                       &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(state.orch_direct_edits == 2);

   guardrails_close_test_sqlite();
}

static void test_orch_discipline_exempt_paths_no_warn(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* Config file: no advisory */
   msg[0] = '\0';
   int rc = pre_tool_check("Edit",
                           "{\"file_path\":\"/project/.aimee/plan-state.json\","
                           "\"old_string\":\"x\",\"new_string\":\"y\"}",
                           &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "orchestrator") == NULL);
   assert(state.orch_direct_edits == 0);

   /* Proposal markdown: no advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Edit",
                       "{\"file_path\":\"/project/docs/proposals/bar.md\","
                       "\"old_string\":\"x\",\"new_string\":\"y\"}",
                       &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "orchestrator") == NULL);
   assert(state.orch_direct_edits == 0);

   /* CLAUDE.md: no advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Edit",
                       "{\"file_path\":\"/project/CLAUDE.md\","
                       "\"old_string\":\"x\",\"new_string\":\"y\"}",
                       &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "orchestrator") == NULL);

   /* JSON config file: no advisory */
   msg[0] = '\0';
   rc = pre_tool_check("Edit",
                       "{\"file_path\":\"/project/package.json\","
                       "\"old_string\":\"x\",\"new_string\":\"y\"}",
                       &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "orchestrator") == NULL);

   guardrails_close_test_sqlite();
}

static void test_orch_discipline_delegate_no_warn(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   state.is_delegate = 1; /* delegate session */

   char msg[512];
   /* Even editing a source file should NOT warn in a delegate session */
   msg[0] = '\0';
   int rc = pre_tool_check("Edit",
                           "{\"file_path\":\"/project/src/main.c\",\"old_string\":\"x\","
                           "\"new_string\":\"y\"}",
                           &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "orchestrator") == NULL);
   assert(state.orch_direct_edits == 0);

   guardrails_close_test_sqlite();
}

static void test_orch_discipline_nudge_threshold(void)
{
   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   /* Start one below threshold so the first call reaches it */
   state.hook_call_count = 9; /* ORCH_NUDGE_THRESHOLD - 1 */

   char msg[512];

   /* This call increments hook_call_count to 10 (== threshold) — nudge fires */
   msg[0] = '\0';
   int rc = pre_tool_check("Read", "{\"file_path\":\"/project/README.md\"}", &state, MODE_APPROVE,
                           "/project", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "ADVISORY") != NULL);
   assert(strstr(msg, "delegation") != NULL);
   assert(state.orch_nudge_sent == 1);

   /* Nudge is only sent once — subsequent calls are clean */
   msg[0] = '\0';
   rc = pre_tool_check("Read", "{\"file_path\":\"/project/README.md\"}", &state, MODE_APPROVE,
                       "/project", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "delegation") == NULL);

   guardrails_close_test_sqlite();
}

static void test_orch_discipline_state_roundtrip(void)
{
   char test_path[128];
   guardrails_tmp_path(test_path, sizeof(test_path), "test-orch-discipline", ".state");

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   state.orch_direct_edits = 3;
   state.orch_nudge_sent = 1;

   session_state_force_save(&state, test_path);

   session_state_t loaded;
   session_state_load(&loaded, test_path);
   assert(loaded.orch_direct_edits == 3);
   assert(loaded.orch_nudge_sent == 1);
   assert(loaded.is_delegate == 0); /* not persisted */

   unlink(test_path);
}

static void test_semantic_advisory_pre_tool_check(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-gsem-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char *old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   char *old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;
   char *old_bypass =
       getenv("AIMEE_ANTIPATTERNS_BYPASS") ? strdup(getenv("AIMEE_ANTIPATTERNS_BYPASS")) : NULL;
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");
   platform_setenv("AIMEE_ANTIPATTERNS_BYPASS", "1");

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   assert(config_load(&cfg) == 0);
   snprintf(cfg.guardrails_semantic_mode, sizeof(cfg.guardrails_semantic_mode), "advisory");
   snprintf(cfg.guardrails_semantic_command, sizeof(cfg.guardrails_semantic_command),
            "printf '%%s' '{\"outputs\":{\"risk\":{\"overall\":0.83},"
            "\"labels\":[\"verification_bypass\"],\"recommendation\":\"prompt\"},"
            "\"evidence\":{\"explanation\":\"test\"}}'");
   assert(config_save(&cfg) == 0);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   guardrail_event_counts_t before_counts;
   assert(db1_guardrail_event_counts_7d(&before_counts) == 0);

   char msg[512] = {0};
   char payload[256];
   snprintf(payload, sizeof(payload),
            "{\"file_path\":\"/tmp/test_gsem_advisory_%d.txt\",\"content\":\"hello\"}",
            (int)getpid());
   int rc = pre_tool_check("Write", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd,
                           msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "semantic guardrail: high risk") != NULL);
   assert(strstr(msg, "advisory mode") != NULL);

   /* Still advisory mode: a "block" recommendation must be downgraded to a prompt. */
   snprintf(cfg.guardrails_semantic_command, sizeof(cfg.guardrails_semantic_command),
            "printf '%%s' '{\"outputs\":{\"risk\":{\"overall\":0.95},"
            "\"labels\":[\"destructive_change\"],\"recommendation\":\"block\"},"
            "\"evidence\":{\"explanation\":\"test\"}}'");
   assert(config_save(&cfg) == 0);

   msg[0] = '\0';
   snprintf(payload, sizeof(payload),
            "{\"file_path\":\"/tmp/test_gsem_block_advisory_%d.txt\",\"content\":\"hello\"}",
            (int)getpid());
   rc = pre_tool_check("Write", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg,
                       sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "semantic guardrail: high risk") != NULL);

   guardrail_event_counts_t counts;
   assert(db1_guardrail_event_counts_7d(&counts) == 0);
   assert(counts.warn == before_counts.warn);
   assert(counts.prompt == before_counts.prompt + 2);
   assert(counts.block == before_counts.block);
   assert(counts.dry_run == before_counts.dry_run);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   else
   {
      platform_unsetenv("HOME");
   }
   if (old_aimee_home)
   {
      platform_setenv("AIMEE_HOME", old_aimee_home);
      free(old_aimee_home);
   }
   else
   {
      platform_unsetenv("AIMEE_HOME");
   }
   if (old_no_cache)
   {
      platform_setenv("AIMEE_NO_CACHE", old_no_cache);
      free(old_no_cache);
   }
   else
   {
      platform_unsetenv("AIMEE_NO_CACHE");
   }
   if (old_bypass)
   {
      platform_setenv("AIMEE_ANTIPATTERNS_BYPASS", old_bypass);
      free(old_bypass);
   }
   else
   {
      platform_unsetenv("AIMEE_ANTIPATTERNS_BYPASS");
   }
   platform_test_rmrf(tmpdir);
}

/* --- Read-before-write and stale-edit tests --- */

static void test_write_before_read_blocked(void)
{
   /* Create a real temp file so stat() finds it */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_rbw_%d.txt", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("existing content\n", f);
   fclose(f);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char payload[512];
   snprintf(payload, sizeof(payload), "{\"file_path\":\"%s\",\"content\":\"new content\"}", tmp);

   char msg[512];
   int rc = pre_tool_check("Write", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd,
                           msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "without reading it first") != NULL);

   unlink(tmp);
}

static void test_write_new_file_allowed(void)
{
   /* Path that does not exist — should be allowed through */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_new_%d_nonexistent.txt", (int)getpid());
   unlink(tmp); /* ensure it really doesn't exist */

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char payload[512];
   snprintf(payload, sizeof(payload), "{\"file_path\":\"%s\",\"content\":\"hello\"}", tmp);

   char msg[512];
   int rc = pre_tool_check("Write", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd,
                           msg, sizeof(msg));
   assert(rc == 0);
}

static void test_write_after_read_allowed(void)
{
   /* Create a temp file */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_war_%d.txt", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("existing content\n", f);
   fclose(f);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Simulate a read by recording it in state */
   session_record_read(&state, tmp);
   assert(session_path_was_read(&state, tmp) == 1);

   char payload[512];
   snprintf(payload, sizeof(payload), "{\"file_path\":\"%s\",\"content\":\"updated\"}", tmp);

   char msg[512];
   int rc = pre_tool_check("Write", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd,
                           msg, sizeof(msg));
   assert(rc == 0);

   unlink(tmp);
}

static void test_write_truncating_rewrite_blocked(void)
{
   /* A large existing file overwritten with a tiny fraction of its content is
    * the signature of a truncated full-file rewrite — must be blocked. */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_trunc_%d.c", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   for (int i = 0; i < 400; i++) /* ~8 KB, well over the 4 KB floor */
      fputs("int filler_line_used_to_pad_the_file_well_past_4096_bytes;\n", f);
   fclose(f);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   session_record_read(&state, tmp); /* pass the read-before-write guard */

   char payload[512];
   snprintf(payload, sizeof(payload), "{\"file_path\":\"%s\",\"content\":\"int x;\\n\"}", tmp);

   char msg[512];
   int rc = pre_tool_check("Write", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd,
                           msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "smaller") != NULL);

   unlink(tmp);
}

static void test_write_similar_size_rewrite_allowed(void)
{
   /* A large file rewritten with comparable content is a legitimate full
    * rewrite — must pass (only drastic shrinks are blocked). */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_notrunc_%d.c", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   /* Original ~4.7 KB: just over the 4 KB floor. */
   for (int i = 0; i < 80; i++)
      fputs("int filler_line_used_to_pad_the_file_well_past_4096_bytes;\n", f);
   fclose(f);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   session_record_read(&state, tmp);

   /* New content comfortably above 25% of the original (~3 KB vs ~4.7 KB). */
   static char big[8192];
   size_t off = 0;
   for (int i = 0; i < 250 && off < sizeof(big) - 64; i++)
      off += (size_t)snprintf(big + off, sizeof(big) - off, "int kept_%d;\\n", i);
   char payload[8800];
   snprintf(payload, sizeof(payload), "{\"file_path\":\"%s\",\"content\":\"%s\"}", tmp, big);

   char msg[512];
   int rc = pre_tool_check("Write", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd,
                           msg, sizeof(msg));
   assert(rc == 0);

   unlink(tmp);
}

static void test_edit_unchanged_allowed(void)
{
   /* Create a temp file and record its hash */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_euc_%d.txt", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("hello world\n", f);
   fclose(f);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   session_record_read(&state, tmp);

   char payload[512];
   snprintf(payload, sizeof(payload),
            "{\"file_path\":\"%s\",\"old_string\":\"hello\",\"new_string\":\"hi\"}", tmp);

   char msg[512];
   int rc = pre_tool_check("Edit", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg,
                           sizeof(msg));
   assert(rc == 0);

   unlink(tmp);
}

static void test_edit_stale_content_blocked(void)
{
   /* Create temp file, record read, then modify the file externally */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_esc_%d.txt", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("original content\n", f);
   fclose(f);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   session_record_read(&state, tmp);

   /* Externally modify the file after the read */
   f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("different content now\n", f);
   fclose(f);

   char payload[512];
   snprintf(payload, sizeof(payload),
            "{\"file_path\":\"%s\",\"old_string\":\"original\",\"new_string\":\"changed\"}", tmp);

   char msg[512];
   int rc = pre_tool_check("Edit", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg,
                           sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "changed on disk") != NULL);

   unlink(tmp);
}

static void test_edit_unrelated_region_change_allowed(void)
{
   /* The file changes on disk after read, but the targeted old_string is
    * still present — the agent's anchor survived, so the edit should pass. */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_eur_%d.txt", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("stable anchor here\nsome other data\n", f);
   fclose(f);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   session_record_read(&state, tmp);

   /* Externally change an unrelated line but keep the anchor intact. */
   f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("stable anchor here\ndifferent other data now\n", f);
   fclose(f);

   char payload[512];
   snprintf(payload, sizeof(payload),
            "{\"file_path\":\"%s\",\"old_string\":\"stable anchor\",\"new_string\":\"X\"}", tmp);

   char msg[512];
   int rc = pre_tool_check("Edit", payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd, msg,
                           sizeof(msg));
   assert(rc == 0);

   unlink(tmp);
}

static void test_edit_after_own_edit_allowed(void)
{
   /* Back-to-back Edits without an intervening Read should not trip the
    * stale-edit guard: post_tool_update refreshes the cached hash so the
    * agent's own write is the new baseline. */
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_eae_%d.txt", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("alpha beta gamma\n", f);
   fclose(f);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   session_record_read(&state, tmp);

   /* Simulate an Edit by rewriting the file and notifying post_tool_update. */
   f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("alpha BETA gamma\n", f);
   fclose(f);

   char edit_payload[512];
   snprintf(edit_payload, sizeof(edit_payload),
            "{\"file_path\":\"%s\",\"old_string\":\"beta\",\"new_string\":\"BETA\"}", tmp);
   post_tool_update("Edit", edit_payload, &state);

   /* Second Edit on the freshly-modified file should be permitted. */
   char next_payload[512];
   snprintf(next_payload, sizeof(next_payload),
            "{\"file_path\":\"%s\",\"old_string\":\"gamma\",\"new_string\":\"GAMMA\"}", tmp);

   char msg[512];
   int rc = pre_tool_check("Edit", next_payload, &state, MODE_APPROVE, guardrails_test_worktree_cwd,
                           msg, sizeof(msg));
   assert(rc == 0);

   unlink(tmp);
}

static void test_file_contains_substring_basic(void)
{
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_fcs_%d.txt", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("one two three four\n", f);
   fclose(f);

   assert(file_contains_substring(tmp, "two three") == 1);
   assert(file_contains_substring(tmp, "missing") == 0);
   assert(file_contains_substring(tmp, "") == 0);
   assert(file_contains_substring(NULL, "x") == 0);

   unlink(tmp);
   assert(file_contains_substring(tmp, "one") == 0);
}

static void test_file_content_hash_deterministic(void)
{
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "/tmp/test_hash_%d.txt", (int)getpid());
   FILE *f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("test data for hashing\n", f);
   fclose(f);

   uint64_t h1 = file_content_hash(tmp);
   uint64_t h2 = file_content_hash(tmp);
   assert(h1 != 0);
   assert(h1 == h2);

   /* Changing the file changes the hash */
   f = fopen(tmp, "w");
   assert(f != NULL);
   fputs("different data\n", f);
   fclose(f);
   uint64_t h3 = file_content_hash(tmp);
   assert(h3 != h1);

   unlink(tmp);
}

/* --- Verify gate tests for bash git push / gh pr create --- */

static void test_verify_gate_blocks_bash_git_push(void)
{
   /* Set up a temp git repo with global project.yaml enforce: true
    * and no .last-verify record.  Expect git push to be BLOCKED. */
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-vg-push-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && "
            "git checkout -q -b feature-branch",
            tmpdir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       tmpdir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"git push origin feature-branch\"}", &state,
                           MODE_APPROVE, tmpdir, msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "push blocked") != NULL);

   rc = pre_tool_check("execute_script",
                       "{\"language\":\"bash\",\"body\":\"git push origin feature-branch\"}",
                       &state, MODE_APPROVE, tmpdir, msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "push blocked") != NULL);

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
   vy_clear_home();
}

static void test_verify_gate_blocks_bash_gh_pr_create(void)
{
   /* Same setup, verify gate should also block gh pr create. */
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-vg-pr-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && "
            "git checkout -q -b feature-branch",
            tmpdir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       tmpdir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc =
       pre_tool_check("Bash", "{\"command\":\"gh pr create --title 'test' --body '' --base main\"}",
                      &state, MODE_APPROVE, tmpdir, msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "pr create blocked") != NULL);

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
   vy_clear_home();
}

static void test_verify_gate_pr_create_uses_head_commit_not_worktree(void)
{
   /* Regression (#gh-pr-create verify gate): `gh pr create --head <branch>` must
    * verify the COMMITTED tip of the head branch, not the live working tree of
    * the checkout that holds it. A PR ships the committed branch, so a verified
    * head commit must pass the gate even when the working tree is dirty (or, in
    * the real failure, belongs to a stale/foreign worktree). Prior to the fix the
    * gate hashed the working tree and blocked. */
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-vg-prhead-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && git checkout -q -b testing",
            tmpdir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       tmpdir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   /* Record the COMMITTED head tree as verified (clean tree == HEAD tree). */
   char *head_tree = verify_compute_file_hash(tmpdir);
   assert(head_tree != NULL);
   char lv[512];
   snprintf(lv, sizeof(lv), "%s/.aimee/.last-verify", tmpdir);
   {
      char mk[600];
      snprintf(mk, sizeof(mk), "mkdir -p '%s/.aimee'", tmpdir);
      assert(system(mk) == 0);
   }
   FILE *f = fopen(lv, "w");
   assert(f != NULL);
   fprintf(f, "%ld\n%s\nfailed=0/total=1\n", (long)time(NULL), head_tree);
   fclose(f);
   free(head_tree);

   /* Dirty the working tree so its hash no longer matches the verified commit. */
   snprintf(cmd, sizeof(cmd), "cd '%s' && echo dirty >> f", tmpdir);
   assert(system(cmd) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check(
       "Bash", "{\"command\":\"gh pr create --title 'test' --body '' --base main --head testing\"}",
       &state, MODE_APPROVE, tmpdir, msg, sizeof(msg));
   /* The committed head (branch `testing`) is verified, so the verify gate must
    * NOT block even though the working tree is dirty. */
   assert(strstr(msg, "pr create blocked") == NULL);
   (void)rc;

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
   vy_clear_home();
}

static void test_verify_gate_not_enforced_without_enforce_flag(void)
{
   /* With enforce: false, git push must NOT be blocked by the verify gate. */
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-vg-noblock-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && "
            "git checkout -q -b feature-branch",
            tmpdir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       tmpdir, "verify:\n  enforce: false\n  steps:\n    - name: build\n      run: echo ok\n");

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"git push origin feature-branch\"}", &state,
                           MODE_APPROVE, tmpdir, msg, sizeof(msg));
   /* Must NOT be blocked by verify gate (might be blocked by other checks, but not verify) */
   assert(strstr(msg, "push blocked") == NULL);
   assert(strstr(msg, "pr create blocked") == NULL);
   (void)rc;

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
   vy_clear_home();
}

static void test_verify_gate_worktree_uses_own_last_verify(void)
{
   /* Regression: verify state is shared between main and worktrees via the
    * main repo root (.git/commondir resolution), keyed by tree hash rather
    * than commit hash. A push from a worktree CWD must NOT be blocked when
    * a fresh .last-verify exists at <main>/.aimee/.last-verify with a
    * matching tree hash.
    *
    * Note: project.yaml is shared between main and worktree (single global
    * file keyed by the main repo's basename). Verify state lives at
    * <main>/.aimee/.last-verify and is found from any worktree CWD. */
   char maindir[256], wtdir[256];
   snprintf(maindir, sizeof(maindir), "/tmp/aimee-test-vg-wt-main-XXXXXX");
   snprintf(wtdir, sizeof(wtdir), "/tmp/aimee-test-vg-wt-WT-XXXXXX");
   assert(mkdtemp(maindir) != NULL);
   assert(mkdtemp(wtdir) != NULL);

   /* Init main repo */
   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && mkdir .aimee",
            maindir);
   assert(system(cmd) == 0);

   /* Write project.yaml at the global path keyed by the MAIN repo's basename.
    * The worktree resolves to the same key via --git-common-dir. */
   vy_set_global_yaml(
       maindir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   /* Create a git worktree at wtdir */
   snprintf(cmd, sizeof(cmd), "cd '%s' && git worktree add -q '%s' -b wt-branch", maindir, wtdir);
   assert(system(cmd) == 0);

   /* Compute the tree hash for the worktree HEAD (same as main HEAD since the
    * worktree was branched from the same commit). */
   char *wt_hash = verify_compute_file_hash(wtdir);
   assert(wt_hash != NULL);

   /* Write a fresh .last-verify at the MAIN repo root — the shared location
    * that all worktrees resolve to via git --git-common-dir. */
   char lv_main[512];
   snprintf(lv_main, sizeof(lv_main), "%s/.aimee/.last-verify", maindir);
   FILE *f = fopen(lv_main, "w");
   assert(f != NULL);
   fprintf(f, "%ld\n%s\nfailed=0/total=1\n", (long)time(NULL), wt_hash);
   fclose(f);
   free(wt_hash);
   assert(access(lv_main, F_OK) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(wtdir) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   /* cwd = worktree dir; push should NOT be blocked (fresh state at main repo) */
   int rc = pre_tool_check("Bash", "{\"command\":\"git push origin wt-branch\"}", &state,
                           MODE_APPROVE, wtdir, msg, sizeof(msg));
   assert(strstr(msg, "push blocked") == NULL);
   (void)rc;

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", wtdir, maindir);
   system(cmd);
   vy_clear_home();
}

static void test_verify_gate_uses_tool_workdir(void)
{
   /* Regression: Codex-style shell tool calls can carry the intended process
    * directory as tool_input.workdir. The verify gate must use that directory
    * instead of the hook process cwd, otherwise a verified worktree push is
    * checked against the original checkout. */
   char maindir[256], wtdir[256];
   snprintf(maindir, sizeof(maindir), "/tmp/aimee-test-vg-workdir-main-XXXXXX");
   snprintf(wtdir, sizeof(wtdir), "/tmp/aimee-test-vg-workdir-WT-XXXXXX");
   assert(mkdtemp(maindir) != NULL);
   assert(mkdtemp(wtdir) != NULL);

   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && mkdir .aimee",
            maindir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       maindir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   snprintf(cmd, sizeof(cmd), "cd '%s' && git worktree add -q '%s' -b wt-branch", maindir, wtdir);
   assert(system(cmd) == 0);

   char *wt_hash = verify_compute_file_hash(wtdir);
   assert(wt_hash != NULL);

   /* State resolves to main repo root via git --git-common-dir; write there. */
   char lv_main[512];
   snprintf(lv_main, sizeof(lv_main), "%s/.aimee/.last-verify", maindir);
   FILE *f = fopen(lv_main, "w");
   assert(f != NULL);
   fprintf(f, "%ld\n%s\nfailed=0/total=1\n", (long)time(NULL), wt_hash);
   fclose(f);
   free(wt_hash);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(maindir) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char input[1024];
   snprintf(input, sizeof(input), "{\"command\":\"git push origin wt-branch\",\"workdir\":\"%s\"}",
            wtdir);
   char msg[512] = {0};
   int rc = pre_tool_check("Bash", input, &state, MODE_APPROVE, maindir, msg, sizeof(msg));
   assert(rc != 2);
   assert(strstr(msg, "push blocked") == NULL);
   assert(strstr(msg, "main branch") == NULL);

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", wtdir, maindir);
   system(cmd);
   vy_clear_home();
}

static void test_exec_command_tool_shape_is_shell(void)
{
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   assert(is_shell_tool("functions.exec_command"));
   assert(strcmp(guardrails_canonical_tool_name("functions.exec_command"), "Bash") == 0);

   char msg[512] = {0};
   int rc = pre_tool_check("functions.exec_command",
                           "{\"cmd\":\"sed -n '1,5p' README.md\","
                           "\"workdir\":\"/tmp/.aimee/worktrees/abcdef12/main\"}",
                           &state, MODE_APPROVE, "/tmp/main", msg, sizeof(msg));
   assert(rc != 3);
}

static void test_verify_gate_push_branch_uses_branch_worktree(void)
{
   /* Even if verify can resolve the branch's linked worktree, the session cwd
    * must itself be in a worktree before writes are allowed. */
   char maindir[256], wtdir[256];
   snprintf(maindir, sizeof(maindir), "/tmp/aimee-test-vg-branch-main-XXXXXX");
   snprintf(wtdir, sizeof(wtdir), "/tmp/aimee-test-vg-branch-WT-XXXXXX");
   assert(mkdtemp(maindir) != NULL);
   assert(mkdtemp(wtdir) != NULL);

   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && mkdir .aimee",
            maindir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       maindir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   snprintf(cmd, sizeof(cmd), "cd '%s' && git worktree add -q '%s' -b wt-branch", maindir, wtdir);
   assert(system(cmd) == 0);

   char *wt_hash = verify_compute_file_hash(wtdir);
   assert(wt_hash != NULL);

   /* State resolves to main repo root via git --git-common-dir; write there. */
   char lv_main[512];
   snprintf(lv_main, sizeof(lv_main), "%s/.aimee/.last-verify", maindir);
   FILE *f = fopen(lv_main, "w");
   assert(f != NULL);
   fprintf(f, "%ld\n%s\nfailed=0/total=1\n", (long)time(NULL), wt_hash);
   fclose(f);
   free(wt_hash);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(maindir) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"git push origin wt-branch\"}", &state,
                           MODE_APPROVE, maindir, msg, sizeof(msg));
   /* Auto-redirect forces the push into the branch worktree. */
   assert(rc == 3);
   assert(strstr(msg, wtdir) != NULL);

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", wtdir, maindir);
   system(cmd);
   vy_clear_home();
}

static void test_verify_gate_push_head_refspec_uses_destination_worktree(void)
{
   /* HEAD refspec routing resolves the destination branch worktree and forces
    * the command to run there. */
   char maindir[256], wtdir[256];
   snprintf(maindir, sizeof(maindir), "/tmp/aimee-test-vg-headref-main-XXXXXX");
   snprintf(wtdir, sizeof(wtdir), "/tmp/aimee-test-vg-headref-WT-XXXXXX");
   assert(mkdtemp(maindir) != NULL);
   assert(mkdtemp(wtdir) != NULL);

   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && mkdir .aimee",
            maindir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       maindir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   snprintf(cmd, sizeof(cmd), "cd '%s' && git worktree add -q '%s' -b wt-branch", maindir, wtdir);
   assert(system(cmd) == 0);

   char *wt_hash = verify_compute_file_hash(wtdir);
   assert(wt_hash != NULL);

   /* State resolves to main repo root via git --git-common-dir; write there. */
   char lv_main[512];
   snprintf(lv_main, sizeof(lv_main), "%s/.aimee/.last-verify", maindir);
   FILE *f = fopen(lv_main, "w");
   assert(f != NULL);
   fprintf(f, "%ld\n%s\nfailed=0/total=1\n", (long)time(NULL), wt_hash);
   fclose(f);
   free(wt_hash);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(maindir) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"git push origin HEAD:refs/heads/wt-branch\"}",
                           &state, MODE_APPROVE, maindir, msg, sizeof(msg));
   /* Auto-redirect rewrites the command to run from the destination worktree. */
   assert(rc == 3);
   assert(strstr(msg, wtdir) != NULL);

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", wtdir, maindir);
   system(cmd);
   vy_clear_home();
}

static void test_verify_gate_push_registered_worktree_from_nonrepo_cwd(void)
{
   /* Registered worktrees resolve the verify target and force the command to run
    * in that checkout even when the hook cwd is unrelated. */
   char maindir[256], nonrepo[256];
   snprintf(maindir, sizeof(maindir), "/tmp/aimee-test-vg-reg-main-XXXXXX");
   snprintf(nonrepo, sizeof(nonrepo), "/tmp/aimee-test-vg-reg-cwd-XXXXXX");
   assert(mkdtemp(maindir) != NULL);
   assert(mkdtemp(nonrepo) != NULL);

   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && mkdir .aimee",
            maindir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       maindir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   const char *sid = "reg12345-1234-5678";
   /* Session branch is keyed by the first 16 chars of the sid (WORKTREE_SID_KEY_LEN). */
   const char *branch = "aimee/session/reg12345-1234-56";
   assert(worktree_create_sibling(maindir, sid, NULL) == 0);

   char wt_path[MAX_PATH_LEN];
   assert(worktree_sibling_path(maindir, sid, NULL, wt_path, sizeof(wt_path)) == 0);

   char *wt_hash = verify_compute_file_hash(wt_path);
   assert(wt_hash != NULL);

   /* State resolves to main repo root via git --git-common-dir; write there. */
   char lv_main[512];
   snprintf(lv_main, sizeof(lv_main), "%s/.aimee/.last-verify", maindir);
   FILE *f = fopen(lv_main, "w");
   assert(f != NULL);
   fprintf(f, "%ld\n%s\nfailed=0/total=1\n", (long)time(NULL), wt_hash);
   fclose(f);
   free(wt_hash);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(nonrepo) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char input[1024];
   snprintf(input, sizeof(input), "{\"command\":\"git push origin HEAD:refs/heads/%s\"}", branch);
   char msg[512] = {0};
   int rc = pre_tool_check("Bash", input, &state, MODE_APPROVE, nonrepo, msg, sizeof(msg));
   assert(rc == 3);
   assert(strstr(msg, "cd ") == msg);
   assert(strstr(msg, wt_path) != NULL);
   assert(strstr(msg, "push blocked") == NULL);
   assert(strstr(msg, "main branch") == NULL);

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   worktree_cleanup(maindir, sid, NULL);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", nonrepo, maindir);
   system(cmd);
   vy_clear_home();
}

static void test_verify_gate_pr_create_registered_worktree_from_nonrepo_cwd(void)
{
   /* Registered worktrees resolve the PR target and force the command to run in
    * that checkout even when the hook cwd is an unrelated repo. */
   char maindir[256], nonrepo[256];
   snprintf(maindir, sizeof(maindir), "/tmp/aimee-test-vg-prreg-main-XXXXXX");
   snprintf(nonrepo, sizeof(nonrepo), "/tmp/aimee-test-vg-prreg-cwd-XXXXXX");
   assert(mkdtemp(maindir) != NULL);
   assert(mkdtemp(nonrepo) != NULL);

   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && mkdir .aimee",
            maindir);
   assert(system(cmd) == 0);
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo y > f && git add f && "
            "git commit -q -m init",
            nonrepo);
   assert(system(cmd) == 0);

   const char *verify_yaml =
       "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n";
   vy_set_global_yaml(maindir, verify_yaml);
   vy_set_global_yaml(nonrepo, verify_yaml);

   const char *sid = "prreg123-1234-5678";
   /* Session branch is keyed by the first 16 chars of the sid (WORKTREE_SID_KEY_LEN). */
   const char *branch = "aimee/session/prreg123-1234-56";
   assert(worktree_create_sibling(maindir, sid, NULL) == 0);

   char wt_path[MAX_PATH_LEN];
   assert(worktree_sibling_path(maindir, sid, NULL, wt_path, sizeof(wt_path)) == 0);

   char *wt_hash = verify_compute_file_hash(wt_path);
   assert(wt_hash != NULL);

   /* State resolves to main repo root via git --git-common-dir; write there. */
   char lv_main[512];
   snprintf(lv_main, sizeof(lv_main), "%s/.aimee/.last-verify", maindir);
   FILE *f = fopen(lv_main, "w");
   assert(f != NULL);
   fprintf(f, "%ld\n%s\nfailed=0/total=1\n", (long)time(NULL), wt_hash);
   fclose(f);
   free(wt_hash);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(nonrepo) == 0);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char input[1024];
   snprintf(input, sizeof(input),
            "{\"command\":\"gh pr create --head %s --title test --body '' --base main\"}", branch);
   char msg[512] = {0};
   int rc = pre_tool_check("Bash", input, &state, MODE_APPROVE, nonrepo, msg, sizeof(msg));
   assert(rc == 3);
   assert(strstr(msg, "cd ") == msg);
   assert(strstr(msg, wt_path) != NULL);
   assert(strstr(msg, "pr create blocked") == NULL);

   guardrails_close_test_sqlite();

   assert(chdir(saved_cwd) == 0);
   worktree_cleanup(maindir, sid, NULL);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", nonrepo, maindir);
   system(cmd);
   vy_clear_home();
}

static void test_git_push_delete_skips_merged_pr_gate(void)
{
   char tmpdir[256], fake_bin[256], wtdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-push-delete-XXXXXX");
   snprintf(fake_bin, sizeof(fake_bin), "/tmp/aimee-test-push-delete-bin-XXXXXX");
   snprintf(wtdir, sizeof(wtdir), "/tmp/aimee-test-push-delete-WT-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);
   assert(mkdtemp(fake_bin) != NULL);
   assert(mkdtemp(wtdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && "
            "git commit -q -m init && git checkout -q -b merged-branch",
            tmpdir);
   assert(system(cmd) == 0);
   snprintf(cmd, sizeof(cmd), "cd '%s' && git worktree add -q --detach '%s' HEAD", tmpdir, wtdir);
   assert(system(cmd) == 0);

   char fake_gh[512];
   snprintf(fake_gh, sizeof(fake_gh), "%s/gh", fake_bin);
   FILE *f = fopen(fake_gh, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n"
         "if [ \"$1\" = \"pr\" ] && [ \"$2\" = \"list\" ]; then\n"
         "  printf '[{\"number\":537}]\\n'\n"
         "  exit 0\n"
         "fi\n"
         "exit 1\n",
         f);
   fclose(f);
   assert(chmod(fake_gh, 0755) == 0);

   const char *old_path = getenv("PATH");
   char *saved_path = old_path ? strdup(old_path) : NULL;
   char new_path[1024];
   snprintf(new_path, sizeof(new_path), "%s:%s", fake_bin, old_path ? old_path : "");
   setenv("PATH", new_path, 1);

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"git push origin --delete merged-branch\"}",
                           &state, MODE_APPROVE, wtdir, msg, sizeof(msg));
   assert(rc != 2);
   assert(strstr(msg, "branch has a merged PR") == NULL);
   assert(strstr(msg, "push blocked") == NULL);

   guardrails_close_test_sqlite();

   if (saved_path)
   {
      setenv("PATH", saved_path, 1);
      free(saved_path);
   }
   else
      unsetenv("PATH");

   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s' '%s'", fake_bin, wtdir, tmpdir);
   system(cmd);
}

static void test_git_push_delete_does_not_skip_later_push_gate(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-push-delete-chain-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && git commit -q -m init",
            tmpdir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       tmpdir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check(
       "Bash", "{\"command\":\"git push origin --delete old && git push origin feature\"}", &state,
       MODE_APPROVE, tmpdir, msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "push blocked") != NULL);

   guardrails_close_test_sqlite();

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
   vy_clear_home();
}

static void test_bash_git_push_detection_ignores_quoted_text(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/aimee-test-push-quoted-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && echo x > f && git add f && git commit -q -m init",
            tmpdir);
   assert(system(cmd) == 0);

   vy_set_global_yaml(
       tmpdir, "verify:\n  enforce: true\n  steps:\n    - name: build\n      run: echo ok\n");

   guardrails_open_test_sqlite();
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512] = {0};
   int rc = pre_tool_check("Bash", "{\"command\":\"printf 'git push origin feature'\"}", &state,
                           MODE_APPROVE, tmpdir, msg, sizeof(msg));
   assert(rc != 2);
   assert(strstr(msg, "push blocked") == NULL);

   guardrails_close_test_sqlite();

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
   vy_clear_home();
}

static void test_read_tracking_state_roundtrip(void)
{
   char sid[64];
   snprintf(sid, sizeof(sid), "test-rts-%d", (int)getpid());

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Add fake read records */
   snprintf(state.read_paths[0], MAX_SEEN_LEN, "/tmp/file_a.c");
   snprintf(state.read_paths[1], MAX_SEEN_LEN, "/tmp/file_b.h");
   state.read_path_count = 2;

   state.file_hashes[0].content_hash = 0xDEADBEEFCAFEBABEULL;
   snprintf(state.file_hashes[0].path, MAX_SEEN_LEN, "/tmp/file_a.c");
   state.file_hash_count = 1;
   state.dirty = 1;

   session_state_force_save(&state, sid);

   session_state_t loaded;
   session_state_load(&loaded, sid);

   assert(loaded.read_path_count == 2);
   assert(strcmp(loaded.read_paths[0], "/tmp/file_a.c") == 0);
   assert(strcmp(loaded.read_paths[1], "/tmp/file_b.h") == 0);
   assert(loaded.file_hash_count == 1);
   assert(strcmp(loaded.file_hashes[0].path, "/tmp/file_a.c") == 0);
   assert(loaded.file_hashes[0].content_hash == 0xDEADBEEFCAFEBABEULL);

   db1_session_state_delete(sid);
}

static void test_workflow_parse_pr_target(void)
{
   char sig[64] = "", rule[512] = "";
   int ok = workflow_parse_bash_signal("gh pr create --base testing --title foo", sig, sizeof(sig),
                                       rule, sizeof(rule));
   assert(ok == 1);
   assert(strcmp(sig, "pr-target") == 0);
   assert(strstr(rule, "testing") != NULL);

   /* Quoted target and equals form both accepted. */
   ok = workflow_parse_bash_signal("gh pr create --base='develop'", sig, sizeof(sig), rule,
                                   sizeof(rule));
   assert(ok == 1);
   assert(strstr(rule, "develop") != NULL);

   /* Missing --base → no signal */
   ok =
       workflow_parse_bash_signal("gh pr create --title foo", sig, sizeof(sig), rule, sizeof(rule));
   assert(ok == 0);
}

static void test_workflow_parse_test_command(void)
{
   char sig[64] = "", rule[512] = "";
   int ok = workflow_parse_bash_signal("make test", sig, sizeof(sig), rule, sizeof(rule));
   assert(ok == 1);
   assert(strcmp(sig, "test-command") == 0);
   assert(strstr(rule, "make test") != NULL);

   ok = workflow_parse_bash_signal("pytest tests/", sig, sizeof(sig), rule, sizeof(rule));
   assert(ok == 1);
   assert(strcmp(sig, "test-command") == 0);
   assert(strstr(rule, "pytest") != NULL);

   ok = workflow_parse_bash_signal("cd src && make unit-tests", sig, sizeof(sig), rule,
                                   sizeof(rule));
   assert(ok == 1);
   assert(strstr(rule, "make unit-tests") != NULL);

   /* `grep make test file` must not trip the heuristic. */
   ok = workflow_parse_bash_signal("grep 'make test' README.md", sig, sizeof(sig), rule,
                                   sizeof(rule));
   assert(ok == 0);
}

static void test_workflow_parse_active_branch(void)
{
   char sig[64] = "", rule[512] = "";
   int ok = workflow_parse_bash_signal("git push origin feat/login", sig, sizeof(sig), rule,
                                       sizeof(rule));
   assert(ok == 1);
   assert(strcmp(sig, "active-branch") == 0);
   assert(strstr(rule, "feat/login") != NULL);

   /* Pushes to main/master are too generic to learn from. */
   ok = workflow_parse_bash_signal("git push origin main", sig, sizeof(sig), rule, sizeof(rule));
   assert(ok == 0);

   ok = workflow_parse_bash_signal("git push -u origin master", sig, sizeof(sig), rule,
                                   sizeof(rule));
   assert(ok == 0);
}

static void test_workflow_parse_negative(void)
{
   char sig[64] = "", rule[512] = "";
   int ok = workflow_parse_bash_signal("ls -la", sig, sizeof(sig), rule, sizeof(rule));
   assert(ok == 0);
   ok = workflow_parse_bash_signal("", sig, sizeof(sig), rule, sizeof(rule));
   assert(ok == 0);
}

int main(void)
{
   /* Don't try to autospawn aimee-kb from kb_client; the test fixture
    * doesn't run a kb daemon and pre_tool_check now routes
    * anti-pattern checks through kb_client. Without this env, the
    * spawn attempt blocks the test indefinitely. */
   setenv("AIMEE_KB_NO_AUTOSTART", "1", 1);
   /* Ignore SIGPIPE: kb_client may write to a closed/failed unix
    * socket while attempting the anti-pattern RPC; the default
    * action would terminate the test process before "all tests
    * passed" prints. The production daemons set this in their
    * own main(); tests need it explicitly. */
   signal(SIGPIPE, SIG_IGN);

   char suite_home[512];
   snprintf(suite_home, sizeof(suite_home), "%s/aimee-test-guardrails-home-XXXXXX",
            platform_tmpdir());
   assert(platform_mkdtemp(suite_home) != NULL);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char *old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   char *old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;
   char *old_bundled_skills =
       getenv("AIMEE_BUNDLED_SKILLS_DIR") ? strdup(getenv("AIMEE_BUNDLED_SKILLS_DIR")) : NULL;
   platform_setenv("HOME", suite_home);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");
   {
      char cwd[512];
      char bundled_skills[1024];
      struct stat st;
      assert(getcwd(cwd, sizeof(cwd)) != NULL);
      snprintf(bundled_skills, sizeof(bundled_skills), "%s/skills", cwd);
      if (stat(bundled_skills, &st) != 0 || !S_ISDIR(st.st_mode))
         snprintf(bundled_skills, sizeof(bundled_skills), "%s/../skills", cwd);
      platform_setenv("AIMEE_BUNDLED_SKILLS_DIR", bundled_skills);
   }

   /* Session_state tests round-trip through DB1. Open a throwaway sqlite
    * file for the test run; db1_init applies the schema on first open. */
   char db_path[128];
   snprintf(db_path, sizeof(db_path), "/tmp/test-guardrails-db1-%d.sqlite", (int)getpid());
   unlink(db_path);
   assert(db1_init(db_path) == 0);
   /* anti_patterns is DB2 (Postgres). */

   test_classify_sensitive();
   test_classify_database();
   test_classify_safe();
   test_classify_path_traversal();
   test_classify_edge_cases();
   test_is_write_command();
   test_policy_file_overrides_defaults();
   test_policy_file_reloads_on_change();
   test_is_write_command_edge_cases();
   test_normalize_path();
   test_normalize_path_edge_cases();
   test_plan_mode_blocks_writes();
   test_plan_mode_allows_reads();
   test_session_id();
   test_session_id_override();
   test_canonical_tool_names();
   test_session_state_worktrees();
   test_session_state_save_load_roundtrip();
   test_worktree_mapping_roundtrip();
   test_app_ctx_cfg_pointer();
   test_worktree_for_cwd();
   test_worktree_prefers_specific_git_root();
   test_worktree_sibling_path();
   test_worktree_detect_base_branch_active();
   test_worktree_detect_base_branch_local_default();
   test_worktree_detect_base_branch_fallback();
   test_session_isolation_creates_and_returns_worktree();
   test_session_isolation_sanitizes_malicious_sid();
   test_session_isolation_skips_when_already_in_same_session_worktree();
   test_session_isolation_creates_new_worktree_from_existing_worktree();
   test_session_isolation_skips_when_not_a_git_repo();
   test_session_isolation_idempotent_when_worktree_exists();
   test_session_isolation_missing_no_create_returns_zero();
   test_session_isolation_empty_sid_rejected();
   test_worktree_for_cwd_edge_cases();
   test_malformed_tool_payloads();
   test_anti_pattern_in_session_warning();
   test_anti_pattern_empty_description_falls_back_to_pattern();
   test_anti_pattern_bypass_env();
   test_anti_pattern_no_match_no_warning();
   test_known_subagent_tools_blocked();
   test_unknown_subagent_surface_blocked();
   test_hook_call_count_increments();
   test_no_worktree_blocks_writes();
   test_shell_command_targeting_worktree_allows_write();
   test_write_file_targeting_worktree_allows_stale_cwd();
   test_external_feature_checkout_allows_writes();
   test_external_default_checkout_blocks_writes();
   test_shell_in_main_checkout_forced_to_worktree();
   test_path_tool_redirect_is_cwd_independent();
   test_git_commands_allowed_by_default();
   test_c_source_has_bare_string_newline();
   test_write_c_file_bare_newline_blocked();
   test_bash_command_guard_warns();
   test_skill_dispatch_find_symbols_advisory();
   test_skill_dispatch_trigger_advisories();
   test_bash_command_guard_no_warn_pipelines();
   test_orch_discipline_source_edit_warns();
   test_orch_discipline_exempt_paths_no_warn();
   test_orch_discipline_delegate_no_warn();
   test_orch_discipline_nudge_threshold();
   test_orch_discipline_state_roundtrip();
   test_semantic_advisory_pre_tool_check();
   test_write_before_read_blocked();
   test_write_new_file_allowed();
   test_write_after_read_allowed();
   test_write_truncating_rewrite_blocked();
   test_write_similar_size_rewrite_allowed();
   test_edit_unchanged_allowed();
   test_edit_stale_content_blocked();
   test_edit_unrelated_region_change_allowed();
   test_edit_after_own_edit_allowed();
   test_file_contains_substring_basic();
   test_file_content_hash_deterministic();
   test_read_tracking_state_roundtrip();
   test_verify_gate_blocks_bash_git_push();
   test_verify_gate_blocks_bash_gh_pr_create();
   test_verify_gate_pr_create_uses_head_commit_not_worktree();
   test_verify_gate_not_enforced_without_enforce_flag();
   test_verify_gate_worktree_uses_own_last_verify();
   test_verify_gate_uses_tool_workdir();
   test_exec_command_tool_shape_is_shell();
   test_verify_gate_push_branch_uses_branch_worktree();
   test_verify_gate_push_head_refspec_uses_destination_worktree();
   test_verify_gate_push_registered_worktree_from_nonrepo_cwd();
   test_verify_gate_pr_create_registered_worktree_from_nonrepo_cwd();
   test_git_push_delete_skips_merged_pr_gate();
   test_git_push_delete_does_not_skip_later_push_gate();
   test_bash_git_push_detection_ignores_quoted_text();
   test_workflow_parse_pr_target();
   test_workflow_parse_test_command();
   test_workflow_parse_active_branch();
   test_workflow_parse_negative();
   db1_shutdown();
   unlink(db_path);
   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   else
   {
      platform_unsetenv("HOME");
   }
   if (old_aimee_home)
   {
      platform_setenv("AIMEE_HOME", old_aimee_home);
      free(old_aimee_home);
   }
   else
   {
      platform_unsetenv("AIMEE_HOME");
   }
   if (old_no_cache)
   {
      platform_setenv("AIMEE_NO_CACHE", old_no_cache);
      free(old_no_cache);
   }
   else
   {
      platform_unsetenv("AIMEE_NO_CACHE");
   }
   if (old_bundled_skills)
   {
      platform_setenv("AIMEE_BUNDLED_SKILLS_DIR", old_bundled_skills);
      free(old_bundled_skills);
   }
   else
   {
      platform_unsetenv("AIMEE_BUNDLED_SKILLS_DIR");
   }
   platform_test_rmrf(suite_home);

   /* Provider-native sub-agent spawns canonicalize to "Subagent" (the
    * redirect-to-delegate trigger in the hook); ordinary tools do not. */
   assert(strcmp(guardrails_canonical_tool_name("Task"), "Subagent") == 0);
   assert(strcmp(guardrails_canonical_tool_name("Agent"), "Subagent") == 0);
   assert(strcmp(guardrails_canonical_tool_name("spawn_agent"), "Subagent") == 0);
   assert(strcmp(guardrails_canonical_tool_name("Bash"), "Subagent") != 0);
   assert(strcmp(guardrails_canonical_tool_name("Read"), "Subagent") != 0);

   printf("guardrails: all tests passed\n");
   return 0;
}
