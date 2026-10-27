#ifndef AIMEE_SERVER_NATIVE_CLI_H
#define AIMEE_SERVER_NATIVE_CLI_H
#include "cJSON.h"
#include <aimee/ir/aimee_ir.h>
#include <stdatomic.h>

typedef struct server_native_cli server_native_cli_t;
/* One grant belongs to one retained, already admitted shell source. Requests
 * are serial and nonce-idempotent. No model-call IDs are created here. */
server_native_cli_t *server_native_cli_open(const char *endpoint, const char *session,
                                            const char *actor, const char *cwd,
                                            const char *parent_attempt, unsigned depth);
const char *server_native_cli_token(const server_native_cli_t *grant);
char *const *server_native_cli_environment(const server_native_cli_t *grant);
/* Exact token comparison; caller holds its runtime's list lock while finding
 * and retaining a grant, preventing revocation/free races. */
int server_native_cli_matches(const server_native_cli_t *grant, const char *token);
int server_native_cli_retain(server_native_cli_t *grant);
void server_native_cli_release(server_native_cli_t *grant);
int server_native_cli_cancelled(const server_native_cli_t *grant);
const atomic_int *server_native_cli_cancel_flag(const server_native_cli_t *grant);
/* Marks revoked and waits for admitted callbacks to finish before erasing its
 * credential/environment. The owner must first remove it from lookup. */
void server_native_cli_close(server_native_cli_t *grant);
/* Host-owned identity is checked again on the worker, never taken from body. */
int server_native_cli_owner(const server_native_cli_t *grant, const char *session,
                            const char *actor, const char *cwd);
/* Rejects duplicate/forged correlation/scope fields; maps only existing MCP
 * family declarations or explicit mcp.call. Caller owns arguments. */
int server_native_cli_project(const cJSON *request, char tool[128], cJSON **arguments,
                              char request_id[33]);
/* 0: first request reserved; 1: exact replay (owned response returned);
 * -1: busy, changed nonce arguments, revoked or bounded request cap exhausted.
 * A failed/unknown response remains a consumed request, never re-executed. */
int server_native_cli_begin(server_native_cli_t *grant, const cJSON *request,
                            const char *request_id, char attempt[128], cJSON **replay);
int server_native_cli_finish(server_native_cli_t *grant, const cJSON *response);
/* Execute an exact retained catalog operation using the existing action owner.
 * No model ToolUse IDs or operator authority are synthesized. */
int server_native_cli_execute(server_native_cli_t *grant, const cJSON *request,
                              const aimee_tool_t *advertised, int timeout_ms, cJSON **response);
#endif
