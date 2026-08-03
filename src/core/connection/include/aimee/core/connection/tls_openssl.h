#ifndef AIMEE_CORE_CONNECTION_TLS_OPENSSL_H
#define AIMEE_CORE_CONNECTION_TLS_OPENSSL_H 1

/* OpenSSL backend shared by the Linux/macOS thin client, aimee-server, and
 * aimee-kb. Platform-native thin-client backends implement the same higher-level
 * client API and do not include this header. */
#include <openssl/ssl.h>
#include <aimee/core/connection/control.h>

#ifdef __cplusplus
extern "C"
{
#endif

   SSL_CTX *aimee_core_tls_client_context(void);
   SSL_CTX *aimee_core_tls_server_context(void);

   /* Load an in-memory PEM identity and/or trust anchor into a fresh or existing
    * context. Identity requires both cert and key and verifies their match. */
   int aimee_core_tls_use_identity_pem(SSL_CTX *ctx, const char *cert_pem, const char *key_pem);
   int aimee_core_tls_trust_pem(SSL_CTX *ctx, const char *ca_pem);
   int aimee_core_tls_use_identity_files(SSL_CTX *ctx, const char *cert_path, const char *key_path);
   int aimee_core_tls_trust_file(SSL_CTX *ctx, const char *ca_path);

   /* Configure a client session's SNI and peer name. verify_name supports both
    * DNS names and IP literals and fails closed if neither verifier accepts the
    * supplied host. The fd/BIO may be attached before or after this call. */
   int aimee_core_tls_configure_client_session(SSL *ssl, const char *host, int verify_name);

   /* Create a client SSL object and attach fd. The context and fd remain owned
    * by the caller; aimee_core_tls_session_free owns only the SSL object. */
   SSL *aimee_core_tls_client_session_new(SSL_CTX *ctx, int fd, const char *host, int verify_name);

   int aimee_core_tls_handshake_client(SSL *ssl);
   /* Controlled TLS operations require SSL to wrap a nonblocking socket. */
   aimee_core_result_t
   aimee_core_tls_handshake_client_controlled(SSL *ssl, const aimee_core_control_t *control);
   int aimee_core_tls_write_all(SSL *ssl, const void *buffer, size_t length);
   aimee_core_result_t aimee_core_tls_write_all_controlled(SSL *ssl, const void *buffer,
                                                           size_t length,
                                                           const aimee_core_control_t *control,
                                                           size_t *bytes_written);
   long aimee_core_tls_read(SSL *ssl, void *buffer, size_t length);
   aimee_core_result_t aimee_core_tls_read_controlled(SSL *ssl, void *buffer, size_t length,
                                                      const aimee_core_control_t *control,
                                                      size_t *bytes_read);
   int aimee_core_tls_read_exact(SSL *ssl, void *buffer, size_t length);
   void aimee_core_tls_session_free(SSL *ssl);

#ifdef __cplusplus
}
#endif

#endif
