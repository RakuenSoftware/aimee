#ifndef DEC_VAULT_PRINCIPAL_H
#define DEC_VAULT_PRINCIPAL_H 1

#include <stddef.h>

/* vault_principal: the attested identity that keys the per-user credential vault
 * (WP-C). The *vault principal* — "uid:<peer_uid>" for a kernel-attested local
 * UDS peer, or "webuser:<username>" for a webchat user asserted under the
 * root-owned webchat UDS trust boundary — is the SINGLE security key for both
 * the vault file (ownership) and the KEK cache. It is NEVER derived from a client-supplied
 * session_id or the proxy-overridable audit principal.
 *
 * WP-C.0 captures the attested transport + resolves this principal while the
 * connection is live (SO_PEERCRED + root-UDS-gated X-Aimee-Webuser
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
   ATTEST_WEBCHAT_TRUSTED, /* root UDS webchat asserting webuser:<name> (WP-C.2) */
   ATTEST_TLS_BEARER,      /* native-TLS conn authorized by bearer: the bearer over a confidential
                              channel is the operator's authority -> server-principal writes allowed
                              (native-TLS provisioning); no per-user principal (uses VAULT_SERVER) */
   ATTEST_MTLS_CLIENT,     /* mutual-TLS: the peer presented a client cert verified against aimee's
                              client CA -> a per-client "cert:<CN>" principal (mtls-client-identity) */
} attested_transport_t;

/* mTLS client-cert identity: the principal is "cert:" + a sanitized CN. */
#define VAULT_CERT_PRINCIPAL_PREFIX "cert:"
#define VAULT_CERT_CN_MAX           128

/* ONE rule for every attacker-influenced name that becomes part of a vault
 * principal — a client-cert CN and a webchat username both go through this.
 *
 * They used to differ: the CN was sanitized and the webuser name was
 * interpolated raw, for no stated reason, even though the argument for
 * sanitizing applies identically to both. A name is a name; which channel
 * carried it does not change what characters are safe in a principal.
 *
 * Returns 1 iff `name` is a non-empty string of [A-Za-z0-9._-] at most
 * VAULT_CERT_CN_MAX bytes (so any "<prefix>:<name>" principal fits
 * VAULT_PRINCIPAL_MAX); 0 otherwise, and the caller fails closed to NO principal.
 *
 * What the charset buys, concretely:
 *   - no ':' — so a name can never embed the namespace separator and make
 *     "webuser:x" or "cert:x" read as a different principal form
 *   - no '/' — so a principal can never become a path component that traverses
 *   - no control characters or whitespace — a principal reaches audit records and
 *     logs, where a newline is an injection primitive
 *   - no '|' — the file vault's AEAD AAD is "principal|agent|cred", and a '|' in
 *     any component makes two different slots share one AAD
 *
 * A name outside the set gets no vault rather than a mangled one. That is a
 * deliberate refusal, not an oversight: a legitimate POSIX login name is already
 * inside this set, so anything outside it is either a misconfiguration or an
 * attempt. Note it excludes the trailing '$' of a Samba machine account — if such
 * an account ever needs a per-user vault, this is the one place to widen. */
int vault_principal_name_sanitize(const char *name, char *out, size_t out_len);

/* Max length of a vault principal string ("webuser:" + a 128-byte username, or
 * "uid:" + digits) including the NUL. */
#define VAULT_PRINCIPAL_MAX 160

/* Resolve a connection's attested transport + vault principal, fail-closed.
 *
 * Inputs (all captured while the conn is live):
 *   is_tcp           - 1 for the network listener, 0 for the local UDS socket.
 *   peer_uid         - SO_PEERCRED uid for a UDS peer, or -1 when unavailable/TCP.
 *   webuser          - the X-Aimee-Webuser header value, or NULL/"" if absent.
 *   webuser_attested - 1 iff the root-owned UDS peer attests the webchat request.
 *
 * Writes the principal string into out (out[0]=='\0' when there is NO vault
 * identity) and returns the attested transport classification:
 *   - webuser asserted by root UDS peer    -> ATTEST_WEBCHAT_TRUSTED, "webuser:<name>"
 *     (principal EMPTY if the name fails vault_principal_name_sanitize — the
 *     classification is kept so gating still sees a webchat hop, exactly as the
 *     cert path keeps ATTEST_MTLS_CLIENT for an unsanitizable CN)
 *   - webuser asserted without root UDS attestation (spoof) -> classification per the
 *     underlying transport, principal EMPTY (the assertion is refused, not honored)
 *   - plain UDS peer with uid > 0           -> ATTEST_UDS_PEERCRED,    "uid:<n>"
 *   - UDS peer with uid 0 or unknown        -> ATTEST_UDS_PEERCRED,    "" (no uid:0)
 *   - plain TCP                             -> ATTEST_TCP_BEARER,      ""
 *
 * uid 0 is deliberately given NO principal: a zeroed/uninitialised conn (a missed
 * threading hop, or loopback's memset) reads as uid 0, so treating it as a real
 * principal would collapse to acting as root. out must be >= VAULT_PRINCIPAL_MAX. */
attested_transport_t vault_principal_resolve(int is_tcp, int is_tls, long peer_uid,
                                             const char *webuser, int webuser_attested,
                                             const char *cert_cn, char *out, size_t out_len);

#endif /* DEC_VAULT_PRINCIPAL_H */
