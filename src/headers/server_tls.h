/* server_tls.h: native TLS termination for aimee-server (native-TLS phase 1b).
 *
 * Plain server TLS — the server presents a cert+key; the client verifies the
 * server and authenticates with the bearer. No client certificate (not mTLS):
 * a TLS+bearer connection is the attested write path for the credential vault.
 * Each TLS connection is handled by its own conn_worker thread, which owns the
 * SSL end to end (accept -> use -> SSL_free); SSE-offload is refused over TLS
 * (it dups the fd to a second thread that cannot share the SSL — phase 1c). */
#ifndef DEC_SERVER_TLS_H
#define DEC_SERVER_TLS_H 1

#include <openssl/ssl.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Build the process-wide server SSL_CTX from the cert + key PEM files. Returns
    * 0 on success, -1 on failure (logged). Idempotent (no-op once initialized).
    *
    * mtls_mode: 0 = off (plain server TLS), 1 = optional (request a client cert
    * but allow none), 2 = required (refuse a handshake without a valid client
    * cert). When mtls_mode > 0 the SSL_CTX verifies client certs against
    * client_ca_path (a PEM CA bundle); a load failure disables mTLS (logged). */
   int server_tls_init(const char *cert_path, const char *key_path, int mtls_mode,
                       const char *client_ca_path);

   /* Extract the verified mTLS client identity from a handshaked SSL: writes the
    * peer leaf cert's CN into cn_out (and hex serial into serial_out) and returns
    * 1 iff the peer presented a cert AND it verified OK against the client CA;
    * else 0 (cn_out/serial_out emptied). Bearer-only TLS conns return 0. */
   int server_tls_peer_identity(SSL *ssl, char *cn_out, size_t cn_len, char *serial_out,
                                size_t serial_len);

   /* 1 once server_tls_init has succeeded, else 0. */
   int server_tls_enabled(void);

   /* Run the TLS handshake on an accepted fd. Returns a new SSL* (caller owns it:
    * SSL_shutdown + SSL_free) on success, or NULL on handshake failure. */
   SSL *server_tls_accept(int fd);

   /* server_tls_init from the default location (<config>/tls/server.crt + .key). */
   int server_tls_init_default(void);

   /* Live cert reload (live-config-reload): re-read the SAME cert/key files server_tls_init
    * was given and atomically swap the listener's SSL_CTX (new handshakes use the new cert;
    * in-flight connections keep the old until they drain). Validate-or-keep: a cert that fails
    * to load keeps the current one. Returns 1 = reloaded, 0 = TLS not enabled, -1 = kept. */
   int server_tls_reload(void);

   /* Per-connection lifecycle for the conn worker: handshake an accepted fd and
    * register its SSL on the conn-io shim (NULL on failure — caller closes fd);
    * and the teardown (unregister + shutdown + free). */
   SSL *server_tls_begin(int fd);
   void server_tls_end(int fd, SSL *ssl);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SERVER_TLS_H */
