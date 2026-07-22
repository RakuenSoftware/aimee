/* server_mcp_surface.c: server-composition filtering for optional MCP tools. */
#include "server_mcp_surface.h"
#include "roundtable_activation.h"

int server_mcp_tool_available(const char *tool)
{
   return roundtable_tool_available(tool);
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
