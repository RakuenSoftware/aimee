#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_http_client.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef enum
{
   PARSER_HEADERS = 0,
   PARSER_CONTENT_LENGTH,
   PARSER_CHUNK_SIZE,
   PARSER_CHUNK_DATA,
   PARSER_CHUNK_DATA_CR,
   PARSER_CHUNK_DATA_LF,
   PARSER_TRAILER_CR,
   PARSER_TRAILER_LF,
   PARSER_DONE,
   PARSER_FAILED,
   PARSER_ABORTED
} parser_state_t;

struct kb_http_response_parser
{
   parser_state_t state;
   kb_http_result_t terminal;
   size_t body_max, body_seen, remaining;
   unsigned char headers[KB_HTTP_HEADER_BLOCK_MAX + 1U];
   size_t headers_len;
   char chunk_line[KB_HTTP_CHUNK_LINE_MAX + 1U];
   size_t chunk_line_len;
   int chunk_saw_cr;
   kb_http_response_t response;
   kb_http_headers_fn headers_cb;
   kb_http_body_fn body_cb;
   void *context;
   kb_http_gate_t gate;
   int64_t deadline_ns; /* zero for pure parser use */
};

static int64_t now_ns(void);
static kb_http_result_t parser_fail(kb_http_response_parser_t *p, kb_http_result_t result);

static kb_http_result_t parser_deadline(kb_http_response_parser_t *p)
{
   if (p->deadline_ns)
   {
      int64_t now = now_ns();
      if (now < 0 || now >= p->deadline_ns)
         return parser_fail(p, KB_HTTP_TIMEOUT);
   }
   return KB_HTTP_MORE;
}

static int token_char(unsigned char c)
{
   if (c < 0x80 && isalnum(c))
      return 1;
   return strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int name_equal(const char *a, size_t an, const char *b)
{
   size_t bn = strlen(b);
   if (an != bn)
      return 0;
   for (size_t i = 0; i < an; i++)
      if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
         return 0;
   return 1;
}

static kb_http_result_t parser_fail(kb_http_response_parser_t *p, kb_http_result_t result)
{
   if (p->state == PARSER_ABORTED)
      return KB_HTTP_CALLBACK_ABORT;
   if (p->state == PARSER_FAILED)
      return p->terminal;
   p->state = result == KB_HTTP_CALLBACK_ABORT ? PARSER_ABORTED : PARSER_FAILED;
   p->terminal = result;
   return result;
}

static int parse_decimal(const char *s, size_t n, size_t *out)
{
   if (!n)
      return -1;
   size_t value = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (s[i] < '0' || s[i] > '9')
         return -1;
      unsigned digit = (unsigned)(s[i] - '0');
      if (value > (SIZE_MAX - digit) / 10U)
         return -1;
      value = value * 10U + digit;
   }
   *out = value;
   return 0;
}

static void trim_ows(const char **value, size_t *length)
{
   while (*length && (**value == ' ' || **value == '\t'))
   {
      (*value)++;
      (*length)--;
   }
   while (*length && ((*value)[*length - 1] == ' ' || (*value)[*length - 1] == '\t'))
      (*length)--;
}

static int parse_status_line(const char *line, size_t length, int *status)
{
   if (length < 13 || memcmp(line, "HTTP/1.1 ", 9) != 0 || !isdigit((unsigned char)line[9]) ||
       !isdigit((unsigned char)line[10]) || !isdigit((unsigned char)line[11]) || line[12] != ' ')
      return -1;
   int value = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
   if (value < 200 || value > 599 || (value >= 300 && value <= 399))
      return -1;
   for (size_t i = 13; i < length; i++)
      if (((unsigned char)line[i] < 0x20 && line[i] != '\t') || (unsigned char)line[i] == 0x7f)
         return -1;
   *status = value;
   return 0;
}

static kb_http_result_t parse_headers(kb_http_response_parser_t *p)
{
   char *block = (char *)p->headers;
   size_t at = 0, line_count = 0;
   int have_cl = 0, have_te = 0, have_ct = 0;
   while (at + 1 < p->headers_len)
   {
      size_t start = at;
      while (at + 1 < p->headers_len && !(block[at] == '\r' && block[at + 1] == '\n'))
         at++;
      if (at + 1 >= p->headers_len || at - start > KB_HTTP_HEADER_LINE_MAX)
         return parser_fail(p, at - start > KB_HTTP_HEADER_LINE_MAX ? KB_HTTP_TOO_LARGE
                                                                   : KB_HTTP_MALFORMED_RESPONSE);
      size_t length = at - start;
      at += 2;
      if (line_count++ == 0)
      {
         if (parse_status_line(block + start, length, &p->response.status) != 0)
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
         continue;
      }
      if (!length)
         break;
      if (line_count > KB_HTTP_HEADER_COUNT_MAX + 1U || block[start] == ' ' || block[start] == '\t')
         return parser_fail(p, line_count > KB_HTTP_HEADER_COUNT_MAX + 1U ? KB_HTTP_TOO_LARGE
                                                                         : KB_HTTP_MALFORMED_RESPONSE);
      size_t colon = 0;
      while (colon < length && block[start + colon] != ':')
      {
         if (!token_char((unsigned char)block[start + colon]))
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
         colon++;
      }
      if (!colon || colon == length)
         return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
      const char *value = block + start + colon + 1;
      size_t value_len = length - colon - 1;
      for (size_t i = 0; i < value_len; i++)
         if (((unsigned char)value[i] < 0x20 && value[i] != '\t') ||
             (unsigned char)value[i] == 0x7f)
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
      trim_ows(&value, &value_len);
      const char *name = block + start;
      if (name_equal(name, colon, "content-length"))
      {
         if (have_cl++ || parse_decimal(value, value_len, &p->response.content_length) != 0)
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
      }
      else if (name_equal(name, colon, "transfer-encoding"))
      {
         if (have_te++ || value_len != 7 || strncasecmp(value, "chunked", 7) != 0)
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
      }
      else if (name_equal(name, colon, "content-type"))
      {
         if (have_ct++ || !value_len || value_len >= sizeof(p->response.content_type))
            return parser_fail(p, have_ct > 1 || !value_len ? KB_HTTP_MALFORMED_RESPONSE
                                                            : KB_HTTP_TOO_LARGE);
         memcpy(p->response.content_type, value, value_len);
         p->response.content_type[value_len] = 0;
      }
   }
   if (at != p->headers_len || (have_cl && have_te) || (!have_cl && !have_te))
      return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
   if (have_cl)
   {
      if (p->response.content_length > p->body_max)
         return parser_fail(p, KB_HTTP_TOO_LARGE);
      p->response.framing = KB_HTTP_FRAMING_CONTENT_LENGTH;
      p->remaining = p->response.content_length;
      p->state = p->remaining ? PARSER_CONTENT_LENGTH : PARSER_DONE;
   }
   else
   {
      p->response.framing = KB_HTTP_FRAMING_CHUNKED;
      p->state = PARSER_CHUNK_SIZE;
   }
   if (parser_deadline(p) != KB_HTTP_MORE)
      return p->terminal;
   kb_http_gate_t gate = p->headers_cb(&p->response, p->context);
   if (gate != KB_HTTP_GATE_DELIVER && gate != KB_HTTP_GATE_DISCARD &&
       gate != KB_HTTP_GATE_ABORT)
      return parser_fail(p, KB_HTTP_CALLBACK_ABORT);
   if (gate == KB_HTTP_GATE_ABORT)
      return parser_fail(p, KB_HTTP_CALLBACK_ABORT);
   if (parser_deadline(p) != KB_HTTP_MORE)
      return p->terminal;
   p->gate = p->response.status >= 200 && p->response.status <= 299 ? gate
                                                                    : KB_HTTP_GATE_DISCARD;
   return KB_HTTP_MORE;
}

kb_http_result_t kb_http_response_parser_init(kb_http_response_parser_t **out, size_t body_max,
                                              kb_http_headers_fn headers_cb,
                                              kb_http_body_fn body_cb, void *context)
{
   if (!out || *out || !body_max || body_max > KB_HTTP_BODY_MAX || !headers_cb || !body_cb)
      return KB_HTTP_INVALID_ARGUMENT;
   kb_http_response_parser_t *p = calloc(1, sizeof(*p));
   if (!p)
      return KB_HTTP_INTERNAL_ERROR;
   p->body_max = body_max;
   p->headers_cb = headers_cb;
   p->body_cb = body_cb;
   p->context = context;
   p->state = PARSER_HEADERS;
   *out = p;
   return KB_HTTP_OK;
}

static kb_http_result_t deliver(kb_http_response_parser_t *p, const unsigned char *bytes,
                                size_t length)
{
   if (!length || p->gate == KB_HTTP_GATE_DISCARD)
      return KB_HTTP_MORE;
   if (parser_deadline(p) != KB_HTTP_MORE)
      return p->terminal;
   kb_http_body_action_t action = p->body_cb(bytes, length, p->context);
   if (action == KB_HTTP_BODY_CALLER_ABORT)
      return parser_fail(p, KB_HTTP_CALLBACK_ABORT);
   if (action != KB_HTTP_BODY_CONTINUE)
      return parser_fail(p, KB_HTTP_INTERNAL_ERROR);
   if (parser_deadline(p) != KB_HTTP_MORE)
      return p->terminal;
   return KB_HTTP_MORE;
}

static kb_http_result_t parse_chunk_size(kb_http_response_parser_t *p)
{
   if (!p->chunk_line_len)
      return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
   size_t value = 0;
   for (size_t i = 0; i < p->chunk_line_len; i++)
   {
      unsigned char c = (unsigned char)p->chunk_line[i];
      unsigned digit;
      if (c >= '0' && c <= '9')
         digit = c - '0';
      else if (c >= 'a' && c <= 'f')
         digit = c - 'a' + 10U;
      else if (c >= 'A' && c <= 'F')
         digit = c - 'A' + 10U;
      else
         return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE); /* extensions/OWS forbidden */
      if (value > (SIZE_MAX - digit) / 16U)
         return parser_fail(p, KB_HTTP_TOO_LARGE);
      value = value * 16U + digit;
   }
   p->chunk_line_len = 0;
   p->chunk_saw_cr = 0;
   if (!value)
   {
      p->state = PARSER_TRAILER_CR;
      return KB_HTTP_MORE;
   }
   if (value > p->body_max - p->body_seen)
      return parser_fail(p, KB_HTTP_TOO_LARGE);
   p->remaining = value;
   p->state = PARSER_CHUNK_DATA;
   return KB_HTTP_MORE;
}

kb_http_result_t kb_http_response_parser_feed(kb_http_response_parser_t *p,
                                              const unsigned char *bytes, size_t length)
{
   if (!p || (!bytes && length))
      return KB_HTTP_INVALID_ARGUMENT;
   if (p->state == PARSER_ABORTED || p->state == PARSER_FAILED)
      return p->terminal;
   if (p->state == PARSER_DONE && length)
      return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
   size_t at = 0;
   while (at < length)
   {
      if (p->state == PARSER_HEADERS)
      {
         unsigned char c = bytes[at++];
         if (p->headers_len == KB_HTTP_HEADER_BLOCK_MAX)
            return parser_fail(p, KB_HTTP_TOO_LARGE);
         if (c == '\n' && (!p->headers_len || p->headers[p->headers_len - 1] != '\r'))
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
         p->headers[p->headers_len++] = c;
         if (p->headers_len >= 4 &&
             memcmp(p->headers + p->headers_len - 4, "\r\n\r\n", 4) == 0)
         {
            kb_http_result_t result = parse_headers(p);
            if (result != KB_HTTP_MORE)
               return result;
         }
         continue;
      }
      if (p->state == PARSER_CONTENT_LENGTH || p->state == PARSER_CHUNK_DATA)
      {
         size_t take = length - at;
         if (take > p->remaining)
            take = p->remaining;
         kb_http_result_t result = deliver(p, bytes + at, take);
         if (result != KB_HTTP_MORE)
            return result;
         at += take;
         p->remaining -= take;
         p->body_seen += take;
         if (!p->remaining)
            p->state = p->response.framing == KB_HTTP_FRAMING_CONTENT_LENGTH
                           ? PARSER_DONE
                           : PARSER_CHUNK_DATA_CR;
         continue;
      }
      if (p->state == PARSER_CHUNK_SIZE)
      {
         unsigned char c = bytes[at++];
         if (p->chunk_saw_cr)
         {
            if (c != '\n')
               return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
            kb_http_result_t result = parse_chunk_size(p);
            if (result != KB_HTTP_MORE)
               return result;
         }
         else if (c == '\r')
            p->chunk_saw_cr = 1;
         else if (c == '\n')
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
         else
         {
            if (p->chunk_line_len == KB_HTTP_CHUNK_LINE_MAX)
               return parser_fail(p, KB_HTTP_TOO_LARGE);
            p->chunk_line[p->chunk_line_len++] = (char)c;
         }
         continue;
      }
      unsigned char c = bytes[at++];
      if (p->state == PARSER_CHUNK_DATA_CR)
      {
         if (c != '\r')
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
         p->state = PARSER_CHUNK_DATA_LF;
      }
      else if (p->state == PARSER_CHUNK_DATA_LF)
      {
         if (c != '\n')
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
         p->state = PARSER_CHUNK_SIZE;
      }
      else if (p->state == PARSER_TRAILER_CR)
      {
         if (c != '\r')
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE); /* nonempty trailers forbidden */
         p->state = PARSER_TRAILER_LF;
      }
      else if (p->state == PARSER_TRAILER_LF)
      {
         if (c != '\n')
            return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
         p->state = PARSER_DONE;
      }
      else
         return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
   }
   return KB_HTTP_MORE;
}

kb_http_result_t kb_http_response_parser_finish_eof(kb_http_response_parser_t *p)
{
   if (!p)
      return KB_HTTP_INVALID_ARGUMENT;
   if (p->state == PARSER_ABORTED || p->state == PARSER_FAILED)
      return p->terminal;
   if (p->state != PARSER_DONE)
      return parser_fail(p, KB_HTTP_MALFORMED_RESPONSE);
   return KB_HTTP_OK;
}

void kb_http_response_parser_free(kb_http_response_parser_t **parser)
{
   if (!parser || !*parser)
      return;
   OPENSSL_cleanse(*parser, sizeof(**parser));
   free(*parser);
   *parser = NULL;
}

static int64_t now_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return -1;
   return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int deadline_poll_ms(int64_t deadline)
{
   int64_t now = now_ns();
   if (now < 0 || now >= deadline)
      return 0;
   int64_t ns = deadline - now;
   int64_t ms = (ns + 999999LL) / 1000000LL;
   return ms > INT_MAX ? INT_MAX : (int)ms;
}

static int deadline_expired(int64_t deadline)
{
   int64_t now = now_ns();
   return now < 0 || now >= deadline;
}

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   atomic_int refs;
   int done, abandoned;
   struct gaicb request;
   struct addrinfo hints;
   char host[256];
   char service[16];
} dns_query_t;

static void dns_query_release(dns_query_t *q)
{
   if (atomic_fetch_sub(&q->refs, 1) == 1)
   {
      pthread_cond_destroy(&q->cond);
      pthread_mutex_destroy(&q->mutex);
      free(q);
   }
}

static void dns_complete(union sigval value)
{
   dns_query_t *q = value.sival_ptr;
   pthread_mutex_lock(&q->mutex);
   q->done = 1;
   int abandoned = q->abandoned;
   pthread_cond_signal(&q->cond);
   pthread_mutex_unlock(&q->mutex);
   if (abandoned && q->request.ar_result)
   {
      freeaddrinfo(q->request.ar_result);
      q->request.ar_result = NULL;
   }
   dns_query_release(q);
}

static kb_http_result_t resolve_deadline(const char *host, const char *service, int64_t deadline,
                                         struct addrinfo **result)
{
   *result = NULL;
   dns_query_t *q = calloc(1, sizeof(*q));
   if (!q)
      return KB_HTTP_INTERNAL_ERROR;
   if (strnlen(host, sizeof(q->host)) == sizeof(q->host) ||
       strnlen(service, sizeof(q->service)) == sizeof(q->service) ||
       pthread_mutex_init(&q->mutex, NULL) != 0)
   {
      free(q);
      return KB_HTTP_INTERNAL_ERROR;
   }
   pthread_condattr_t attr;
   if (pthread_condattr_init(&attr) != 0)
   {
      pthread_mutex_destroy(&q->mutex);
      free(q);
      return KB_HTTP_INTERNAL_ERROR;
   }
   if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
       pthread_cond_init(&q->cond, &attr) != 0)
   {
      pthread_condattr_destroy(&attr);
      pthread_mutex_destroy(&q->mutex);
      free(q);
      return KB_HTTP_INTERNAL_ERROR;
   }
   pthread_condattr_destroy(&attr);
   atomic_init(&q->refs, 2);
   snprintf(q->host, sizeof(q->host), "%s", host);
   snprintf(q->service, sizeof(q->service), "%s", service);
   q->hints.ai_family = AF_UNSPEC;
   q->hints.ai_socktype = SOCK_STREAM;
   q->hints.ai_protocol = IPPROTO_TCP;
   q->request.ar_name = q->host;
   q->request.ar_service = q->service;
   q->request.ar_request = &q->hints;
   struct gaicb *requests[] = {&q->request};
   struct sigevent event = {.sigev_notify = SIGEV_THREAD,
                            .sigev_value.sival_ptr = q,
                            .sigev_notify_function = dns_complete};
   if (getaddrinfo_a(GAI_NOWAIT, requests, 1, &event) != 0)
   {
      atomic_store(&q->refs, 1);
      dns_query_release(q);
      return KB_HTTP_RESOLVE_ERROR;
   }
   struct timespec until = {.tv_sec = deadline / 1000000000LL,
                            .tv_nsec = deadline % 1000000000LL};
   pthread_mutex_lock(&q->mutex);
   int wait_result = 0;
   while (!q->done && wait_result == 0)
      wait_result = pthread_cond_timedwait(&q->cond, &q->mutex, &until);
   if (!q->done)
   {
      q->abandoned = 1;
      pthread_mutex_unlock(&q->mutex);
      (void)gai_cancel(&q->request);
      dns_query_release(q);
      return KB_HTTP_TIMEOUT;
   }
   pthread_mutex_unlock(&q->mutex);
   int gai = gai_error(&q->request);
   if (gai == 0)
   {
      *result = q->request.ar_result;
      q->request.ar_result = NULL;
   }
   dns_query_release(q);
   return gai == 0 && *result ? KB_HTTP_OK : KB_HTTP_RESOLVE_ERROR;
}

static kb_http_result_t wait_fd(int fd, short events, int64_t deadline)
{
   for (;;)
   {
      int timeout = deadline_poll_ms(deadline);
      if (!timeout)
         return KB_HTTP_TIMEOUT;
      struct pollfd pfd = {.fd = fd, .events = events};
      int rc = poll(&pfd, 1, timeout);
      if (rc > 0)
      {
         if (deadline_expired(deadline))
            return KB_HTTP_TIMEOUT;
         return (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) ? KB_HTTP_IO_ERROR : KB_HTTP_OK;
      }
      if (rc == 0)
         return KB_HTTP_TIMEOUT;
      if (errno != EINTR)
         return KB_HTTP_IO_ERROR;
   }
}

static kb_http_result_t connect_deadline(struct addrinfo *addresses, int64_t deadline, int *out_fd)
{
   *out_fd = -1;
   kb_http_result_t last = KB_HTTP_CONNECT_ERROR;
   for (struct addrinfo *a = addresses; a; a = a->ai_next)
   {
      if (!deadline_poll_ms(deadline))
         return KB_HTTP_TIMEOUT;
      int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
      if (fd < 0)
         continue;
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
      {
         close(fd);
         continue;
      }
      int rc = connect(fd, a->ai_addr, a->ai_addrlen);
      if (rc != 0 && errno == EINPROGRESS)
      {
         last = wait_fd(fd, POLLOUT, deadline);
         int error = 0;
         socklen_t error_len = sizeof(error);
         if (last == KB_HTTP_OK &&
             (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) != 0 || error))
            last = KB_HTTP_CONNECT_ERROR;
      }
      else if (rc == 0)
         last = KB_HTTP_OK;
      else
         last = KB_HTTP_CONNECT_ERROR;
      if (last == KB_HTTP_OK)
      {
         *out_fd = fd;
         return KB_HTTP_OK;
      }
      close(fd);
      if (last == KB_HTTP_TIMEOUT)
         return last;
   }
   return last;
}

static kb_http_result_t ssl_wait(SSL *ssl, int rc, int64_t deadline)
{
   int error = SSL_get_error(ssl, rc);
   if (error == SSL_ERROR_WANT_READ)
      return wait_fd(SSL_get_fd(ssl), POLLIN, deadline);
   if (error == SSL_ERROR_WANT_WRITE)
      return wait_fd(SSL_get_fd(ssl), POLLOUT, deadline);
   return KB_HTTP_TLS_ERROR;
}

static kb_http_result_t ssl_handshake(SSL *ssl, int64_t deadline)
{
   for (;;)
   {
      if (deadline_expired(deadline))
         return KB_HTTP_TIMEOUT;
      int rc = SSL_connect(ssl);
      if (deadline_expired(deadline))
         return KB_HTTP_TIMEOUT;
      if (rc == 1)
         return KB_HTTP_OK;
      kb_http_result_t wait = ssl_wait(ssl, rc, deadline);
      if (wait != KB_HTTP_OK)
         return wait == KB_HTTP_TIMEOUT ? wait : KB_HTTP_TLS_ERROR;
   }
}

static kb_http_result_t ssl_write_all(SSL *ssl, const unsigned char *bytes, size_t length,
                                      int64_t deadline)
{
   size_t offset = 0;
   while (offset < length)
   {
      if (deadline_expired(deadline))
         return KB_HTTP_TIMEOUT;
      size_t written = 0;
      int rc = SSL_write_ex(ssl, bytes + offset, length - offset, &written);
      if (deadline_expired(deadline))
         return KB_HTTP_TIMEOUT;
      if (rc == 1)
      {
         if (!written)
            return KB_HTTP_IO_ERROR;
         offset += written;
         continue;
      }
      kb_http_result_t wait = ssl_wait(ssl, rc, deadline);
      if (wait != KB_HTTP_OK)
         return wait == KB_HTTP_TIMEOUT ? wait : KB_HTTP_IO_ERROR;
   }
   return KB_HTTP_OK;
}

static int authority_valid(const char *host)
{
   size_t n = host ? strnlen(host, 256) : 0;
   if (!n || n == 256 || host[0] == '.' || host[n - 1] == '.')
      return 0;
   for (size_t i = 0; i < n; i++)
      if ((unsigned char)host[i] >= 0x80 ||
          !(isalnum((unsigned char)host[i]) || host[i] == '-' || host[i] == '.'))
         return 0;
   return 1;
}

static int hex_digit(unsigned char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int origin_path_valid(const char *target)
{
   size_t length = target ? strnlen(target, 4096) : 0;
   if (!length || length == 4096 || target[0] != '/' || target[1] == '/')
      return 0;
   for (size_t i = 1; i < length; i++)
   {
      unsigned char c = (unsigned char)target[i];
      if (c == '%')
      {
         if (i + 2 >= length || !hex_digit((unsigned char)target[i + 1]) ||
             !hex_digit((unsigned char)target[i + 2]))
            return 0;
         i += 2;
         continue;
      }
      if (c < 0x80 && (isalnum(c) || strchr("/-._~!$&'()*+,;=:@", c)))
         continue;
      return 0; /* includes backslash, raw non-ASCII, query, fragment, and controls */
   }
   return 1;
}

kb_http_result_t kb_http_request_validate(const kb_http_request_t *r)
{
   if (!r || !authority_valid(r->authority) || r->connect_timeout_ms <= 0 ||
       r->total_timeout_ms <= 0 ||
       strcmp(r->method ? r->method : "", "POST") != 0 || !origin_path_valid(r->target) ||
       !r->headers || !r->header_count ||
       r->header_count > KB_HTTP_REQUEST_HEADERS_MAX || (!r->body && r->body_len) ||
       r->body_len > KB_HTTP_BODY_MAX || !r->response_body_max ||
       r->response_body_max > KB_HTTP_BODY_MAX)
      return KB_HTTP_INVALID_ARGUMENT;
   int host_count = 0;
   for (size_t i = 0; i < r->header_count; i++)
   {
      const char *name = r->headers[i].name, *value = r->headers[i].value;
      size_t nn = name ? strnlen(name, 128) : 0;
      size_t vn = value ? strnlen(value, 16384) : 0;
      if (!nn || nn == 128 || !value || vn == 16384 || !vn || value[0] == ' ' ||
          value[vn - 1] == ' ')
         return KB_HTTP_INVALID_ARGUMENT;
      for (size_t j = 0; j < nn; j++)
         if (!token_char((unsigned char)name[j]))
            return KB_HTTP_INVALID_ARGUMENT;
      for (size_t j = 0; j < vn; j++)
         if ((unsigned char)value[j] < 0x20 || (unsigned char)value[j] == 0x7f)
            return KB_HTTP_INVALID_ARGUMENT;
      if (strcasecmp(name, "content-length") == 0 || strcasecmp(name, "transfer-encoding") == 0 ||
          strcasecmp(name, "connection") == 0)
         return KB_HTTP_INVALID_ARGUMENT;
      if (strcasecmp(name, "host") == 0)
      {
         host_count++;
         if (strcmp(value, r->authority) != 0)
            return KB_HTTP_INVALID_ARGUMENT;
      }
      for (size_t j = 0; j < i; j++)
         if (strcasecmp(name, r->headers[j].name) == 0)
            return KB_HTTP_INVALID_ARGUMENT;
   }
   return host_count == 1 ? KB_HTTP_OK : KB_HTTP_INVALID_ARGUMENT;
}

static SSL_CTX *client_context(void)
{
   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   if (!ctx)
      return NULL;
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
   if (SSL_CTX_set_default_verify_paths(ctx) != 1)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   return ctx;
}

static kb_http_result_t build_request_bytes(const kb_http_request_t *r, unsigned char **out,
                                            size_t *out_len)
{
   size_t cap = strlen(r->method) + strlen(r->target) + 64U;
   for (size_t i = 0; i < r->header_count; i++)
   {
      size_t add = strlen(r->headers[i].name) + strlen(r->headers[i].value) + 4U;
      if (cap > SIZE_MAX - add)
         return KB_HTTP_TOO_LARGE;
      cap += add;
   }
   if (cap > SIZE_MAX - 64U)
      return KB_HTTP_TOO_LARGE;
   cap += 64U;
   unsigned char *buffer = malloc(cap);
   if (!buffer)
      return KB_HTTP_INTERNAL_ERROR;
   int n = snprintf((char *)buffer, cap, "%s %s HTTP/1.1\r\n", r->method, r->target);
   size_t at = n > 0 && (size_t)n < cap ? (size_t)n : cap;
   for (size_t i = 0; i < r->header_count && at < cap; i++)
   {
      n = snprintf((char *)buffer + at, cap - at, "%s: %s\r\n", r->headers[i].name,
                   r->headers[i].value);
      if (n <= 0 || (size_t)n >= cap - at)
         at = cap;
      else
         at += (size_t)n;
   }
   if (at < cap)
   {
      n = snprintf((char *)buffer + at, cap - at,
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n", r->body_len);
      if (n <= 0 || (size_t)n >= cap - at)
         at = cap;
      else
         at += (size_t)n;
   }
   if (at == cap)
   {
      free(buffer);
      return KB_HTTP_INTERNAL_ERROR;
   }
   *out = buffer;
   *out_len = at;
   return KB_HTTP_OK;
}

kb_http_result_t kb_http_tls_exchange(const kb_http_request_t *request,
                                     kb_http_response_t *response,
                                     kb_http_headers_fn headers_cb, kb_http_body_fn body_cb,
                                     void *context)
{
   if (response)
      memset(response, 0, sizeof(*response));
   if (!response || !headers_cb || !body_cb || kb_http_request_validate(request) != KB_HTTP_OK)
      return KB_HTTP_INVALID_ARGUMENT;
   int64_t now = now_ns();
   /* total_timeout_ms is a positive int, so conversion to nanoseconds fits int64_t. */
   if (now < 0)
      return KB_HTTP_INVALID_ARGUMENT;
   int64_t total_ns = (int64_t)request->total_timeout_ms * 1000000LL;
   if (now > INT64_MAX - total_ns)
      return KB_HTTP_INVALID_ARGUMENT;
   int64_t deadline = now + total_ns;
   int connect_ms = request->connect_timeout_ms < request->total_timeout_ms
                        ? request->connect_timeout_ms
                        : request->total_timeout_ms;
   int64_t connect_deadline_ns = now + (int64_t)connect_ms * 1000000LL;
   sigset_t pipe_set, old_mask, initially_pending;
   sigemptyset(&pipe_set);
   sigaddset(&pipe_set, SIGPIPE);
   sigemptyset(&initially_pending);
   int pipe_blocked = pthread_sigmask(SIG_BLOCK, &pipe_set, &old_mask) == 0;
   if (pipe_blocked)
      (void)sigpending(&initially_pending);
   char service[16];
   snprintf(service, sizeof(service), "%d", 443);
   struct addrinfo *addresses = NULL;
   kb_http_result_t result =
       resolve_deadline(request->authority, service, connect_deadline_ns, &addresses);
   int fd = -1;
   SSL_CTX *ctx = NULL;
   SSL *ssl = NULL;
   unsigned char *request_bytes = NULL;
   size_t request_bytes_len = 0;
   kb_http_response_parser_t *parser = NULL;
   if (result != KB_HTTP_OK)
      goto done;
   result = connect_deadline(addresses, connect_deadline_ns, &fd);
   if (result != KB_HTTP_OK)
      goto done;
   ctx = client_context();
   ssl = ctx ? SSL_new(ctx) : NULL;
   if (!ssl || SSL_set_fd(ssl, fd) != 1 ||
       SSL_set_tlsext_host_name(ssl, request->authority) != 1 ||
       SSL_set1_host(ssl, request->authority) != 1)
   {
      result = KB_HTTP_TLS_ERROR;
      goto done;
   }
   result = ssl_handshake(ssl, deadline);
   if (result != KB_HTTP_OK)
      goto done;
   result = build_request_bytes(request, &request_bytes, &request_bytes_len);
   if (result != KB_HTTP_OK)
      goto done;
   result = ssl_write_all(ssl, request_bytes, request_bytes_len, deadline);
   if (result == KB_HTTP_OK && request->body_len)
      result = ssl_write_all(ssl, request->body, request->body_len, deadline);
   if (result != KB_HTTP_OK)
      goto done;
   result = kb_http_response_parser_init(&parser, request->response_body_max, headers_cb, body_cb,
                                         context);
   if (result != KB_HTTP_OK)
      goto done;
   parser->deadline_ns = deadline;
   for (;;)
   {
      unsigned char buffer[16384];
      size_t got = 0;
      if (deadline_expired(deadline))
      {
         result = KB_HTTP_TIMEOUT;
         break;
      }
      int rc = SSL_read_ex(ssl, buffer, sizeof(buffer), &got);
      if (deadline_expired(deadline))
      {
         if (rc == 1 && got)
            OPENSSL_cleanse(buffer, got);
         result = KB_HTTP_TIMEOUT;
         break;
      }
      if (rc == 1)
      {
         if (!got)
         {
            result = KB_HTTP_IO_ERROR;
            break;
         }
         if (deadline_expired(deadline))
            result = KB_HTTP_TIMEOUT;
         else
            result = kb_http_response_parser_feed(parser, buffer, got);
         OPENSSL_cleanse(buffer, got);
         if (result == KB_HTTP_MORE && deadline_expired(deadline))
            result = KB_HTTP_TIMEOUT;
         if (result != KB_HTTP_MORE)
            break;
         continue;
      }
      int error = SSL_get_error(ssl, rc);
      if (error == SSL_ERROR_ZERO_RETURN || (error == SSL_ERROR_SYSCALL && rc == 0))
      {
         result = deadline_expired(deadline) ? KB_HTTP_TIMEOUT
                                             : kb_http_response_parser_finish_eof(parser);
         break;
      }
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
      {
         result = wait_fd(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline);
         if (result == KB_HTTP_OK)
            continue;
         break;
      }
      result = KB_HTTP_IO_ERROR;
      break;
   }
   if (result == KB_HTTP_OK)
      *response = parser->response;

done:
   if (result != KB_HTTP_OK)
      memset(response, 0, sizeof(*response));
   kb_http_response_parser_free(&parser);
   if (request_bytes)
   {
      OPENSSL_cleanse(request_bytes, request_bytes_len);
      free(request_bytes);
   }
   freeaddrinfo(addresses);
   if (ssl)
   {
      if (deadline_poll_ms(deadline))
         (void)SSL_shutdown(ssl);
      SSL_free(ssl);
   }
   SSL_CTX_free(ctx);
   if (fd >= 0)
      close(fd);
   if (pipe_blocked)
   {
      sigset_t pending;
      if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) &&
          !sigismember(&initially_pending, SIGPIPE))
      {
         struct timespec zero = {0};
         (void)sigtimedwait(&pipe_set, NULL, &zero);
      }
      (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
   }
   return result;
}
