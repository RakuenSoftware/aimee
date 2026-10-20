#ifndef AIMEE_SERVER_NATIVE_EXECUTION_H
#define AIMEE_SERVER_NATIVE_EXECUTION_H
#include "cli_codex.h"
#include "provider_cli_adapter.h"
/* Server-owned Codex scheduler: the host retains credentials, tool/workspace
 * authority, canonical history and original results. Child HOME/env are private.
 * The creating task retains its outer workspace lease until this returns. */
int server_native_codex_chat_stream(const agent_t *selected,
                                    const cli_codex_chat_request_t *request,
                                    cli_codex_chat_event_cb callback, void *context,
                                    cli_codex_chat_result_t *out);
/* Internal host owner: actor comes from authenticated request custody. Recover
 * all prior native bindings/SDKs for this owned session before history/model
 * selection. Current-turn ownership, live physical leases and ambiguity refuse. */
int server_native_session_reconcile(const char *session, const char *actor, const char *active_turn,
                                    char error[512]);
/* Join bounded custody retries before DB/module/log teardown. New qualified
 * admissions refuse; late already-owned custody parks safely until exit. */
void server_native_execution_shutdown(void);
#endif
