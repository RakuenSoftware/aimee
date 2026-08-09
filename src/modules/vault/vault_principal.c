/* vault_principal.c: resolve a connection's attested vault principal (WP-C.0).
 * Pure, side-effect-free classification — the single place the "who owns this
 * vault" decision is made, so it can be unit-tested exhaustively in isolation
 * before any crypto (WP-C.1) consumes it. See vault_principal.h for the policy. */
#include "vault_principal.h"
#include <stdio.h>
#include <string.h>

int vault_principal_name_sanitize(const char *name, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!name || !name[0] || !out || out_len == 0)
      return 0;
   size_t n = strlen(name);
   if (n > VAULT_CERT_CN_MAX || n >= out_len)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)name[i];
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
      if (!ok)
         return 0; /* reject ':' (namespace collision), '/', whitespace, control, etc. */
   }
   memcpy(out, name, n);
   out[n] = '\0';
   return 1;
}

attested_transport_t vault_principal_resolve(int is_tcp, int is_tls, long peer_uid,
                                             const char *webuser, int webuser_attested,
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
      if (vault_principal_name_sanitize(cert_cn, san, sizeof(san)))
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

   /* A webuser assertion is honored ONLY under the root-owned webchat UDS trust
    * boundary. Asserted WITHOUT valid kernel attestation
    * it is a spoof: the principal is refused (empty), and the connection is
    * classified by its underlying transport — never granted a vault identity. */
   if (webuser_asserted && webuser_attested)
   {
      /* Same sanitize-or-no-principal rule as the cert CN above. An unusable name
       * yields NO vault identity but KEEPS the ATTEST_WEBCHAT_TRUSTED
       * classification, so gating still sees a webchat hop rather than silently
       * downgrading it to a bearer — the cert path's reasoning, applied here. */
      char san[VAULT_CERT_CN_MAX + 1];
      if (vault_principal_name_sanitize(webuser, san, sizeof(san)))
         snprintf(out, out_len, "webuser:%s", san);
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
      /* Local UDS peer: a kernel-attested uid owns the matching uid: vault.
       *
       * uid 0 is included. It was excluded to stop a "missed hop" from acting as
       * root, but the sentinel for a missed hop is -1, not 0: the caller
       * (server_http_identity_capture) initialises peer_uid to -1 and overwrites it
       * ONLY when platform_ipc_peer_cred succeeds, so 0 here means the kernel said
       * root. A zeroed server_conn_t is caught by a different guard -- ATTEST_NONE
       * is the enum's zero value, so such a conn never classifies as
       * UDS_PEERCRED at all.
       *
       * Excluding it did not make root safe, only mute: the local root operator
       * owns .vault/.server-master.key on disk and can decrypt the store offline
       * regardless. What it actually did was leave `aimee vault set-server` refusing
       * every root caller -- the ordinary case inside the server container -- while
       * naming principal "(unknown)" in the remediation, telling the operator to run
       * `aimee vault capability grant (unknown)`. Unusable advice for an
       * unreachable path.
       *
       * An unknown uid (-1) still gets NO principal and still fails closed. */
      if (peer_uid >= 0)
         snprintf(out, out_len, "uid:%ld", peer_uid);
      return ATTEST_UDS_PEERCRED;
   }

   /* TCP at the network edge, bearer-authorized but no OS-user attestation. A
    * native-TLS conn (confidential channel) is the operator-attested write path;
    * plaintext TCP is not (D2b). Direct-TCP multi-user per-user vault is still out
    * of scope (D17) -> empty principal either way. */
   return tls_bearer ? ATTEST_TLS_BEARER : ATTEST_TCP_BEARER;
}
