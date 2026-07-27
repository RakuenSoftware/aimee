#define KB_HTTP_CLIENT_TESTING 1
#include "kb/http/kb_http_client.h"
#include "kb/http/kb_http_resolver_protocol.h"

#include <assert.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
   int headers_called, body_called, abort_body, raise_sigpipe;
   kb_http_gate_t gate;
   kb_http_response_t response;
   unsigned char body[1024];
   size_t body_len;
} capture_t;

static kb_http_gate_t capture_headers(const kb_http_response_t *response, void *context)
{
   capture_t *capture = context;
   assert(capture->headers_called == 0);
   assert(capture->body_called == 0);
   capture->headers_called = 1;
   capture->response = *response;
   return capture->gate;
}

static kb_http_body_action_t capture_body(const unsigned char *bytes, size_t length, void *context)
{
   capture_t *capture = context;
   assert(capture->headers_called == 1);
   assert(length > 0);
   capture->body_called++;
   if (capture->raise_sigpipe)
      assert(raise(SIGPIPE) == 0);
   if (capture->abort_body)
      return KB_HTTP_BODY_CALLER_ABORT;
   assert(capture->body_len + length <= sizeof(capture->body));
   memcpy(capture->body + capture->body_len, bytes, length);
   capture->body_len += length;
   return KB_HTTP_BODY_CONTINUE;
}

static kb_http_result_t parse_parts_ex(const unsigned char *wire, size_t length, size_t split,
                                       size_t body_max, capture_t *capture, int deliver_error_body)
{
   kb_http_response_parser_t *parser = NULL;
   assert(kb_http_response_parser_init(&parser, body_max, capture_headers, capture_body, capture) ==
          KB_HTTP_OK);
   kb_http_response_parser_deliver_error_body(parser, deliver_error_body);
   kb_http_result_t result = kb_http_response_parser_feed(parser, wire, split);
   if (result == KB_HTTP_MORE)
      result = kb_http_response_parser_feed(parser, wire + split, length - split);
   if (result == KB_HTTP_MORE)
      result = kb_http_response_parser_finish_eof(parser);
   kb_http_response_parser_free(&parser);
   assert(parser == NULL);
   return result;
}

static kb_http_result_t parse_parts(const unsigned char *wire, size_t length, size_t split,
                                    size_t body_max, capture_t *capture)
{
   kb_http_response_parser_t *parser = NULL;
   assert(kb_http_response_parser_init(&parser, body_max, capture_headers, capture_body, capture) ==
          KB_HTTP_OK);
   kb_http_result_t result = kb_http_response_parser_feed(parser, wire, split);
   if (result == KB_HTTP_MORE)
      result = kb_http_response_parser_feed(parser, wire + split, length - split);
   if (result == KB_HTTP_MORE)
      result = kb_http_response_parser_finish_eof(parser);
   kb_http_response_parser_free(&parser);
   assert(parser == NULL);
   return result;
}

static void content_length_boundaries(void)
{
   static const unsigned char response[] =
       "HTTP/1.1 200 OK\r\nContent-Type:\t application/json \t\r\nContent-Length: 5\r\n"
       "X-Request-Id: safe\r\n\r\nhello";
   const size_t length = sizeof(response) - 1;
   for (size_t split = 0; split <= length; split++)
   {
      capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
      assert(parse_parts(response, length, split, 64, &capture) == KB_HTTP_OK);
      assert(capture.headers_called == 1 && capture.response.status == 200);
      assert(capture.response.framing == KB_HTTP_FRAMING_CONTENT_LENGTH);
      assert(capture.response.content_length == 5);
      assert(strcmp(capture.response.content_type, "application/json") == 0);
      assert(capture.body_len == 5 && memcmp(capture.body, "hello", 5) == 0);
   }

   capture_t bytewise = {.gate = KB_HTTP_GATE_DELIVER};
   kb_http_response_parser_t *parser = NULL;
   assert(kb_http_response_parser_init(&parser, 64, capture_headers, capture_body, &bytewise) ==
          KB_HTTP_OK);
   for (size_t i = 0; i < length; i++)
      assert(kb_http_response_parser_feed(parser, response + i, 1) == KB_HTTP_MORE);
   assert(kb_http_response_parser_finish_eof(parser) == KB_HTTP_OK);
   assert(bytewise.body_len == 5 && bytewise.body_called == 5);
   kb_http_response_parser_free(&parser);
}

static void chunked_boundaries(void)
{
   static const unsigned char response[] =
       "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.amazon.eventstream\r\n"
       "Transfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
   const size_t length = sizeof(response) - 1;
   for (size_t split = 0; split <= length; split++)
   {
      capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
      assert(parse_parts(response, length, split, 64, &capture) == KB_HTTP_OK);
      assert(capture.response.framing == KB_HTTP_FRAMING_CHUNKED);
      assert(capture.response.content_length == 0);
      assert(capture.body_len == 9 && memcmp(capture.body, "Wikipedia", 9) == 0);
   }
}

/* The non-2xx body gate, both ways.
 *
 * The DEFAULT (discard) is not merely a performance choice: an error body from
 * arbitrary egress is attacker-influenced content nobody reads, so it must not be
 * buffered. The OPT-IN exists because RFC 6749 §5.2 makes the OAuth token endpoint's
 * 400 body the error carrier — {"error":"invalid_grant"} appears nowhere else.
 *
 * This test exists because the opt-in did not, and its absence was invisible: the
 * token exchange's _DENIED result was UNREACHABLE, so every identity-provider
 * refusal surfaced as "the IdP sent us something unparseable". Nothing in the unit
 * suite could show that, because it only appears when a real IdP answers 400. */
static void error_body_gate(void)
{
   /* Content-Length is the exact body length: {"error":"invalid_grant"} is 25 bytes. */
   static const unsigned char oauth_error[] =
       "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 25\r\n\r\n"
       "{\"error\":\"invalid_grant\"}";
   const size_t len = sizeof(oauth_error) - 1;

   /* Default: the headers callback still runs, the body does not reach it. */
   capture_t discarded = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts_ex(oauth_error, len, len, 64, &discarded, 0) == KB_HTTP_OK);
   assert(discarded.headers_called == 1);
   assert(discarded.body_called == 0 && discarded.body_len == 0);
   assert(discarded.response.status == 400);

   /* Opted in: the same response delivers its body, which is the only place the
    * OAuth error code appears. */
   capture_t delivered = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts_ex(oauth_error, len, len, 64, &delivered, 1) == KB_HTTP_OK);
   assert(delivered.headers_called == 1);
   assert(delivered.body_called >= 1 && delivered.body_len == 25);
   assert(memcmp(delivered.body, "{\"error\":\"invalid_grant\"}", 25) == 0);
   assert(delivered.response.status == 400);

   /* The opt-in does not override the headers callback's own decision: a callback
    * that says DISCARD is still obeyed. The flag widens WHICH statuses may deliver,
    * it does not force delivery. */
   capture_t refused = {.gate = KB_HTTP_GATE_DISCARD};
   assert(parse_parts_ex(oauth_error, len, len, 64, &refused, 1) == KB_HTTP_OK);
   assert(refused.body_called == 0 && refused.body_len == 0);

   /* And it changes nothing for a 2xx, which delivered already. */
   static const unsigned char okay[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
   capture_t two_xx = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts_ex(okay, sizeof(okay) - 1, sizeof(okay) - 1, 64, &two_xx, 0) == KB_HTTP_OK);
   assert(two_xx.body_len == 3);
   capture_t two_xx_opt = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts_ex(okay, sizeof(okay) - 1, sizeof(okay) - 1, 64, &two_xx_opt, 1) ==
          KB_HTTP_OK);
   assert(two_xx_opt.body_len == 3);

   /* The body ceiling still applies to a delivered error body — opting in must not
    * become a way to make kb buffer an unbounded 400. */
   capture_t bounded = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts_ex(oauth_error, len, len, 8, &bounded, 1) != KB_HTTP_OK);

   /* NULL is tolerated, so a caller that never sets the flag cannot crash. */
   kb_http_response_parser_deliver_error_body(NULL, 1);
}

static void gate_and_abort(void)
{
   static const unsigned char denied[] = "HTTP/1.1 429 Slow Down\r\nContent-Type: "
                                         "application/json\r\nContent-Length: 6\r\n\r\nsecret";
   capture_t discard = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts(denied, sizeof(denied) - 1, sizeof(denied) - 1, 64, &discard) == KB_HTTP_OK);
   assert(discard.headers_called == 1 && discard.body_called == 0 && discard.body_len == 0);

   static const unsigned char okay[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
   capture_t abort_headers = {.gate = KB_HTTP_GATE_ABORT};
   assert(parse_parts(okay, sizeof(okay) - 1, sizeof(okay) - 1, 64, &abort_headers) ==
          KB_HTTP_CALLBACK_ABORT);
   assert(abort_headers.body_called == 0);

   capture_t abort_body = {.gate = KB_HTTP_GATE_DELIVER, .abort_body = 1};
   kb_http_response_parser_t *parser = NULL;
   assert(kb_http_response_parser_init(&parser, 64, capture_headers, capture_body, &abort_body) ==
          KB_HTTP_OK);
   assert(kb_http_response_parser_feed(parser, okay, sizeof(okay) - 1) == KB_HTTP_CALLBACK_ABORT);
   assert(kb_http_response_parser_feed(parser, NULL, 0) == KB_HTTP_CALLBACK_ABORT);
   assert(kb_http_response_parser_finish_eof(parser) == KB_HTTP_CALLBACK_ABORT);
   kb_http_response_parser_free(&parser);
}

static void post_completion_surplus(void)
{
   static const char *const complete[] = {
       "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n"};
   for (size_t i = 0; i < sizeof(complete) / sizeof(complete[0]); i++)
   {
      capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
      kb_http_response_parser_t *parser = NULL;
      assert(kb_http_response_parser_init(&parser, 64, capture_headers, capture_body, &capture) ==
             KB_HTTP_OK);
      assert(kb_http_response_parser_feed(parser, (const unsigned char *)complete[i],
                                          strlen(complete[i])) == KB_HTTP_MORE);
      assert(kb_http_response_parser_feed(parser, (const unsigned char *)"x", 1) ==
             KB_HTTP_MALFORMED_RESPONSE);
      assert(kb_http_response_parser_finish_eof(parser) == KB_HTTP_MALFORMED_RESPONSE);
      kb_http_response_parser_free(&parser);
   }
}

static void malformed_matrix(void)
{
   static const char *const malformed[] = {
       "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 100 Continue\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\nContent-Length: 0\n\n",
       "HTTP/1.1 200 OK\r\n folded: bad\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nBad Name: x\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Type: a\r\nContent-Type: b\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nX: y\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: +1\r\n\r\nx",
       "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxy",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;x=y\r\na\r\n0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naX0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\nX: y\r\n\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFFFFFFFF\r\n"};
   for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++)
   {
      capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
      size_t length = strlen(malformed[i]);
      kb_http_result_t result =
          parse_parts((const unsigned char *)malformed[i], length, length / 2, 64, &capture);
      assert(result == KB_HTTP_MALFORMED_RESPONSE || result == KB_HTTP_TOO_LARGE);
   }

   static const unsigned char too_large[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nxxxx";
   capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts(too_large, sizeof(too_large) - 1, 0, 3, &capture) == KB_HTTP_TOO_LARGE);
}

static void request_validation(void)
{
   kb_http_header_t headers[] = {{"host", "bedrock-runtime.us-east-1.amazonaws.com"},
                                 {"content-type", "application/json"}};
   kb_http_request_t request = {.authority = "bedrock-runtime.us-east-1.amazonaws.com",
                                .method = "GET",
                                .target = "/model/x/converse",
                                .headers = headers,
                                .header_count = 2,
                                .body = (const unsigned char *)"{}",
                                .body_len = 2,
                                .response_body_max = 64,
                                .connect_timeout_ms = 1,
                                .total_timeout_ms = 1};
   kb_http_response_t response = {.status = 999};
   capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
   assert(kb_http_request_validate(&request) == KB_HTTP_INVALID_ARGUMENT);
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
   assert(response.status == 0);
   request.method = "POST";
   request.target = "//authority-form";
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
   request.target = "/x?query";
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
   request.target = "/x";
   headers[1].name = "Content-Length";
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
   headers[1] = headers[0];
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
}

static void origin_path_validation(void)
{
   kb_http_header_t headers[] = {{"host", "bedrock-runtime.us-east-1.amazonaws.com"}};
   kb_http_request_t request = {.authority = headers[0].value,
                                .method = "POST",
                                .target = "/",
                                .headers = headers,
                                .header_count = 1,
                                .body = (const unsigned char *)"{}",
                                .body_len = 2,
                                .response_body_max = 64,
                                .connect_timeout_ms = 1,
                                .total_timeout_ms = 1};
   static const char *const valid[] = {"/", "/model/a%3Ab/converse", "/a/b:c@d!$&'()*+,;=-._~"};
   for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
   {
      request.target = valid[i];
      assert(kb_http_request_validate(&request) == KB_HTTP_OK);
   }
   char raw_non_ascii[] = {'/', 'x', (char)0xc3, (char)0xa9, 0};
   const char *invalid[] = {"//x",  "/x\\y", "/x%",  "/x%0",  "/x%GG",      "/x%0g",
                            "/x?y", "/x#y",  "/x y", "/x\ty", raw_non_ascii};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
   {
      request.target = invalid[i];
      assert(kb_http_request_validate(&request) == KB_HTTP_INVALID_ARGUMENT);
   }
}

static void authenticated_tls_eof_policy(void)
{
   assert(kb_http_client_test__tls_eof_is_authenticated(SSL_ERROR_ZERO_RETURN));
   assert(!kb_http_client_test__tls_eof_is_authenticated(SSL_ERROR_SYSCALL));
   assert(!kb_http_client_test__tls_eof_is_authenticated(SSL_ERROR_SSL));
   assert(!kb_http_client_test__tls_eof_is_authenticated(SSL_ERROR_WANT_READ));
}

static void poll_readiness_precedes_hangup(void)
{
   int sockets[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
   assert(write(sockets[1], "x", 1) == 1);
   assert(close(sockets[1]) == 0);
   assert(kb_http_client_test__wait_fd(sockets[0], POLLIN, 1000) == KB_HTTP_OK);
   char byte = 0;
   assert(read(sockets[0], &byte, 1) == 1 && byte == 'x');
   assert(close(sockets[0]) == 0);
}

static void resolver_hang_releases_capacity(void)
{
   enum
   {
      HANG_RUNS = 20
   };
   for (size_t i = 0; i < HANG_RUNS; i++)
      assert(kb_http_client_test__resolve("localhost", 10, 1) == KB_HTTP_TIMEOUT);
   size_t high_water = 0;
   assert(kb_http_client_test__dns_wait_idle(1000, &high_water) == KB_HTTP_OK);
   assert(high_water > 0 && high_water <= 16);
   assert(kb_http_client_test__resolve("localhost", 2000, 0) == KB_HTTP_OK);
}

typedef struct
{
   int timeout_ms, hang, async_cancel;
   kb_http_result_t result;
} resolver_thread_t;

static void *resolver_thread_main(void *opaque)
{
   resolver_thread_t *thread = opaque;
   if (thread->async_cancel)
      assert(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) == 0);
   thread->result = kb_http_client_test__resolve("localhost", thread->timeout_ms, thread->hang);
   return NULL;
}

static void wait_for_dns_slots(size_t target)
{
   struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
   for (size_t attempt = 0; attempt < 2000; attempt++)
   {
      size_t used = 0, high_water = 0;
      assert(kb_http_client_test__dns_slots(&used, &high_water) == KB_HTTP_OK);
      if (used == target)
         return;
      nanosleep(&pause, NULL);
   }
   assert(!"resolver slot target was not reached");
}

static void resolver_cancellation_releases_exact_children(void)
{
   resolver_thread_t active = {.timeout_ms = 5000, .hang = 1, .async_cancel = 1};
   pthread_t active_thread;
   assert(pthread_create(&active_thread, NULL, resolver_thread_main, &active) == 0);
   wait_for_dns_slots(1);
   struct timespec helper_start = {.tv_sec = 0, .tv_nsec = 20000000L};
   nanosleep(&helper_start, NULL);
   assert(pthread_cancel(active_thread) == 0);
   void *joined = NULL;
   assert(pthread_join(active_thread, &joined) == 0 && joined == PTHREAD_CANCELED);
   size_t high_water = 0;
   assert(kb_http_client_test__dns_wait_idle(2000, &high_water) == KB_HTTP_OK);

   enum
   {
      CAP = 16
   };
   resolver_thread_t holders[CAP];
   pthread_t holder_threads[CAP];
   for (size_t i = 0; i < CAP; i++)
   {
      holders[i] = (resolver_thread_t){.timeout_ms = 5000, .hang = 1, .async_cancel = 1};
      assert(pthread_create(&holder_threads[i], NULL, resolver_thread_main, &holders[i]) == 0);
   }
   wait_for_dns_slots(CAP);
   resolver_thread_t waiter = {.timeout_ms = 5000, .hang = 0, .async_cancel = 1};
   pthread_t waiter_thread;
   assert(pthread_create(&waiter_thread, NULL, resolver_thread_main, &waiter) == 0);
   nanosleep(&helper_start, NULL);
   assert(pthread_cancel(waiter_thread) == 0);
   joined = NULL;
   assert(pthread_join(waiter_thread, &joined) == 0 && joined == PTHREAD_CANCELED);

   size_t used = 0;
   assert(kb_http_client_test__dns_slots(&used, &high_water) == KB_HTTP_OK && used == CAP);
   for (size_t i = 0; i < CAP; i++)
      assert(pthread_cancel(holder_threads[i]) == 0);
   for (size_t i = 0; i < CAP; i++)
   {
      joined = NULL;
      assert(pthread_join(holder_threads[i], &joined) == 0 && joined == PTHREAD_CANCELED);
   }
   assert(kb_http_client_test__dns_wait_idle(2000, &high_water) == KB_HTTP_OK);
   assert(high_water == CAP);
   assert(kb_http_client_test__resolve("localhost", 2000, 0) == KB_HTTP_OK);
}

static void resolver_rejects_non_https_port(void)
{
   unsigned char wire[17] = {0};
   kb_resolver_put_u32(wire, KB_RESOLVER_RESPONSE_MAGIC);
   wire[4] = KB_RESOLVER_VERSION;
   wire[5] = KB_RESOLVER_STATUS_OK;
   wire[6] = 1;
   wire[8] = 4;
   wire[9] = 4;
   wire[13] = 127;
   wire[16] = 1;
   kb_resolver_put_u16(wire + 10, 80);
   assert(kb_http_client_test__parse_resolver_response(wire, sizeof(wire)) ==
          KB_HTTP_RESOLVE_ERROR);
   kb_resolver_put_u16(wire + 10, 443);
   assert(kb_http_client_test__parse_resolver_response(wire, sizeof(wire)) == KB_HTTP_OK);
}

static volatile sig_atomic_t sigpipe_hits;

static void count_sigpipe(int signal_number)
{
   if (signal_number == SIGPIPE)
      sigpipe_hits++;
}

static void sigpipe_transport_isolation(void)
{
   struct sigaction action = {.sa_handler = count_sigpipe}, old_action;
   sigemptyset(&action.sa_mask);
   assert(sigaction(SIGPIPE, &action, &old_action) == 0);

   sigset_t pipe_set, caller_mask, active_mask, after_mask;
   sigemptyset(&pipe_set);
   sigaddset(&pipe_set, SIGPIPE);
   assert(pthread_sigmask(SIG_UNBLOCK, &pipe_set, &caller_mask) == 0);
   assert(pthread_sigmask(SIG_SETMASK, NULL, &active_mask) == 0);
   assert(!sigismember(&active_mask, SIGPIPE));

   int sockets[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
   /* SSL_get_fd is how the nonblocking handshake chooses its poll descriptor.
    * Custom socket BIOs must advertise BIO_TYPE_DESCRIPTOR for this to work. */
   assert(kb_http_client_test__nosigpipe_ssl_fd(sockets[0]) == sockets[0]);
   assert(close(sockets[1]) == 0);
   errno = 0;
   assert(kb_http_client_test__nosigpipe_bio_write(sockets[0]) < 0);
   assert(errno == EPIPE && sigpipe_hits == 0);
   assert(pthread_sigmask(SIG_SETMASK, NULL, &after_mask) == 0);
   assert(!sigismember(&after_mask, SIGPIPE));
   assert(close(sockets[0]) == 0);

   static const unsigned char wire[] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
   capture_t capture = {.gate = KB_HTTP_GATE_DELIVER, .raise_sigpipe = 1};
   assert(parse_parts(wire, sizeof(wire) - 1, sizeof(wire) - 1, 8, &capture) == KB_HTTP_OK);
   assert(sigpipe_hits == 1);

   assert(pthread_sigmask(SIG_BLOCK, &pipe_set, NULL) == 0);
   assert(raise(SIGPIPE) == 0);
   sigset_t pending;
   assert(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE));
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
   assert(close(sockets[1]) == 0);
   errno = 0;
   assert(kb_http_client_test__nosigpipe_bio_write(sockets[0]) < 0 && errno == EPIPE);
   assert(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE));
   assert(close(sockets[0]) == 0);
   struct timespec zero = {0};
   assert(sigtimedwait(&pipe_set, NULL, &zero) == SIGPIPE);
   assert(sigpending(&pending) == 0 && !sigismember(&pending, SIGPIPE));

   assert(pthread_sigmask(SIG_SETMASK, &caller_mask, NULL) == 0);
   assert(sigaction(SIGPIPE, &old_action, NULL) == 0);
}

static void callbacks_complete_synchronously(void)
{
   static const unsigned char wire[] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
   capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
   kb_http_response_parser_t *parser = NULL;
   assert(kb_http_response_parser_init(&parser, 8, capture_headers, capture_body, &capture) ==
          KB_HTTP_OK);
   assert(kb_http_response_parser_feed(parser, wire, sizeof(wire) - 1) == KB_HTTP_MORE);
   /* Both callbacks have returned before feed returns; no borrowed input escapes. */
   assert(capture.headers_called == 1 && capture.body_called == 1 && capture.body_len == 1);
   assert(kb_http_response_parser_finish_eof(parser) == KB_HTTP_OK);
   kb_http_response_parser_free(&parser);
}

int main(void)
{
   content_length_boundaries();
   chunked_boundaries();
   gate_and_abort();
   error_body_gate();
   post_completion_surplus();
   malformed_matrix();
   request_validation();
   origin_path_validation();
   authenticated_tls_eof_policy();
   poll_readiness_precedes_hangup();
   sigpipe_transport_isolation();
   callbacks_complete_synchronously();
   resolver_hang_releases_capacity();
   resolver_cancellation_releases_exact_children();
   resolver_rejects_non_https_port();
   puts("kb http client: strict parser and request validation passed");
   return 0;
}
