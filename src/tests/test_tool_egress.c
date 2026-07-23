/* test_tool_egress.c -- the egress declaration registry and its fail-closed
 * properties. The point of this registry is that a tool cannot be silently
 * ungated, so the tests below assert the *absence* of a bypass as much as the
 * presence of a classification. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tool_egress.h"
#include "wfe_externalization.h"

int wfe_native_tool_externalizes(const char *tool_name, const char *cmd);

/* The regression this whole module exists for: web_read performs real outbound
 * HTTP and was absent from both name-matched gates. */
static void test_web_read_is_external(void)
{
   assert(tool_egress_for("web_read") == TOOL_EGRESS_EXTERNAL);
   assert(tool_egress_is_external("web_read"));
   assert(tool_egress_is_external("WebRead")); /* case-insensitive */
   assert(tool_egress_is_external("webread")); /* declared alias */
   /* and it must now be visible to BOTH gates */
   assert(wfe_is_externalization_tool("web_read"));
   assert(wfe_native_tool_externalizes("web_read", NULL));
   printf("  PASS: web_read is classified external and gated by both paths\n");
}

/* The two gates previously disagreed about web_search: the native gate treated
 * it as a web tool while the externalization deny-list did not. One declaration
 * now drives both. */
static void test_web_search_agrees_across_gates(void)
{
   assert(tool_egress_for("web_search") == TOOL_EGRESS_EXTERNAL);
   assert(wfe_is_externalization_tool("web_search"));
   assert(wfe_native_tool_externalizes("web_search", NULL));
   printf("  PASS: web_search classified consistently across both gates\n");
}

static void test_publishing_tools_are_external(void)
{
   assert(tool_egress_for("git_push") == TOOL_EGRESS_EXTERNAL);
   assert(tool_egress_for("git_pr") == TOOL_EGRESS_EXTERNAL);
   printf("  PASS: publishing tools are external\n");
}

/* Reaching a socket is not the test; leaving the trust boundary is. The
 * knowledge-base tools talk to an internal service and must NOT be gated as
 * externalization, or ordinary retrieval would be blocked pre-delivery. */
static void test_internal_service_tools_are_not_external(void)
{
   assert(tool_egress_for("search_docs") == TOOL_EGRESS_NONE);
   assert(tool_egress_for("search_memory") == TOOL_EGRESS_NONE);
   assert(!tool_egress_is_external("search_docs"));
   assert(!wfe_is_externalization_tool("search_docs"));
   assert(!wfe_is_externalization_tool("search_memory"));
   printf("  PASS: internal-service tools are not externalization\n");
}

/* Ordinary local tools must stay ungated, otherwise the gate is useless. */
static void test_local_tools_are_none(void)
{
   static const char *local[] = {"read_file",  "write_file",  "edit_file", "list_files",
                                 "grep",       "git_log",     "git_diff",  "git_status",
                                 "git_commit", "find_symbol", NULL};
   for (int i = 0; local[i]; i++)
   {
      assert(tool_egress_for(local[i]) == TOOL_EGRESS_NONE);
      assert(!tool_egress_is_external(local[i]));
   }
   printf("  PASS: local tools declared none\n");
}

/* Shell-shaped tools are honestly recorded as command-dependent rather than
 * falsely asserted safe; their gating happens via command inspection. */
static void test_command_tools_are_command_class(void)
{
   assert(tool_egress_for("bash") == TOOL_EGRESS_COMMAND);
   assert(tool_egress_for("execute_script") == TOOL_EGRESS_COMMAND);
   assert(tool_egress_for("run_tests") == TOOL_EGRESS_COMMAND);
   assert(tool_egress_for("run_background_process") == TOOL_EGRESS_COMMAND);
   /* command-class is not itself externalization-by-name */
   assert(!tool_egress_is_external("bash"));
   printf("  PASS: command-dependent tools declared command\n");
}

/* An unknown name is UNSET, and callers must read that as "not classified
 * here", falling through to their own heuristics -- never as "safe". */
static void test_unknown_is_unset_not_safe(void)
{
   assert(tool_egress_for("mcp__github__create_pull_request") == TOOL_EGRESS_UNSET);
   assert(tool_egress_for("totally_made_up_tool") == TOOL_EGRESS_UNSET);
   assert(tool_egress_for("") == TOOL_EGRESS_UNSET);
   assert(tool_egress_for(NULL) == TOOL_EGRESS_UNSET);
   /* is_external answers "known external built-in?", so unknown is 0 ... */
   assert(!tool_egress_is_external("mcp__github__create_pull_request"));
   /* ... but the caller's own fallback must still catch it. */
   assert(wfe_is_externalization_tool("mcp__github__create_pull_request"));
   assert(wfe_is_externalization_tool("curl"));
   assert(wfe_is_externalization_tool("wget"));
   printf("  PASS: unknown names are UNSET and still caught by fallback\n");
}

/* Dynamically registered third-party tools cannot be covered by a startup
 * invariant, so they must be denied by DEFAULT rather than by name matching --
 * otherwise the fail-open hole simply moves from built-ins to MCP tools. */
static void test_third_party_mcp_is_external_by_default(void)
{
   /* names that match no deny substring at all */
   assert(wfe_is_externalization_tool("mcp__weather__get_forecast"));
   assert(wfe_is_externalization_tool("mcp__acme__lookup"));
   assert(wfe_is_externalization_tool("MCP__Acme__Lookup")); /* case-insensitive */
   /* aimee's own MCP server is the same trust domain: gating it would break
    * in-process delegation during gated runs */
   assert(!wfe_is_externalization_tool("mcp__aimee__delegate"));
   assert(!wfe_is_externalization_tool("mcp__aimee__search_memory"));
   /* a non-MCP ordinary tool is unaffected */
   assert(!wfe_is_externalization_tool("read_file"));
   /* The own-server exemption must not be prefix-spoofable: a third-party
    * server named so its tools start with our prefix must NOT be exempted. */
   assert(wfe_is_externalization_tool("mcp__aimeeevil__exfiltrate"));
   assert(wfe_is_externalization_tool("mcp__aimee_evil__x"));
   assert(wfe_is_externalization_tool("mcp__aimeex__y"));
   printf("  PASS: third-party MCP tools default to external (prefix not spoofable)\n");
}

/* No entry may be left undeclared, and no duplicate canonical names. */
static void test_registry_self_consistency(void)
{
   int n = tool_egress_count();
   assert(n > 0);
   for (int i = 0; i < n; i++)
   {
      const char *name = tool_egress_name_at(i);
      assert(name && name[0]);
      assert(tool_egress_class_at(i) != TOOL_EGRESS_UNSET);
      for (int j = i + 1; j < n; j++)
         assert(strcmp(name, tool_egress_name_at(j)) != 0);
   }
   assert(tool_egress_name_at(-1) == NULL);
   assert(tool_egress_name_at(n) == NULL);
   assert(tool_egress_class_at(n) == TOOL_EGRESS_UNSET);
   printf("  PASS: registry self-consistent (%d entries, no dups, none unset)\n", n);
}

/* Aliases must resolve to a real canonical entry with the same class, so an
 * alias can never stand in for the declaration a built-in is required to have. */
static void test_aliases_resolve_to_canonical(void)
{
   int n = tool_egress_count();
   for (int i = 0; i < n; i++)
   {
      const char *canon = tool_egress_canonical_at(i);
      if (!canon)
         continue;
      int target = -1;
      for (int j = 0; j < n; j++)
         if (tool_egress_names_equal(canon, tool_egress_name_at(j)) &&
             tool_egress_canonical_at(j) == NULL)
            target = j;
      assert(target >= 0);
      assert(tool_egress_class_at(target) == tool_egress_class_at(i));
   }
   printf("  PASS: aliases resolve to a canonical entry of the same class\n");
}

/* Duplicates must be detected the same case-insensitive way lookup resolves. */
static void test_no_case_variant_duplicates(void)
{
   int n = tool_egress_count();
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
         assert(!tool_egress_names_equal(tool_egress_name_at(i), tool_egress_name_at(j)));
   printf("  PASS: no case-variant duplicate declarations\n");
}

static void test_class_names(void)
{
   assert(strcmp(tool_egress_class_name(TOOL_EGRESS_NONE), "none") == 0);
   assert(strcmp(tool_egress_class_name(TOOL_EGRESS_EXTERNAL), "external") == 0);
   assert(strcmp(tool_egress_class_name(TOOL_EGRESS_COMMAND), "command") == 0);
   assert(strcmp(tool_egress_class_name(TOOL_EGRESS_UNSET), "unset") == 0);
   printf("  PASS: class names\n");
}

int main(void)
{
   test_web_read_is_external();
   test_web_search_agrees_across_gates();
   test_publishing_tools_are_external();
   test_internal_service_tools_are_not_external();
   test_local_tools_are_none();
   test_command_tools_are_command_class();
   test_unknown_is_unset_not_safe();
   test_third_party_mcp_is_external_by_default();
   test_registry_self_consistency();
   test_aliases_resolve_to_canonical();
   test_no_case_variant_duplicates();
   test_class_names();
   printf("tool_egress: all tests passed\n");
   return 0;
}
