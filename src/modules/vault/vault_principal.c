/* vault_principal.c: resolve a connection's attested vault principal (WP-C.0).
 * Pure, side-effect-free classification — the single place the "who owns this
 * vault" decision is made, so it can be unit-tested exhaustively in isolation
 * before any crypto (WP-C.1) consumes it. See vault_principal.h for the policy. */
#include "vault_principal.h"
#include <stdio.h>
#include <string.h>

int vault_principal_cert_sanitize(const char *cn, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!cn || !cn[0] || !out || out_len == 0)
      return 0;
   size_t n = strlen(cn);
   if (n > VAULT_CERT_CN_MAX || n >= out_len)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)cn[i];
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
      if (!ok)
         return 0; /* reject ':' (namespace collision), '/', whitespace, control, etc. */
   }
   memcpy(out, cn, n);
   out[n] = '\0';
   return 1;
}

attested_transport_t vault_principal_resolve(int is_tcp, int is_tls, long peer_uid,
                                             const char *webuser, int webuser_token_ok,
                                             const char *cert_cn, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!out || out_len < VAULT_PRINCIPAL_MAX)
      return ATTEST_NONE;

   /* A verified mTLS client cert is the strongest network identity and wins over
    * bearer-only / a tokenless webuser: the cert chain was verified against
    * aimee's client CA upstream (server_tls), so its CN names a real client.
    * Sanitize the CN here (fail-closed): a CN that can't sanitize — e.g. "uid:0",
    * an embedded ':' / newline / path-traversal — yields NO principal, but the
    * connection is still classified ATTEST_MTLS_CLIENT so `required`-mode gating
    * refuses it rather than silently downgrading to a bearer identity. */
   if (cert_cn && cert_cn[0] && is_tcp && is_tls)
   {
      char san[VAULT_CERT_CN_MAX + 1];
      if (vault_principal_cert_sanitize(cert_cn, san, sizeof(san)))
         snprintf(out, out_len, VAULT_CERT_PRINCIPAL_PREFIX "%s", san);
      return ATTEST_MTLS_CLIENT;
   }

   int webuser_asserted = webuser && webuser[0];
   /* A bearer-authorized native-TLS conn (confidential channel) is the operator's
    * authority for server-principal writes — but it carries no OS-user/webuser
    * identity, so it gets the same EMPTY per-user principal as plain TCP. The
    * distinct classification is what later authorizes server-principal writes
    * (vault_capability_server_write_allowed); a TCP_BEARER (plaintext) does not.
    *
    * TLS_BEARER is granted ONLY to a conn that asserts NO webuser at all (a clean
    * operator/CLI connection). A conn that DOES assert a webuser is a webchat hop:
    * with a valid token it wins the per-user vault below; without one it is a spoof
    * (or a misconfigured webchat forwarding an end-user header) and must NOT be
    * silently promoted to server-principal authority just because the socket has
    * TLS — that would let any webchat end-user mint a server credential. Such a
    * tokenless webuser is refused (classified by raw transport, no server write).
    *
    * Gated on is_tcp as well: TLS_BEARER is a NETWORK provisioning channel. A
    * local UDS fd never carries an SSL handle (only the network listener wraps
    * TLS), but requiring is_tcp makes that intent explicit and fail-closed — a
    * (misconfigured) UDS+SSL conn falls through to the kernel-attested uid: path
    * rather than gaining bearer-only server-write authority. */
   int tls_bearer = is_tcp && is_tls && !webuser_asserted;

   /* A webuser assertion is honored ONLY under the server.token trust boundary
    * (the secret only the webchat backend holds). Asserted WITHOUT a valid token
    * it is a spoof: the principal is refused (empty), and the connection is
    * classified by its underlying transport — never granted a vault identity. */
   if (webuser_asserted && webuser_token_ok)
   {
      snprintf(out, out_len, "webuser:%s", webuser);
      return ATTEST_WEBCHAT_TRUSTED;
   }

   /* A webuser asserted WITHOUT the valid token is a spoof. It is refused a
    * principal entirely — it must NOT fall through to the uid: path, or a
    * request from the shared webchat service account would silently receive that
    * account's uid: vault, leaking credentials across webchat users (the whole
    * reason the webuser: principal exists). It is likewise NOT promoted to
    * TLS_BEARER (tls_bearer is false whenever a webuser is asserted): a tokenless
    * webuser never earns server-principal authority. Classify by raw transport
    * (no server write), no vault. */
   if (webuser_asserted)
      return is_tcp ? ATTEST_TCP_BEARER : ATTEST_UDS_PEERCRED;

   if (!is_tcp)
   {
      /* Local UDS peer. A kernel-attested uid > 0 owns a uid: vault. uid 0
       * (root) and an unknown uid (-1) get NO principal: an un-attested or
       * zeroed conn reads as uid 0, so granting it would collapse to acting as
       * root on a missed hop. Fail-closed -> empty principal, vault refuses. */
      if (peer_uid > 0)
         snprintf(out, out_len, "uid:%ld", peer_uid);
      return ATTEST_UDS_PEERCRED;
   }

   /* TCP at the network edge, bearer-authorized but no OS-user attestation. A
    * native-TLS conn (confidential channel) is the operator-attested write path;
    * plaintext TCP is not (D2b). Direct-TCP multi-user per-user vault is still out
    * of scope (D17) -> empty principal either way. */
   return tls_bearer ? ATTEST_TLS_BEARER : ATTEST_TCP_BEARER;
}
