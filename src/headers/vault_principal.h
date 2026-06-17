#ifndef DEC_VAULT_PRINCIPAL_H
#define DEC_VAULT_PRINCIPAL_H 1

#include <stddef.h>

/* vault_principal: the attested identity that keys the per-user credential vault
 * (WP-C). The *vault principal* — "uid:<peer_uid>" for a kernel-attested local
 * UDS peer, or "webuser:<username>" for a webchat user asserted under the
 * server.token trust boundary — is the SINGLE security key for both the vault
 * file (ownership) and the KEK cache. It is NEVER derived from a client-supplied
 * session_id or the proxy-overridable audit principal.
 *
 * WP-C.0 captures the attested transport + resolves this principal while the
 * connection is live (SO_PEERCRED + the server.token-gated X-Aimee-Webuser
 * header), then threads it across the closed-connection boundary
 * (handle_conn thread-locals -> loopback_rpc fake conn -> compute_ctx_t) so the
 * detached delegate worker can reach the right vault. The crypto core that
 * consumes it lands in WP-C.1/C.2. */

/* The kind of attestation behind a connection's identity. Ordered so that
 * ATTEST_NONE (the zero value) is the un-attested default a missed hop or a
 * memset() collapses to — every vault path is fail-closed against it. */
typedef enum
{
   ATTEST_NONE = 0,   /* un-attested: no vault (a missed hop must not become uid:0) */
   ATTEST_TCP_BEARER, /* plaintext network conn authorized by bearer; no OS-user attestation and no
                         confidential channel -> no server-principal writes (D2b) */
   ATTEST_UDS_PEERCRED,    /* local UDS conn, kernel-attested peer uid -> uid:<n> principal */
   ATTEST_WEBCHAT_TRUSTED, /* webchat backend asserting webuser:<name> under server.token (WP-C.2)
                            */
   ATTEST_TLS_BEARER,      /* native-TLS conn authorized by bearer: the bearer over a confidential
                              channel is the operator's authority -> server-principal writes allowed
                              (native-TLS provisioning); no per-user principal (uses VAULT_SERVER) */
   ATTEST_MTLS_CLIENT,     /* mutual-TLS: the peer presented a client cert verified against aimee's
                              client CA -> a per-client "cert:<CN>" principal (mtls-client-identity) */
} attested_transport_t;

/* mTLS client-cert identity: the principal is "cert:" + a sanitized CN. The CN is
 * limited to [A-Za-z0-9._-] so it can never embed a ':' and collide with the
 * uid:/webuser: namespaces; the "cert:" prefix is added by the server, never by
 * the cert. */
#define VAULT_CERT_PRINCIPAL_PREFIX "cert:"
#define VAULT_CERT_CN_MAX           128

/* Validate + copy a client-cert CN into out (NUL-terminated). Returns 1 iff cn is
 * a non-empty string of [A-Za-z0-9._-], at most VAULT_CERT_CN_MAX bytes (so the
 * "cert:<CN>" principal fits VAULT_PRINCIPAL_MAX); 0 otherwise (caller fails
 * closed). This is what blocks principal-spoofing CNs (e.g. "uid:0", embedded
 * ':' / newlines / path traversal). */
int vault_principal_cert_sanitize(const char *cn, char *out, size_t out_len);

/* Max length of a vault principal string ("webuser:" + a 128-byte username, or
 * "uid:" + digits) including the NUL. */
#define VAULT_PRINCIPAL_MAX 160

/* Resolve a connection's attested transport + vault principal, fail-closed.
 *
 * Inputs (all captured while the conn is live):
 *   is_tcp           - 1 for the network listener, 0 for the local UDS socket.
 *   peer_uid         - SO_PEERCRED uid for a UDS peer, or -1 when unavailable/TCP.
 *   webuser          - the X-Aimee-Webuser header value, or NULL/"" if absent.
 *   webuser_token_ok - 1 iff the request also presented the valid server.token
 *                      bearer (the secret only the webchat backend holds).
 *
 * Writes the principal string into out (out[0]=='\0' when there is NO vault
 * identity) and returns the attested transport classification:
 *   - webuser asserted WITH a valid token  -> ATTEST_WEBCHAT_TRUSTED, "webuser:<name>"
 *   - webuser asserted WITHOUT a valid token (spoof) -> classification per the
 *     underlying transport, principal EMPTY (the assertion is refused, not honored)
 *   - plain UDS peer with uid > 0           -> ATTEST_UDS_PEERCRED,    "uid:<n>"
 *   - UDS peer with uid 0 or unknown        -> ATTEST_UDS_PEERCRED,    "" (no uid:0)
 *   - plain TCP                             -> ATTEST_TCP_BEARER,      ""
 *
 * uid 0 is deliberately given NO principal: a zeroed/uninitialised conn (a missed
 * threading hop, or loopback's memset) reads as uid 0, so treating it as a real
 * principal would collapse to acting as root. out must be >= VAULT_PRINCIPAL_MAX. */
attested_transport_t vault_principal_resolve(int is_tcp, int is_tls, long peer_uid,
                                             const char *webuser, int webuser_token_ok,
                                             const char *cert_cn, char *out, size_t out_len);

#endif /* DEC_VAULT_PRINCIPAL_H */
