#include <aimee/core/connection/tls_openssl.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <limits.h>

static aimee_core_result_t tls_wait(SSL *ssl, int result, const aimee_core_control_t *control)
{
   int error = SSL_get_error(ssl, result);
   if (error == SSL_ERROR_WANT_READ)
      return aimee_core_wait_fd(SSL_get_fd(ssl), AIMEE_CORE_WAIT_READ, control);
   if (error == SSL_ERROR_WANT_WRITE)
      return aimee_core_wait_fd(SSL_get_fd(ssl), AIMEE_CORE_WAIT_WRITE, control);
   return AIMEE_CORE_TLS_ERROR;
}

static SSL_CTX *context_new(const SSL_METHOD *method)
{
   SSL_CTX *ctx = SSL_CTX_new(method);
   if (!ctx)
      return NULL;
   if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   return ctx;
}

SSL_CTX *aimee_core_tls_client_context(void)
{
   return context_new(TLS_client_method());
}

SSL_CTX *aimee_core_tls_server_context(void)
{
   return context_new(TLS_server_method());
}

int aimee_core_tls_use_identity_pem(SSL_CTX *ctx, const char *cert_pem, const char *key_pem)
{
   if (!ctx || !cert_pem || !*cert_pem || !key_pem || !*key_pem)
      return -1;
   BIO *cert_bio = BIO_new_mem_buf(cert_pem, -1);
   BIO *key_bio = BIO_new_mem_buf(key_pem, -1);
   X509 *cert = cert_bio ? PEM_read_bio_X509(cert_bio, NULL, NULL, NULL) : NULL;
   EVP_PKEY *key = key_bio ? PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL) : NULL;
   int ok = cert && key && SSL_CTX_use_certificate(ctx, cert) == 1 &&
            SSL_CTX_use_PrivateKey(ctx, key) == 1 && SSL_CTX_check_private_key(ctx) == 1;
   EVP_PKEY_free(key);
   X509_free(cert);
   BIO_free(key_bio);
   BIO_free(cert_bio);
   return ok ? 0 : -1;
}

int aimee_core_tls_trust_pem(SSL_CTX *ctx, const char *ca_pem)
{
   if (!ctx || !ca_pem || !*ca_pem)
      return -1;
   BIO *bio = BIO_new_mem_buf(ca_pem, -1);
   X509 *ca = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
   int ok = ca && X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), ca) == 1;
   X509_free(ca);
   BIO_free(bio);
   return ok ? 0 : -1;
}

int aimee_core_tls_use_identity_files(SSL_CTX *ctx, const char *cert_path, const char *key_path)
{
   if (!ctx || !cert_path || !*cert_path || !key_path || !*key_path)
      return -1;
   int ok = SSL_CTX_use_certificate_chain_file(ctx, cert_path) == 1 &&
            SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) == 1 &&
            SSL_CTX_check_private_key(ctx) == 1;
   return ok ? 0 : -1;
}

int aimee_core_tls_trust_file(SSL_CTX *ctx, const char *ca_path)
{
   if (!ctx || !ca_path || !*ca_path)
      return -1;
   int ok = SSL_CTX_load_verify_locations(ctx, ca_path, NULL) == 1;
   return ok ? 0 : -1;
}

int aimee_core_tls_configure_client_session(SSL *ssl, const char *host, int verify_name)
{
   if (!ssl || !host || !*host || SSL_set_tlsext_host_name(ssl, host) != 1)
      return -1;
   if (!verify_name)
      return 0;
   X509_VERIFY_PARAM *parameters = SSL_get0_param(ssl);
   if (!parameters)
      return -1;
   X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
   return X509_VERIFY_PARAM_set1_ip_asc(parameters, host) == 1 ||
                  X509_VERIFY_PARAM_set1_host(parameters, host, 0) == 1
              ? 0
              : -1;
}

SSL *aimee_core_tls_client_session_new(SSL_CTX *ctx, int fd, const char *host, int verify_name)
{
   if (!ctx || fd < 0)
      return NULL;
   SSL *ssl = SSL_new(ctx);
   if (!ssl)
      return NULL;
   if (SSL_set_fd(ssl, fd) != 1 ||
       aimee_core_tls_configure_client_session(ssl, host, verify_name) != 0)
   {
      SSL_free(ssl);
      return NULL;
   }
   return ssl;
}

int aimee_core_tls_handshake_client(SSL *ssl)
{
   return ssl && SSL_connect(ssl) == 1 ? 0 : -1;
}

aimee_core_result_t aimee_core_tls_handshake_client_controlled(SSL *ssl,
                                                               const aimee_core_control_t *control)
{
   if (!ssl || !control)
      return AIMEE_CORE_INVALID;
   for (;;)
   {
      aimee_core_result_t checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      int result = SSL_connect(ssl);
      checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      if (result == 1)
         return AIMEE_CORE_OK;
      checked = tls_wait(ssl, result, control);
      if (checked != AIMEE_CORE_OK)
         return checked;
   }
}

int aimee_core_tls_write_all(SSL *ssl, const void *buffer, size_t length)
{
   if (!ssl || (!buffer && length))
      return -1;
   const unsigned char *bytes = buffer;
   size_t offset = 0;
   while (offset < length)
   {
      size_t written = 0;
      if (SSL_write_ex(ssl, bytes + offset, length - offset, &written) != 1 || !written)
         return -1;
      offset += written;
   }
   return 0;
}

aimee_core_result_t aimee_core_tls_write_all_controlled(SSL *ssl, const void *buffer, size_t length,
                                                        const aimee_core_control_t *control,
                                                        size_t *bytes_written)
{
   if (bytes_written)
      *bytes_written = 0;
   if (!ssl || (!buffer && length) || !control || !bytes_written)
      return AIMEE_CORE_INVALID;
   const unsigned char *bytes = buffer;
   while (*bytes_written < length)
   {
      aimee_core_result_t checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      size_t written = 0;
      int result = SSL_write_ex(ssl, bytes + *bytes_written, length - *bytes_written, &written);
      if (written)
         *bytes_written += written;
      checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      if (result == 1)
      {
         if (!written)
            return AIMEE_CORE_IO_ERROR;
         continue;
      }
      checked = tls_wait(ssl, result, control);
      if (checked != AIMEE_CORE_OK)
         return checked == AIMEE_CORE_TLS_ERROR ? AIMEE_CORE_IO_ERROR : checked;
   }
   return AIMEE_CORE_OK;
}

long aimee_core_tls_read(SSL *ssl, void *buffer, size_t length)
{
   if (!ssl || !buffer || !length)
      return -1;
   size_t read_length = 0;
   if (SSL_read_ex(ssl, buffer, length, &read_length) == 1)
      return read_length > LONG_MAX ? -1 : (long)read_length;
   return SSL_get_error(ssl, 0) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

aimee_core_result_t aimee_core_tls_read_controlled(SSL *ssl, void *buffer, size_t length,
                                                   const aimee_core_control_t *control,
                                                   size_t *bytes_read)
{
   if (bytes_read)
      *bytes_read = 0;
   if (!ssl || !buffer || !length || !control || !bytes_read)
      return AIMEE_CORE_INVALID;
   for (;;)
   {
      aimee_core_result_t checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      size_t received = 0;
      int result = SSL_read_ex(ssl, buffer, length, &received);
      if (received)
         *bytes_read = received;
      checked = aimee_core_control_check(control);
      if (checked != AIMEE_CORE_OK)
         return checked;
      if (result == 1)
         return received ? AIMEE_CORE_OK : AIMEE_CORE_IO_ERROR;
      int error = SSL_get_error(ssl, result);
      if (error == SSL_ERROR_ZERO_RETURN)
         return AIMEE_CORE_EOF;
      checked = tls_wait(ssl, result, control);
      if (checked != AIMEE_CORE_OK)
         return checked == AIMEE_CORE_TLS_ERROR ? AIMEE_CORE_IO_ERROR : checked;
   }
}

int aimee_core_tls_read_exact(SSL *ssl, void *buffer, size_t length)
{
   unsigned char *bytes = buffer;
   size_t offset = 0;
   while (offset < length)
   {
      long received = aimee_core_tls_read(ssl, bytes + offset, length - offset);
      if (received <= 0)
         return -1;
      offset += (size_t)received;
   }
   return 0;
}

void aimee_core_tls_session_free(SSL *ssl)
{
   if (!ssl)
      return;
   (void)SSL_shutdown(ssl);
   SSL_free(ssl);
}
