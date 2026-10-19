#ifndef AIMEE_SERVER_NATIVE_RUNTIME_H
#define AIMEE_SERVER_NATIVE_RUNTIME_H

#include "agent_config.h"
#include "request_context.h"

typedef struct server_native_runtime server_native_runtime_t;

/* Own one native child's local transport. The source session must already belong
 * to the attested request owner. No operator/global CLI credential is borrowed.
 * Model forwarding requires stable Vault source lineage (currently Codex);
 * other native credential owners still require their custody cutover.
 * Caller retains its active workspace/delegate resource leases until close.
 * Close only after the native child exits; it joins all admitted callbacks.
 * Callback workers inherit model permissions and never operator controls. */
server_native_runtime_t *server_native_runtime_open(const agent_t *selected,
                                                    const request_context_t *owner,
                                                    const char *session_id);
const char *server_native_runtime_url(const server_native_runtime_t *runtime);
const char *server_native_runtime_token(const server_native_runtime_t *runtime);
const char *server_native_runtime_binding(const server_native_runtime_t *runtime);
/* A cache scope includes the captured host authority and these independently
 * attested launch/resource identities. Digests must describe the actual SDK
 * build, effective launch settings (including sandbox, built-ins and hooks),
 * and the stable workspace lease; never hash credentials or process pointers.
 * Account replay binding alone is not an execution/cache profile. */
typedef struct
{
   const char *sdk;
   const char *build_digest;
   const char *launch_digest;
   const char *workspace_digest;
} server_native_execution_profile_t;
int server_native_runtime_cache_scope(const server_native_runtime_t *runtime,
                                      const server_native_execution_profile_t *profile,
                                      char scope[65]);
void server_native_runtime_close(server_native_runtime_t *runtime);

#endif
