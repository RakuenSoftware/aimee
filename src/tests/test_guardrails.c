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

#include "test_guardrails_cases_a.inc"
#include "test_guardrails_cases_b.inc"

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
