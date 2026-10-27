#ifndef AIMEE_SERVER_ACP_RESOURCES_H
#define AIMEE_SERVER_ACP_RESOURCES_H
#include "server_session_capture.h"
#include <aimee/tools/agent_tools.h>
#include <aimee/ir/session_event.h>
typedef struct server_acp_resources server_acp_resources_t;
/* Borrowed capture/control/workspace lifetime remains with the synchronous
 * execution owner. Requires an existing canonical sink; never replaces its
 * pending-control registration or persistence semantics. */
server_acp_resources_t *server_acp_resources_open(server_session_capture_t *capture,
                                                  const char *source_session, const char *actor,
                                                  const char *cwd, int tools_enabled);
const aimee_request_t *server_acp_resources_catalog(const server_acp_resources_t *owner);
int server_acp_resources_sink(const aimee_session_event_t *event, void *owner);
int server_acp_resources_execute(const aimee_block_t *call, aimee_block_t *result, void *owner);
/* Private MCP view of the existing role-filtered registry. request_key belongs
 * to one host-received pending request, not an upstream model generation. */
const aimee_request_t *server_acp_resources_mcp_catalog(const server_acp_resources_t *owner);
int server_acp_resources_mcp_execute(server_acp_resources_t *owner, const char *request_key,
                                     const char *tool, const struct cJSON *arguments,
                                     int timeout_ms, aimee_block_t *result);
typedef int (*server_acp_bash_open_fn)(const char *committed_digest,
                                       const aimee_request_t *actual_catalog,
                                       agent_tools_bash_scope_t *scope, void **lease,
                                       void *context);
typedef void (*server_acp_bash_close_fn)(void *lease, void *context);
/* Private constructor-only binding. Opens only after a committed original Bash
 * call is claimed and admitted; never a tool update or listener-thread grant. */
int server_acp_resources_set_bash_owner(server_acp_resources_t *owner, server_acp_bash_open_fn open,
                                        server_acp_bash_close_fn close, void *context);
void server_acp_resources_close(server_acp_resources_t *owner);
#endif
