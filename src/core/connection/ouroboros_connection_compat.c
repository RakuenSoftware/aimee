/* Thin compatibility adapter forwarding non-TLS aimee/core/connection headers
 * (auth, control, endpoint, http1, socket) to ouroboros-connection. */
#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/control.h>
#include <aimee/core/connection/endpoint.h>
#include <aimee/core/connection/http1.h>
#include <aimee/core/connection/socket.h>

#include <ouroboros/connection/auth.h>
#include <ouroboros/connection/control.h>
#include <ouroboros/connection/endpoint.h>
#include <ouroboros/connection/http1.h>
#include <ouroboros/connection/socket.h>

#include <string.h>

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
   ouro_result_t r = ouro_control_init(&oc, deadline_ns, cancel_poll_ms, (ouro_cancel_fn)cancelled,
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
   ouro_result_t r = ouro_control_init_timeout(&oc, timeout_ms, cancel_poll_ms,
                                               (ouro_cancel_fn)cancelled, cancel_context);
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
