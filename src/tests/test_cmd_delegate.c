/* test_cmd_delegate.c: unit tests for CLI delegation chain depth guard */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_process.h"
#include "cmd_agent_delegate_impl.h"
#include "delegate_role.h"
#include "model_registry.h"
#include "log.h"
#include "posix/agent_tools_internal.h"
#include "provider_cli_adapter.h"
#include "cJSON.h"

/* role_template_max_turns() (via delegate_role.o, reached by the max-turns policy)
 * reads the role_templates dir under config_default_dir() and parses `max_turns:`
 * frontmatter (-1 when absent). Point it at a temp dir and lay down the templates
 * the policy assertions expect (note: the "test" role canonicalizes to validate). */
static char g_roles_dir[256];
const char *config_default_dir(void)
{
   return g_roles_dir;
}
static void write_role_template(const char *canonical, int max_turns)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/role_templates/%s.md", g_roles_dir, canonical);
   FILE *f = fopen(path, "w");
   assert(f);
   fprintf(f, "---\nmax_turns: %d\n---\nbody\n", max_turns);
   fclose(f);
}
static void setup_role_templates(void)
{
   snprintf(g_roles_dir, sizeof(g_roles_dir), "/tmp/aimee-test-cmddel-XXXXXX");
   assert(mkdtemp(g_roles_dir));
   char sub[512];
   snprintf(sub, sizeof(sub), "%s/role_templates", g_roles_dir);
   assert(mkdir(sub, 0700) == 0);
   write_role_template("review", 20);
   write_role_template("validate", 12); /* "test" -> validate */
   write_role_template("diagnose", 16);
}

/* Pull in only the declarations we need. */
int delegate_check_chain_depth(int max_depth, char *errbuf, size_t errbuf_sz);

/* ---- Helpers ---- */

int agent_has_role(const agent_t *agent, const char *role)
{
   if (!agent || !role)
      return 0;
   for (int i = 0; i < agent->role_count; i++)
      if (strcmp(agent->roles[i], role) == 0)
         return 1;
   return 0;
}

int agent_is_exec_role(const agent_t *agent, const char *role)
{
   if (!agent || !role)
      return 0;
   for (int i = 0; i < agent->exec_role_count; i++)
      if (strcmp(agent->exec_roles[i], role) == 0)
         return 1;
   return 0;
}

int agent_is_available_for_routing(const agent_t *agent)
{
   return agent && agent->enabled;
}

agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   if (!cfg || !name)
      return NULL;
   for (int i = 0; i < cfg->agent_count; i++)
      if (strcmp(cfg->agents[i].name, name) == 0)
         return &cfg->agents[i];
   return NULL;
}

agent_t *agent_route(agent_config_t *cfg, const char *role)
{
   if (!cfg || !role)
      return NULL;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (agent_is_available_for_routing(ag) &&
          (agent_has_role(ag, role) || agent_is_exec_role(ag, role)))
         return ag;
   }
   return NULL;
}

agent_t *agent_route_at_tier(agent_config_t *cfg, const char *role, int tier)
{
   if (!cfg || !role)
      return NULL;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (agent_is_available_for_routing(ag) && ag->cost_tier == tier &&
          (agent_has_role(ag, role) || agent_is_exec_role(ag, role)))
         return ag;
   }
   return NULL;
}

int model_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   if (!model_id || !model_id[0] || !out)
      return 0;
   memset(out, 0, sizeof(*out));
   snprintf(out->provider, sizeof(out->provider), "%s",
            (provider && provider[0]) ? provider : "openai");
   snprintf(out->model_id, sizeof(out->model_id), "%s", model_id);
   out->flags = MODEL_CAP_STREAMING;
   if (!strstr(model_id, "notools"))
      out->flags |= MODEL_CAP_TOOLS;
   out->context_window = 128000;
   if (strstr(model_id, "vision"))
      out->flags |= MODEL_CAP_VISION;
   if (strstr(model_id, "pdf"))
      out->flags |= MODEL_CAP_PDF;
   if (strstr(model_id, "audio"))
      out->flags |= MODEL_CAP_AUDIO;
   if (strstr(model_id, "tinyctx"))
      out->context_window = 2048;
   if (strstr(model_id, "deprecated"))
      out->deprecated = 1;
   return 1;
}

/* delegate_routing.c logs a warning when it relaxes an unmet inferred modality
 * cap; stub it (this test doesn't link log.o). */
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

/* delegate_routing.c falls back to the CLI adapter's declared window for tmux-CLI
 * agents; stub the lookup so this test doesn't link the adapter machinery. Only
 * "codex" resolves (272k) — an unknown cli_kind returns NULL (stays dropped). */
const provider_cli_adapter_t *provider_cli_adapter_get(const char *cli_kind)
{
   static const provider_cli_adapter_t codex = {.cli_kind = "codex",
                                                .caps = {.max_context_tokens = 272000}};
   if (cli_kind && strcmp(cli_kind, "codex") == 0)
      return &codex;
   return NULL;
}

void model_capability_flags_string(unsigned flags, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   struct
   {
      unsigned flag;
      const char *name;
   } rows[] = {{MODEL_CAP_REASONING, "reasoning"},
               {MODEL_CAP_TOOLS, "tools"},
               {MODEL_CAP_VISION, "vision"},
               {MODEL_CAP_PDF, "pdf"},
               {MODEL_CAP_AUDIO, "audio"},
               {MODEL_CAP_STREAMING, "streaming"},
               {0, NULL}};
   for (int i = 0; rows[i].name; i++)
   {
      if (!(flags & rows[i].flag))
         continue;
      size_t used = strlen(out);
      if (used >= out_len)
         break;
      snprintf(out + used, out_len - used, "%s%s", used ? "," : "", rows[i].name);
   }
   if (!out[0])
      snprintf(out, out_len, "-");
}

static void clear_depth_env(void)
{
   platform_setenv("AIMEE_DELEGATE_DEPTH", "");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "");
}

/* ---- Tests: delegate_check_chain_depth ---- */

static void test_depth_zero_when_env_unset(void)
{
   clear_depth_env();
   char errbuf[256] = {0};
   /* max_depth=3: first call (depth 1) should succeed */
   int rc = delegate_check_chain_depth(3, errbuf, sizeof(errbuf));
   assert(rc == 0);
   assert(errbuf[0] == '\0');
   /* env var should now be "1" */
   const char *val = getenv("AIMEE_DELEGATE_DEPTH");
   assert(val != NULL);
   assert(strcmp(val, "1") == 0);
   clear_depth_env();
   printf("  PASS: test_depth_zero_when_env_unset\n");
}

static void test_depth_increments_from_env(void)
{
   platform_setenv("AIMEE_DELEGATE_DEPTH", "2");
   char errbuf[256] = {0};
   int rc = delegate_check_chain_depth(5, errbuf, sizeof(errbuf));
   assert(rc == 0);
   assert(errbuf[0] == '\0');
   const char *val = getenv("AIMEE_DELEGATE_DEPTH");
   assert(val != NULL);
   assert(strcmp(val, "3") == 0);
   clear_depth_env();
   printf("  PASS: test_depth_increments_from_env\n");
}

static void test_depth_blocked_at_limit(void)
{
   /* parent_depth=3, current=4 > max_depth=3 -> blocked */
   platform_setenv("AIMEE_DELEGATE_DEPTH", "3");
   char errbuf[256] = {0};
   int rc = delegate_check_chain_depth(3, errbuf, sizeof(errbuf));
   assert(rc == -1);
   assert(strstr(errbuf, "depth limit exceeded") != NULL);
   /* env should be unchanged at "3" */
   const char *val = getenv("AIMEE_DELEGATE_DEPTH");
   assert(val != NULL);
   assert(strcmp(val, "3") == 0);
   clear_depth_env();
   printf("  PASS: test_depth_blocked_at_limit\n");
}

static void test_depth_allowed_at_limit_minus_one(void)
{
   /* parent_depth=2, current=3 == max_depth=3 -> allowed */
   platform_setenv("AIMEE_DELEGATE_DEPTH", "2");
   char errbuf[256] = {0};
   int rc = delegate_check_chain_depth(3, errbuf, sizeof(errbuf));
   assert(rc == 0);
   const char *val = getenv("AIMEE_DELEGATE_DEPTH");
   assert(val != NULL);
   assert(strcmp(val, "3") == 0);
   clear_depth_env();
   printf("  PASS: test_depth_allowed_at_limit_minus_one\n");
}

static void test_depth_custom_limit(void)
{
   clear_depth_env();
   char errbuf[256] = {0};

   /* depth 1..5 all succeed with max_depth=5 */
   for (int i = 0; i < 5; i++)
   {
      int rc = delegate_check_chain_depth(5, errbuf, sizeof(errbuf));
      assert(rc == 0);
      char expected[16];
      snprintf(expected, sizeof(expected), "%d", i + 1);
      assert(strcmp(getenv("AIMEE_DELEGATE_DEPTH"), expected) == 0);
   }

   /* depth 6 > max_depth=5 -> blocked */
   int rc = delegate_check_chain_depth(5, errbuf, sizeof(errbuf));
   assert(rc == -1);
   assert(strstr(errbuf, "6/5") != NULL);

   clear_depth_env();
   printf("  PASS: test_depth_custom_limit\n");
}

static void test_depth_errbuf_null_safe(void)
{
   platform_setenv("AIMEE_DELEGATE_DEPTH", "3");
   /* Passing NULL errbuf must not crash */
   int rc = delegate_check_chain_depth(3, NULL, 0);
   assert(rc == -1);
   clear_depth_env();
   printf("  PASS: test_depth_errbuf_null_safe\n");
}

static void test_depth_error_message_content(void)
{
   platform_setenv("AIMEE_DELEGATE_DEPTH", "3");
   char errbuf[512] = {0};
   delegate_check_chain_depth(3, errbuf, sizeof(errbuf));
   assert(strstr(errbuf, "delegation chain depth limit exceeded") != NULL);
   assert(strstr(errbuf, "max_delegation_depth") != NULL);
   clear_depth_env();
   printf("  PASS: test_depth_error_message_content\n");
}

static void test_delegate_chain_env_clear_policy(void)
{
   assert(delegate_chain_env_should_clear("2", "", 0, 0) == 1);
   assert(delegate_chain_env_should_clear("2", NULL, 0, 0) == 1);
   assert(delegate_chain_env_should_clear("2", "deleg-parent", 0, 0) == 0);
   assert(delegate_chain_env_should_clear("2", "deleg-parent", 1, 1) == 0);
   assert(delegate_chain_env_should_clear("2", "deleg-parent", 1, 0) == 1);
   assert(delegate_chain_env_should_clear("", "deleg-parent", 0, 0) == 0);
   assert(delegate_chain_env_should_clear("", "deleg-parent", 1, 0) == 1);
   printf("  PASS: test_delegate_chain_env_clear_policy\n");
}

static void test_guarded_lxc_readonly_root_matching(void)
{
   const char *ro = "/repo";
   const char *rw = "/repo/.aimee/worktrees/session/main";

   assert(agent_tools_cmd_refers_to_readonly_root("cat /repo/src/main.c", ro, rw) == 1);
   assert(agent_tools_cmd_refers_to_readonly_root("cat '/repo/src/main.c'", ro, rw) == 1);
   assert(agent_tools_cmd_refers_to_readonly_root("cat /repo", ro, rw) == 1);
   assert(agent_tools_cmd_refers_to_readonly_root("cat /repo-other/src/main.c", ro, rw) == 0);
   assert(agent_tools_cmd_refers_to_readonly_root("cat /tmp/repo/src/main.c", ro, rw) == 0);
   assert(agent_tools_cmd_refers_to_readonly_root("cat /repo2/src/main.c", ro, rw) == 0);
   assert(agent_tools_cmd_refers_to_readonly_root("cat /repo/.aimee/worktrees/session/main/src.c",
                                                  ro, rw) == 0);
   printf("  PASS: test_guarded_lxc_readonly_root_matching\n");
}

static void test_role_default_tools(void)
{
   assert(delegate_role_enable_tools_by_default("search") == 1);
   assert(delegate_role_enable_tools_by_default("execute") == 1);
   assert(delegate_role_enable_tools_by_default("diagnose") == 1);
   assert(delegate_role_enable_tools_by_default("validate") == 1);
   assert(delegate_role_enable_tools_by_default("inspect") == 1);
   assert(delegate_role_enable_tools_by_default("test") == 1);
   assert(delegate_role_enable_tools_by_default("check") == 1);
   assert(delegate_role_enable_tools_by_default("research") == 1);
   assert(delegate_role_enable_tools_by_default("code") == 0);
   assert(delegate_role_enable_tools_by_default(NULL) == 0);
   assert(delegate_role_enable_tools_by_default("") == 0);
   printf("  PASS: test_role_default_tools\n");
}

static void test_role_result_cache_policy(void)
{
   assert(delegate_role_result_cache_enabled("review") == 0);
   assert(delegate_role_result_cache_enabled("validate") == 0);
   assert(delegate_role_result_cache_enabled("diagnose") == 0);
   assert(delegate_role_result_cache_enabled("search") == 0);
   assert(delegate_role_result_cache_enabled("execute") == 0);
   assert(delegate_role_result_cache_enabled("code") == 0);
   assert(delegate_role_result_cache_enabled("refactor") == 0);
   assert(delegate_role_result_cache_enabled("inspect") == 0);
   assert(delegate_role_result_cache_enabled("test") == 0);
   assert(delegate_role_result_cache_enabled("summarize") == 1);
   assert(delegate_role_result_cache_enabled("format") == 1);
   assert(delegate_role_result_cache_enabled("draft") == 1);
   assert(delegate_role_result_cache_enabled("reason") == 0);
   assert(delegate_role_result_cache_enabled("custom-review") == 0);
   assert(delegate_role_result_cache_enabled(NULL) == 0);
   assert(delegate_role_result_cache_enabled("") == 0);
   printf("  PASS: test_role_result_cache_policy\n");
}

static void test_prompt_plan_inline_prompt_only(void)
{
   delegate_prompt_plan_t plan;
   assert(delegate_resolve_prompt_inputs("summarize this text", NULL, &plan) == 0);
   assert(strcmp(plan.task_prompt, "summarize this text") == 0);
   assert(strcmp(plan.user_prompt, "summarize this text") == 0);
   assert(plan.owned_user_prompt == NULL);
   printf("  PASS: test_prompt_plan_inline_prompt_only\n");
}

static void test_prompt_plan_file_only(void)
{
   delegate_prompt_plan_t plan;
   assert(delegate_resolve_prompt_inputs(NULL, "file payload here", &plan) == 0);
   assert(strcmp(plan.task_prompt, "Work from the user prompt provided below.") == 0);
   assert(strcmp(plan.user_prompt, "file payload here") == 0);
   assert(plan.owned_user_prompt == NULL);
   printf("  PASS: test_prompt_plan_file_only\n");
}

static void test_prompt_plan_prompt_and_file(void)
{
   delegate_prompt_plan_t plan;
   assert(delegate_resolve_prompt_inputs("extract the marker", "MARKER_ABC_123", &plan) == 0);
   assert(strcmp(plan.task_prompt, "extract the marker") == 0);
   assert(plan.owned_user_prompt != NULL);
   assert(strstr(plan.user_prompt, "extract the marker") == plan.user_prompt);
   assert(strstr(plan.user_prompt, "# Prompt File\nMARKER_ABC_123") != NULL);
   free(plan.owned_user_prompt);
   printf("  PASS: test_prompt_plan_prompt_and_file\n");
}

static void test_prompt_plan_requires_prompt_source(void)
{
   delegate_prompt_plan_t plan;
   assert(delegate_resolve_prompt_inputs(NULL, NULL, &plan) == -1);
   printf("  PASS: test_prompt_plan_requires_prompt_source\n");
}

static void test_validation_bundle_appended_for_review_roles(void)
{
   char *code_prompt = strdup("base prompt");
   char *unchanged = delegate_maybe_append_validation_bundle("code", ".", code_prompt, NULL, 0);
   assert(unchanged == code_prompt);
   free(unchanged);

   char *review_prompt =
       delegate_maybe_append_validation_bundle("review", ".", NULL, "base prompt", 0);
   assert(review_prompt != NULL);
   assert(strstr(review_prompt, "base prompt") == review_prompt);
   assert(strstr(review_prompt, "Validation Evidence Bundle") != NULL);
   assert(strstr(review_prompt, "zero-result search command") != NULL);
   assert(strstr(review_prompt, "Directory layout claims") != NULL);
   assert(strstr(review_prompt, "whole relevant source tree") != NULL);
   assert(strstr(review_prompt, "do not infer sibling headers") != NULL);
   free(review_prompt);

   char *diagnose_prompt =
       delegate_maybe_append_validation_bundle("diagnose", ".", NULL, "base prompt", 0);
   assert(diagnose_prompt != NULL);
   assert(strstr(diagnose_prompt, "Validation Evidence Bundle") != NULL);
   assert(strstr(diagnose_prompt, "Directory layout claims") != NULL);
   assert(strstr(diagnose_prompt, "whole relevant source tree") != NULL);
   free(diagnose_prompt);

   char *inspect_prompt =
       delegate_maybe_append_validation_bundle("inspect", ".", NULL, "base prompt", 0);
   assert(inspect_prompt != NULL);
   assert(strstr(inspect_prompt, "Validation Evidence Bundle") != NULL);
   assert(strstr(inspect_prompt, "whole relevant source tree") != NULL);
   free(inspect_prompt);

   char *test_prompt = delegate_maybe_append_validation_bundle("test", ".", NULL, "base prompt", 0);
   assert(test_prompt != NULL);
   assert(strstr(test_prompt, "Validation Evidence Bundle") != NULL);
   assert(strstr(test_prompt, "whole relevant source tree") != NULL);
   free(test_prompt);
   printf("  PASS: test_validation_bundle_appended_for_review_roles\n");
}

/* When the caller supplies the review target (target_provided=1, e.g. a diff via
 * --prompt-file), the host-cwd "Validation Evidence Bundle" is suppressed and the
 * reviewer is pointed at aimee's branch-indexed capabilities instead. */
static void test_provided_target_suppresses_cwd_bundle(void)
{
   char *review =
       delegate_maybe_append_validation_bundle("review", ".", NULL, "the diff to review", 1);
   assert(review != NULL);
   assert(strstr(review, "the diff to review") == review);
   assert(strstr(review, "Review Target & Exploration") != NULL);
   assert(strstr(review, "explore_via_aimee") != NULL);
   assert(strstr(review, "code_search") != NULL);
   /* the wrong-tree cwd bundle must NOT be present */
   assert(strstr(review, "Validation Evidence Bundle") == NULL);
   free(review);

   /* Applies to any role, not just review roles, when a target is provided. */
   char *coder = delegate_maybe_append_validation_bundle("code", ".", NULL, "base", 1);
   assert(coder != NULL);
   assert(strstr(coder, "Review Target & Exploration") != NULL);
   free(coder);
   printf("  PASS: test_provided_target_suppresses_cwd_bundle\n");
}

static void test_review_evidence_drift_detects_reversed_snippet(void)
{
   char root[] = "/tmp/aimee-review-drift-XXXXXX";
   assert(mkdtemp(root) != NULL);
   char srcdir[512];
   snprintf(srcdir, sizeof(srcdir), "%s/src", root);
   assert(mkdir(srcdir, 0700) == 0);
   char path[512];
   snprintf(path, sizeof(path), "%s/src/kb_client.c", root);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("void f(void)\n"
         "{\n"
         "   cJSON *req = cJSON_CreateObject();\n"
         "   if (!req)\n"
         "      return;\n"
         "   char *resp = kb_client_v1_post_json(\"/v1/internal/ingest/job/claim\", req, 1000, "
         "NULL);\n"
         "   cJSON_Delete(req);\n"
         "   return;\n"
         "}\n",
         f);
   fclose(f);

   char err[256];
   const char *fresh =
       "Findings\n"
       "**Severity: high | Location: `src/kb_client.c:6`**\n"
       "```c\n"
       "char *resp = kb_client_v1_post_json(\"/v1/internal/ingest/job/claim\", req, 1000, NULL);\n"
       "cJSON_Delete(req);\n"
       "```\n";
   assert(delegate_check_review_evidence_drift(fresh, root, err, sizeof(err)) == 0);

   const char *stale =
       "Findings\n"
       "**Severity: critical | Location: `src/kb_client.c:6`**\n"
       "```c\n"
       "cJSON_Delete(req);\n"
       "char *resp = kb_client_v1_post_json(\"/v1/internal/ingest/job/claim\", req, 1000, NULL);\n"
       "```\n";
   assert(delegate_check_review_evidence_drift(stale, root, err, sizeof(err)) == 1);
   assert(strstr(err, "delegate evidence drift") != NULL);

   unlink(path);
   rmdir(srcdir);
   rmdir(root);
   printf("  PASS: test_review_evidence_drift_detects_reversed_snippet\n");
}

static void test_review_evidence_drift_ignores_historical_diff_snippet(void)
{
   char root[] = "/tmp/aimee-review-diff-drift-XXXXXX";
   assert(mkdtemp(root) != NULL);
   char srcdir[512];
   snprintf(srcdir, sizeof(srcdir), "%s/src", root);
   assert(mkdir(srcdir, 0700) == 0);
   char path[512];
   snprintf(path, sizeof(path), "%s/src/work.c", root);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("int claim(void)\n"
         "{\n"
         "   lane_guard();\n"
         "   return 0;\n"
         "}\n",
         f);
   fclose(f);

   char err[256];
   const char *review = "Findings\n"
                        "**HIGH -- Correctness**\n"
                        "**Location:** `src/work.c:3`\n"
                        "The diff changes the code from:\n"
                        "```c\n"
                        "return 0;\n"
                        "lane_guard();\n"
                        "```\n"
                        "to:\n"
                        "```c\n"
                        "lane_guard();\n"
                        "return 0;\n"
                        "```\n";
   assert(delegate_check_review_evidence_drift(review, root, err, sizeof(err)) == 0);

   unlink(path);
   rmdir(srcdir);
   rmdir(root);
   printf("  PASS: test_review_evidence_drift_ignores_historical_diff_snippet\n");
}

static void test_review_evidence_drift_ignores_inline_review_annotation(void)
{
   char root[] = "/tmp/aimee-review-annotation-drift-XXXXXX";
   assert(mkdtemp(root) != NULL);
   char srcdir[512];
   snprintf(srcdir, sizeof(srcdir), "%s/src", root);
   assert(mkdir(srcdir, 0700) == 0);
   char path[512];
   snprintf(path, sizeof(path), "%s/src/Makefile", root);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("$(OBJDIR)/server/server_kb_workers.o: server/server_kb_workers.c\n"
         "\t@mkdir -p $(dir $@)\n"
         "\t$(CC) -c $(C_FLAGS) -o $@ $<\n"
         "\n"
         "$(OBJDIR)/server/%.o: %.c\n"
         "\t@mkdir -p $(dir $@)\n"
         "\t$(CC) -c $(C_FLAGS) -DAIMEE_DB2_DISABLED -o $@ $<\n",
         f);
   fclose(f);

   char err[256];
   const char *review = "**Location:** `src/Makefile:1`\n"
                        "```makefile\n"
                        "$(OBJDIR)/server/server_kb_workers.o: server/server_kb_workers.c\n"
                        "\t@mkdir -p $(dir $@)\n"
                        "\t$(CC) -c $(C_FLAGS) -o $@ $<           \xE2\x86\x90"
                        " line 3, missing DB2 flag\n"
                        "\n"
                        "$(OBJDIR)/server/%.o: %.c\n"
                        "\t@mkdir -p $(dir $@)\n"
                        "\t$(CC) -c $(C_FLAGS) -DAIMEE_DB2_DISABLED -o $@ $<\n"
                        "```\n";
   assert(delegate_check_review_evidence_drift(review, root, err, sizeof(err)) == 0);

   unlink(path);
   rmdir(srcdir);
   rmdir(root);
   printf("  PASS: test_review_evidence_drift_ignores_inline_review_annotation\n");
}

static void test_review_evidence_guard_rejects_clean_claim_on_dirty_worktree(void)
{
   char root[] = "/tmp/aimee-review-clean-claim-XXXXXX";
   assert(mkdtemp(root) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q", root);
   assert(system(cmd) == 0);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup("No uncommitted diff exists. The working tree is clean.");
   assert(result.response != NULL);

   int rc = 0;
   delegate_apply_review_evidence_guard("review", root, &rc, &result, 0);
   assert(rc == 0);
   assert(result.error[0] == '\0');
   free(result.response);

   char path[512];
   snprintf(path, sizeof(path), "%s/pending.txt", root);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("pending\n", f);
   fclose(f);

   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup("No uncommitted diff exists. The working tree is clean.");
   assert(result.response != NULL);

   rc = 0;
   delegate_apply_review_evidence_guard("validate", root, &rc, &result, 0);
   assert(rc == -1);
   assert(strstr(result.error, "delegate evidence drift") != NULL);
   free(result.response);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
   assert(system(cmd) == 0);
   printf("  PASS: test_review_evidence_guard_rejects_clean_claim_on_dirty_worktree\n");
}

static void test_diagnose_evidence_guard_allows_nonreview_snippets(void)
{
   char root[] = "/tmp/aimee-diagnose-snippet-XXXXXX";
   assert(mkdtemp(root) != NULL);
   char srcdir[512];
   snprintf(srcdir, sizeof(srcdir), "%s/docs", root);
   assert(mkdir(srcdir, 0700) == 0);
   char path[512];
   snprintf(path, sizeof(path), "%s/docs/proposal.md", root);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("one\n"
         "two\n"
         "three\n",
         f);
   fclose(f);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup("Finding\n"
                            "**Location: `docs/proposal.md:2`**\n"
                            "```md\n"
                            "this is an explanatory quote, not an exact current-file snippet\n"
                            "```\n");
   assert(result.response != NULL);

   int rc = 0;
   delegate_apply_review_evidence_guard("diagnose", root, &rc, &result, 0);
   assert(rc == 0);
   assert(result.error[0] == '\0');
   free(result.response);

   unlink(path);
   rmdir(srcdir);
   rmdir(root);
   printf("  PASS: test_diagnose_evidence_guard_allows_nonreview_snippets\n");
}

static void test_apply_max_turns_override(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   cfg.agents[0].max_turns = -1;
   cfg.agents[1].max_turns = 7;

   delegate_apply_max_turns_override(&cfg, 40);
   assert(cfg.agents[0].max_turns == 40);
   assert(cfg.agents[1].max_turns == 40);

   delegate_apply_max_turns_override(&cfg, 0);
   assert(cfg.agents[0].max_turns == 0);
   assert(cfg.agents[1].max_turns == 0);

   delegate_apply_max_turns_override(&cfg, -1);
   assert(cfg.agents[0].max_turns == 0);
   assert(cfg.agents[1].max_turns == 0);
   printf("  PASS: test_apply_max_turns_override\n");
}

static void test_apply_max_turns_policy_caps_inspection_roles(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 3;
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].max_turns = -1;
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].max_turns = 8;
   snprintf(cfg.agents[2].roles[0], sizeof(cfg.agents[2].roles[0]), "code");
   cfg.agents[2].role_count = 1;
   cfg.agents[2].max_turns = 50;

   delegate_apply_max_turns_policy(&cfg, "review", -1);
   assert(cfg.agents[0].max_turns == 20);
   assert(cfg.agents[1].max_turns == 8);
   assert(cfg.agents[2].max_turns == 50);

   delegate_apply_max_turns_policy(&cfg, "review", 3);
   assert(cfg.agents[0].max_turns == 3);
   assert(cfg.agents[1].max_turns == 3);
   assert(cfg.agents[2].max_turns == 3);
   printf("  PASS: test_apply_max_turns_policy_caps_inspection_roles\n");
}

static void test_apply_max_turns_policy_aliases(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "validate");
   cfg.agents[0].role_count = 1;
   /* Declared nothing (-1): the role floor applies. (A declared cap >= 0 would
    * be honored verbatim — see test_apply_max_turns_policy_caps_inspection_roles.)
    * This case verifies the "test" role alias resolves to the validate floor. */
   cfg.agents[0].max_turns = -1;

   assert(delegate_default_max_turns_for_role("test") == 12);
   delegate_apply_max_turns_policy(&cfg, "test", -1);
   assert(cfg.agents[0].max_turns == 12);
   printf("  PASS: test_apply_max_turns_policy_aliases\n");
}

static void test_delegate_route_preflight_rejects_unknown_role(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   cfg.agents[0].enabled = 1;
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "diagnose");
   cfg.agents[0].role_count = 1;

   char errbuf[128];
   assert(delegate_route_preflight(&cfg, "scout", errbuf, sizeof(errbuf)) == -1);
   assert(strstr(errbuf, "no agent available for role 'scout'") != NULL);
   assert(delegate_route_preflight(&cfg, "diagnose", errbuf, sizeof(errbuf)) == 0);
   assert(errbuf[0] == '\0');
   printf("  PASS: test_delegate_route_preflight_rejects_unknown_role\n");
}

static void test_tier_override_keeps_same_tier_pool(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 3;
   snprintf(cfg.default_agent, sizeof(cfg.default_agent), "minimax");

   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "mistral-plan");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "diagnose");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;

   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "minimax");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "diagnose");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 0;
   cfg.agents[1].enabled = 1;

   snprintf(cfg.agents[2].name, sizeof(cfg.agents[2].name), "paid");
   snprintf(cfg.agents[2].roles[0], sizeof(cfg.agents[2].roles[0]), "diagnose");
   cfg.agents[2].role_count = 1;
   cfg.agents[2].cost_tier = 2;
   cfg.agents[2].enabled = 1;

   char errbuf[128];
   assert(delegate_apply_route_overrides(&cfg, "diagnose", NULL, 0, NULL, NULL, errbuf,
                                         sizeof(errbuf)) == 0);
   assert(cfg.agents[0].enabled == 1);
   assert(cfg.agents[1].enabled == 1);
   assert(cfg.agents[2].enabled == 0);
   printf("  PASS: test_tier_override_keeps_same_tier_pool\n");
}

static void test_delegate_checkout_records_unavailable_heads(void)
{
   cJSON *resp = cJSON_CreateObject();
   assert(resp != NULL);
   cJSON_AddStringToObject(resp, "response", "body");

   delegate_checkout_add_result_ex(resp, "/tmp/aimee-missing-delegate-worktree", "abc123",
                                   "/tmp/aimee-missing-parent-worktree", "def456", NULL);

   cJSON *finish_head = cJSON_GetObjectItem(resp, "finish_head");
   cJSON *checkout_drift = cJSON_GetObjectItem(resp, "checkout_drift");
   cJSON *parent_finish_head = cJSON_GetObjectItem(resp, "parent_worktree_finish_head");
   cJSON *parent_drift = cJSON_GetObjectItem(resp, "parent_worktree_drift");
   assert(cJSON_IsString(finish_head));
   assert(strcmp(finish_head->valuestring, "unavailable") == 0);
   assert(cJSON_IsTrue(checkout_drift));
   assert(cJSON_IsString(parent_finish_head));
   assert(strcmp(parent_finish_head->valuestring, "unavailable") == 0);
   assert(cJSON_IsTrue(parent_drift));

   cJSON_Delete(resp);
   printf("  PASS: test_delegate_checkout_records_unavailable_heads\n");
}

static void test_via_override_rejects_role_mismatch(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "coder");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "code");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;

   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "reviewer");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;

   char errbuf[128];
   assert(delegate_apply_route_overrides(&cfg, "review", "coder", -1, NULL, NULL, errbuf,
                                         sizeof(errbuf)) == -1);
   assert(strstr(errbuf, "cannot handle role 'review'") != NULL);
   assert(cfg.agents[0].enabled == 1);
   assert(cfg.agents[1].enabled == 1);

   errbuf[0] = '\0';
   assert(delegate_apply_route_overrides(&cfg, "code", "coder", -1, NULL, NULL, errbuf,
                                         sizeof(errbuf)) == 0);
   assert(errbuf[0] == '\0');
   assert(cfg.agents[0].enabled == 1);
   assert(cfg.agents[1].enabled == 0);
   printf("  PASS: test_via_override_rejects_role_mismatch\n");
}

static void test_capability_filter_drops_deprecated_on_auto_route(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "old");
   snprintf(cfg.agents[0].model, sizeof(cfg.agents[0].model), "deprecated-model");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "openai");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "diagnose");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;

   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "new");
   snprintf(cfg.agents[1].model, sizeof(cfg.agents[1].model), "vision-model");
   snprintf(cfg.agents[1].provider, sizeof(cfg.agents[1].provider), "openai");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "diagnose");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;

   char errbuf[128];
   assert(delegate_filter_route_capabilities(&cfg, "diagnose", MODEL_CAP_VISION, 0, 1, errbuf,
                                             sizeof(errbuf)) == 0);
   assert(cfg.agents[0].enabled == 0);
   assert(cfg.agents[1].enabled == 1);
   printf("  PASS: test_capability_filter_drops_deprecated_on_auto_route\n");
}

static void test_capability_filter_enforces_min_context(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "small");
   snprintf(cfg.agents[0].model, sizeof(cfg.agents[0].model), "vision-tinyctx-model");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "openai");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "diagnose");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;

   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "large");
   snprintf(cfg.agents[1].model, sizeof(cfg.agents[1].model), "vision-large-model");
   snprintf(cfg.agents[1].provider, sizeof(cfg.agents[1].provider), "openai");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "diagnose");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;

   char errbuf[128];
   assert(delegate_filter_route_capabilities(&cfg, "diagnose", MODEL_CAP_VISION, 10000, 0, errbuf,
                                             sizeof(errbuf)) == 0);
   assert(cfg.agents[0].enabled == 0);
   assert(cfg.agents[1].enabled == 1);
   printf("  PASS: test_capability_filter_enforces_min_context\n");
}

static void test_capability_filter_honors_tools_enabled(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "metadata-only");
   snprintf(cfg.agents[0].model, sizeof(cfg.agents[0].model), "notools-model");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "openai");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "diagnose");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 0;

   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "configured-tools");
   snprintf(cfg.agents[1].model, sizeof(cfg.agents[1].model), "notools-model");
   snprintf(cfg.agents[1].provider, sizeof(cfg.agents[1].provider), "openai");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "diagnose");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;

   char errbuf[128];
   assert(delegate_filter_route_capabilities(&cfg, "diagnose", MODEL_CAP_TOOLS, 0, 0, errbuf,
                                             sizeof(errbuf)) == 0);
   assert(cfg.agents[0].enabled == 0);
   assert(cfg.agents[1].enabled == 1);
   printf("  PASS: test_capability_filter_honors_tools_enabled\n");
}

/* A tmux-CLI agent (codex) has no `model` and may have context_window=0 (records
 * registered before the window was persisted). The filter must fall back to the
 * CLI adapter's declared window so it survives a min-context floor — and still
 * drop a CLI agent whose adapter is unknown / declares no window. */
static void test_capability_filter_cli_agent_uses_adapter_context(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   /* codex: no model, no explicit window — resolves via the codex adapter (272k). */
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "codex");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "codex");
   snprintf(cfg.agents[0].cli_kind, sizeof(cfg.agents[0].cli_kind), "codex");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].middleware.context_window = 0;

   /* unknown CLI kind, no model/window — must NOT resolve a window, so dropped. */
   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "mystery-cli");
   snprintf(cfg.agents[1].provider, sizeof(cfg.agents[1].provider), "mystery");
   snprintf(cfg.agents[1].cli_kind, sizeof(cfg.agents[1].cli_kind), "no-such-cli");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].middleware.context_window = 0;

   char errbuf[128];
   assert(delegate_filter_route_capabilities(&cfg, "review", MODEL_CAP_TOOLS, 5131, 0, errbuf,
                                             sizeof(errbuf)) == 0);
   assert(cfg.agents[0].enabled == 1); /* codex kept via adapter window */
   assert(cfg.agents[1].enabled == 0); /* unknown CLI dropped */
   printf("  PASS: test_capability_filter_cli_agent_uses_adapter_context\n");
}

/* An inferred modality cap (vision/pdf/audio) that no model satisfies must NOT
 * hard-fail the fleet: it is relaxed to the hard caps (tools + min_context) so
 * the text models stay routable. Guards the fleet-wide false-fail where a text
 * task merely mentioning an image required vision no model had. */
static void test_capability_filter_relaxes_unmet_modality(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "text-a");
   snprintf(cfg.agents[0].model, sizeof(cfg.agents[0].model), "text-model");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "openai");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "diagnose");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;

   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "text-b");
   snprintf(cfg.agents[1].model, sizeof(cfg.agents[1].model), "text-model");
   snprintf(cfg.agents[1].provider, sizeof(cfg.agents[1].provider), "openai");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "diagnose");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;

   /* Require TOOLS (hard) + VISION (soft); no text-model has vision. The hard
    * TOOLS must be enforced, the unmet VISION relaxed -> both kept, no error. */
   char errbuf[128];
   assert(delegate_filter_route_capabilities(&cfg, "diagnose", MODEL_CAP_TOOLS | MODEL_CAP_VISION,
                                             0, 0, errbuf, sizeof(errbuf)) == 0);
   assert(cfg.agents[0].enabled == 1);
   assert(cfg.agents[1].enabled == 1);
   printf("  PASS: test_capability_filter_relaxes_unmet_modality\n");
}

/* A hard capability (tools) that no model satisfies still fails closed — only the
 * inferred modality caps are soft. */
static void test_capability_filter_hard_cap_still_fails(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "metadata-only");
   snprintf(cfg.agents[0].model, sizeof(cfg.agents[0].model), "notools-model");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "openai");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "diagnose");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 0;

   char errbuf[128];
   assert(delegate_filter_route_capabilities(&cfg, "diagnose", MODEL_CAP_TOOLS, 0, 0, errbuf,
                                             sizeof(errbuf)) == -1);
   assert(cfg.agents[0].enabled == 0);
   printf("  PASS: test_capability_filter_hard_cap_still_fails\n");
}

static void test_capability_inference_audio_extension_not_keyword(void)
{
   /* Regression: prompts describing audio *features* in code (e.g.
    * "implement an STT dispatcher" or "add audio support") must not
    * trigger MODEL_CAP_AUDIO — that would silently block delegates when
    * no audio-capable model is configured.  Only actual audio file
    * extensions (.mp3 .wav .m4a .ogg .flac .aac) should require the cap. */
   unsigned caps = 0;
   int min_ctx = 0;

   /* These code-description prompts must NOT require audio. */
   delegate_infer_capability_requirements(
       "Implement an STT (speech-to-text) dispatcher and audio routing module.", 0, &caps,
       &min_ctx);
   assert((caps & MODEL_CAP_AUDIO) == 0);

   caps = 0;
   delegate_infer_capability_requirements(
       "Add audio support to the gateway — implement the audio platform adapter.", 0, &caps,
       &min_ctx);
   assert((caps & MODEL_CAP_AUDIO) == 0);

   /* File-extension references MUST require audio. */
   caps = 0;
   delegate_infer_capability_requirements("Transcribe recording.mp3 into text.", 0, &caps,
                                          &min_ctx);
   assert((caps & MODEL_CAP_AUDIO) != 0);

   caps = 0;
   delegate_infer_capability_requirements("Process speech.wav and output captions.", 0, &caps,
                                          &min_ctx);
   assert((caps & MODEL_CAP_AUDIO) != 0);

   caps = 0;
   delegate_infer_capability_requirements("Convert podcast.m4a to a transcript.", 0, &caps,
                                          &min_ctx);
   assert((caps & MODEL_CAP_AUDIO) != 0);

   printf("  PASS: test_capability_inference_audio_extension_not_keyword\n");
}

static void test_capability_inference_detects_modalities(void)
{
   char filler[20032];
   memset(filler, 'a', sizeof(filler) - 1);
   filler[sizeof(filler) - 1] = '\0';
   char long_prompt[20224];
   snprintf(long_prompt, sizeof(long_prompt),
            "Analyze image screenshot.png and report pdf coverage. %s", filler);

   unsigned caps = 0;
   int min_ctx = 0;
   delegate_infer_capability_requirements(long_prompt, 1, &caps, &min_ctx);
   assert((caps & MODEL_CAP_TOOLS) != 0);
   assert((caps & MODEL_CAP_VISION) != 0);
   assert((caps & MODEL_CAP_PDF) != 0);
   assert(min_ctx > 0);
   printf("  PASS: test_capability_inference_detects_modalities\n");
}

/* ---- main ---- */

static void test_inline_acp_agent_synthesis(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   char name[MAX_AGENT_NAME] = "";
   assert(delegate_add_inline_acp_agent(&cfg, "claude", "agent", "code", name, sizeof(name)) == 0);
   assert(cfg.agent_count == 1);
   assert(name[0] != '\0');
   agent_t *a = &cfg.agents[0];
   assert(strcmp(a->name, name) == 0);
   assert(strcmp(a->cli_kind, "acp") == 0);
   assert(strcmp(a->backend, AGENT_BACKEND_PROVIDER_CLI) == 0);
   assert(strcmp(a->cli_cmd, "claude agent") == 0); /* command + args joined */
   assert(a->role_count == 1 && strcmp(a->roles[0], "code") == 0);
   assert(a->enabled == 1);
   assert(a->tools_enabled == 1);
   assert(a->max_turns == -1); /* no declared cap; role floor applies */

   /* The synthesized agent is findable by its returned name (so --via routing
    * selects it) and handles the requested role. */
   agent_t *found = agent_find(&cfg, name);
   assert(found == a);
   assert(agent_has_role(found, "code"));
   printf("  PASS: test_inline_acp_agent_synthesis\n");
}

static void test_inline_acp_agent_no_args_and_guards(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   char name[MAX_AGENT_NAME] = "x";

   /* No args: cli_cmd is just the command. */
   assert(delegate_add_inline_acp_agent(&cfg, "aider", NULL, "review", name, sizeof(name)) == 0);
   assert(strcmp(cfg.agents[0].cli_cmd, "aider") == 0);

   /* Empty command is rejected and clears name_out. */
   name[0] = 'z';
   assert(delegate_add_inline_acp_agent(&cfg, "", "x", "code", name, sizeof(name)) == -1);
   assert(name[0] == '\0');
   assert(delegate_add_inline_acp_agent(&cfg, NULL, NULL, "code", name, sizeof(name)) == -1);

   /* A full config (MAX_AGENTS) is rejected rather than overflowing. */
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = MAX_AGENTS;
   assert(delegate_add_inline_acp_agent(&cfg, "claude", NULL, "code", name, sizeof(name)) == -1);
   assert(cfg.agent_count == MAX_AGENTS);
   printf("  PASS: test_inline_acp_agent_no_args_and_guards\n");
}

int main(void)
{
   printf("test_cmd_delegate\n");
   setup_role_templates();
   test_depth_zero_when_env_unset();
   test_depth_increments_from_env();
   test_depth_blocked_at_limit();
   test_depth_allowed_at_limit_minus_one();
   test_depth_custom_limit();
   test_depth_errbuf_null_safe();
   test_depth_error_message_content();
   test_delegate_chain_env_clear_policy();
   test_guarded_lxc_readonly_root_matching();
   test_role_default_tools();
   test_role_result_cache_policy();
   test_prompt_plan_inline_prompt_only();
   test_prompt_plan_file_only();
   test_prompt_plan_prompt_and_file();
   test_prompt_plan_requires_prompt_source();
   test_validation_bundle_appended_for_review_roles();
   test_provided_target_suppresses_cwd_bundle();
   test_review_evidence_drift_detects_reversed_snippet();
   test_review_evidence_drift_ignores_historical_diff_snippet();
   test_review_evidence_drift_ignores_inline_review_annotation();
   test_review_evidence_guard_rejects_clean_claim_on_dirty_worktree();
   test_diagnose_evidence_guard_allows_nonreview_snippets();
   test_apply_max_turns_override();
   test_apply_max_turns_policy_caps_inspection_roles();
   test_apply_max_turns_policy_aliases();
   test_delegate_route_preflight_rejects_unknown_role();
   test_inline_acp_agent_synthesis();
   test_inline_acp_agent_no_args_and_guards();
   test_tier_override_keeps_same_tier_pool();
   test_delegate_checkout_records_unavailable_heads();
   test_via_override_rejects_role_mismatch();
   test_capability_filter_drops_deprecated_on_auto_route();
   test_capability_filter_enforces_min_context();
   test_capability_filter_honors_tools_enabled();
   test_capability_filter_cli_agent_uses_adapter_context();
   test_capability_filter_relaxes_unmet_modality();
   test_capability_filter_hard_cap_still_fails();
   test_capability_inference_audio_extension_not_keyword();
   test_capability_inference_detects_modalities();
   printf("All tests passed.\n");
   return 0;
}
