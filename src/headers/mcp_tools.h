#ifndef DEC_MCP_TOOLS_H
#define DEC_MCP_TOOLS_H 1

#include "cJSON.h"

/* Build the complete MCP tools list (core + git tools).
 * Returns a cJSON array suitable for tools/list responses. */
cJSON *mcp_build_tools_list(void);

/* Resolve the active MCP presentation profile: the explicit argument if set,
 * else AIMEE_MCP_TOOL_PROFILE, else "core" (the lean default). Not owned. */
const char *mcp_tool_profile_effective(const char *explicit_profile);

/* Filter a served tools/list IN PLACE to the named profile (NULL => resolve via
 * mcp_tool_profile_effective). "full" / unknown is a no-op (fail open); "core"
 * or "lean" keeps only the Tier-0 set. Returns the number of tools removed. */
int mcp_filter_tools_for_profile(cJSON *tools, const char *profile);

/* Append the find_tools / describe_tool discovery meta-tools to a tools list.
 * Called by mcp_build_tools_list so they are always present (and in the core
 * profile), keeping a lean presentation lossless. */
void mcp_add_discovery_tools(cJSON *tools);

#endif /* DEC_MCP_TOOLS_H */
