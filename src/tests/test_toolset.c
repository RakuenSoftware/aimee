#include "toolset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int has_tool(char tools[][TOOLSET_TOOL_MAX], int n, const char *name)
{
   for (int i = 0; i < n; i++)
      if (strcmp(tools[i], name) == 0)
         return 1;
   return 0;
}

static void write_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(text, f);
   fclose(f);
}

static void test_full_stack_resolves_union(void)
{
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   char err[TOOLSET_ERROR_MAX] = "";
   int n = toolset_resolve(&reg, "full_stack", tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
   assert(n > 0);
   assert(has_tool(tools, n, "bash"));
   assert(has_tool(tools, n, "git_status"));
   assert(has_tool(tools, n, "read_file"));
   assert(has_tool(tools, n, "search_memory"));
   /* edit_file is a write-capable tool the code role must expose (delegates
    * need surgical edits, not just whole-file write_file). */
   assert(has_tool(tools, n, "write_file"));
   assert(has_tool(tools, n, "edit_file"));
   for (int i = 1; i < n; i++)
      assert(strcmp(tools[i - 1], tools[i]) < 0);
   printf("  full_stack_resolves_union: ok\n");

   /* The code and current_code roles both resolve edit_file directly. */
   n = toolset_resolve(&reg, "code", tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
   assert(n > 0 && has_tool(tools, n, "edit_file") && has_tool(tools, n, "write_file"));
   n = toolset_resolve(&reg, "current_code", tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
   assert(n > 0 && has_tool(tools, n, "edit_file"));
   printf("  code_roles_resolve_edit_file: ok\n");
}

/* A coding delegate must resolve the git WRITE tools, and a reviewer must not.
 *
 * Registering a tool is not the same as a role being able to call it:
 * agent_tools_filter_for_role drops anything the role's toolset does not name. A
 * live delegate reported "there is no git_commit tool in my available toolset"
 * while the builtin registry carried it and the server had it wired — the tools
 * were being filtered straight back out here. require_aimee_git tells delegates to
 * use these instead of a shell, so if this resolution regresses, the rule starts
 * pointing at tools the delegate cannot see. */
static void test_git_write_tools_reach_coding_roles_only(void)
{
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   char err[TOOLSET_ERROR_MAX] = "";

   /* code (and full_stack, which the code/refactor/execute roles actually map to) */
   const char *coding[] = {"code", "full_stack"};
   for (size_t i = 0; i < sizeof(coding) / sizeof(coding[0]); i++)
   {
      int n = toolset_resolve(&reg, coding[i], tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
      assert(n > 0);
      assert(has_tool(tools, n, "git_commit"));
      assert(has_tool(tools, n, "git_push"));
      assert(has_tool(tools, n, "git_branch"));
      assert(has_tool(tools, n, "git_pr"));
      assert(has_tool(tools, n, "git_status")); /* the read-only set still comes too */
   }

   /* A reviewer reads; it does not commit. `git` (read-only) is inherited by
    * readonly -> review, so the write set had to be a SEPARATE toolset or every
    * reviewer would have silently gained push. */
   const char *readers[] = {"review", "review_indexed", "readonly"};
   for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); i++)
   {
      int n = toolset_resolve(&reg, readers[i], tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
      assert(n > 0);
      assert(!has_tool(tools, n, "git_commit"));
      assert(!has_tool(tools, n, "git_push"));
      assert(!has_tool(tools, n, "git_pr"));
   }
   printf("  git_write_tools_reach_coding_roles_only: ok\n");
}

/* find_callers must reach BOTH a reviewer and a coding delegate.
 *
 * It is the only tool that answers "is this still called?" — code_search is
 * semantic (it matched a file not containing the symbol) and find_symbol returns
 * definitions only. A review panel asked to judge a deletion had neither, so it
 * hedged: "unsafe absent a full audit of call sites". The lens that asks for a
 * call path is only honest while this resolves. */
static void test_find_callers_reaches_reviewers_and_coders(void)
{
   /* The tool is declared native in the server's MCP table, which this binary does
    * not link — so register it here exactly as mcp_tool_register_native_surface()
    * would. That is the point of the check: the mechanism must carry a tool all the
    * way from one declaration to a resolved role, through BOTH gates that used to
    * need hand-editing (KNOWN_TOOLS, which silently pruned git_write's four tools,
    * and toolset membership, which agent_tools_filter_for_role enforces). */
   toolset_register_native_tool("index_find_callers", "core");
   toolset_register_native_tool("index_find_callers", "review_indexed");

   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   char err[TOOLSET_ERROR_MAX] = "";
   const char *roles[] = {"review_indexed", "review",     "readonly",
                          "code",           "full_stack", "script_rpc"};
   for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++)
   {
      int n = toolset_resolve(&reg, roles[i], tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
      assert(n > 0);
      if (strcmp(roles[i], "script_rpc") == 0)
         continue; /* the scripted RPC surface is pinned separately; not a reviewer */
      assert(has_tool(tools, n, "index_find_callers"));
   }
   printf("  find_callers_reaches_reviewers_and_coders: ok\n");
}

/* A registration naming a set that does not exist must not invent it: an orphan set
 * nothing includes would leave the tool advertised and unreachable — the silent
 * failure this whole mechanism exists to end. It should be reported and dropped. */
static void test_native_tool_unknown_toolset_is_not_invented(void)
{
   toolset_register_native_tool("index_find_callers", "no_such_toolset");
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   assert(toolset_registry_find(&reg, "no_such_toolset") == NULL);
   printf("  native_tool_unknown_toolset_is_not_invented: ok\n");
}

/* review_indexed carries aimee's branch-indexed capabilities plus the read-only
 * worktree tools (read_file/list_files/grep). Slice 7: those reads are
 * REACHABILITY-GATED in agent_tools_tool_allowed_for_role (granted only when the
 * active provider can see the review worktree — see test_review_read_reachability_gate
 * in test_toolset_thread_scope), not excluded from the toolset. The toolset itself
 * must still NEVER carry write/exec or git tools: a reviewer must not edit what it
 * judges, and a caller-provided-diff review keeps no worktree-mutating power. */
static void test_review_indexed_read_tools_gated_no_write(void)
{
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   char err[TOOLSET_ERROR_MAX] = "";
   int n = toolset_resolve(&reg, "review_indexed", tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
   assert(n > 0);
   assert(has_tool(tools, n, "code_search"));
   assert(has_tool(tools, n, "find_symbol"));
   assert(has_tool(tools, n, "search_memory"));
   /* Read-only worktree tools ARE carried now (agent-layer reachability-gated). */
   assert(has_tool(tools, n, "read_file"));
   assert(has_tool(tools, n, "list_files"));
   assert(has_tool(tools, n, "grep"));
   /* Never write/exec/git — the must-not-edit invariant lives in the toolset. */
   assert(!has_tool(tools, n, "write_file"));
   assert(!has_tool(tools, n, "edit_file"));
   assert(!has_tool(tools, n, "bash"));
   assert(!has_tool(tools, n, "git_diff"));
   printf("  review_indexed_read_tools_gated_no_write: ok\n");
}

static void test_cycle_rejected(void)
{
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-toolset-cycle-%ld.yaml", (long)getpid());
   write_text(path, "toolsets:\n  a:\n    include:\n      - b\n  b:\n    include:\n      - a\n");
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char err[TOOLSET_ERROR_MAX] = "";
   assert(toolset_registry_load_file(&reg, path, err, sizeof(err)) != 0);
   assert(strstr(err, "cycle") != NULL);
   unlink(path);
   printf("  cycle_rejected: ok\n");
}

static void test_unknown_tool_dropped(void)
{
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-toolset-unknown-%ld.yaml", (long)getpid());
   write_text(path, "toolsets:\n"
                    "  custom:\n"
                    "    tools:\n"
                    "      - read_file\n"
                    "      - not_registered\n");
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char err[TOOLSET_ERROR_MAX] = "";
   assert(toolset_registry_load_file(&reg, path, err, sizeof(err)) == 0);
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   int n = toolset_resolve(&reg, "custom", tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
   assert(n == 1);
   assert(has_tool(tools, n, "read_file"));
   assert(!has_tool(tools, n, "not_registered"));
   unlink(path);
   printf("  unknown_tool_dropped: ok\n");
}

static void test_core_edit_flows_to_include(void)
{
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   toolset_def_t *core = NULL;
   for (int i = 0; i < reg.count; i++)
      if (strcmp(reg.sets[i].name, "core") == 0)
         core = &reg.sets[i];
   assert(core != NULL);
   snprintf(core->tools[core->tool_count++], TOOLSET_TOOL_MAX, "%s", "env_get");

   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   char err[TOOLSET_ERROR_MAX] = "";
   int n = toolset_resolve(&reg, "readonly", tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
   assert(n > 0);
   assert(has_tool(tools, n, "env_get"));
   printf("  core_edit_flows_to_include: ok\n");
}

static void test_delegate_role_toolset(void)
{
   assert(strcmp(toolset_for_delegate_role("review"), "review_indexed") == 0);
   assert(strcmp(toolset_for_delegate_role("inspect"), "current_code") == 0);
   assert(strcmp(toolset_for_delegate_role("validate"), "validate") == 0);
   assert(strcmp(toolset_for_delegate_role("test"), "validate") == 0);
   assert(strcmp(toolset_for_delegate_role("check"), "validate") == 0);
   assert(strcmp(toolset_for_delegate_role("search"), "readonly") == 0);
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   char err[TOOLSET_ERROR_MAX] = "";
   int n = toolset_resolve(&reg, toolset_for_delegate_role("review"), tools, TOOLSET_MAX_TOOLS, err,
                           sizeof(err));
   /* review carries branch-index nav tools plus the read-only worktree tools
    * (reachability-gated at the agent layer, slice 7); never write/exec. */
   assert(has_tool(tools, n, "read_file"));
   assert(!has_tool(tools, n, "write_file") && !has_tool(tools, n, "bash"));
   assert(has_tool(tools, n, "find_symbol") && has_tool(tools, n, "search_memory"));
   n = toolset_resolve(&reg, toolset_for_delegate_role("validate"), tools, TOOLSET_MAX_TOOLS, err,
                       sizeof(err));
   assert(has_tool(tools, n, "bash"));
   assert(has_tool(tools, n, "execute_script"));
   assert(has_tool(tools, n, "verify"));
   n = toolset_resolve(&reg, toolset_for_delegate_role("search"), tools, TOOLSET_MAX_TOOLS, err,
                       sizeof(err));
   assert(has_tool(tools, n, "read_file"));
   assert(has_tool(tools, n, "find_symbol"));
   assert(has_tool(tools, n, "verify"));
   assert(!has_tool(tools, n, "bash"));
   printf("  delegate_role_toolset: ok\n");
}

static void test_script_rpc_toolset(void)
{
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   assert(strcmp(reg.script_allowed_tools, "script_rpc") == 0);
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   char err[TOOLSET_ERROR_MAX] = "";
   int n =
       toolset_resolve(&reg, reg.script_allowed_tools, tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
   assert(has_tool(tools, n, "read_file"));
   assert(has_tool(tools, n, "request_input"));
   assert(!has_tool(tools, n, "write_file"));
   printf("  script_rpc_toolset: ok\n");
}

static void test_script_allowed_tools_config(void)
{
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-toolset-script-%ld.yaml", (long)getpid());
   write_text(path, "toolsets:\n"
                    "  scripts_readonly:\n"
                    "    tools:\n"
                    "      - read_file\n"
                    "script:\n"
                    "  allowed_tools: scripts_readonly\n");
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char err[TOOLSET_ERROR_MAX] = "";
   assert(toolset_registry_load_file(&reg, path, err, sizeof(err)) == 0);
   assert(strcmp(reg.script_allowed_tools, "scripts_readonly") == 0);
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   int n =
       toolset_resolve(&reg, reg.script_allowed_tools, tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
   assert(n == 1);
   assert(has_tool(tools, n, "read_file"));
   unlink(path);
   printf("  script_allowed_tools_config: ok\n");
}

static void test_script_allowed_tools_unknown_rejected(void)
{
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-toolset-script-bad-%ld.yaml", (long)getpid());
   write_text(path, "script:\n"
                    "  allowed_tools: missing_set\n");
   toolset_registry_t reg;
   toolset_registry_init(&reg);
   char err[TOOLSET_ERROR_MAX] = "";
   assert(toolset_registry_load_file(&reg, path, err, sizeof(err)) != 0);
   assert(strstr(err, "script.allowed_tools") != NULL);
   unlink(path);
   printf("  script_allowed_tools_unknown_rejected: ok\n");
}

int main(void)
{
   printf("test_toolset:\n");
   test_full_stack_resolves_union();
   test_git_write_tools_reach_coding_roles_only();
   test_find_callers_reaches_reviewers_and_coders();
   test_review_indexed_read_tools_gated_no_write();
   test_cycle_rejected();
   test_unknown_tool_dropped();
   test_core_edit_flows_to_include();
   test_delegate_role_toolset();
   test_script_rpc_toolset();
   test_script_allowed_tools_config();
   test_script_allowed_tools_unknown_rejected();
   /* Last: registrations are process-global (the server registers once at startup),
    * so this test's deliberately-bad entry would otherwise re-report on every
    * subsequent registry_init and drown the other tests' output. */
   test_native_tool_unknown_toolset_is_not_invented();
   printf("All toolset tests passed.\n");
   return 0;
}
