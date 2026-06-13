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

   /* Without work_name: session-level worktree. */
   int rc = worktree_sibling_path("/root/dev/aimee", "fadc648f-1234-5678", NULL, buf, sizeof(buf));
   assert(rc == 0);
   assert(strcmp(buf, "/root/dev/aimee/.aimee/worktrees/fadc648f/main") == 0);

   /* With work_name: per-delegate worktree. */
   rc = worktree_sibling_path("/root/dev/aimee", "fadc648f-1234-5678", "task01", buf, sizeof(buf));
   assert(rc == 0);
   assert(strcmp(buf, "/root/dev/aimee/.aimee/worktrees/fadc648f/task01") == 0);
}

static void test_worktree_detect_base_branch_active(void)
{
   /* Create a temporary git repo, checkout a named branch, and verify
    * that worktree_detect_base_branch() returns that branch name. */
   char tmpdir[] = "/tmp/test_wt_branch_XXXXXX";
   if (mkdtemp(tmpdir) == NULL)
   {
      fprintf(stderr, "test_worktree_detect_base_branch_active: mkdtemp failed, skipping\n");
      return;
   }

   /* Init repo and create a commit so the branch ref exists */
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
   /* Create and checkout a distinct branch */
   snprintf(cmd, sizeof(cmd), "git -C '%s' checkout -qb 'feat/test-branch' 2>/dev/null", tmpdir);
   (void)system(cmd);

   char branch[64];
   worktree_detect_base_branch(tmpdir, branch, sizeof(branch));
   assert(strcmp(branch, "feat/test-branch") == 0);

   /* Cleanup */
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
   snprintf(expected, sizeof(expected), "%s/aimee/.aimee/worktrees/fadc648f/main", tmpdir);
   assert(strcmp(target, expected) == 0);

   /* Worktree must exist on disk after create_if_missing */
   struct stat st;
   assert(stat(target, &st) == 0 && S_ISDIR(st.st_mode));
   char git_file[MAX_PATH_LEN];
   snprintf(git_file, sizeof(git_file), "%s/.git", target);
   assert(stat(git_file, &st) == 0);

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
   snprintf(expected, sizeof(expected), "%s/aimee/.aimee/worktrees/ccccdddd/main", tmpdir);
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
