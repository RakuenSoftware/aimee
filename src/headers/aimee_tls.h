/* aimee_tls.h: minimal OpenSSL TLS client wrapper around a connected socket.
 *
 * Compiled only in WITH_TLS builds (Linux/macOS); the Windows thin client is
 * built without it and refuses https:// instead. aimee_client.c uses this to
 * speak TLS to an https:// remote aimee-server. Certificate verification is on
 * by default (system trust store + SNI + hostname check); set AIMEE_TLS_INSECURE=1
 * to skip it for self-signed/dev servers.
 */
#ifndef DEC_AIMEE_TLS_H
#define DEC_AIMEE_TLS_H 1

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

#ifdef __cplusplus
}
#endif

#endif /* DEC_AIMEE_TLS_H */
