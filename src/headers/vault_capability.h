/* vault_capability.h: the vault:write:server capability grants store (P2/D2c).
 *
 * A server-principal credential is autonomously decryptable by every delegate, so
 * the power to WRITE one (a CLIENT-SUPPLIED secret landing in the server vault —
 * e.g. `agent add --key` or `vault set --server`) must not be conferred by the
 * transport bearer alone. This module is a tiny operator-managed allow-list of
 * attested principals (uid:<n> / webuser:<name>) that hold vault:write:server. The
 * server-principal write gate consults it alongside an attested-transport check
 * (D2b: UDS_PEERCRED / WEBCHAT_TRUSTED only). NOTE: server-OBTAINED secrets (the
 * codex / OAuth tokens the server itself fetched) are NOT gated by this — only
 * client-supplied injections are.
 *
 * Grants are minted only over a kernel-attested UDS connection and stored in a
 * 0600 file under <config>/.vault/; the store is process-safe (mutex). */
#ifndef DEC_VAULT_CAPABILITY_H
#define DEC_VAULT_CAPABILITY_H 1

#include <stddef.h>

#include "vault_principal.h" /* attested_transport_t */

#ifdef __cplusplus
extern "C"
{
#endif

/* The single capability this module governs. */
#define VAULT_CAP_WRITE_SERVER "vault:write:server"

   /* Grant / revoke vault:write:server to/from an attested principal string (e.g.
    * "uid:1000", "webuser:alice"). Idempotent. Returns 0 on success, -1 on a bad
    * argument or I/O error. */
   int vault_capability_grant(const char *principal);
   int vault_capability_revoke(const char *principal);

   /* Returns 1 iff `principal` currently holds vault:write:server. A NULL/empty
    * principal is never granted. */
   int vault_capability_has(const char *principal);

   /* Copy the newline-separated list of granted principals (no secrets) into `out`.
    * Returns the count (>=0), or -1 on error. */
   int vault_capability_list(char *out, size_t out_len);

   /* The server-principal write gate (D2b/D2c): returns 1 iff `transport` is an
    * attested local/webchat transport (never a bare TCP bearer) AND `principal`
    * holds vault:write:server. The single source of truth for the gate. */
   int vault_capability_server_write_allowed(attested_transport_t transport, const char *principal);

   /* Test seam: override the store path (NULL resets to the default). */
   void vault_capability_set_path_for_test(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DEC_VAULT_CAPABILITY_H */
