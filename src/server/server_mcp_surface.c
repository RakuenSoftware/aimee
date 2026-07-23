/* server_mcp_surface.c: server-composition filtering for optional MCP tools. */
#include "server_mcp_surface.h"
#include "aimee_features.h"
#if AIMEE_WITH_ROUNDTABLE
#include "roundtable_activation.h"
#endif

int server_mcp_tool_available(const char *tool)
{
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
