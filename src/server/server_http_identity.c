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
#include "server_conn_io.h" /* server_conn_io_has_ssl/get_ssl — native-TLS attestation */
#include "server_tls.h"     /* server_tls_peer_identity — mTLS client cert CN */
#include "aimee_home.h"     /* aimee_home */
#include "platform_ipc.h"
#include "vault_principal.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

/* Per-thread captured identity for the request currently being routed. Thread-
 * local for the same reason as the front-end's g_rpc_conn_caps: each connection
 * is handled on its own worker thread and these are read synchronously on that
 * thread (handle_conn -> loopback_rpc) before the compute worker detaches.
 * Defaults are the un-attested, no-vault state. */
static _Thread_local long tl_peer_uid = -1;
static _Thread_local attested_transport_t tl_transport = ATTEST_NONE;
static _Thread_local char tl_principal[VAULT_PRINCIPAL_MAX] = "";
/* Query string of the in-flight request ("k=v&…", no '?'); set around the route
 * call, cleared with the rest. Points into the request buffer — read only within
 * the handler. */
static _Thread_local const char *tl_query = "";
/* The in-flight request's inbound aimee-session-id header value and bearer token,
 * captured for the economizer gateway-mutation session-key resolver (which needs
 * them alongside the vault principal). Copied out of the request buffer at capture
 * and cleared (bearer zeroed) with the rest — valid only during the route handler
 * on the serving thread. Empty when absent. */
static _Thread_local char tl_session_hdr[80] = "";
/* Bearer buffer sized for long tokens (JWT/OIDC bearers can exceed 512B). Truncation
 * beyond this is deterministic (same input -> same truncated string -> same session
 * key), so it only risks two >2KB bearers that share a 2KB prefix sharing a disable
 * bucket — a benign availability edge, not a security boundary. */
static _Thread_local char tl_bearer[2048] = "";

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
   /* mTLS: a verified client cert on this TLS conn yields a per-client cert:<CN>
    * principal (resolved + sanitized in vault_principal_resolve). */
   char cert_cn[VAULT_CERT_CN_MAX + 1] = "";
   if (is_tls)
   {
      char serial[80];
      server_tls_peer_identity(server_conn_io_get_ssl(fd), cert_cn, sizeof(cert_cn), serial,
                               sizeof(serial));
   }
   tl_transport = vault_principal_resolve(is_tcp, is_tls, peer_uid, webuser, webuser_token_ok,
                                          cert_cn, tl_principal, sizeof(tl_principal));

   /* Capture the economizer session-key inputs (aimee-session-id + bearer) for
    * buffered gateway handlers. Purely additive; empty when absent. */
   tl_session_hdr[0] = '\0';
   tl_bearer[0] = '\0';
   if (buf)
   {
      http_header(buf, "aimee-session-id", tl_session_hdr, sizeof(tl_session_hdr));
      char authz[2048] = "";
      /* Bearer scheme token is case-insensitive (RFC 7235 §2.1). */
      if (http_header(buf, "Authorization", authz, sizeof(authz)) &&
          strncasecmp(authz, "Bearer ", 7) == 0)
         snprintf(tl_bearer, sizeof(tl_bearer), "%s", authz + 7);
   }
}

const char *server_http_identity_session_hdr(void)
{
   return tl_session_hdr;
}

const char *server_http_identity_bearer(void)
{
   return tl_bearer;
}

const char *server_http_identity_principal(void)
{
   return tl_principal;
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
   tl_query = "";
   tl_session_hdr[0] = '\0';
   memset(tl_bearer, 0, sizeof(tl_bearer)); /* zero the secret, don't just truncate */
}

void server_http_identity_set_query(const char *q)
{
   tl_query = q ? q : "";
}

const char *server_http_identity_query(void)
{
   return tl_query ? tl_query : "";
}

void http_error_json(char *resp, size_t cap, const char *msg)
{
   if (!resp || cap == 0)
      return;
   if (!msg)
      msg = "error";
   if (cap < 16)
   {
      resp[0] = '\0';
      return;
   }
   size_t o = (size_t)snprintf(resp, cap, "{\"error\":\"");
   /* Leave room for the longest single escape (\uXXXX = 6) + closing "\"}" + NUL. */
   for (const char *p = msg; *p && o + 9 < cap; p++)
   {
      unsigned char c = (unsigned char)*p;
      switch (c)
      {
      case '"':
         resp[o++] = '\\';
         resp[o++] = '"';
         break;
      case '\\':
         resp[o++] = '\\';
         resp[o++] = '\\';
         break;
      case '\n':
         resp[o++] = '\\';
         resp[o++] = 'n';
         break;
      case '\r':
         resp[o++] = '\\';
         resp[o++] = 'r';
         break;
      case '\t':
         resp[o++] = '\\';
         resp[o++] = 't';
         break;
      default:
         if (c < 0x20)
            o += (size_t)snprintf(resp + o, cap - o, "\\u%04x", c);
         else
            resp[o++] = (char)c;
         break;
      }
   }
   snprintf(resp + o, cap - o, "\"}");
}
