/* test_cmd_delegate.c: unit tests for CLI delegation chain depth guard */
#include "support/delegate_role_seam_stub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_process.h"
#include "cmd_agent_delegate_impl.h"
#include <aimee/delegates/delegate_role.h>
#include "model_registry.h"
#include "log.h"
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/delegates/module_api.h>

extern aimee_module_status_t aimee_delegates_module_handler(const aimee_module_invocation_t *,
                                                            const uint8_t *, uint32_t, uint8_t *,
                                                            uint32_t, uint32_t *, void *);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

/* Capability inference is the delegates module's rule now, so the unit test
 * asks the module the same way production does, through the C wire-parity
 * fixture, rather than reimplementing the answer locally. */
static int capabilities_via_module(const char *prompt, int tools_enabled, unsigned *required_caps,
                                   int *min_context)
{
   size_t prompt_len = prompt ? strlen(prompt) : 0;
   size_t request_cap = AIMEE_DELEGATES_CAP_HEADER_LEN + prompt_len;
   uint8_t *request = malloc(request_cap);
   if (!request)
      return -1;
   size_t request_len =
       aimee_delegates_cap_request_encode(prompt, prompt_len, tools_enabled, request, request_cap);
   uint8_t response[AIMEE_DELEGATES_CAP_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DELEGATES_STAGE_CAPABILITIES};
   int rc =
       request_len > 0 && aimee_delegates_module_handler(
                              &invocation, request, (uint32_t)request_len, response,
                              sizeof(response), &response_len, NULL) == AIMEE_MODULE_STATUS_OK
           ? aimee_delegates_cap_response_decode(response, response_len, required_caps, min_context)
           : -1;
   free(request);
   return rc;
}
#include "modules/tools/agent_tools_internal.h"
#include "provider_cli_adapter.h"
#include "cJSON.h"
#include <aimee/delegates/delegate_launch_args.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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
   snprintf(g_roles_dir, sizeof(g_roles_dir), "%s/aimee-test-cmddel-XXXXXX", platform_tmpdir());
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

/* Mirrors agent_config.c: the catalog (vendor) identity for capability lookup,
 * falling back to the wire provider when unset. Stubbed here because these tests
 * link delegate_routing.o without agent_config.o. */
const char *agent_catalog_provider(const agent_t *agent)
{
   if (!agent)
      return "";
   return agent->catalog_provider[0] ? agent->catalog_provider : agent->provider;
}

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
agent_route_block_t agent_routing_block_reason(const agent_t *agent, char *detail, size_t detail_sz)
{
   if (detail && detail_sz)
      detail[0] = '\0';
   if (!agent)
      return AGENT_ROUTE_NULL;
   return agent->enabled ? AGENT_ROUTE_OK : AGENT_ROUTE_POLICY_EXCLUDED;
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

/* When the caller supplies the review target (target_provided=1, e.g. a diff via
 * --prompt-file), the host-cwd "Validation Evidence Bundle" is suppressed and the
 * reviewer is pointed at aimee's branch-indexed capabilities instead. */
/* Build an mkdtemp template under TMPDIR rather than hardcoding /tmp: a shared
 * /tmp accumulates an entry per run, and the sandbox gives each session its own
 * TMPDIR precisely so those land somewhere disposable. */
static void review_tmp_template(char *out, size_t cap, const char *name)
{
   const char *base = getenv("TMPDIR");
   if (!base || !base[0])
      base = "/tmp";
   size_t len = strlen(base);
   while (len > 1 && base[len - 1] == '/')
      len--;
   snprintf(out, cap, "%.*s/%s-XXXXXX", (int)len, base, name);
}

/* A review-evidence provider the tests drive directly.
 *
 * WHICH roles are guarded, which are snippet-checked, and what counts as
 * claiming there was nothing to review are the module's rules, pinned against
 * the module (server-go/modules/delegates/reviewevidence_test.go) -- including
 * the two fixtures that used to live here. Restating them in this harness would
 * put the rule in two places again.
 *
 * What only this side can test is that the guard HONOURS a verdict: that it
 * runs the checkout comparison exactly when told to, reports the module's
 * wording, and leaves an unguarded review alone. So the verdict is set by the
 * test, not decided here. */
static unsigned g_review_verdict;
static const char *g_review_message = "";
static unsigned g_review_flags_seen;

static int test_review_evidence_provider(const char *role, const char *response, unsigned flags,
                                         unsigned *verdict, char *message, size_t message_cap)
{
   (void)role;
   (void)response;
   g_review_flags_seen = flags;
   if (verdict)
      *verdict = g_review_verdict;
   if (message && message_cap)
      snprintf(message, message_cap, "%s", g_review_message);
   return 0;
}

static void test_review_evidence_drift_detects_reversed_snippet(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-review-drift-XXXXXX", platform_tmpdir());
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
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-review-diff-drift-XXXXXX", platform_tmpdir());
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
   char root[256];
   snprintf(root, sizeof root, "%s/aimee-review-annotation-drift-XXXXXX", platform_tmpdir());
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

/* The guard reports the module's contradiction verdict, in the module's words.
 *
 * This replaces a fixture that pinned the ROLE list and the claim keywords by
 * driving a real git worktree. Those are rules and they now live in one place;
 * what could only be tested here is that the verdict reaches the caller. */
static void test_review_evidence_guard_reports_a_contradiction(void)
{
   char root[512];
   review_tmp_template(root, sizeof(root), "aimee-review-verdict");
   assert(mkdtemp(root) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q", root);
   assert(system(cmd) == 0);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup("No uncommitted diff exists.");
   assert(result.response != NULL);

   g_review_verdict = AIMEE_DELEGATES_REVIEW_GUARDED | AIMEE_DELEGATES_REVIEW_CONTRADICTION;
   g_review_message = "delegate evidence drift: the wording the module chose";
   int rc = 0;
   delegate_apply_review_evidence_guard("review", root, &rc, &result, 0);
   assert(rc == -1);
   assert(strcmp(result.error, "delegate evidence drift: the wording the module chose") == 0);
   free(result.response);

   /* An unguarded verdict leaves the review alone, whatever it said. */
   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup("No uncommitted diff exists.");
   assert(result.response != NULL);
   g_review_verdict = 0;
   g_review_message = "";
   rc = 0;
   delegate_apply_review_evidence_guard("code", root, &rc, &result, 0);
   assert(rc == 0);
   assert(result.error[0] == '\0');
   free(result.response);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
   assert(system(cmd) == 0);
   printf("  PASS: test_review_evidence_guard_reports_a_contradiction\n");
}

/* The caller runs the checkout comparison exactly when the verdict asks for it.
 *
 * The response below cites a snippet that does NOT match the file, so the
 * difference between the two halves is entirely whether CHECK_SNIPPETS was set.
 * This replaces a fixture that asserted the same thing via the role "diagnose";
 * which roles get the check is now the module's to say. */
static void test_review_evidence_guard_runs_the_snippet_check_only_when_asked(void)
{
   char root[512];
   review_tmp_template(root, sizeof(root), "aimee-review-snippet");
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

   /* The reversed-order snippet from the drift test above: known to drift, so
    * the only difference between the two halves below is the verdict. */
   const char *response =
       "Findings\n"
       "**Severity: critical | Location: `src/kb_client.c:6`**\n"
       "```c\n"
       "cJSON_Delete(req);\n"
       "char *resp = kb_client_v1_post_json(\"/v1/internal/ingest/job/claim\", req, 1000, NULL);\n"
       "```\n";

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup(response);
   assert(result.response != NULL);

   g_review_verdict = AIMEE_DELEGATES_REVIEW_GUARDED;
   g_review_message = "";
   int rc = 0;
   delegate_apply_review_evidence_guard("diagnose", root, &rc, &result, 0);
   assert(rc == 0);
   assert(result.error[0] == '\0');
   free(result.response);

   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup(response);
   assert(result.response != NULL);

   g_review_verdict = AIMEE_DELEGATES_REVIEW_GUARDED | AIMEE_DELEGATES_REVIEW_CHECK_SNIPPETS;
   rc = 0;
   delegate_apply_review_evidence_guard("review", root, &rc, &result, 0);
   assert(rc == -1);
   assert(strstr(result.error, "delegate evidence drift") != NULL);
   free(result.response);

   unlink(path);
   rmdir(srcdir);
   rmdir(root);
   printf("  PASS: test_review_evidence_guard_runs_the_snippet_check_only_when_asked\n");
}

/* The worktree-dirty fact is computed here and travels WITH the question. A
 * module that never learns the worktree is dirty cannot catch the one thing
 * this guard exists to catch. */
static void test_review_evidence_guard_sends_the_facts_it_owns(void)
{
   char root[512];
   review_tmp_template(root, sizeof(root), "aimee-review-facts");
   assert(mkdtemp(root) != NULL);
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "git -C '%s' init -q", root);
   assert(system(cmd) == 0);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup("a report");
   assert(result.response != NULL);

   g_review_verdict = AIMEE_DELEGATES_REVIEW_GUARDED;
   g_review_message = "";
   g_review_flags_seen = 0xffffffffu;
   int rc = 0;
   delegate_apply_review_evidence_guard("review", root, &rc, &result, 0);
   assert((g_review_flags_seen & AIMEE_DELEGATES_REVIEW_WORKTREE_DIRTY) == 0);
   assert((g_review_flags_seen & AIMEE_DELEGATES_REVIEW_TARGET_PROVIDED) == 0);
   free(result.response);

   char path[512];
   snprintf(path, sizeof(path), "%s/pending.txt", root);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("pending\n", f);
   fclose(f);

   memset(&result, 0, sizeof(result));
   result.success = 1;
   result.response = strdup("a report");
   assert(result.response != NULL);
   g_review_flags_seen = 0;
   rc = 0;
   delegate_apply_review_evidence_guard("review", root, &rc, &result, 1);
   assert((g_review_flags_seen & AIMEE_DELEGATES_REVIEW_WORKTREE_DIRTY) != 0);
   assert((g_review_flags_seen & AIMEE_DELEGATES_REVIEW_TARGET_PROVIDED) != 0);
   free(result.response);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
   assert(system(cmd) == 0);
   printf("  PASS: test_review_evidence_guard_sends_the_facts_it_owns\n");
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
   assert(cfg.route_pinned == 1);

   /* A later unpinned request cannot inherit the positive pin bit. */
   assert(delegate_apply_route_overrides(&cfg, "code", NULL, -1, NULL, NULL, errbuf,
                                         sizeof(errbuf)) == 0);
   assert(cfg.route_pinned == 0);
   printf("  PASS: test_via_override_rejects_role_mismatch\n");
}

/* The capability POLICY moved to the module (stage 17): the predicate, the
 * context-window precedence and the modality relaxation are decided and tested
 * there, where the fleet-wide relaxation can actually be exercised.
 *
 * What stays here is the wrapper's own job, and it is the half most likely to
 * rot silently: RESOLVING each agent's facts out of the catalog, the registry
 * and the CLI adapters, and APPLYING the verdict to the fleet. A recording
 * provider makes both visible. */
static unsigned g_rf_flags[8];
static int g_rf_override_ctx[8], g_rf_catalog_ctx[8], g_rf_cli_ctx[8];
static unsigned g_rf_count, g_rf_required_caps;
static int g_rf_min_context, g_rf_drop_deprecated;
static int g_rf_keep[8];

static int recording_route_filter(const uint8_t *request, size_t request_len, uint8_t *response,
                                  size_t response_cap, size_t *response_len)
{
   if (request_len < AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN)
      return -1;
   g_rf_drop_deprecated = request[5];
   g_rf_count = aimee_delegates_get_u32(request + 8);
   g_rf_required_caps = aimee_delegates_get_u32(request + 12);
   g_rf_min_context = (int)aimee_delegates_get_u32(request + 16);
   if (g_rf_count > 8)
      return -1;

   for (unsigned i = 0; i < g_rf_count; i++)
   {
      const uint8_t *at = request + AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN +
                          (size_t)i * AIMEE_DELEGATES_ROUTEFILTER_AGENT_LEN;
      g_rf_flags[i] = aimee_delegates_get_u32(at);
      g_rf_override_ctx[i] = (int)aimee_delegates_get_u32(at + 8);
      g_rf_catalog_ctx[i] = (int)aimee_delegates_get_u32(at + 12);
      g_rf_cli_ctx[i] = (int)aimee_delegates_get_u32(at + 16);
   }

   size_t need = 16u + (size_t)g_rf_count * 4u;
   if (response_cap < need)
      return -1;
   memset(response, 0, need);
   aimee_delegates_put_u32(response, AIMEE_DELEGATES_ROUTEFILTER_RESPONSE_MAGIC);
   unsigned kept = 0;
   for (unsigned i = 0; i < g_rf_count; i++)
      if (g_rf_keep[i])
      {
         aimee_delegates_put_u32(response + 16 + i * 4, 1u);
         kept++;
      }
   aimee_delegates_put_u32(response + 4, kept);
   *response_len = need;
   return 0;
}

static void test_route_filter_resolves_facts_and_applies_the_verdict(void)
{
   delegate_register_route_filter_provider(recording_route_filter);

   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   /* A tmux-CLI agent: no model and no explicit window, so its window can only
    * come from the codex adapter. That resolution is the wrapper's, and the
    * module cannot do it -- so it must arrive on the wire. */
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "codex");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "codex");
   snprintf(cfg.agents[0].cli_kind, sizeof(cfg.agents[0].cli_kind), "codex");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;

   /* An explicit override, which must be sent as itself rather than resolved. */
   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "other");
   snprintf(cfg.agents[1].provider, sizeof(cfg.agents[1].provider), "mystery");
   snprintf(cfg.agents[1].cli_kind, sizeof(cfg.agents[1].cli_kind), "no-such-cli");
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 0;
   cfg.agents[1].middleware.context_window = 400000;

   g_rf_keep[0] = 1; /* the module's answer: keep the first, drop the second */
   g_rf_keep[1] = 0;

   char errbuf[128];
   assert(delegate_filter_route_capabilities(&cfg, "review", MODEL_CAP_TOOLS, 5131, 1, errbuf,
                                             sizeof(errbuf)) == 0);

   /* The requirement reached the module unchanged. */
   assert(g_rf_count == 2);
   assert(g_rf_required_caps == MODEL_CAP_TOOLS);
   assert(g_rf_min_context == 5131);
   assert(g_rf_drop_deprecated == 1);

   /* Facts the module cannot look up: role membership, the agent's own tools
    * setting, and the CLI adapter's window. */
   assert(g_rf_flags[0] & AIMEE_DELEGATES_RF_ENABLED);
   assert(g_rf_flags[0] & AIMEE_DELEGATES_RF_HAS_ROLE);
   assert(g_rf_flags[0] & AIMEE_DELEGATES_RF_TOOLS);
   assert(g_rf_cli_ctx[0] > 0); /* resolved from the codex adapter */
   assert(g_rf_override_ctx[0] == 0);

   assert(!(g_rf_flags[1] & AIMEE_DELEGATES_RF_TOOLS)); /* tools off travels */
   assert(g_rf_override_ctx[1] == 400000);              /* sent, not resolved */
   assert(g_rf_cli_ctx[1] == 0);                        /* unknown CLI has none */

   /* And the verdict was applied to the fleet. */
   assert(cfg.agents[0].enabled == 1);
   assert(cfg.agents[1].enabled == 0);
   printf("  PASS: test_route_filter_resolves_facts_and_applies_the_verdict\n");
}

/* With no provider there is no verdict, and the router must refuse rather than
 * route on requirements nothing checked. */
static void test_route_filter_refuses_without_a_provider(void)
{
   delegate_register_route_filter_provider(NULL);

   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "a");
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;

   char errbuf[128] = "";
   assert(delegate_filter_route_capabilities(&cfg, "review", MODEL_CAP_TOOLS, 0, 0, errbuf,
                                             sizeof(errbuf)) == -1);
   assert(errbuf[0] != '\0');

   delegate_register_route_filter_provider(recording_route_filter);
   printf("  PASS: test_route_filter_refuses_without_a_provider\n");
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

/* agent_config.c is not linked here (this test stubs the agent layer), so the
 * scope name helper is mirrored. Kept behaviourally identical. */
const char *agent_scope_name(agent_scope_t s)
{
   switch (s)
   {
   case AGENT_SCOPE_BOUNDED:
      return "bounded";
   case AGENT_SCOPE_WHOLE_TASK:
      return "whole_task";
   default:
      return "";
   }
}

/* The scope filter is what makes `--scope bounded` actually shift work to a local
 * seat, and what stops `--scope whole_task` reaching one that cannot serve it.
 * Its error must name the fleet and each ceiling: "nothing can serve this" alone
 * leaves the operator guessing which seat to change. */
static void test_delegate_filter_route_scope(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.agents[0].name, "local");
   cfg.agents[0].enabled = 1;
   cfg.agents[0].max_scope = AGENT_SCOPE_BOUNDED;
   strcpy(cfg.agents[1].name, "capable");
   cfg.agents[1].enabled = 1;
   cfg.agents[1].max_scope = AGENT_SCOPE_UNSET; /* no ceiling */

   char err[512];

   /* Bounded work: both seats remain eligible. */
   assert(delegate_filter_route_scope(&cfg, AGENT_SCOPE_BOUNDED, err, sizeof(err)) == 0);
   assert(cfg.agents[0].enabled == 1 && cfg.agents[1].enabled == 1);

   /* Whole-task work: the ceiling disables the local seat, not the fleet. */
   assert(delegate_filter_route_scope(&cfg, AGENT_SCOPE_WHOLE_TASK, err, sizeof(err)) == 0);
   assert(cfg.agents[0].enabled == 0);
   assert(cfg.agents[1].enabled == 1);

   /* UNSET is a no-op here — the routing filter resolves it to whole_task. */
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   strcpy(cfg.agents[0].name, "local");
   cfg.agents[0].enabled = 1;
   cfg.agents[0].max_scope = AGENT_SCOPE_BOUNDED;
   assert(delegate_filter_route_scope(&cfg, AGENT_SCOPE_UNSET, err, sizeof(err)) == 0);
   assert(cfg.agents[0].enabled == 1);

   /* Nothing can serve it: fail, and SAY WHICH SEATS EXIST and their ceilings. */
   assert(delegate_filter_route_scope(&cfg, AGENT_SCOPE_WHOLE_TASK, err, sizeof(err)) != 0);
   assert(strstr(err, "whole_task") != NULL);
   assert(strstr(err, "local") != NULL);
   assert(strstr(err, "bounded") != NULL);

   printf("  PASS: test_delegate_filter_route_scope\n");
}

static int chain_via_module(unsigned op, int has_depth, int has_parent, int parent_known,
                            int parent_active, int parent_depth, int max_depth, int *flag,
                            int32_t *current_depth)
{
   uint8_t request[AIMEE_DELEGATES_CHAIN_REQUEST_LEN];
   uint8_t response[AIMEE_DELEGATES_CHAIN_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DELEGATES_STAGE_CHAIN};
   return aimee_delegates_chain_request_encode(op, has_depth, has_parent, parent_known,
                                               parent_active, (int32_t)parent_depth,
                                               (int32_t)max_depth, request, sizeof(request)) == 0 &&
                  aimee_delegates_module_handler(&invocation, request, sizeof(request), response,
                                                 sizeof(response), &response_len,
                                                 NULL) == AIMEE_MODULE_STATUS_OK
              ? aimee_delegates_chain_response_decode(response, response_len, flag, current_depth)
              : -1;
}

int main(void)
{
   delegate_role_seam_install();
   /* Fails closed until the module is reachable: no capability is asserted from
    * a local guess. */
   {
      unsigned caps = 0xffffffffu;
      int min_ctx = -1;
      delegate_infer_capability_requirements("screenshot.png", 1, &caps, &min_ctx);
      assert(caps == 0 && min_ctx == 0);
   }
   delegate_routing_register_capability_provider(capabilities_via_module);
   delegate_register_chain_provider(chain_via_module);
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
   test_prompt_plan_inline_prompt_only();
   test_prompt_plan_file_only();
   test_prompt_plan_prompt_and_file();
   test_prompt_plan_requires_prompt_source();
   delegate_register_review_evidence_provider(test_review_evidence_provider);
   test_review_evidence_drift_detects_reversed_snippet();
   test_review_evidence_drift_ignores_historical_diff_snippet();
   test_review_evidence_drift_ignores_inline_review_annotation();
   test_review_evidence_guard_reports_a_contradiction();
   test_review_evidence_guard_runs_the_snippet_check_only_when_asked();
   test_review_evidence_guard_sends_the_facts_it_owns();
   test_apply_max_turns_override();
   test_apply_max_turns_policy_caps_inspection_roles();
   test_apply_max_turns_policy_aliases();
   test_delegate_route_preflight_rejects_unknown_role();
   test_inline_acp_agent_synthesis();
   test_inline_acp_agent_no_args_and_guards();
   test_tier_override_keeps_same_tier_pool();
   test_delegate_checkout_records_unavailable_heads();
   test_via_override_rejects_role_mismatch();
   test_route_filter_resolves_facts_and_applies_the_verdict();
   test_route_filter_refuses_without_a_provider();
   test_capability_inference_audio_extension_not_keyword();
   test_capability_inference_detects_modalities();
   test_delegate_filter_route_scope();
   printf("All tests passed.\n");
   return 0;
}
