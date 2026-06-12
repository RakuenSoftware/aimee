/* vault_principal.c: resolve a connection's attested vault principal (WP-C.0).
 * Pure, side-effect-free classification — the single place the "who owns this
 * vault" decision is made, so it can be unit-tested exhaustively in isolation
 * before any crypto (WP-C.1) consumes it. See vault_principal.h for the policy. */
#include "vault_principal.h"
#include <stdio.h>
#include <string.h>

attested_transport_t vault_principal_resolve(int is_tcp, long peer_uid, const char *webuser,
                                             int webuser_token_ok, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!out || out_len < VAULT_PRINCIPAL_MAX)
      return ATTEST_NONE;

   int webuser_asserted = webuser && webuser[0];

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
    * reason the webuser: principal exists). Classify by transport, no vault. */
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

   /* Plain TCP: bearer-authorized at the network edge but with no OS-user
    * attestation. Direct-TCP multi-user vault is out of scope (D17) -> no
    * principal. */
   return ATTEST_TCP_BEARER;
}
