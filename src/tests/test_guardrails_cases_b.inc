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
   cfg.guardrails_semantic_enabled = 1;
   cfg.guardrails_semantic_dry_run = 0;
   cfg.guardrails_semantic_advisory_only = 1;
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

   cfg.guardrails_semantic_allow_ml_only_block = 1;
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
   const char *branch = "aimee/session/reg12345";
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
   const char *branch = "aimee/session/prreg123";
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
