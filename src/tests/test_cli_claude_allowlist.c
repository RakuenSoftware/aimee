/* test_cli_claude_allowlist.c -- every tool handed to the provider CLI must be
 * accounted for by the workflow egress gate.
 *
 * WHY THIS EXISTS, AND WHY IT IS SMALL
 *
 * The egress gate decides "can this tool send data outside the boundary?" for
 * built-in tools by declaration (tool_egress.c, enforced at startup) and for
 * third-party MCP tools by defaulting them to external. Neither mechanism can
 * see the provider CLI's --allowedTools list: those tools are the host CLI's,
 * not aimee's, and they are named in a source file rather than registered.
 *
 * WebFetch and WebSearch are deliberately absent: the no-network container must
 * use aimee's mediated web tools. Bash is recognised as a shell tool and the
 * remaining provider tools are local.
 *
 * The real exposure is temporal: a name-matched gate is fail-open for anything
 * added after the list was written, and this allowlist is precisely where
 * "added after" happens. Adding an egress-capable tool here would be silently
 * ungated. This test closes that by forcing every entry into one of three
 * buckets, so a new arrival fails until somebody classifies it.
 *
 * That is the whole fix. It does not need registration-time metadata for host
 * tools, because the list is static and short -- the problem is that nothing was
 * checking it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "provider_cli_adapter.h"
#include "wfe_externalization.h"

int wfe_is_shell_tool(const char *tool_name);

/* Reviewed as local: reads, edits, and searches within the workspace. These do
 * not reach the network, so the gate correctly permits them pre-delivery --
 * denying them would break gated runs entirely. Extend ONLY with review, and
 * only for a tool that genuinely cannot carry data out. */
static const char *const KNOWN_LOCAL[] = {
    "Edit", "Read", "Write", "Glob", "Grep", "NotebookEdit",
};

/* aimee's own MCP server is the same trust domain: gating it would deny
 * in-process delegation during gated runs. A THIRD-PARTY mcp__ server is not
 * covered here -- those default to external in wfe_externalization.c. */
static int is_own_mcp(const char *t)
{
   return strcmp(t, "mcp__aimee") == 0 || strncmp(t, "mcp__aimee__", 12) == 0;
}

/* Claude Code allowlist entries may carry an argument pattern, e.g. `Bash(*)`.
 * The gate only ever sees the bare tool name, so compare on that. */
static void bare_name(const char *entry, char *out, size_t cap)
{
   size_t i = 0;
   while (entry[i] && entry[i] != '(' && i + 1 < cap)
   {
      out[i] = entry[i];
      i++;
   }
   out[i] = '\0';
}

static int in_known_local(const char *t)
{
   for (size_t i = 0; i < sizeof(KNOWN_LOCAL) / sizeof(KNOWN_LOCAL[0]); i++)
      if (strcmp(t, KNOWN_LOCAL[i]) == 0)
         return 1;
   return 0;
}

static void test_every_allowed_tool_is_accounted_for(void)
{
   size_t n = cli_claude_allowed_tools_count();
   const char *const *tools = cli_claude_allowed_tools();
   assert(n > 0);

   for (size_t i = 0; i < n; i++)
   {
      char name[128];
      bare_name(tools[i], name, sizeof(name));

      int gated = wfe_is_externalization_tool(name);
      int shell = wfe_is_shell_tool(name);
      int local = in_known_local(name) || is_own_mcp(name);

      if (!gated && !shell && !local)
      {
         fprintf(stderr,
                 "\n"
                 "  Tool \"%s\" is offered to the provider CLI but the egress gate\n"
                 "  does not account for it. Decide which it is:\n"
                 "    - it can send data outside the boundary  -> add it to\n"
                 "      wfe_externalization.c DENY_EXACT (it will be denied pre-delivery);\n"
                 "    - it runs a caller-supplied command      -> add it to\n"
                 "      wfe_native_gate.c wfe_is_shell_tool (command inspection gates it);\n"
                 "    - it is genuinely local                  -> add it to KNOWN_LOCAL\n"
                 "      in this file, with review.\n"
                 "  Do not silence this by picking the easiest bucket.\n\n",
                 name);
         assert(0 && "unclassified tool on the provider-CLI allowlist");
      }
      /* a tool must not be claimed as local while also being gated: that would
       * mean the reviewed list disagrees with the gate */
      if (local && gated)
      {
         fprintf(stderr, "tool \"%s\" is on KNOWN_LOCAL but the gate treats it as egress\n", name);
         assert(0 && "KNOWN_LOCAL contradicts the egress gate");
      }
   }
   printf("  PASS: all %zu provider-CLI tools accounted for by the egress gate\n", n);
}

/* Pin the specific classifications that matter, so a change to the gate that
 * silently ungated the two egress tools would fail here too. */
static void test_egress_tools_are_gated(void)
{
   assert(wfe_is_externalization_tool("WebFetch"));
   assert(wfe_is_externalization_tool("WebSearch"));
   assert(wfe_is_shell_tool("Bash"));
   size_t n = cli_claude_allowed_tools_count();
   const char *const *tools = cli_claude_allowed_tools();
   for (size_t i = 0; i < n; i++)
   {
      assert(strcmp(tools[i], "WebFetch") != 0);
      assert(strcmp(tools[i], "WebSearch") != 0);
   }
   /* and the local ones stay permitted, or gated runs break */
   assert(!wfe_is_externalization_tool("Read"));
   assert(!wfe_is_externalization_tool("Edit"));
   assert(!wfe_is_externalization_tool("Grep"));
   printf("  PASS: provider web tools absent; local tools permitted\n");
}

int main(void)
{
   test_every_allowed_tool_is_accounted_for();
   test_egress_tools_are_gated();
   printf("cli_claude_allowlist: all tests passed\n");
   return 0;
}
