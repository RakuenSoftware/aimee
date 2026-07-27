/* server_mcp_surface.h: server-composition filtering for optional MCP tools. */
#ifndef AIMEE_SERVER_MCP_SURFACE_H
#define AIMEE_SERVER_MCP_SURFACE_H

#include "cJSON.h"

/* Remove tools whose owning optional module is inactive. Returns the number
 * removed. Ownership remains in the module; this helper only composes a list. */
int server_mcp_filter_unavailable_tools(cJSON *tools);
int server_mcp_tool_available(const char *tool);

#endif
