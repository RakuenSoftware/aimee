#ifndef AIMEE_SERVER_ACP_EXECUTION_H
#define AIMEE_SERVER_ACP_EXECUTION_H
#include "provider_cli_adapter.h"
/* The strong host dispatch alone creates the retained external-agent owner.
 * No provider SDK/profile/cache provenance or ambient fallback is implied. */
int server_native_execute_owned(const provider_cli_cfg_t *cfg, agent_result_t *out);
int server_acp_execute_owned(const provider_cli_cfg_t *cfg, agent_result_t *out);
/* Producer-only pre-acquisition endpoint. It refuses every request until the
 * admitted ACP constructor attaches its actual owner. No wire-provided path. */
typedef struct server_acp_container_scope server_acp_container_scope_t;
server_acp_container_scope_t *server_acp_container_scope_open(void);
const char *server_acp_container_scope_socket(const server_acp_container_scope_t *scope);
void server_acp_container_scope_close(server_acp_container_scope_t *scope);
int server_acp_container_bind(const agent_t *selected, const char *task, const char *image,
                              const char *workspace, int read_only,
                              server_acp_container_scope_t **scope);
/* One producer-local plan spans every real dispatch and repair. Each selected
 * attempt receives fresh resources; close must precede business application. */
typedef struct server_delegate_container_plan server_delegate_container_plan_t;
server_delegate_container_plan_t *server_delegate_container_plan_open(const char *delegation,
                                                                      const char *image,
                                                                      const char *workspace,
                                                                      int read_only);
int server_delegate_container_plan_close(server_delegate_container_plan_t *plan);
#endif
