/* Host-issued native execution authority. Handles contain no caller claims. */
#ifndef AIMEE_SERVER_NATIVE_DELEGATE_SOURCE_H
#define AIMEE_SERVER_NATIVE_DELEGATE_SOURCE_H
#include "agent_config.h"
#include <aimee/ir/aimee_ir.h>
#include <stddef.h>
#define SERVER_NATIVE_DELEGATE_SCOPE_LEN 65

typedef struct server_native_delegate_source_scope server_native_delegate_source_scope_t;
/* Called only at a host-authenticated workflow/panel boundary. Sealed source
 * context is durable; neither user JSON nor a Submitter string can mint it. */
int server_native_delegate_source_mint(const char *purpose, char handle[65], char *error,
                                       size_t capacity);
int server_native_delegate_source_mint_for_workspace(const char *purpose, const char *workspace,
                                                     char handle[65], char *error, size_t capacity);
/* Private authenticated host-event resolver. A registered background trigger
 * receives an explicit server-service source, never an inferred human actor. */
int server_native_delegate_source_trigger(const struct cJSON *registration,
                                          const char *configuration_sha256, char handle[65],
                                          char *error, size_t capacity);
/* The bridge checks authenticated module source before calling this function.
 * Configuration/profile, source ownership and workspace custody are checked
 * again here. On failure no caller TLS is changed. */
int server_native_delegate_source_bind(
    const char *handle, unsigned principal_ref, const char *operation, const char *role,
    const char *registration, const char *workdir, const aimee_request_t *request, int tools,
    int max_turns, long timeout_ms, server_native_delegate_source_scope_t **scope,
    agent_config_t *config, agent_t *selected, char *error, size_t capacity);
/* Called by the host worker after registered persona policy composition and
 * before dispatch. Records the effective owned canonical seed, never display. */
int server_native_delegate_source_admit(server_native_delegate_source_scope_t *scope,
                                        const char *persona,
                                        const aimee_request_t *effective_request, char *error,
                                        size_t capacity);
void server_native_delegate_source_unbind(server_native_delegate_source_scope_t *scope);
/* Host-owned retirement; never accepts actor or credential claims from callers. */
int server_native_delegate_source_revoke(const char *handle);
#endif
