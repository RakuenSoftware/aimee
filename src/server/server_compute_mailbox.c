/* Delegation execution bookmarks and canonical question compatibility. */
#include "server_compute_internal.h"
#include "server_delegate_scope.h"
#include "server_session_question.h"
#include "server_session_control.h"
#include "util.h"
#include <string.h>

/* Delegation depth tracking: parent delegation ID and current depth for this thread */
__thread char tl_parent_delegation_id[64] = {0};
__thread int tl_delegation_depth = 0;

void server_delegate_scope_capture(server_delegate_scope_t *out)
{
   memcpy(out->parent, tl_parent_delegation_id, sizeof out->parent);
   out->depth = tl_delegation_depth;
}
void server_delegate_scope_restore(const server_delegate_scope_t *scope)
{
   memcpy(tl_parent_delegation_id, scope->parent, sizeof tl_parent_delegation_id);
   tl_delegation_depth = scope->depth;
}

/* Returns the delegation ID of the currently-running delegate on this
 * thread, or NULL when not inside a delegate. Provides agent_tools_dispatch
 * with a write-tracking key that's distinct from the caller's session_id —
 * lets db1_session_stale_reads tell "the child wrote what the parent read"
 * apart from "this session read and wrote the same file." */
const char *delegation_active_id(void)
{
   return tl_parent_delegation_id[0] ? tl_parent_delegation_id : NULL;
}

/* Called by tool_request_input from within the delegate agent */
char *delegation_request_input(const char *question)
{
   if (server_control_scope_current())
      return server_session_question(question, DELEGATION_INPUT_TIMEOUT);
   return safe_strdup("error: request_input requires an owned canonical execution control scope");
}

int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   /* Compatibility route only: delegation IDs cannot identify or authorize a
    * canonical pending question. Never acknowledge an undeliverable answer. */
   return server_send_error(conn,
                            "delegate.reply is unavailable; answer canonical questions through "
                            "the operator chat control reply endpoint",
                            NULL);
}
