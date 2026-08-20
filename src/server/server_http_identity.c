/* server_http_identity.c: WP-C.0 attested-identity capture + threading for the
 * /v1 front-end. Isolated from server_http.c (at the line-count limit) so the
 * SO_PEERCRED capture, the root-owned webchat UDS assertion, and the
 * synthesized-conn propagation live in one small, independently-testable unit.
 * See server_http_identity.h for the three-hop model. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* struct ucred / SO_PEERCRED via platform_ipc */
#endif
#include "server_http_identity.h"
#include "server_http.h"    /* http_header */
#include "server_conn_io.h" /* server_conn_io_has_ssl/get_ssl — native-TLS attestation */
#include "server_tls.h"     /* server_tls_peer_identity — mTLS client cert CN */
#include "kb_mgmt_status.h"
#include "server.h" /* server_ct_equal */
#include "platform_ipc.h"
#include "vault_principal.h"
#include <aimee/core/connection/auth.h>
#include <ctype.h>
#include <string.h>

#define AIMEE_SESSION_BEARER_MARKER ".aimee-session."

int server_http_session_bearer_unbind(const char *presented, char *bearer, size_t bearer_n,
                                      char *session_id, size_t session_n)
{
   if (!bearer || bearer_n == 0 || !session_id || session_n == 0)
      return 0;
   snprintf(bearer, bearer_n, "%s", presented ? presented : "");
   session_id[0] = '\0';
   if (!presented)
      return 0;
   const char *mark = strstr(presented, AIMEE_SESSION_BEARER_MARKER);
   if (!mark || strlen(mark + sizeof(AIMEE_SESSION_BEARER_MARKER) - 1) != 32)
      return 0;
   const char *sid = mark + sizeof(AIMEE_SESSION_BEARER_MARKER) - 1;
   for (int i = 0; i < 32; i++)
      if (!isdigit((unsigned char)sid[i]) && !(sid[i] >= 'a' && sid[i] <= 'f'))
         return 0;
   size_t base_n = (size_t)(mark - presented);
   if (base_n == 0 || base_n >= bearer_n || 33 > session_n)
      return 0;
   memcpy(bearer, presented, base_n);
   bearer[base_n] = '\0';
   memcpy(session_id, sid, 33);
   return 1;
}

/* Compare a presented credential against the configured bearer, ignoring any
 * `.aimee-session.<32hex>` suffix the client appended to scope its connection to
 * one session. The suffix is client-chosen routing metadata, not a secret, so it
 * must not change whether the credential authenticates -- otherwise every
 * session-scoped client would be rejected at the door. The base token is still
 * compared in constant time. */
int server_http_bearer_matches(const char *presented, const char *bearer_cfg)
{
   if (!presented || !presented[0] || !bearer_cfg || !bearer_cfg[0])
      return 0;
   char base[4097], sid[33];
   (void)server_http_session_bearer_unbind(presented, base, sizeof(base), sid, sizeof(sid));
   return server_ct_equal(base, bearer_cfg);
}

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
static _Thread_local char tl_bearer[4097] = "";
static _Thread_local char tl_status_staple[KB_MGMT_STATUS_JSON_MAX + 1] = "";
static _Thread_local server_tls_peer_cert_t tl_peer_cert;
static _Thread_local server_tls_peer_cert_t tl_local_cert;
static _Thread_local char tl_local_fingerprint[65] = "";

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
   int webuser_attested = 0;
   if (buf && http_header(buf, "X-Aimee-Webuser", webuser, sizeof(webuser)) && webuser[0])
   {
      /* Webchat runs as root and reaches the server only over its Unix socket.
       * SO_PEERCRED is an unforgeable local attestation, so no shared plaintext
       * no shared bearer file is needed. TCP can never assert a webuser this way. */
      webuser_attested = !is_tcp && peer_uid == 0;
   }

   tl_peer_uid = peer_uid;
   /* A native-TLS connection (the fd has a registered SSL) is a confidential,
    * bearer-authorized channel — the operator's authority for server-principal
    * vault writes (native-TLS provisioning). */
   int is_tls = server_conn_io_has_ssl(fd);
   /* mTLS: a verified client cert on this TLS conn yields a per-client cert:<CN>
    * principal (resolved + sanitized in vault_principal_resolve). */
   char cert_cn[VAULT_CERT_CN_MAX + 1] = "";
   memset(&tl_peer_cert, 0, sizeof(tl_peer_cert));
   memset(&tl_local_cert, 0, sizeof(tl_local_cert));
   tl_local_fingerprint[0] = '\0';
   if (is_tls)
   {
      char serial[80];
      SSL *ssl = server_conn_io_get_ssl(fd);
      server_tls_peer_identity(ssl, cert_cn, sizeof(cert_cn), serial, sizeof(serial));
      server_tls_peer_cert(ssl, &tl_peer_cert);
      if (server_tls_local_cert(ssl, &tl_local_cert))
         snprintf(tl_local_fingerprint, sizeof(tl_local_fingerprint), "%s",
                  tl_local_cert.fingerprint);
   }
   tl_transport = vault_principal_resolve(is_tcp, is_tls, peer_uid, webuser, webuser_attested,
                                          cert_cn, tl_principal, sizeof(tl_principal));

   /* Capture the economizer session-key inputs (aimee-session-id + bearer) for
    * buffered gateway handlers. Purely additive; empty when absent. */
   tl_session_hdr[0] = '\0';
   tl_bearer[0] = '\0';
   tl_status_staple[0] = '\0';
   if (buf)
   {
      http_header(buf, "aimee-session-id", tl_session_hdr, sizeof(tl_session_hdr));
      char authz[4105] = "";
      if (http_header(buf, "Authorization", authz, sizeof(authz)))
      {
         const char *bearer = aimee_core_bearer_token(authz);
         if (bearer)
         {
            char bound_sid[80];
            (void)server_http_session_bearer_unbind(bearer, tl_bearer, sizeof(tl_bearer), bound_sid,
                                                    sizeof(bound_sid));
            if (!tl_session_hdr[0] && bound_sid[0])
               snprintf(tl_session_hdr, sizeof(tl_session_hdr), "%s", bound_sid);
         }
      }
      /* The connection bearer may arrive in x-api-key instead of Authorization
       * -- that split is the supported shape when Authorization carries a caller
       * identity JWT. Recover the session binding from there too, or a client
       * that scopes itself that way authenticates but presents NO session, and
       * every per-session decision keyed on it (persona delivery, economizer
       * session keys) silently treats each request as a brand new session. */
      char api_key[512] = "";
      if (!tl_session_hdr[0] && http_header(buf, "x-api-key", api_key, sizeof(api_key)) &&
          api_key[0])
      {
         char base[sizeof(tl_bearer)], bound_sid[80];
         if (server_http_session_bearer_unbind(api_key, base, sizeof(base), bound_sid,
                                               sizeof(bound_sid)) &&
             bound_sid[0])
            snprintf(tl_session_hdr, sizeof(tl_session_hdr), "%s", bound_sid);
      }
      http_header(buf, "X-Aimee-Management-Status", tl_status_staple, sizeof(tl_status_staple));
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

const char *server_http_identity_status_staple(void)
{
   return tl_status_staple;
}

const server_tls_peer_cert_t *server_http_identity_peer_cert(void)
{
   return tl_peer_cert.fingerprint[0] ? &tl_peer_cert : NULL;
}

const char *server_http_identity_local_fingerprint(void)
{
   return tl_local_fingerprint;
}

const server_tls_peer_cert_t *server_http_identity_local_cert(void)
{
   return tl_local_cert.fingerprint[0] ? &tl_local_cert : NULL;
}

const char *server_http_identity_principal(void)
{
   return tl_principal;
}

void server_http_identity_override_principal(const char *principal)
{
   if (!principal || !principal[0])
      return;
   snprintf(tl_principal, sizeof(tl_principal), "%s", principal);
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
   memset(tl_status_staple, 0, sizeof(tl_status_staple));
   memset(&tl_peer_cert, 0, sizeof(tl_peer_cert));
   memset(&tl_local_cert, 0, sizeof(tl_local_cert));
   memset(tl_local_fingerprint, 0, sizeof(tl_local_fingerprint));
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
