/* Portable native TLS client contract owned by aimee-core-connection.
 * OpenSSL, Secure Transport, and Schannel implement this same API. */
#ifndef AIMEE_CORE_CONNECTION_NATIVE_TLS_H
#define AIMEE_CORE_CONNECTION_NATIVE_TLS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct aimee_tls aimee_tls_t;

   /* Perform a TLS handshake over the already-connected socket |fd|, using |host|
    * for SNI and hostname verification. Returns an opaque handle (caller frees
    * via aimee_tls_free; the fd is NOT closed here) or NULL on failure. */
   aimee_tls_t *aimee_tls_connect(int fd, const char *host);

   /* Write exactly |len| bytes. Returns 0 on success, -1 on error. */
   int aimee_tls_write_all(aimee_tls_t *t, const void *buf, size_t len);

   /* Read up to |len| bytes. Returns count (0 on clean close), -1 on error. */
   long aimee_tls_read(aimee_tls_t *t, void *buf, size_t len);

   /* Shut down and free the handle (does not close the underlying fd). */
   void aimee_tls_free(aimee_tls_t *t);

   /* Decide whether client mTLS material under |home| should be presented as
    * this client's certificate, filling the resolved <home>/tls/client.{crt,key}
    * paths. Returns 1 (present: both exist, key is owner-only), 0 (none
    * configured), or -1 (refused: the key is group/world-readable — fail
    * closed). Pure; the OpenSSL backend calls it before the handshake. Exposed
    * for unit testing the fail-closed permission gate. */
   int aimee_tls_client_cert_eligible(const char *home, char *crt, size_t crt_n, char *key,
                                      size_t key_n);

   /* Suppress presenting the stored mTLS identity for subsequent handshakes in
    * this process (off by default; process-local, never touches disk).
    *
    * `aimee remote set/trust` uses this to retry a failed trust probe WITHOUT the
    * stored certificate. A client cert the server refuses — typically one minted
    * by a PREVIOUS server instance's client-CA, after the server was
    * re-provisioned — is otherwise indistinguishable from an unreachable host:
    * TLS 1.3 sends client-auth failures only after the client's handshake has
    * already completed, so the connect succeeds and the *read* fails with no
    * status. Retrying suppressed turns that ambiguity into a definite answer.
    *
    * This deliberately bypasses the fail-closed permission gate, so it must stay
    * scoped to a diagnostic probe: turn it on, probe, turn it off. Never leave it
    * set across ordinary requests, which would silently downgrade a broken mTLS
    * deployment to bearer-only TLS — exactly what the gate exists to prevent. */
   void aimee_tls_suppress_client_cert(int on);

   /* Open a fresh TLS connection to |host|:|port| WITHOUT verifying the server,
    * and return its leaf certificate as PEM (caller free()s *pem_out) plus the
    * SHA-256 fingerprint as uppercase colon-hex in |fp_out|. For `aimee remote
    * set/trust` to pin a self-signed/private server (trust-on-first-use): the
    * caller surfaces the fingerprint for an out-of-band check and writes the PEM
    * to <aimee_home>/remote-ca.pem, which aimee_tls_connect then trusts with
    * verification still ON. Returns 0 on success, -1 on failure (incl. platforms
    * where it is not yet implemented). */
   int aimee_tls_fetch_peer_cert(const char *host, const char *port, char **pem_out, char *fp_out,
                                 size_t fp_n);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_CORE_CONNECTION_NATIVE_TLS_H */
