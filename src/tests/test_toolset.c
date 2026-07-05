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

/* review_indexed gives a reviewer aimee's branch-indexed capabilities and NO
 * filesystem/git tools, so a caller-provided-diff review cannot fall back to
 * reading a worktree it does not have. */
static void test_review_indexed_excludes_filesystem(void)
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
   assert(!has_tool(tools, n, "read_file"));
   assert(!has_tool(tools, n, "list_files"));
   assert(!has_tool(tools, n, "grep"));
   assert(!has_tool(tools, n, "git_diff"));
   printf("  review_indexed_excludes_filesystem: ok\n");
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
   assert(strcmp(toolset_for_delegate_role("review"), "current_code") == 0);
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
   assert(has_tool(tools, n, "read_file"));
   assert(!has_tool(tools, n, "find_symbol"));
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
   test_review_indexed_excludes_filesystem();
   test_cycle_rejected();
   test_unknown_tool_dropped();
   test_core_edit_flows_to_include();
   test_delegate_role_toolset();
   test_script_rpc_toolset();
   test_script_allowed_tools_config();
   test_script_allowed_tools_unknown_rejected();
   printf("All toolset tests passed.\n");
   return 0;
}
