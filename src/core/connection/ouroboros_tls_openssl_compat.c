/* Thin compatibility adapter forwarding aimee/core/connection/tls_openssl.h
 * to ouroboros-connection. Kept in its own translation unit so non-TLS
 * binaries linking libaimee-core-connection.a do not pull OpenSSL. */
#include <aimee/core/connection/tls_openssl.h>
#include <ouroboros/connection/tls_openssl.h>

static ouro_io_control_t to_ouro_control(const aimee_core_control_t *c)
{
   ouro_io_control_t oc;
   oc.deadline_ns = c->deadline_ns;
   oc.cancel_poll_ms = c->cancel_poll_ms;
   oc.cancelled = (ouro_cancel_fn)c->cancelled;
   oc.cancel_context = c->cancel_context;
   return oc;
}

SSL_CTX *aimee_core_tls_client_context(void)
{
   return ouro_tls_client_context();
}

SSL_CTX *aimee_core_tls_server_context(void)
{
   return ouro_tls_server_context();
}

int aimee_core_tls_use_identity_pem(SSL_CTX *ctx, const char *cert_pem, const char *key_pem)
{
   return ouro_tls_use_identity_pem(ctx, cert_pem, key_pem);
}

int aimee_core_tls_trust_pem(SSL_CTX *ctx, const char *ca_pem)
{
   return ouro_tls_trust_pem(ctx, ca_pem);
}

int aimee_core_tls_use_identity_files(SSL_CTX *ctx, const char *cert_path, const char *key_path)
{
   return ouro_tls_use_identity_files(ctx, cert_path, key_path);
}

int aimee_core_tls_trust_file(SSL_CTX *ctx, const char *ca_path)
{
   return ouro_tls_trust_file(ctx, ca_path);
}

int aimee_core_tls_configure_client_session(SSL *ssl, const char *host, int verify_name)
{
   return ouro_tls_configure_client_session(ssl, host, verify_name);
}

SSL *aimee_core_tls_client_session_new(SSL_CTX *ctx, int fd, const char *host, int verify_name)
{
   return ouro_tls_client_session_new(ctx, fd, host, verify_name);
}

int aimee_core_tls_handshake_client(SSL *ssl)
{
   return ouro_tls_handshake_client(ssl);
}

aimee_core_result_t aimee_core_tls_handshake_client_controlled(SSL *ssl,
                                                               const aimee_core_control_t *control)
{
   if (!control)
      return (aimee_core_result_t)ouro_tls_handshake_client_controlled(ssl, NULL);
   ouro_io_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_tls_handshake_client_controlled(ssl, &oc);
}

int aimee_core_tls_write_all(SSL *ssl, const void *buffer, size_t length)
{
   return ouro_tls_write_all(ssl, buffer, length);
}

aimee_core_result_t aimee_core_tls_write_all_controlled(SSL *ssl, const void *buffer, size_t length,
                                                        const aimee_core_control_t *control,
                                                        size_t *bytes_written)
{
   if (!control)
      return (aimee_core_result_t)ouro_tls_write_all_controlled(ssl, buffer, length, NULL,
                                                                bytes_written);
   ouro_io_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_tls_write_all_controlled(ssl, buffer, length, &oc,
                                                             bytes_written);
}

long aimee_core_tls_read(SSL *ssl, void *buffer, size_t length)
{
   return ouro_tls_read(ssl, buffer, length);
}

aimee_core_result_t aimee_core_tls_read_controlled(SSL *ssl, void *buffer, size_t length,
                                                   const aimee_core_control_t *control,
                                                   size_t *bytes_read)
{
   if (!control)
      return (aimee_core_result_t)ouro_tls_read_controlled(ssl, buffer, length, NULL, bytes_read);
   ouro_io_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_tls_read_controlled(ssl, buffer, length, &oc, bytes_read);
}

int aimee_core_tls_read_exact(SSL *ssl, void *buffer, size_t length)
{
   return ouro_tls_read_exact(ssl, buffer, length);
}

void aimee_core_tls_session_free(SSL *ssl)
{
   ouro_tls_session_free(ssl);
}
