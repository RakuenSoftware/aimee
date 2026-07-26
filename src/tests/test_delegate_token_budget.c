/* test_delegate_token_budget.c: unit tests for delegate_prompt_limit() and
 * delegate_token_budget_load() */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "agent_coord.h"
#include "model_registry.h"
#include "platform_path.h"
#include "platform_test_util.h"

/* --- helpers --- */

static char g_tmpdir[256];

static void setup_tmpdir(void)
{
   snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/aimee-test-dtb-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(g_tmpdir) != NULL);
}

static void cleanup_tmpdir(void)
{
   platform_test_rmrf(g_tmpdir);
   g_tmpdir[0] = '\0';
}

/* Build a string of n_chars 'x' characters (null-terminated). Caller frees. */
static char *make_long_string(size_t n_chars)
{
   char *s = malloc(n_chars + 1);
   assert(s != NULL);
   memset(s, 'x', n_chars);
   s[n_chars] = '\0';
   return s;
}

/* --- delegate_prompt_limit tests --- */

static void test_under_budget_passthrough(void)
{
   const char *prompt = "Short prompt that is well under budget.";
   char *out = delegate_prompt_limit(prompt, DELEGATE_TOKEN_BUDGET_DEFAULT);
   assert(out != NULL);
   assert(strcmp(out, prompt) == 0);
   free(out);
   printf("  under_budget_passthrough: ok\n");
}

static void test_null_prompt_returns_null(void)
{
   char *out = delegate_prompt_limit(NULL, DELEGATE_TOKEN_BUDGET_DEFAULT);
   assert(out == NULL);
   printf("  null_prompt_returns_null: ok\n");
}

static void test_over_budget_truncated(void)
{
   /* 100K chars = 25K tokens, well over 20K default budget */
   char *big = make_long_string(100 * 1024);
   char *out = delegate_prompt_limit(big, DELEGATE_TOKEN_BUDGET_DEFAULT);
   assert(out != NULL);
   /* Output must be shorter than input */
   assert(strlen(out) < strlen(big));
   /* Must include the truncation marker */
   assert(strstr(out, "[TRUNCATED") != NULL);
   free(big);
   free(out);
   printf("  over_budget_truncated: ok\n");
}

static void test_skill_content_truncated_first(void)
{
   /* Construct a prompt with a short base and large skill section */
   const char *base = "You are an expert agent. Complete the task.\n";
   /* 80K chars of skill content = 20K tokens of skill alone */
   char *skill_content = make_long_string(80 * 1024);

   size_t total = strlen(base) + strlen("\n### ACTIVE SKILL: foo\n") + strlen(skill_content) + 1;
   char *prompt = malloc(total);
   assert(prompt != NULL);
   snprintf(prompt, total, "%s\n### ACTIVE SKILL: foo\n%s", base, skill_content);

   char *out = delegate_prompt_limit(prompt, 20000);
   assert(out != NULL);
   /* Base must be preserved */
   assert(strncmp(out, base, strlen(base)) == 0);
   /* Skill marker must still be present (we only cut skill content, not the marker) */
   assert(strstr(out, "### ACTIVE SKILL:") != NULL);
   /* Truncation marker must appear */
   assert(strstr(out, "[TRUNCATED") != NULL);
   /* Full skill content must NOT be present */
   assert(strlen(out) < strlen(prompt));

   free(skill_content);
   free(prompt);
   free(out);
   printf("  skill_content_truncated_first: ok\n");
}

static void test_task_description_minimum_preserved(void)
{
   /* Budget of 100 tokens = 400 chars; minimum is 2K tokens = 8K chars.
    * The function should clamp to the minimum so the output is not smaller
    * than the minimum task size. */
   char *big = make_long_string(100 * 1024);
   /* Use a very small explicit budget */
   char *out = delegate_prompt_limit(big, 100);
   assert(out != NULL);
   /* Output should be at least DELEGATE_TOKEN_BUDGET_MIN_TASK * 4 chars long
    * (plus the truncation marker). */
   assert(strlen(out) >= (size_t)(DELEGATE_TOKEN_BUDGET_MIN_TASK * 4));
   assert(strstr(out, "[TRUNCATED") != NULL);
   free(big);
   free(out);
   printf("  task_description_minimum_preserved: ok\n");
}

static void test_truncated_output_is_valid_string(void)
{
   char *big = make_long_string(200 * 1024);
   char *out = delegate_prompt_limit(big, 10000);
   assert(out != NULL);
   /* strlen must not overrun — just verifying it completes */
   size_t len = strlen(out);
   assert(len > 0);
   assert(out[len] == '\0');
   free(big);
   free(out);
   printf("  truncated_output_is_valid_string: ok\n");
}

/* --- Progressive truncation order tests --- */

static void test_file_refs_truncated_before_skill(void)
{
   /* Build a prompt with: short base + file reference blocks + skill section.
    * Budget is tight enough that something must be cut.
    * File refs should be cut first, skill preserved if possible. */
   const char *base = "You are an agent.\n";
   char *file_content = make_long_string(40 * 1024); /* 10K tokens */

   /* Build: base + file ref block + skill section */
   size_t total = strlen(base) + 20 + strlen(file_content) + 50 + 4096 + 1;
   char *prompt = malloc(total);
   assert(prompt != NULL);
   snprintf(prompt, total,
            "%s\n--- @big_file.c ---\n%s\n---\n### ACTIVE SKILL: test\nSkill content here.", base,
            file_content);

   /* Budget allows base + skill but not file ref (set to ~3K tokens) */
   char *out = delegate_prompt_limit(prompt, 3000);
   assert(out != NULL);
   /* File reference should have been removed */
   assert(strstr(out, "--- @big_file.c ---") == NULL);
   /* Truncation marker for file content should be present */
   assert(strstr(out, "[TRUNCATED: injected file content") != NULL);
   /* Skill section should still be present (it's small enough) */
   assert(strstr(out, "### ACTIVE SKILL: test") != NULL);
   assert(strstr(out, "Skill content here.") != NULL);

   free(file_content);
   free(prompt);
   free(out);
   printf("  file_refs_truncated_before_skill: ok\n");
}

static void test_context_sections_truncated_before_skill(void)
{
   /* Build a prompt with a context section and skill section.
    * Context should be removed before skill. */
   char *ctx_content = make_long_string(40 * 1024);
   size_t total = 256 + strlen(ctx_content) + 256;
   char *prompt = malloc(total);
   assert(prompt != NULL);
   snprintf(prompt, total,
            "Base instructions.\n# Relevant Context\n%s\n### ACTIVE SKILL: test\nSmall skill.",
            ctx_content);

   /* Budget tight enough to require cutting context */
   char *out = delegate_prompt_limit(prompt, 3000);
   assert(out != NULL);
   /* Context section should have been removed */
   assert(strstr(out, "# Relevant Context") == NULL);
   assert(strstr(out, "[TRUNCATED: context section") != NULL);
   /* Skill should remain */
   assert(strstr(out, "### ACTIVE SKILL: test") != NULL);

   free(ctx_content);
   free(prompt);
   free(out);
   printf("  context_sections_truncated_before_skill: ok\n");
}

static void test_multiple_file_refs_removed_progressively(void)
{
   /* Build a prompt with two file reference blocks. The last one should
    * be removed first, then the first one if still over budget. */
   char *file1 = make_long_string(20 * 1024);
   char *file2 = make_long_string(20 * 1024);
   size_t total = 256 + strlen(file1) + strlen(file2) + 256;
   char *prompt = malloc(total);
   assert(prompt != NULL);
   snprintf(prompt, total,
            "Task: do stuff.\n--- @first.c ---\n%s\n---\n--- @second.c ---\n%s\n---\nEnd.", file1,
            file2);

   /* Budget allows base + one file but not both (~8K tokens) */
   char *out = delegate_prompt_limit(prompt, 8000);
   assert(out != NULL);
   /* At least one file ref should be removed */
   assert(strstr(out, "[TRUNCATED: injected file content") != NULL);

   free(file1);
   free(file2);
   free(prompt);
   free(out);
   printf("  multiple_file_refs_removed_progressively: ok\n");
}

static void test_preloaded_file_contents_truncated(void)
{
   /* Test that "# Pre-loaded File Contents" sections are recognized
    * as context and truncated in phase 2. */
   char *files = make_long_string(40 * 1024);
   size_t total = 256 + strlen(files) + 128;
   char *prompt = malloc(total);
   assert(prompt != NULL);
   snprintf(prompt, total, "Task description.\n# Pre-loaded File Contents\n%s\nEnd of prompt.",
            files);

   char *out = delegate_prompt_limit(prompt, 3000);
   assert(out != NULL);
   assert(strstr(out, "# Pre-loaded File Contents") == NULL);
   assert(strstr(out, "[TRUNCATED: context section") != NULL);
   /* Task description must be preserved */
   assert(strstr(out, "Task description.") != NULL);

   free(files);
   free(prompt);
   free(out);
   printf("  preloaded_file_contents_truncated: ok\n");
}

/* --- delegate_token_budget_load tests ---
 *
 * delegate_token_budget_load reads ~/.config/aimee/projects/<name>/project.yaml,
 * keyed by the basename of the project's main repo root. We override HOME so
 * the global path resolves under a sandbox dir, then write project.yaml there. */

static char g_dtb_saved_home[4096];
static int g_dtb_home_was_set;
static char g_dtb_saved_aimee_home[4096];
static int g_dtb_aimee_home_was_set;
static char g_dtb_saved_aimee_profile[4096];
static int g_dtb_aimee_profile_was_set;
static char g_dtb_fake_home[256];

static void dtb_save_env(const char *name, char *saved, size_t saved_len, int *was_set)
{
   const char *old = getenv(name);
   if (old)
   {
      snprintf(saved, saved_len, "%s", old);
      *was_set = 1;
   }
   else
   {
      saved[0] = '\0';
      *was_set = 0;
   }
}

static void dtb_restore_env(const char *name, const char *saved, int was_set)
{
   if (was_set)
      assert(platform_setenv(name, saved) == 0);
   else
      assert(platform_unsetenv(name) == 0);
}

static void dtb_setup_home(void)
{
   if (g_dtb_fake_home[0] != '\0')
      return;

   dtb_save_env("HOME", g_dtb_saved_home, sizeof(g_dtb_saved_home), &g_dtb_home_was_set);
   dtb_save_env("AIMEE_HOME", g_dtb_saved_aimee_home, sizeof(g_dtb_saved_aimee_home),
                &g_dtb_aimee_home_was_set);
   dtb_save_env("AIMEE_PROFILE", g_dtb_saved_aimee_profile, sizeof(g_dtb_saved_aimee_profile),
                &g_dtb_aimee_profile_was_set);

   snprintf(g_dtb_fake_home, sizeof(g_dtb_fake_home), "%s/aimee-test-dtb-home-XXXXXX",
            platform_tmpdir());
   assert(platform_mkdtemp(g_dtb_fake_home) != NULL);
   assert(platform_setenv("HOME", g_dtb_fake_home) == 0);
   assert(platform_unsetenv("AIMEE_HOME") == 0);
   assert(platform_unsetenv("AIMEE_PROFILE") == 0);
}

static void dtb_set_global_yaml(const char *project_dir, const char *yaml)
{
   dtb_setup_home();

   const char *base = strrchr(project_dir, '/');
   base = base ? base + 1 : project_dir;

   char dir[1024], path[1024];
   snprintf(dir, sizeof(dir), "%s/.config/aimee/projects/%s", g_dtb_fake_home, base);
   assert(platform_mkdir_p(dir, 0700) == 0);

   snprintf(path, sizeof(path), "%s/project.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(yaml, f);
   fclose(f);
}

static void dtb_clear_home(void)
{
   if (g_dtb_fake_home[0] == '\0')
      return;
   platform_test_rmrf(g_dtb_fake_home);
   g_dtb_fake_home[0] = '\0';
   dtb_restore_env("HOME", g_dtb_saved_home, g_dtb_home_was_set);
   dtb_restore_env("AIMEE_HOME", g_dtb_saved_aimee_home, g_dtb_aimee_home_was_set);
   dtb_restore_env("AIMEE_PROFILE", g_dtb_saved_aimee_profile, g_dtb_aimee_profile_was_set);
}

static void test_load_default_when_no_yaml(void)
{
   /* g_tmpdir has no project.yaml at the global path */
   int budget = delegate_token_budget_load(g_tmpdir, NULL);
   assert(budget == DELEGATE_TOKEN_BUDGET_DEFAULT);
   printf("  load_default_when_no_yaml: ok\n");
}

static void test_load_custom_budget_from_yaml(void)
{
   /* Init g_tmpdir as a fresh git repo so resolve_main_repo_root returns
    * its absolute path (basename matches the dir we key on). */
   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && touch f && git add f && "
            "git commit -q -m init",
            g_tmpdir);
   assert(system(cmd) == 0);

   dtb_set_global_yaml(g_tmpdir, "name: test\ndelegate_token_budget: 5000\n");

   int budget = delegate_token_budget_load(g_tmpdir, NULL);
   assert(budget == 5000);
   printf("  load_custom_budget_from_yaml: ok\n");
}

static void test_load_ignores_invalid_value(void)
{
   /* Create a separate git repo so its basename is distinct from g_tmpdir */
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/subtest_dtb", g_tmpdir);
   assert(mkdir(subdir, 0755) == 0);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && touch f && git add f && "
            "git commit -q -m init",
            subdir);
   assert(system(cmd) == 0);

   dtb_set_global_yaml(subdir, "name: test\ndelegate_token_budget: 0\n");

   int budget = delegate_token_budget_load(subdir, NULL);
   assert(budget == DELEGATE_TOKEN_BUDGET_DEFAULT);
   printf("  load_ignores_invalid_value: ok\n");
}

static void test_load_per_role_budget(void)
{
   /* Create a git repo with per-role budget overrides */
   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/subtest_role", g_tmpdir);
   assert(mkdir(subdir, 0755) == 0);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email t@t && "
            "git config user.name t && touch f && git add f && "
            "git commit -q -m init",
            subdir);
   assert(system(cmd) == 0);

   dtb_set_global_yaml(subdir, "name: test\n"
                               "delegate_token_budget: 15000\n"
                               "delegate_token_budget_review: 10000\n"
                               "delegate_token_budget_code: 25000\n");

   /* Role-specific budget should override global */
   int budget_review = delegate_token_budget_load(subdir, "review");
   assert(budget_review == 10000);

   int budget_code = delegate_token_budget_load(subdir, "code");
   assert(budget_code == 25000);

   /* Unknown role should fall back to global */
   int budget_other = delegate_token_budget_load(subdir, "explain");
   assert(budget_other == 15000);

   /* NULL role should use global */
   int budget_null = delegate_token_budget_load(subdir, NULL);
   assert(budget_null == 15000);

   printf("  load_per_role_budget: ok\n");
}

/* --- main --- */

/* A 20K constant truncated every delegate whose prompt exceeded it -- 1807 times
 * on one deployment -- regardless of what the model could actually hold. The
 * budget has to follow the model: 128K means 128K, 1M means 1M. */
static void test_budget_follows_the_model_context_window(void)
{
   /* Registry-known models: the budget tracks the window, not the constant. */
   int small = delegate_token_budget_for_agent("gpt-4o", 0);
   int large = delegate_token_budget_for_agent("gemini-1.5-pro", 0);
   assert(small > DELEGATE_TOKEN_BUDGET_DEFAULT);
   assert(large > small);
   printf("  PASS: test_budget_follows_the_model_context_window\n");
}

/* Output needs somewhere to go: the input budget is the window minus the model's
 * output ceiling, never the whole window. */
static void test_budget_reserves_room_for_output(void)
{
   int budget = delegate_token_budget_for_agent("gpt-4o", 0);
   assert(budget > 0);
   assert(budget < model_context_window("gpt-4o"));
   printf("  PASS: test_budget_reserves_room_for_output\n");
}

/* An operator's explicit per-agent context_window overrides the registry, the
 * same precedence agent_effective_context_window() uses at runtime. */
static void test_agent_context_window_overrides_registry(void)
{
   int budget = delegate_token_budget_for_agent("gpt-4o", 500000);
   assert(budget > delegate_token_budget_for_agent("gpt-4o", 0));
   /* Unknown model with an explicit window is still usable. */
   assert(delegate_token_budget_for_agent("not-a-real-model-xyz", 300000) > 0);
   /* Unknown model with nothing to go on reports 0 so the caller keeps its default. */
   assert(delegate_token_budget_for_agent("not-a-real-model-xyz", 0) == 0);
   printf("  PASS: test_agent_context_window_overrides_registry\n");
}

/* The agent is chosen after the prompt is built, so the prompt must fit whichever
 * one wins -- the smallest eligible window is the only safe budget. */
static void test_budget_across_agents_takes_the_smallest(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   snprintf(cfg.agents[0].model, sizeof(cfg.agents[0].model), "%s", "gemini-1.5-pro");
   snprintf(cfg.agents[1].model, sizeof(cfg.agents[1].model), "%s", "gpt-4o");
   int smallest = delegate_token_budget_for_agents(&cfg);
   assert(smallest == delegate_token_budget_for_agent("gpt-4o", 0));
   assert(smallest < delegate_token_budget_for_agent("gemini-1.5-pro", 0));
   /* No agents: nothing is known, so the caller keeps its default. */
   cfg.agent_count = 0;
   assert(delegate_token_budget_for_agents(&cfg) == 0);
   assert(delegate_token_budget_for_agents(NULL) == 0);
   printf("  PASS: test_budget_across_agents_takes_the_smallest\n");
}

int main(void)
{
   printf("test_delegate_token_budget\n");

   setup_tmpdir();
   dtb_setup_home();

   test_under_budget_passthrough();
   test_null_prompt_returns_null();
   test_over_budget_truncated();
   test_skill_content_truncated_first();
   test_task_description_minimum_preserved();
   test_truncated_output_is_valid_string();
   test_file_refs_truncated_before_skill();
   test_context_sections_truncated_before_skill();
   test_multiple_file_refs_removed_progressively();
   test_preloaded_file_contents_truncated();
   test_load_default_when_no_yaml();
   test_load_custom_budget_from_yaml();
   test_load_ignores_invalid_value();
   test_load_per_role_budget();
   test_budget_follows_the_model_context_window();
   test_budget_reserves_room_for_output();
   test_agent_context_window_overrides_registry();
   test_budget_across_agents_takes_the_smallest();

   dtb_clear_home();
   cleanup_tmpdir();
   printf("All tests passed.\n");
   return 0;
}
