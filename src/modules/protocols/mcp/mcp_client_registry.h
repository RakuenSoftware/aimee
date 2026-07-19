#ifndef DEC_MCP_CLIENT_REGISTRY_H
#define DEC_MCP_CLIENT_REGISTRY_H 1

#include "config.h"
#include "mcp_client.h"

/* Boot the process-wide MCP client registry from config.
 * Returns the number of live sessions after startup. Never fails hard; entries
 * that cannot start are skipped and should be surfaced through stderr/logs. */
int mcp_client_registry_boot(const config_t *cfg);

/* Close all live sessions and clear the registry. Safe to call repeatedly. */
void mcp_client_registry_shutdown(void);

/* Lookup a live session by configured client name. Returned pointer is owned by
 * the registry and remains valid until shutdown. */
mcp_client_session_t *mcp_client_registry_get(const char *name);

/* Number of live sessions currently registered. */
int mcp_client_registry_count(void);

/* Name of the live session at |index|, or NULL if out of range. */
const char *mcp_client_registry_name_at(int index);

/* Fetch and cache tools/list lazily for a live client. On success, |*out_tools|
 * receives a deep copy of the cached tool schema array/object. Caller owns it. */
int mcp_client_registry_list_tools(const char *name, int timeout_ms, cJSON **out_tools,
                                   char *err_buf, size_t err_buf_len);

/* Build a flat array of live remote tools with namespaced names of the form
 * "<client>:<tool>". Each entry uses MCP tool shape:
 *   {"name","description","inputSchema"}.
 * Caller owns the returned array. */
cJSON *mcp_client_registry_build_namespaced_tools(int timeout_ms);

/* Lookup a single namespaced remote tool schema. On success, |*out_tool|
 * receives a duplicated MCP-style tool object with a namespaced "name". */
int mcp_client_registry_get_tool_schema(const char *qualified_name, int timeout_ms,
                                        cJSON **out_tool, char *err_buf, size_t err_buf_len);

/* Dispatch a namespaced remote tool call to the matching live MCP client. */
int mcp_client_registry_call_tool(const char *qualified_name, const cJSON *args, int timeout_ms,
                                  cJSON **out_result, char *err_buf, size_t err_buf_len);

#endif /* DEC_MCP_CLIENT_REGISTRY_H */
