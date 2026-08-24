/* server_tls.h: native TLS termination for aimee-server (native-TLS phase 1b).
 *
 * Plain server TLS — the server presents a cert+key; the client verifies the
 * server and authenticates with the bearer. The ordinary listener always lets
 * a cert-less TLS handshake reach HTTP parsing so a new client can redeem its
 * enrollment bearer; required-mTLS posture is enforced per request and permits
 * only the exact enrollment routes without a verified client certificate.
 * Each TLS connection is handled by its own conn_worker thread, which owns the
 * SSL end to end (accept -> use -> SSL_free); SSE-offload is refused over TLS
 * (it dups the fd to a second thread that cannot share the SSL — phase 1c). */
#ifndef DEC_SERVER_TLS_H
#define DEC_SERVER_TLS_H 1

#include <openssl/ssl.h>
#include <stddef.h>

typedef struct
{
   char cn[257];
   char issuer[601];
   char serial_norm[129];
   char fingerprint[65];
   char channel_binding[65];
   int management_profile;
} server_tls_peer_cert_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Build the process-wide server SSL_CTX from the public cert PEM file and a
    * private key. A NULL/empty key_path loads the key directly from Vault; a
    * path remains available only to narrow test/management callers. Returns 0
    * on success, -1 on failure (logged). Idempotent once initialized.
    *
    * mtls_mode: 0 = off (plain server TLS), 1 = optional, 2 = required by the
    * HTTP authorization layer. Modes 1 and 2 both request a client certificate
    * and validate any certificate presented against client_ca_path (a PEM CA
    * bundle), while allowing a cert-less handshake solely so the HTTP layer can
    * authorize enrollment. A CA load failure refuses the TLS context. */
   int server_tls_init(const char *cert_path, const char *key_path, int mtls_mode,
                       const char *client_ca_path);

   /* Build the distinct P5 management-listener context. The three PEM files are
    * opened without following symlinks, captured once into bounded buffers, and
    * used for both the stored SHA-256 identities and context construction. The
    * published context is process-lifetime owned. A later call succeeds only
    * when all three captured PEM byte strings are identical to the originals. */
   int server_tls_management_init(const char *cert_path, const char *key_path,
                                  const char *client_ca_path);

   /* Production management listener variant: public cert/CA remain files while
    * the private-key PEM is supplied from the process-local Vault cache. */
   int server_tls_management_init_vault(const char *cert_path, const char *key_pem,
                                        const char *client_ca_path);

   /* Extract the verified mTLS client identity from a handshaked SSL: writes the
    * peer leaf cert's CN into cn_out (and hex serial into serial_out) and returns
    * 1 iff the peer presented a cert AND it verified OK against the client CA;
    * else 0 (cn_out/serial_out emptied). Bearer-only TLS conns return 0. */
   int server_tls_peer_identity(SSL *ssl, char *cn_out, size_t cn_len, char *serial_out,
                                size_t serial_len);

   /* Exact verified leaf identity plus an RFC 5705 exporter binding for this
    * live TLS connection. Strings are lowercase/canonical where applicable. */
   int server_tls_peer_cert(SSL *ssl, server_tls_peer_cert_t *out);
   int server_tls_local_fingerprint(SSL *ssl, char out[65]);
   /* Canonical identity of the certificate presented by this server-side SSL. */
   int server_tls_local_cert(SSL *ssl, server_tls_peer_cert_t *out);

   /* Run the TLS handshake on an accepted fd. Returns a new SSL* (caller owns it:
    * SSL_shutdown + SSL_free) on success, or NULL on handshake failure. */
   SSL *server_tls_accept(int fd);

   /* Why TLS could not start. Three unrelated failures used to share one return
    * value, and the caller reported all of them as "TLS cert/key not loadable" --
    * so a server whose certificate was perfectly good, but whose mTLS ramp
    * self-test could not reach DB1, told the operator to go and look at the
    * certificate. Measured: with no db1 module the log read
    *
    *   WARN  db1.pki: DB1 pki is unreachable (module call result 1)
    *   WARN  pki.ramp: mTLS ramp startup self-test failed; refusing mTLS startup
    *   ERROR server.http: tls_port=8743 set but TLS cert/key not loadable
    *
    * and only the last line is what an operator reads. */
   typedef enum
   {
      SERVER_TLS_INIT_OK = 0,
      SERVER_TLS_INIT_ERR_MTLS_RAMP = -1,  /* the ramp self-test refused (DB1 pki) */
      SERVER_TLS_INIT_ERR_CLIENT_CA = -2,  /* aimee's client CA could not be made/loaded */
      SERVER_TLS_INIT_ERR_IDENTITY = -3,   /* the server cert/key itself */
   } server_tls_init_result_t;

   /* A human-readable cause for the value above. Never NULL. */
   const char *server_tls_init_result_str(int result);

   /* Initialize from <config>/tls/server.crt plus the Vault-held private key.
    * Returns SERVER_TLS_INIT_OK, or one of the negative causes above. */
   int server_tls_init_default(void);

   /* Wait for the store the mTLS ramp needs before server_tls_init_default runs.
      The caller supplies the probe so this translation unit keeps no DB1
      dependency. No-op unless mTLS is configured on. */
   void server_tls_wait_for_store(int (*store_ready)(void));

   /* Live cert reload (live-config-reload): re-read the same public cert and
    * Vault key (or explicit test key path) and atomically swap the SSL_CTX;
    * in-flight connections keep the old until they drain). Validate-or-keep: a cert that fails
    * to load keeps the current one. Returns 1 = reloaded, 0 = TLS not enabled, -1 = kept. */
   int server_tls_reload(void);

   /* Current effective mTLS posture (0=off, 1=optional, 2=required). The value
    * may advance at runtime, so callers use this accessor rather than caching
    * startup configuration. */
   int server_tls_mtls_mode(void);

   /* Build a fully validated application-required mTLS context without publishing it. The
    * caller may durably commit its posture and then pass the context to
    * server_tls_activate_required, whose pointer swap is infallible. NULL means
    * TLS is absent/already required, or validation failed. */
   SSL_CTX *server_tls_prepare_required(void);
   int server_tls_activate_required(SSL_CTX *prepared);
   void server_tls_discard_prepared(SSL_CTX *prepared);

   /* Per-connection lifecycle for the conn worker: handshake an accepted fd and
    * register its SSL on the conn-io shim (NULL on failure — caller closes fd);
    * and the teardown (unregister + shutdown + free). */
   SSL *server_tls_begin(int fd);
   SSL *server_tls_management_begin(int fd);
   void server_tls_end(int fd, SSL *ssl);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SERVER_TLS_H */
