#ifndef DEC_SERVER_MCP_DELEGATE_H
#define DEC_SERVER_MCP_DELEGATE_H 1

#include "server.h"

int handle_mcp_delegate_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *args, const char *sid);
/* Native primary orchestration uses the normal authenticated dispatch owner.
 * The returned JSON text is owned by the caller. NULL refuses an unavailable hop. */
char *server_mcp_delegate_native(const char *tool, const cJSON *args, const char *sid);

#endif /* DEC_SERVER_MCP_DELEGATE_H */
