/* Thin compatibility adapter forwarding aimee/core/connection headers to
 * ouroboros-connection (ouroboros/connection headers), keeping Aimee's own
 * AIMEE_HOME / AIMEE_PROFILE directory resolver and env-var bridges in Aimee. */
#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/control.h>
#include <aimee/core/connection/endpoint.h>
#include <aimee/core/connection/http1.h>
#include <aimee/core/connection/native_tls.h>
#include <aimee/core/connection/socket.h>
#if defined(AIMEE_CORE_HAS_OPENSSL)
#include <aimee/core/connection/tls_openssl.h>
#include <ouroboros/connection/tls_openssl.h>
#endif

#include <ouroboros/connection/auth.h>
#include <ouroboros/connection/control.h>
#include <ouroboros/connection/endpoint.h>
#include <ouroboros/connection/http1.h>
#include <ouroboros/connection/native_tls.h>
#include <ouroboros/connection/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- auth ----------------------------------------------------------------- */

int aimee_core_credential_equal(const char *left, const char *right)
{
   return ouro_credential_equal(left, right);
}

const char *aimee_core_bearer_token(const char *authorization_value)
{
   return ouro_bearer_token(authorization_value);
}

int aimee_core_bearer_token_span(const char *value, size_t value_len, const char **token_out,
                                 size_t *token_len_out)
{
   return ouro_bearer_token_span(value, value_len, token_out, token_len_out);
}

int aimee_core_bearer_value(char *out, size_t out_size, const char *token)
{
   return ouro_bearer_value(out, out_size, token);
}

int aimee_core_host_is_loopback(const char *host)
{
   return ouro_host_is_loopback(host);
}

int aimee_core_would_leak_credential(int transport_is_secure, const char *host,
                                     const char *credential)
{
   return ouro_would_leak_credential(transport_is_secure, host, credential);
}

/* --- control -------------------------------------------------------------- */

static ouro_control_t to_ouro_control(const aimee_core_control_t *c)
{
   ouro_control_t oc;
   oc.deadline_ns = c->deadline_ns;
   oc.cancel_poll_ms = c->cancel_poll_ms;
   oc.cancelled = (ouro_cancel_fn)c->cancelled;
   oc.cancel_context = c->cancel_context;
   return oc;
}

static void from_ouro_control(aimee_core_control_t *dst, const ouro_control_t *src)
{
   dst->deadline_ns = src->deadline_ns;
   dst->cancel_poll_ms = src->cancel_poll_ms;
   dst->cancelled = (aimee_core_cancel_fn)src->cancelled;
   dst->cancel_context = src->cancel_context;
}

int64_t aimee_core_now_ns(void)
{
   return ouro_now_ns();
}

aimee_core_result_t aimee_core_control_init(aimee_core_control_t *control, int64_t deadline_ns,
                                            int cancel_poll_ms, aimee_core_cancel_fn cancelled,
                                            void *cancel_context)
{
   if (!control)
      return AIMEE_CORE_INVALID;
   ouro_control_t oc;
   ouro_result_t r =
       ouro_control_init(&oc, deadline_ns, cancel_poll_ms, (ouro_cancel_fn)cancelled,
                         cancel_context);
   if (r == OURO_OK)
      from_ouro_control(control, &oc);
   return (aimee_core_result_t)r;
}

aimee_core_result_t aimee_core_control_init_timeout(aimee_core_control_t *control, int timeout_ms,
                                                    int cancel_poll_ms,
                                                    aimee_core_cancel_fn cancelled,
                                                    void *cancel_context)
{
   if (!control)
      return AIMEE_CORE_INVALID;
   ouro_control_t oc;
   ouro_result_t r =
       ouro_control_init_timeout(&oc, timeout_ms, cancel_poll_ms, (ouro_cancel_fn)cancelled,
                                 cancel_context);
   if (r == OURO_OK)
      from_ouro_control(control, &oc);
   return (aimee_core_result_t)r;
}

aimee_core_result_t aimee_core_control_check(const aimee_core_control_t *control)
{
   if (!control)
      return AIMEE_CORE_OK;
   ouro_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_control_check(&oc);
}

int aimee_core_control_remaining_ms(const aimee_core_control_t *control)
{
   if (!control)
      return -1;
   ouro_control_t oc = to_ouro_control(control);
   return ouro_control_remaining_ms(&oc);
}

aimee_core_result_t aimee_core_wait_fd(int fd, aimee_core_wait_t events,
                                       const aimee_core_control_t *control)
{
   if (!control)
      return (aimee_core_result_t)ouro_wait_fd(fd, (ouro_wait_t)events, NULL);
   ouro_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_wait_fd(fd, (ouro_wait_t)events, &oc);
}

/* --- endpoint ------------------------------------------------------------- */

int aimee_core_endpoint_parse(const char *value, aimee_core_endpoint_t *out)
{
   if (!out)
      return -1;
   ouro_endpoint_t ep;
   int rc = ouro_endpoint_parse(value, &ep);
   if (rc == 0)
   {
      memcpy(out->host, ep.host, sizeof(out->host));
      memcpy(out->port, ep.port, sizeof(out->port));
      out->secure = ep.secure;
   }
   return rc;
}

/* --- http1 ---------------------------------------------------------------- */

static void copy_resp_to_ouro(ouro_http1_response_t *dst, const aimee_core_http1_response_t *src)
{
   dst->data = src->data;
   dst->length = src->length;
   dst->header_length = src->header_length;
   dst->content_length = src->content_length;
   dst->has_content_length = src->has_content_length;
   dst->connection_close = src->connection_close;
   dst->status = src->status;
   dst->transfer_chunked = src->transfer_chunked;
}

static void copy_resp_from_ouro(aimee_core_http1_response_t *dst, const ouro_http1_response_t *src)
{
   dst->data = src->data;
   dst->length = src->length;
   dst->header_length = src->header_length;
   dst->content_length = src->content_length;
   dst->has_content_length = src->has_content_length;
   dst->connection_close = src->connection_close;
   dst->status = src->status;
   dst->transfer_chunked = src->transfer_chunked;
}

int aimee_core_http1_headers_parse(aimee_core_http1_response_t *response, int allow_chunked)
{
   if (!response)
      return -1;
   ouro_http1_response_t orsp;
   copy_resp_to_ouro(&orsp, response);
   int rc = ouro_http1_headers_parse(&orsp, allow_chunked);
   copy_resp_from_ouro(response, &orsp);
   return rc;
}

int aimee_core_http1_response_read(const aimee_core_http1_io_t *io, size_t header_max,
                                   size_t response_max, int require_content_length,
                                   aimee_core_http1_response_t *response)
{
   if (!io || !response)
      return -1;
   ouro_http1_io_t oio = {io->context, (ouro_http1_read_fn)io->read,
                          (ouro_http1_write_all_fn)io->write_all};
   ouro_http1_response_t orsp;
   memset(&orsp, 0, sizeof(orsp));
   int rc = ouro_http1_response_read(&oio, header_max, response_max, require_content_length, &orsp);
   copy_resp_from_ouro(response, &orsp);
   return rc;
}

int aimee_core_http1_exchange(const aimee_core_http1_io_t *io, const void *request,
                              size_t request_length, size_t header_max, size_t response_max,
                              int require_content_length, aimee_core_http1_response_t *response)
{
   if (!io || !response)
      return -1;
   ouro_http1_io_t oio = {io->context, (ouro_http1_read_fn)io->read,
                          (ouro_http1_write_all_fn)io->write_all};
   ouro_http1_response_t orsp;
   memset(&orsp, 0, sizeof(orsp));
   int rc = ouro_http1_exchange(&oio, request, request_length, header_max, response_max,
                                require_content_length, &orsp);
   copy_resp_from_ouro(response, &orsp);
   return rc;
}

void aimee_core_http1_response_free(aimee_core_http1_response_t *response)
{
   if (!response)
      return;
   ouro_http1_response_t orsp;
   copy_resp_to_ouro(&orsp, response);
   ouro_http1_response_free(&orsp);
   copy_resp_from_ouro(response, &orsp);
}

/* --- socket --------------------------------------------------------------- */

int aimee_core_socket_connect(const char *host, const char *port, int timeout_ms)
{
   return ouro_socket_connect(host, port, timeout_ms);
}

int aimee_core_socket_listen_loopback(unsigned port, unsigned *bound_port)
{
   return ouro_socket_listen_loopback(port, bound_port);
}

int aimee_core_socket_accept(int listener)
{
   return ouro_socket_accept(listener);
}

void aimee_core_socket_shutdown(int fd)
{
   ouro_socket_shutdown(fd);
}

int aimee_core_socket_peer_closed(int fd)
{
   return ouro_socket_peer_closed(fd);
}

int aimee_core_connect_slice_ms(int remaining_ms, int candidates, int attempted)
{
   return ouro_connect_slice_ms(remaining_ms, candidates, attempted);
}

aimee_core_result_t aimee_core_socket_connect_controlled(const char *host, const char *port,
                                                         unsigned flags,
                                                         const aimee_core_control_t *control,
                                                         int *out_fd)
{
   if (!control)
      return (aimee_core_result_t)ouro_socket_connect_controlled(host, port, flags, NULL, out_fd);
   ouro_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_socket_connect_controlled(host, port, flags, &oc, out_fd);
}

#ifndef _WIN32
aimee_core_result_t aimee_core_socket_connect_addresses(const struct addrinfo *addresses,
                                                        unsigned flags,
                                                        const aimee_core_control_t *control,
                                                        int *out_fd)
{
   if (!control)
      return (aimee_core_result_t)ouro_socket_connect_addresses(addresses, flags, NULL, out_fd);
   ouro_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_socket_connect_addresses(addresses, flags, &oc, out_fd);
}
#endif

int aimee_core_socket_set_timeouts(int fd, int receive_timeout_ms, int send_timeout_ms)
{
   return ouro_socket_set_timeouts(fd, receive_timeout_ms, send_timeout_ms);
}

int aimee_core_socket_write_all(int fd, const void *buffer, size_t length)
{
   return ouro_socket_write_all(fd, buffer, length);
}

aimee_core_result_t aimee_core_socket_write_all_controlled(int fd, const void *buffer,
                                                           size_t length,
                                                           const aimee_core_control_t *control,
                                                           size_t *bytes_written)
{
   if (!control)
      return (aimee_core_result_t)ouro_socket_write_all_controlled(fd, buffer, length, NULL,
                                                                   bytes_written);
   ouro_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_socket_write_all_controlled(fd, buffer, length, &oc,
                                                                bytes_written);
}

long aimee_core_socket_read(int fd, void *buffer, size_t length)
{
   return ouro_socket_read(fd, buffer, length);
}

aimee_core_result_t aimee_core_socket_read_controlled(int fd, void *buffer, size_t length,
                                                      const aimee_core_control_t *control,
                                                      size_t *bytes_read)
{
   if (!control)
      return (aimee_core_result_t)ouro_socket_read_controlled(fd, buffer, length, NULL, bytes_read);
   ouro_control_t oc = to_ouro_control(control);
   return (aimee_core_result_t)ouro_socket_read_controlled(fd, buffer, length, &oc, bytes_read);
}

int aimee_core_socket_wait_readable(int fd, int timeout_ms)
{
   return ouro_socket_wait_readable(fd, timeout_ms);
}

void aimee_core_socket_close(int fd)
{
   ouro_socket_close(fd);
}

/* --- OpenSSL TLS backend -------------------------------------------------- */

#if defined(AIMEE_CORE_HAS_OPENSSL)
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
   ouro_control_t oc = to_ouro_control(control);
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
   ouro_control_t oc = to_ouro_control(control);
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
   ouro_control_t oc = to_ouro_control(control);
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
#endif

/* --- Native TLS client ---------------------------------------------------- */

#if defined(AIMEE_CORE_HAS_NATIVE_TLS)
static const char *aimee_default_tls_home(void)
{
   static _Thread_local char path[4096];
   const char *override = getenv("AIMEE_HOME");
   if (override && override[0])
   {
      int n = snprintf(path, sizeof(path), "%s", override);
      return n > 0 && (size_t)n < sizeof(path) ? path : NULL;
   }
   const char *home = getenv("HOME");
#ifdef _WIN32
   if (!home || !home[0])
      home = getenv("USERPROFILE");
#endif
   if (!home || !home[0])
      return NULL;
   const char *profile = getenv("AIMEE_PROFILE");
   int n = profile && profile[0]
               ? snprintf(path, sizeof(path), "%s/.config/aimee/profiles/%s", home, profile)
               : snprintf(path, sizeof(path), "%s/.config/aimee", home);
   return n > 0 && (size_t)n < sizeof(path) ? path : NULL;
}

static void aimee_tls_bridge_env(void)
{
   ouro_native_tls_set_home_resolver(aimee_default_tls_home);
   const char *insec = getenv("AIMEE_TLS_INSECURE");
   if (insec && insec[0] && !getenv("OURO_TLS_INSECURE"))
   {
#ifdef _WIN32
      _putenv_s("OURO_TLS_INSECURE", insec);
#else
      setenv("OURO_TLS_INSECURE", insec, 0);
#endif
   }
   const char *p12 = getenv("AIMEE_TLS_CLIENT_P12_PASS");
   if (p12 && p12[0] && !getenv("OURO_TLS_CLIENT_P12_PASS"))
   {
#ifdef _WIN32
      _putenv_s("OURO_TLS_CLIENT_P12_PASS", p12);
#else
      setenv("OURO_TLS_CLIENT_P12_PASS", p12, 0);
#endif
   }
}

aimee_tls_t *aimee_tls_connect(int fd, const char *host)
{
   aimee_tls_bridge_env();
   return (aimee_tls_t *)ouro_native_tls_connect(fd, host);
}

aimee_tls_t *aimee_tls_connect_home(int fd, const char *host, const char *home)
{
   aimee_tls_bridge_env();
   return (aimee_tls_t *)ouro_native_tls_connect_home(fd, host, home);
}

int aimee_tls_write_all(aimee_tls_t *t, const void *buf, size_t len)
{
   return ouro_native_tls_write_all((ouro_native_tls_t *)t, buf, len);
}

long aimee_tls_read(aimee_tls_t *t, void *buf, size_t len)
{
   return ouro_native_tls_read((ouro_native_tls_t *)t, buf, len);
}

void aimee_tls_free(aimee_tls_t *t)
{
   ouro_native_tls_free((ouro_native_tls_t *)t);
}

int aimee_tls_client_cert_eligible(const char *home, char *crt, size_t crt_n, char *key,
                                   size_t key_n)
{
   return ouro_native_tls_client_cert_eligible(home, crt, crt_n, key, key_n);
}

void aimee_tls_suppress_client_cert(int on)
{
   ouro_native_tls_suppress_client_cert(on);
}

int aimee_tls_fetch_peer_cert(const char *host, const char *port, char **pem_out, char *fp_out,
                              size_t fp_n)
{
   return ouro_native_tls_fetch_peer_cert(host, port, pem_out, fp_out, fp_n);
}
#endif
