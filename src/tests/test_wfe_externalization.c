/* test_wfe_externalization.c -- the pre-delivery externalization guard (I3):
 * externalization primitives are denied until gate.deliver has passed; ordinary
 * tools are always permitted at this layer; post-delivery the guard lifts. */
#include <assert.h>
#include <stdio.h>

#include "wfe_externalization.h"

int main(void)
{
   printf("wfe-externalization: ");

   /* externalization primitives are recognized (exact + substring/MCP forms) */
   assert(wfe_is_externalization_tool("pr.open"));
   assert(wfe_is_externalization_tool("merge"));
   assert(wfe_is_externalization_tool("git_push"));
   assert(wfe_is_externalization_tool("WebFetch")); /* case-insensitive */
   /* web_read performs real server-side egress (posix/web_read.c) and is a
    * registered tool (server/agent_tools.c). It must be gated like any other
    * egress primitive; it was previously absent from the deny-list. */
   assert(wfe_is_externalization_tool("web_read"));
   assert(wfe_is_externalization_tool("WebRead"));
   assert(wfe_is_externalization_tool("mcp__github__create_pull_request")); /* substring */
   assert(wfe_is_externalization_tool("deploy_release"));
   assert(wfe_is_externalization_tool("notify_slack"));

   /* ordinary in-worktree tools are NOT externalization */
   assert(!wfe_is_externalization_tool("Read"));
   assert(!wfe_is_externalization_tool("Edit"));
   assert(!wfe_is_externalization_tool("Grep"));
   assert(!wfe_is_externalization_tool("aimee_git_verify"));
   assert(!wfe_is_externalization_tool(""));
   assert(!wfe_is_externalization_tool(NULL));

   /* pre-delivery (delivered=0): externalization denied, ordinary permitted */
   assert(wfe_externalization_tool_permitted("Read", 0) == 1);
   assert(wfe_externalization_tool_permitted("Edit", 0) == 1);
   assert(wfe_externalization_tool_permitted("pr.open", 0) == 0);
   assert(wfe_externalization_tool_permitted("merge", 0) == 0);
   assert(wfe_externalization_tool_permitted("mcp__github__create_pull_request", 0) == 0);
   assert(wfe_externalization_tool_permitted(NULL, 0) == 0); /* fail closed */

   /* post-delivery (delivered=1): the guard lifts, all permitted */
   assert(wfe_externalization_tool_permitted("pr.open", 1) == 1);
   assert(wfe_externalization_tool_permitted("merge", 1) == 1);
   assert(wfe_externalization_tool_permitted(NULL, 1) == 1);

   printf("ok\n");
   return 0;
}
