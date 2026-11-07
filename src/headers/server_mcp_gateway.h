/* Complete MCP result for recognized gateway tools; NULL for unrecognized. */
#pragma once
typedef struct cJSON cJSON;
cJSON *mcp_gateway_tool_dispatch(const char *tool, cJSON *jargs);
