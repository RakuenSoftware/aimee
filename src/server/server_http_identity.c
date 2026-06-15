/* server_http_identity.c: WP-C.0 attested-identity capture + threading for the
 * /v1 front-end. Isolated from server_http.c (at the line-count limit) so the
 * SO_PEERCRED capture, the server.token-gated webuser assertion, and the
 * synthesized-conn propagation live in one small, independently-testable unit.
 * See server_http_identity.h for the three-hop model. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* struct ucred / SO_PEERCRED via platform_ipc */
#endif
#include "server_http_identity.h"
#include "server_http.h"    /* http_header */
#include "server.h"         /* server_ct_equal, SERVER_TOKEN_FILE */
#include "server_conn_io.h" /* server_conn_io_has_ssl — native-TLS attestation */
#include "aimee_home.h"     /* aimee_home */
#include "platform_ipc.h"
#include "vault_principal.h"
#include <pthread.h>
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

/* The shared secret that authenticates a webchat `webuser:` assertion is the
 * server.token file (0600, in AIMEE_HOME — the secret only the webchat backend
 * holds), NOT the configured TCP /v1 bearer (g_bearer): those are independent
 * secrets, and g_bearer is empty on a UDS-only server. Loaded once and cached
 * (token rotation needs a restart, as with g_bearer). Returns NULL if absent. */
static const char *server_token_secret(void)
{
   static char tok[256];
   static int loaded = 0;
   static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
   pthread_mutex_lock(&mu);
   if (!loaded)
   {
      const char *home = aimee_home();
      char path[1024];
      if (home && home[0] &&
          (size_t)snprintf(path, sizeof(path), "%s/%s", home, SERVER_TOKEN_FILE) < sizeof(path))
      {
         FILE *f = fopen(path, "rb");
         if (f)
         {
            if (fgets(tok, sizeof(tok), f))
            {
               size_t n = strlen(tok);
               while (n && (tok[n - 1] == '\n' || tok[n - 1] == '\r' || tok[n - 1] == ' ' ||
                            tok[n - 1] == '\t'))
                  tok[--n] = '\0';
               if (tok[0])
                  loaded = 1; /* cache only a non-empty token; else retry next call */
            }
            fclose(f);
         }
      }
   }
   const char *out = loaded ? tok : NULL;
   pthread_mutex_unlock(&mu);
   return out;
}

void server_http_identity_capture(int fd, int is_tcp, const char *buf)
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
      const char *tok = server_token_secret();
      char wauth[512] = "";
      if (tok && http_header(buf, "Authorization", wauth, sizeof(wauth)) &&
          strncmp(wauth, "Bearer ", 7) == 0 && server_ct_equal(wauth + 7, tok))
         webuser_token_ok = 1;
   }

   tl_peer_uid = peer_uid;
   /* A native-TLS connection (the fd has a registered SSL) is a confidential,
    * bearer-authorized channel — the operator's authority for server-principal
    * vault writes (native-TLS provisioning). */
   int is_tls = server_conn_io_has_ssl(fd);
   tl_transport = vault_principal_resolve(is_tcp, is_tls, peer_uid, webuser, webuser_token_ok,
                                          tl_principal, sizeof(tl_principal));
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
