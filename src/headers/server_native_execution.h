#ifndef AIMEE_SERVER_NATIVE_EXECUTION_H
#define AIMEE_SERVER_NATIVE_EXECUTION_H
#include "cli_codex.h"
#include "provider_cli_adapter.h"
/* Internal host owner: actor comes from authenticated request custody. Recover
 * all prior native bindings/SDKs for this owned session before history/model
 * selection. Current-turn ownership, live physical leases and ambiguity refuse. */
int server_native_session_reconcile(const char *session, const char *actor, const char *active_turn,
                                    char error[512]);
/* Authenticated browse owner: validate every prior record, then defer only an
 * exact retained process-local writer. Orphans require the same physical lease,
 * re-read durable authority and qualified whole-tree proof as strict recovery. */
int server_native_session_reconcile_browse(const char *session, const char *actor, char error[512]);
/* Join bounded custody retries before DB/module/log teardown. New qualified
 * admissions refuse; late already-owned custody parks safely until exit. */
void server_native_execution_shutdown(void);
#endif
