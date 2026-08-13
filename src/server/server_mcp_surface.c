/* server_mcp_surface.c: server-composition filtering for optional MCP tools. */
#include "server_mcp_surface.h"
#include "aimee_features.h"
#include "server_mcp_internal.h" /* ast_grep_available */
#include <string.h>
#if AIMEE_WITH_ROUNDTABLE
#include "roundtable_activation.h"
#endif

int server_mcp_tool_available(const char *tool)
{
   /* ast_grep_search needs an ast-grep binary this deployment may simply not
    * have: nothing in the image build or any deploy script installs one, so in a
    * container it is never present. Advertising it anyway costs the agent a call
    * to discover the capability is absent -- and before the resolver was fixed it
    * did not even learn that, because the tool answered "No matches found."
    *
    * Withheld rather than left listed for the same reason delegate tools are
    * withheld when delegation is off: a tool on the surface is a promise that it
    * can run. This is a real deployment state, not a presentation profile, so it
    * applies to every profile including "full". */
   if (tool && strcmp(tool, "ast_grep_search") == 0 && !ast_grep_available())
      return 0;
#if AIMEE_WITH_ROUNDTABLE
   return roundtable_tool_available(tool);
#else
   (void)tool;
   return 1;
#endif
}

int server_mcp_filter_unavailable_tools(cJSON *tools)
{
   if (!cJSON_IsArray(tools))
      return 0;
   int removed = 0;
   for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; i--)
   {
      cJSON *item = cJSON_GetArrayItem(tools, i);
      cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      if (cJSON_IsString(name) && !server_mcp_tool_available(name->valuestring))
      {
         cJSON_DeleteItemFromArray(tools, i);
         removed++;
      }
   }
   return removed;
}
