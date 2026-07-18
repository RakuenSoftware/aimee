#pragma once
typedef struct cJSON cJSON;
/* Add gateway-specific MCP tools to the tool list. Called from mcp_build_tools_list(). */
void mcp_add_gateway_tools(cJSON *tools);