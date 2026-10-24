#ifndef AIMEE_SERVER_NATIVE_RUNTIME_H
#define AIMEE_SERVER_NATIVE_RUNTIME_H

#include "agent_config.h"
#include "request_context.h"
#include "server_native_tool_source.h"
#include <aimee/ir/aimee_ir.h>

typedef struct server_native_runtime server_native_runtime_t;
struct cJSON;

/* Read-only private callback proof, active only while a constructor-admitted
 * service runtime forwards its own request. No setter or wire capability exists;
 * it never changes the owner's honest NONE transport or grants operator access. */
int server_native_runtime_forward_service_admitted(void);

/* Own one native child's local transport. The source session must already belong
 * to the attested request owner. No operator/global CLI credential is borrowed.
 * Model forwarding requires stable selected Vault source lineage (Codex OAuth
 * or API-key and Claude OAuth connections).
 * Caller retains its active workspace/delegate resource leases until close.
 * Close only after the native child exits; it joins all admitted callbacks.
 * Callback workers inherit model permissions and never operator authority.
 * MCP callbacks retain the session/principal-bound pending-control owner;
 * model-forwarding callbacks have no control binding. Both retain the durable
 * capture owner, independently of the producer connection, until callbacks join.
 * Model responses commit through that owner before replay-proof capture; message
 * identities include the host callback nonce, while tool identities stay exact. */
server_native_runtime_t *server_native_runtime_open(const agent_t *selected,
                                                    const request_context_t *owner,
                                                    const char *session_id);
/* Host-prepared, post-policy profile. Copies every owned field; profile must
 * have no original native request bytes and must match the selected model.
 * A retained running capture is required. Every model callback uses its fresh
 * canonical log snapshot, replacing SDK-proposed context/catalog/selectors only
 * at native entry. Original buffered/streamed calls must match the final
 * post-policy catalog before tool completion is released to the SDK. Parallel
 * model callbacks refuse rather than fork history.
 * This constructor alone does not attest a launch or activate SDK execution. */
server_native_runtime_t *server_native_runtime_open_request(const agent_t *selected,
                                                            const request_context_t *owner,
                                                            const char *session_id,
                                                            const aimee_request_t *profile);
/* Explicit host binding from an advertised logical tool to the existing
 * registry. mcp_name is the exact MCP tools/list name, or NULL for Codex tools.
 * This is a derived protocol mapping, never a registry or effect permission. */
typedef struct
{
   const char *space, *name, *registry_name, *mcp_name;
} server_native_tool_binding_t;
server_native_runtime_t *
server_native_runtime_open_bound_request(const agent_t *selected, const request_context_t *owner,
                                         const char *session, const aimee_request_t *profile,
                                         const server_native_tool_binding_t *bindings, int count);
/* Creator-only release after independently verifying this actual native launch.
 * Bound runtimes hold model forwarding until release; SDK bytes cannot release
 * it. Digest is non-secret launch configuration identity, not a credential. */
/* Install host-owned reserve40 identity before SDK/cache access. Does not
 * grant effects: source47 independently validates the registered pending claim. */
int server_native_runtime_set_native_owner(server_native_runtime_t *runtime,
                                           const struct cJSON *reservation);
/* Creator-only before launch. Bounds admitted model-generation requests;
 * count_tokens, MCP and tool routes do not spend turns. Every admitted attempt
 * spends one turn, including a failed provider request; no retry credit. */
int server_native_runtime_set_turn_limit(server_native_runtime_t *runtime, int limit);
int server_native_runtime_turn_limit_reached(const server_native_runtime_t *runtime);
/* Creator reads after callback quiescence: admitted generations plus the first
 * attempted generation refused by the cap. This is diagnostic, not retry credit. */
int server_native_runtime_model_turns(const server_native_runtime_t *runtime);
int server_native_runtime_attest_launch(server_native_runtime_t *runtime, const char *digest);
/* Host-only original-call admission. Revalidates the committed source prefix
 * and claims once for this turn; it does not execute or authorize any effect.
 * out is zero-initialized or previously owned. SDK callbacks still require
 * the supervised dispatcher after this succeeds. */
int server_native_runtime_claim_tool(server_native_runtime_t *runtime,
                                     const aimee_block_t *proposed,
                                     server_native_tool_claim_t *out);
/* Resolve a host-bound original-ID reference under the same source proof and
 * once-only claim. A reused ID refuses even if its old occurrence was claimed.
 * This does not accept SDK arguments or authorize execution; the original
 * proposed-call API retains its exact argument comparison. */
int server_native_runtime_claim_reference(server_native_runtime_t *runtime, const char *id,
                                          const char *space, const char *name,
                                          server_native_tool_claim_t *out);
/* Synchronous typed SDK callback on the creating producer thread, while its
 * capture, browser sink and workspace lease remain bound. Uses source-owned
 * arguments/catalog and existing supervised policy/effect/receipt owners.
 * Commits the actual result before returning it. No worker borrows the sink. */
int server_native_runtime_execute_tool(const aimee_block_t *call, aimee_block_t *result,
                                       void *runtime);
const char *server_native_runtime_url(const server_native_runtime_t *runtime);
const char *server_native_runtime_token(const server_native_runtime_t *runtime);
const char *server_native_runtime_binding(const server_native_runtime_t *runtime);
/* Immutable initial canonical source, including the running host turn. Borrowed
 * until close; NULL for a runtime without a capture owner. Native files are not
 * consulted. Later observations do not change this initial snapshot. */
const struct cJSON *server_native_runtime_source_history(const server_native_runtime_t *runtime);
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
/* After the child is reaped, join its transport callbacks before consuming the
 * original canonical terminal content and cumulative usage from this execution's
 * committed model completions. No SDK-normalized result is imported. */
/* Deactivate every bridge effect while retaining the bound port until the
 * independently owned native process boundary is confirmed empty. */
void server_native_runtime_deactivate_hold_port(server_native_runtime_t *runtime);
void server_native_runtime_quiesce(server_native_runtime_t *runtime);
int server_native_runtime_result(const server_native_runtime_t *runtime, aimee_response_t *out);
void server_native_runtime_close(server_native_runtime_t *runtime);

#endif
