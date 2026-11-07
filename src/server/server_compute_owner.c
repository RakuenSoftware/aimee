/* server_compute_owner.c: immutable request owner across compute handoffs. */
#include "server_compute_impl.h"
#include "server_http_identity.h"
#include "agent_config.h"
#include "server_session_control.h"
#include <stdio.h>
#include <string.h>

void compute_ctx_capture_request_context(compute_ctx_t *cctx, const server_conn_t *conn)
{
   request_context_snapshot(&cctx->request_context);
   request_context_snapshot_t *snapshot = &cctx->request_context;
   request_context_t *owner = &snapshot->value;
   server_control_scope_t *controls = server_control_scope_current();
   if (snapshot->active && controls)
      owner->control_reply_capability =
          owner->control_reply_capability && server_control_scope_can_reply(controls);
   /* A conn-free continuation inherits its actual owner, not CAPS_ALL. Only
    * genuinely context-free server work retains the historical server scope. */
   int inherited_owner = snapshot->active && owner->vault_transport != ATTEST_NONE;
   cctx->conn_caps = conn ? conn->capabilities : inherited_owner ? owner->capabilities : CAPS_ALL;
   if (conn && conn->attested_transport != ATTEST_NONE)
   {
      if (snapshot->active && (owner->vault_transport != conn->attested_transport ||
                               strcmp(owner->vault_principal, conn->vault_principal)))
         request_context_snapshot_clear(snapshot);
      snapshot->active = 1;
      int local = conn->attested_transport == ATTEST_UDS_PEERCRED ||
                  conn->attested_transport == ATTEST_WEBCHAT_TRUSTED;
      owner->peer_uid = local ? (long)conn->peer_uid : -1;
      owner->transport = local ? REQ_TRANSPORT_UDS : REQ_TRANSPORT_TCP;
      owner->vault_transport = conn->attested_transport;
      snprintf(owner->vault_principal, sizeof owner->vault_principal, "%s", conn->vault_principal);
   }
   else if (conn && inherited_owner)
      cctx->conn_caps &= owner->capabilities;
   owner->capabilities = cctx->conn_caps;
   cctx->attested_transport = snapshot->active ? owner->vault_transport : ATTEST_NONE;
   snprintf(cctx->vault_principal, sizeof cctx->vault_principal, "%s",
            snapshot->active && owner->vault_transport != ATTEST_NONE ? owner->vault_principal
            : (conn && conn->vault_principal[0])                      ? conn->vault_principal
                                                 : agent_get_request_vault_principal());
}

void compute_ctx_bind_request_context(const compute_ctx_t *cctx)
{
   request_context_restore(cctx ? &cctx->request_context : NULL);
   server_http_identity_bind_request_owner(request_context_get());
   agent_set_request_vault_principal(cctx ? cctx->vault_principal : NULL);
}
void compute_ctx_clear_request_context(void)
{
   agent_set_request_session(NULL);
   agent_set_request_codex_creds(NULL, NULL);
   server_http_identity_clear();
   request_context_clear();
   agent_set_request_vault_principal(NULL);
}
