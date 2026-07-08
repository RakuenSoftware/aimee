#ifndef DEC_SERVER_MCP_ENSEMBLE_H
#define DEC_SERVER_MCP_ENSEMBLE_H 1

#include "server.h"

int server_mcp_is_ensemble_tool(const char *tool);
int server_mcp_handle_ensemble_tool(server_conn_t *conn, const char *tool, cJSON *args,
                                    cJSON **content_out, cJSON **structured_out);
cJSON *server_mcp_compact_context(const char *session_id);
cJSON *server_mcp_set_primary_agent(const char *session_id, cJSON *args);
cJSON *server_mcp_upsert_persona(cJSON *args);
cJSON *server_mcp_upsert_role_template(cJSON *args);

#endif
