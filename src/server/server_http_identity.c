/* server_http_identity.c: WP-C.0 attested-identity capture + threading for the
 * /v1 front-end. Isolated from server_http.c (at the line-count limit) so the
 * SO_PEERCRED capture, the server.token-gated webuser assertion, and the
 * synthesized-conn propagation live in one small, independently-testable unit.
 * See server_http_identity.h for the three-hop model. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* struct ucred / SO_PEERCRED via platform_ipc */
#endif
#include "server_http_identity.h"
#include "server_http.h" /* http_header */
#include "server.h"      /* server_ct_equal */
#include "platform_ipc.h"
#include "vault_principal.h"
#include <stdio.h>
#include <string.h>

/* Per-thread captured identity for the request currently being routed. Thread-
 * local for the same reason as the front-end's g_rpc_conn_caps: each connection
 * is handled on its own worker thread and these are read synchronously on that
 * thread (handle_conn -> loopback_rpc) before the compute worker detaches.
 * Defaults are the un-attested, no-vault state. */
static _Thread_local long tl_peer_uid = -1;
static _Thread_local attested_transport_t tl_transport = ATTEST_NONE;
static _Thread_local char tl_principal[VAULT_PRINCIPAL_MAX] = "";

void server_http_identity_capture(int fd, int is_tcp, const char *buf, const char *bearer)
{
   long peer_uid = -1;
   if (!is_tcp)
   {
      platform_peer_cred_t pc;
      if (platform_ipc_peer_cred(fd, &pc) == 0)
         peer_uid = (long)pc.uid;
   }

   char webuser[128] = "";
   int webuser_token_ok = 0;
   if (buf && http_header(buf, "X-Aimee-Webuser", webuser, sizeof(webuser)) && webuser[0])
   {
      /* A webuser assertion is honored only with the valid server.token bearer
       * (the secret only the webchat backend holds). A spoofed header without it
       * is refused by vault_principal_resolve (empty principal). */
      char wauth[512] = "";
      if (bearer && bearer[0] && http_header(buf, "Authorization", wauth, sizeof(wauth)) &&
          strncmp(wauth, "Bearer ", 7) == 0 && server_ct_equal(wauth + 7, bearer))
         webuser_token_ok = 1;
   }

   tl_peer_uid = peer_uid;
   tl_transport = vault_principal_resolve(is_tcp, peer_uid, webuser, webuser_token_ok, tl_principal,
                                          sizeof(tl_principal));
}

void server_http_identity_apply(server_conn_t *conn)
{
   if (!conn)
      return;
   conn->peer_uid = (tl_peer_uid > 0) ? (uid_t)tl_peer_uid : 0;
   conn->attested_transport = tl_transport;
   snprintf(conn->vault_principal, sizeof(conn->vault_principal), "%s", tl_principal);
}

void server_http_identity_clear(void)
{
   tl_peer_uid = -1;
   tl_transport = ATTEST_NONE;
   tl_principal[0] = '\0';
}
