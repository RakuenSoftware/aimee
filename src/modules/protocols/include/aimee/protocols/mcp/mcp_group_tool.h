/* mcp_group_tool.h: one MCP tool per command group, multiplexed by `command`.
 *
 * `aimee memory get` and MCP `memory` with command=get are the same command,
 * spelled the same way -- not memory_get, not search_memory. Built from the
 * command registry, so the verbs a tool advertises ARE the verbs the owning module
 * registered for MCP; neither can drift from the other.
 *
 * Follows `git` and `index`, which are already multiplexed this way. The reason is
 * recorded in mcp_tool_profile.c: a tool reachable only through find_tools ->
 * describe_tool -> call_tool is one agents do not pay for, so a group with many
 * verbs has to cost ONE entry in tools/list rather than one per verb. */
#ifndef DEC_MCP_GROUP_TOOL_H
#define DEC_MCP_GROUP_TOOL_H 1

struct cJSON;

/* Build the multiplexed tool for `group`, or NULL when the group has no verb on
 * the MCP surface (a tool whose `command` accepts nothing wastes the agent a call
 * to discover it can do nothing). Caller owns the result. */
struct cJSON *mcp_group_tool_build(const char *group, const char *summary);

#endif /* DEC_MCP_GROUP_TOOL_H */
