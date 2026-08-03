#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/control.h>
#include <aimee/core/connection/endpoint.h>
#include <aimee/core/connection/http1.h>
#include <aimee/core/connection/socket.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

typedef struct
{
   const char *response;
   size_t response_length;
   size_t response_offset;
   size_t read_chunk;
   char request[256];
   size_t request_length;
} memory_http_io_t;

static long memory_http_read(void *context, void *buffer, size_t length)
{
   memory_http_io_t *io = context;
   if (io->response_offset == io->response_length)
      return 0;
   size_t available = io->response_length - io->response_offset;
   if (length > available)
      length = available;
   if (io->read_chunk && length > io->read_chunk)
      length = io->read_chunk;
   memcpy(buffer, io->response + io->response_offset, length);
   io->response_offset += length;
   return (long)length;
}

static int memory_http_write(void *context, const void *buffer, size_t length)
{
   memory_http_io_t *io = context;
   if (length > sizeof(io->request))
      return -1;
   memcpy(io->request, buffer, length);
   io->request_length = length;
   return 0;
}

static void test_credentials(void)
{
   assert(aimee_core_credential_equal("secret", "secret"));
   assert(!aimee_core_credential_equal("secret", "secreu"));
   assert(!aimee_core_credential_equal("secret", "secret-longer"));
   assert(aimee_core_credential_equal(NULL, ""));

   assert(strcmp(aimee_core_bearer_token("Bearer abc.def"), "abc.def") == 0);
   assert(strcmp(aimee_core_bearer_token("bearer scope:service:kb:key"), "scope:service:kb:key") ==
          0);
   assert(aimee_core_bearer_token("Basic abc") == NULL);
   assert(aimee_core_bearer_token("Bearer bad token") == NULL);
   assert(aimee_core_bearer_token("Bearer\tbad") == NULL);
   const char span[] = "Bearer bounded";
   const char *bounded = NULL;
   size_t bounded_len = 0;
   assert(aimee_core_bearer_token_span(span, sizeof(span) - 1, &bounded, &bounded_len) == 0);
   assert(bounded_len == 7 && memcmp(bounded, "bounded", bounded_len) == 0);

   char value[64];
   assert(aimee_core_bearer_value(value, sizeof(value), "token") == 0);
   assert(strcmp(value, "Bearer token") == 0);
   assert(aimee_core_bearer_value(value, sizeof(value), "bad\r\ntoken") == -1);
}

static void test_cleartext_policy(void)
{
   assert(aimee_core_host_is_loopback("localhost"));
   assert(aimee_core_host_is_loopback("LOCALHOST"));
   assert(aimee_core_host_is_loopback("127.9.8.7"));
   assert(aimee_core_host_is_loopback("::1"));
   assert(!aimee_core_host_is_loopback("10.0.0.5"));
   assert(aimee_core_would_leak_credential(0, "10.0.0.5", "token"));
   assert(!aimee_core_would_leak_credential(1, "10.0.0.5", "token"));
   assert(!aimee_core_would_leak_credential(0, "127.0.0.1", "token"));
}

static void test_endpoints(void)
{
   aimee_core_endpoint_t endpoint;
   assert(aimee_core_endpoint_parse("https://server.example:8743/v1", &endpoint) == 0);
   assert(strcmp(endpoint.host, "server.example") == 0);
   assert(strcmp(endpoint.port, "8743") == 0);
   assert(endpoint.secure == 1);

   assert(aimee_core_endpoint_parse("http://[2001:db8::1]:8741/v1", &endpoint) == 0);
   assert(strcmp(endpoint.host, "2001:db8::1") == 0);
   assert(strcmp(endpoint.port, "8741") == 0);
   assert(endpoint.secure == 0);

   assert(aimee_core_endpoint_parse("kb.internal", &endpoint) == 0);
   assert(strcmp(endpoint.port, "80") == 0);
   assert(aimee_core_endpoint_parse("ftp://server.example", &endpoint) == -1);
   assert(aimee_core_endpoint_parse("http://user@server.example", &endpoint) == -1);
   assert(aimee_core_endpoint_parse("server.example:http", &endpoint) == -1);
   assert(aimee_core_endpoint_parse("server.example:0", &endpoint) == -1);
   assert(aimee_core_endpoint_parse("server.example:65536", &endpoint) == -1);
}

static void test_http1_engine(void)
{
   static const char framed[] =
       "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: keep-alive\r\n\r\nhello";
   memory_http_io_t memory = {
       .response = framed, .response_length = sizeof(framed) - 1, .read_chunk = 3};
   aimee_core_http1_io_t io = {
       .context = &memory, .read = memory_http_read, .write_all = memory_http_write};
   aimee_core_http1_response_t response;
   static const char request[] = "GET / HTTP/1.1\r\n\r\n";
   assert(aimee_core_http1_exchange(&io, request, sizeof(request) - 1, 1024, 4096, 1, &response) ==
          0);
   assert(memory.request_length == sizeof(request) - 1);
   assert(memcmp(memory.request, request, sizeof(request) - 1) == 0);
   assert(response.status == 200);
   assert(response.has_content_length && response.content_length == 5);
   assert(!response.connection_close);
   assert(strcmp(response.data + response.header_length, "hello") == 0);
   aimee_core_http1_response_free(&response);

   static const char close_delimited[] =
       "HTTP/1.0 503 Unavailable\r\nConnection: close\r\n\r\nretry";
   memory = (memory_http_io_t){.response = close_delimited,
                               .response_length = sizeof(close_delimited) - 1};
   io.context = &memory;
   assert(aimee_core_http1_response_read(&io, 1024, 4096, 0, &response) == 0);
   assert(response.status == 503 && response.connection_close);
   aimee_core_http1_response_free(&response);

   memory = (memory_http_io_t){.response = close_delimited,
                               .response_length = sizeof(close_delimited) - 1};
   assert(aimee_core_http1_response_read(&io, 1024, 4096, 1, &response) == -1);

   static const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
   memory = (memory_http_io_t){.response = chunked, .response_length = sizeof(chunked) - 1};
   assert(aimee_core_http1_response_read(&io, 1024, 4096, 0, &response) == -1);

   static const char duplicate[] =
       "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx";
   memory = (memory_http_io_t){.response = duplicate, .response_length = sizeof(duplicate) - 1};
   assert(aimee_core_http1_response_read(&io, 1024, 4096, 1, &response) == -1);
}

#ifndef _WIN32
typedef struct
{
   int checks;
} cancel_test_t;

static int cancel_after_first_check(void *context)
{
   cancel_test_t *state = context;
   return state->checks++ > 0;
}

static void test_controlled_io(void)
{
   int sockets[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

   aimee_core_control_t control;
   assert(aimee_core_control_init_timeout(&control, 1000, 0, NULL, NULL) == AIMEE_CORE_OK);
   static const char message[] = "controlled";
   size_t transferred = 0;
   assert(aimee_core_socket_write_all_controlled(sockets[0], message, sizeof(message), &control,
                                                 &transferred) == AIMEE_CORE_OK);
   assert(transferred == sizeof(message));
   char received[sizeof(message)] = {0};
   assert(aimee_core_socket_read_controlled(sockets[1], received, sizeof(received), &control,
                                            &transferred) == AIMEE_CORE_OK);
   assert(transferred == sizeof(message) && memcmp(received, message, sizeof(message)) == 0);

   cancel_test_t cancellation = {0};
   assert(aimee_core_control_init_timeout(&control, 1000, 1, cancel_after_first_check,
                                          &cancellation) == AIMEE_CORE_OK);
   assert(aimee_core_wait_fd(sockets[0], AIMEE_CORE_WAIT_READ, &control) == AIMEE_CORE_CANCELLED);

   assert(aimee_core_control_init_timeout(&control, 1, 0, NULL, NULL) == AIMEE_CORE_OK);
   assert(aimee_core_wait_fd(sockets[0], AIMEE_CORE_WAIT_READ, &control) == AIMEE_CORE_TIMEOUT);
   close(sockets[0]);
   close(sockets[1]);
}
#endif

int main(void)
{
   test_credentials();
   test_cleartext_policy();
   test_endpoints();
   test_http1_engine();
#ifndef _WIN32
   test_controlled_io();
#endif
   puts("test_communication: ok");
   return 0;
}
