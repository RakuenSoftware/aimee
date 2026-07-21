#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_http_client.h"
#include "kb_http_resolver_protocol.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <openssl/crypto.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
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
static kb_http_result_t wait_fd(int fd, short events, int64_t deadline);

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
         return parser_fail(p, line_count > KB_HTTP_HEADER_COUNT_MAX + 1U
                                   ? KB_HTTP_TOO_LARGE
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
   if (gate != KB_HTTP_GATE_DELIVER && gate != KB_HTTP_GATE_DISCARD && gate != KB_HTTP_GATE_ABORT)
      return parser_fail(p, KB_HTTP_CALLBACK_ABORT);
   if (gate == KB_HTTP_GATE_ABORT)
      return parser_fail(p, KB_HTTP_CALLBACK_ABORT);
   if (parser_deadline(p) != KB_HTTP_MORE)
      return p->terminal;
   p->gate = p->response.status >= 200 && p->response.status <= 299 ? gate : KB_HTTP_GATE_DISCARD;
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
         if (p->headers_len >= 4 && memcmp(p->headers + p->headers_len - 4, "\r\n\r\n", 4) == 0)
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
            p->state = p->response.framing == KB_HTTP_FRAMING_CONTENT_LENGTH ? PARSER_DONE
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

/* Resolver helpers are capped process-wide. A slot is held until the exact
 * child is reaped, including after deadline-driven SIGKILL. */
#define DNS_QUERY_PROCESS_CAP 16U
static pthread_mutex_t dns_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dns_slots_cond;
static pthread_cond_t dns_reap_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t dns_pool_once = PTHREAD_ONCE_INIT;
static size_t dns_slots_used;
static size_t dns_slots_high_water;
static pid_t dns_reap_queue[DNS_QUERY_PROCESS_CAP];
static size_t dns_reap_head, dns_reap_count;
static int dns_pool_ready;

typedef struct
{
   int request_pipe[2];
   int response_pipe[2];
   pid_t pid;
   int slot_owned;
} resolver_cleanup_t;

static void resolver_cleanup(void *opaque);

static void dns_slot_release(void)
{
   pthread_mutex_lock(&dns_pool_mutex);
   if (dns_slots_used)
      dns_slots_used--;
   pthread_cond_broadcast(&dns_slots_cond);
   pthread_mutex_unlock(&dns_pool_mutex);
}

static void *dns_reaper_main(void *unused)
{
   (void)unused;
   for (;;)
   {
      pthread_mutex_lock(&dns_pool_mutex);
      while (!dns_reap_count)
         pthread_cond_wait(&dns_reap_cond, &dns_pool_mutex);
      pid_t pid = dns_reap_queue[dns_reap_head];
      dns_reap_head = (dns_reap_head + 1U) % DNS_QUERY_PROCESS_CAP;
      dns_reap_count--;
      pthread_mutex_unlock(&dns_pool_mutex);

      while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
         ;
      dns_slot_release();
   }
   return NULL;
}

/* The caller has already killed pid and transfers both exact-child ownership and
 * its charged process slot. Queue capacity cannot be exhausted: every queued or
 * reaping child still owns one of the DNS_QUERY_PROCESS_CAP slots. */
static void dns_reap_enqueue(pid_t pid)
{
   pthread_mutex_lock(&dns_pool_mutex);
   size_t tail = (dns_reap_head + dns_reap_count) % DNS_QUERY_PROCESS_CAP;
   dns_reap_queue[tail] = pid;
   dns_reap_count++;
   pthread_cond_signal(&dns_reap_cond);
   pthread_mutex_unlock(&dns_pool_mutex);
}

static void dns_pool_init(void)
{
   pthread_condattr_t cond_attr;
   if (pthread_condattr_init(&cond_attr) != 0)
      return;
   if (pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) != 0 ||
       pthread_cond_init(&dns_slots_cond, &cond_attr) != 0)
   {
      pthread_condattr_destroy(&cond_attr);
      return;
   }
   pthread_condattr_destroy(&cond_attr);
   pthread_attr_t thread_attr;
   if (pthread_attr_init(&thread_attr) != 0)
      return;
   int thread_error = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
   size_t reaper_stack = 128U * 1024U;
   if (reaper_stack < (size_t)PTHREAD_STACK_MIN)
      reaper_stack = (size_t)PTHREAD_STACK_MIN;
   if (!thread_error)
      thread_error = pthread_attr_setstacksize(&thread_attr, reaper_stack);
   for (size_t i = 0; !thread_error && i < DNS_QUERY_PROCESS_CAP; i++)
   {
      pthread_t reaper;
      thread_error = pthread_create(&reaper, &thread_attr, dns_reaper_main, NULL);
   }
   pthread_attr_destroy(&thread_attr);
   if (thread_error)
      return;
   dns_pool_ready = 1;
}

static void mutex_unlock_cleanup(void *mutex)
{
   pthread_mutex_unlock(mutex);
}

static kb_http_result_t dns_slot_acquire(int64_t deadline, int *slot_owned)
{
   if (pthread_once(&dns_pool_once, dns_pool_init) != 0 || !dns_pool_ready)
      return KB_HTTP_INTERNAL_ERROR;
   struct timespec until = {.tv_sec = deadline / 1000000000LL, .tv_nsec = deadline % 1000000000LL};
   int entry_cancel_state;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &entry_cancel_state) != 0)
      return KB_HTTP_INTERNAL_ERROR;
   if (pthread_mutex_lock(&dns_pool_mutex) != 0)
   {
      (void)pthread_setcancelstate(entry_cancel_state, NULL);
      return KB_HTTP_INTERNAL_ERROR;
   }
   kb_http_result_t result = KB_HTTP_OK;
   pthread_cleanup_push(mutex_unlock_cleanup, &dns_pool_mutex);
   (void)pthread_setcancelstate(entry_cancel_state, NULL);
   int wait_result = 0;
   while (dns_slots_used >= DNS_QUERY_PROCESS_CAP && wait_result == 0)
      wait_result = pthread_cond_timedwait(&dns_slots_cond, &dns_pool_mutex, &until);
   if (deadline_expired(deadline))
      result = KB_HTTP_TIMEOUT;
   else if (dns_slots_used >= DNS_QUERY_PROCESS_CAP)
      result = wait_result == ETIMEDOUT || deadline_expired(deadline) ? KB_HTTP_TIMEOUT
                                                                      : KB_HTTP_INTERNAL_ERROR;
   else
   {
      int mutation_cancel_state;
      (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &mutation_cancel_state);
      dns_slots_used++;
      *slot_owned = 1;
      if (dns_slots_used > dns_slots_high_water)
         dns_slots_high_water = dns_slots_used;
      entry_cancel_state = mutation_cancel_state;
   }
   (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
   pthread_cleanup_pop(1);
   (void)pthread_setcancelstate(entry_cancel_state, NULL);
   return result;
}

static void resolved_addresses_free(struct addrinfo *addresses)
{
   while (addresses)
   {
      struct addrinfo *next = addresses->ai_next;
      free(addresses->ai_addr);
      free(addresses);
      addresses = next;
   }
}

static int resolver_helper_path(char path[PATH_MAX], const char *override)
{
   if (override)
   {
      size_t length = strnlen(override, PATH_MAX);
      if (!length || length == PATH_MAX || override[0] != '/')
         return -1;
      memcpy(path, override, length + 1U);
      return 0;
   }
   ssize_t length = readlink("/proc/self/exe", path, PATH_MAX - 1U);
   if (length <= 0 || length >= PATH_MAX)
      return -1;
   path[length] = 0;
   char *slash = strrchr(path, '/');
   static const char helper[] = "aimee-kb-resolver";
   if (!slash || (size_t)(slash - path) + sizeof(helper) >= PATH_MAX)
      return -1;
   memcpy(slash + 1, helper, sizeof(helper));
   return 0;
}

static int fd_nonblocking(int fd)
{
   int flags = fcntl(fd, F_GETFL, 0);
   return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}

static int pipe_cloexec(int fds[2])
{
   int raw[2];
   if (pipe2(raw, O_CLOEXEC) != 0)
      return -1;
   for (size_t i = 0; i < 2; i++)
   {
      if (raw[i] > STDERR_FILENO)
         fds[i] = raw[i];
      else
      {
         fds[i] = fcntl(raw[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
         close(raw[i]);
         if (fds[i] < 0)
         {
            if (i)
               close(fds[0]);
            return -1;
         }
      }
   }
   return 0;
}

static int socketpair_cloexec(int fds[2])
{
   int raw[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, raw) != 0)
      return -1;
   for (size_t i = 0; i < 2; i++)
   {
      if (raw[i] > STDERR_FILENO)
         fds[i] = raw[i];
      else
      {
         fds[i] = fcntl(raw[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
         close(raw[i]);
         if (fds[i] < 0)
         {
            if (i)
               close(fds[0]);
            return -1;
         }
      }
   }
   return 0;
}

static kb_http_result_t socket_write_deadline(int fd, const unsigned char *bytes, size_t length,
                                              int64_t deadline)
{
   size_t at = 0;
   while (at < length)
   {
      if (deadline_expired(deadline))
         return KB_HTTP_TIMEOUT;
      ssize_t n = send(fd, bytes + at, length - at, MSG_NOSIGNAL);
      if (n > 0)
         at += (size_t)n;
      else if (n < 0 && errno == EINTR)
         continue;
      else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
         kb_http_result_t wait = wait_fd(fd, POLLOUT, deadline);
         if (wait != KB_HTTP_OK)
            return wait;
      }
      else
         return KB_HTTP_IO_ERROR;
   }
   return deadline_expired(deadline) ? KB_HTTP_TIMEOUT : KB_HTTP_OK;
}

static kb_http_result_t pipe_read_deadline(int fd, unsigned char *bytes, size_t capacity,
                                           size_t *length, int64_t deadline)
{
   *length = 0;
   for (;;)
   {
      if (deadline_expired(deadline))
         return KB_HTTP_TIMEOUT;
      unsigned char surplus;
      unsigned char *target = *length < capacity ? bytes + *length : &surplus;
      size_t available = *length < capacity ? capacity - *length : 1U;
      ssize_t n = read(fd, target, available);
      if (n > 0)
      {
         if (*length == capacity)
            return KB_HTTP_RESOLVE_ERROR;
         *length += (size_t)n;
      }
      else if (n == 0)
         return deadline_expired(deadline) ? KB_HTTP_TIMEOUT : KB_HTTP_OK;
      else if (errno == EINTR)
         continue;
      else if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
         int timeout = deadline_poll_ms(deadline);
         if (!timeout)
            return KB_HTTP_TIMEOUT;
         struct pollfd pfd = {.fd = fd, .events = POLLIN};
         int rc = poll(&pfd, 1, timeout);
         if (rc == 0)
            return KB_HTTP_TIMEOUT;
         if (rc < 0)
         {
            if (errno == EINTR)
               continue;
            return KB_HTTP_IO_ERROR;
         }
         if (pfd.revents & (POLLERR | POLLNVAL))
            return KB_HTTP_IO_ERROR;
         if (!(pfd.revents & (POLLIN | POLLHUP)))
            continue;
      }
      else
         return KB_HTTP_IO_ERROR;
   }
}

static kb_http_result_t child_reap_deadline(pid_t *pid, int64_t deadline, int *status)
{
   for (;;)
   {
      int wait_cancel_state;
      (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &wait_cancel_state);
      pid_t rc = waitpid(*pid, status, WNOHANG);
      if (rc == *pid)
      {
         *pid = -1;
         kb_http_result_t result = deadline_expired(deadline) ? KB_HTTP_TIMEOUT : KB_HTTP_OK;
         (void)pthread_setcancelstate(wait_cancel_state, NULL);
         return result;
      }
      int wait_error = errno;
      (void)pthread_setcancelstate(wait_cancel_state, NULL);
      if (rc < 0 && wait_error != EINTR)
         return KB_HTTP_INTERNAL_ERROR;
      if (deadline_expired(deadline))
         return KB_HTTP_TIMEOUT;
      (void)poll(NULL, 0, 1);
   }
}

static void resolver_cleanup(void *opaque)
{
   resolver_cleanup_t *cleanup = opaque;
   int old_cancel_state;
   (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);
   for (size_t i = 0; i < 2; i++)
   {
      if (cleanup->request_pipe[i] >= 0)
      {
         close(cleanup->request_pipe[i]);
         cleanup->request_pipe[i] = -1;
      }
      if (cleanup->response_pipe[i] >= 0)
      {
         close(cleanup->response_pipe[i]);
         cleanup->response_pipe[i] = -1;
      }
   }
   if (cleanup->pid > 0)
   {
      (void)kill(cleanup->pid, SIGKILL);
      dns_reap_enqueue(cleanup->pid);
      cleanup->slot_owned = 0;
      cleanup->pid = -1;
   }
   if (cleanup->slot_owned)
   {
      dns_slot_release();
      cleanup->slot_owned = 0;
   }
   (void)pthread_setcancelstate(old_cancel_state, NULL);
}

static kb_http_result_t parse_resolver_response(const unsigned char *wire, size_t length,
                                                struct addrinfo **result)
{
   *result = NULL;
   if (length < 8 || kb_resolver_get_u32(wire) != KB_RESOLVER_RESPONSE_MAGIC ||
       wire[4] != KB_RESOLVER_VERSION || wire[7] != 0 || wire[6] > KB_RESOLVER_RECORD_MAX)
      return KB_HTTP_RESOLVE_ERROR;
   if (wire[5] != KB_RESOLVER_STATUS_OK)
      return length == 8 && wire[6] == 0 ? KB_HTTP_RESOLVE_ERROR : KB_HTTP_RESOLVE_ERROR;
   if (!wire[6])
      return KB_HTTP_RESOLVE_ERROR;
   size_t at = 8;
   struct addrinfo **tail = result;
   for (size_t i = 0; i < wire[6]; i++)
   {
      if (at + 5U > length || wire[at + 4] != 0)
         goto malformed;
      unsigned family = wire[at], address_len = wire[at + 1];
      uint16_t port = kb_resolver_get_u16(wire + at + 2);
      at += 5;
      if (port != 443U ||
          ((family != 4 || address_len != 4) && (family != 6 || address_len != 16)) ||
          at + address_len > length)
         goto malformed;
      struct addrinfo *a = calloc(1, sizeof(*a));
      struct sockaddr *sa =
          calloc(1, family == 4 ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
      if (!a || !sa)
      {
         free(a);
         free(sa);
         resolved_addresses_free(*result);
         *result = NULL;
         return KB_HTTP_INTERNAL_ERROR;
      }
      a->ai_family = family == 4 ? AF_INET : AF_INET6;
      a->ai_socktype = SOCK_STREAM;
      a->ai_protocol = IPPROTO_TCP;
      a->ai_addr = sa;
      if (family == 4)
      {
         struct sockaddr_in *in = (struct sockaddr_in *)sa;
         in->sin_family = AF_INET;
         in->sin_port = htons(port);
         memcpy(&in->sin_addr, wire + at, address_len);
         a->ai_addrlen = sizeof(*in);
      }
      else
      {
         struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)sa;
         in6->sin6_family = AF_INET6;
         in6->sin6_port = htons(port);
         memcpy(&in6->sin6_addr, wire + at, address_len);
         a->ai_addrlen = sizeof(*in6);
      }
      at += address_len;
      *tail = a;
      tail = &a->ai_next;
   }
   if (at != length)
      goto malformed;
   return KB_HTTP_OK;

malformed:
   resolved_addresses_free(*result);
   *result = NULL;
   return KB_HTTP_RESOLVE_ERROR;
}

__attribute__((visibility("hidden"))) kb_http_result_t
kb_http_client_test__parse_resolver_response(const unsigned char *wire, size_t length)
{
   struct addrinfo *result = NULL;
   if (!wire)
      return KB_HTTP_INVALID_ARGUMENT;
   kb_http_result_t status = parse_resolver_response(wire, length, &result);
   resolved_addresses_free(result);
   return status;
}

static kb_http_result_t resolve_deadline_with(const char *host, const char *service, unsigned flags,
                                              const char *helper_override, int64_t deadline,
                                              struct addrinfo **result)
{
   *result = NULL;
   size_t host_len = strnlen(host, KB_RESOLVER_HOST_MAX + 1U);
   size_t service_len = strnlen(service, KB_RESOLVER_SERVICE_MAX + 1U);
   if (!host_len || host_len > KB_RESOLVER_HOST_MAX || !service_len ||
       service_len > KB_RESOLVER_SERVICE_MAX || (flags & ~KB_RESOLVER_FLAG_HANG_TEST))
      return KB_HTTP_INVALID_ARGUMENT;
   resolver_cleanup_t cleanup = {
       .request_pipe = {-1, -1}, .response_pipe = {-1, -1}, .pid = -1, .slot_owned = 0};
   kb_http_result_t outcome = KB_HTTP_INTERNAL_ERROR;
   char helper_path[PATH_MAX];
   unsigned char request[8U + KB_RESOLVER_HOST_MAX + KB_RESOLVER_SERVICE_MAX] = {0};
   unsigned char response[KB_RESOLVER_WIRE_MAX];
   size_t response_len = 0;
   pthread_cleanup_push(resolver_cleanup, &cleanup);
   outcome = dns_slot_acquire(deadline, &cleanup.slot_owned);
   if (outcome != KB_HTTP_OK)
      goto done;
   int setup_cancel_state;
   (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &setup_cancel_state);
   int setup_error = resolver_helper_path(helper_path, helper_override) != 0 ||
                     socketpair_cloexec(cleanup.request_pipe) != 0 ||
                     pipe_cloexec(cleanup.response_pipe) != 0;
   (void)pthread_setcancelstate(setup_cancel_state, NULL);
   if (setup_error)
   {
      outcome = KB_HTTP_RESOLVE_ERROR;
      goto done;
   }
   (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &setup_cancel_state);
   posix_spawn_file_actions_t actions;
   if (posix_spawn_file_actions_init(&actions) != 0)
   {
      (void)pthread_setcancelstate(setup_cancel_state, NULL);
      outcome = KB_HTTP_INTERNAL_ERROR;
      goto done;
   }
   int action_error =
       posix_spawn_file_actions_adddup2(&actions, cleanup.request_pipe[0], STDIN_FILENO);
   if (!action_error)
      action_error =
          posix_spawn_file_actions_adddup2(&actions, cleanup.response_pipe[1], STDOUT_FILENO);
   if (!action_error)
      action_error =
          posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
   if (!action_error)
      action_error = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
   char *const argv[] = {helper_path, NULL};
   char *const envp[] = {"LANG=C", "LC_ALL=C", NULL};
   int spawn_error = action_error
                         ? action_error
                         : posix_spawn(&cleanup.pid, helper_path, &actions, NULL, argv, envp);
   posix_spawn_file_actions_destroy(&actions);
   (void)pthread_setcancelstate(setup_cancel_state, NULL);
   if (spawn_error != 0)
   {
      outcome = KB_HTTP_RESOLVE_ERROR;
      goto done;
   }
   (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &setup_cancel_state);
   close(cleanup.request_pipe[0]);
   cleanup.request_pipe[0] = -1;
   close(cleanup.response_pipe[1]);
   cleanup.response_pipe[1] = -1;
   (void)pthread_setcancelstate(setup_cancel_state, NULL);
   if (fd_nonblocking(cleanup.request_pipe[1]) != 0 ||
       fd_nonblocking(cleanup.response_pipe[0]) != 0)
   {
      outcome = KB_HTTP_INTERNAL_ERROR;
      goto done;
   }
   kb_resolver_put_u32(request, KB_RESOLVER_REQUEST_MAGIC);
   request[4] = KB_RESOLVER_VERSION;
   request[5] = (unsigned char)flags;
   request[6] = (unsigned char)host_len;
   request[7] = (unsigned char)service_len;
   memcpy(request + 8, host, host_len);
   memcpy(request + 8 + host_len, service, service_len);
   outcome = socket_write_deadline(cleanup.request_pipe[1], request, 8U + host_len + service_len,
                                   deadline);
   (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &setup_cancel_state);
   close(cleanup.request_pipe[1]);
   cleanup.request_pipe[1] = -1;
   (void)pthread_setcancelstate(setup_cancel_state, NULL);
   if (outcome != KB_HTTP_OK)
      goto done;
   outcome = pipe_read_deadline(cleanup.response_pipe[0], response, sizeof(response), &response_len,
                                deadline);
   if (outcome != KB_HTTP_OK)
      goto done;
   int child_status = 0;
   outcome = child_reap_deadline(&cleanup.pid, deadline, &child_status);
   if (outcome != KB_HTTP_OK)
      goto done;
   if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
   {
      outcome = KB_HTTP_RESOLVE_ERROR;
      goto done;
   }
   outcome = deadline_expired(deadline) ? KB_HTTP_TIMEOUT
                                        : parse_resolver_response(response, response_len, result);

done:
   pthread_cleanup_pop(1);
   return outcome;
}

static kb_http_result_t resolve_deadline(const char *host, const char *service, int64_t deadline,
                                         struct addrinfo **result)
{
   return resolve_deadline_with(host, service, 0, NULL, deadline, result);
}

__attribute__((visibility("hidden"))) kb_http_result_t
kb_http_client_test__resolve(const char *host, int timeout_ms, int hang)
{
   int64_t now = now_ns();
   char helper_path[PATH_MAX];
   if (!host || timeout_ms <= 0 || now < 0 || !realpath("../aimee-kb-resolver", helper_path) ||
       now > INT64_MAX - (int64_t)timeout_ms * 1000000LL)
      return KB_HTTP_INVALID_ARGUMENT;
   struct addrinfo *result = NULL;
   kb_http_result_t status =
       resolve_deadline_with(host, "443", hang ? KB_RESOLVER_FLAG_HANG_TEST : 0, helper_path,
                             now + (int64_t)timeout_ms * 1000000LL, &result);
   resolved_addresses_free(result);
   return status;
}

__attribute__((visibility("hidden"))) kb_http_result_t
kb_http_client_test__dns_wait_idle(int timeout_ms, size_t *high_water)
{
   int64_t now = now_ns();
   if (!high_water || timeout_ms <= 0 || now < 0 ||
       now > INT64_MAX - (int64_t)timeout_ms * 1000000LL)
      return KB_HTTP_INVALID_ARGUMENT;
   if (pthread_once(&dns_pool_once, dns_pool_init) != 0 || !dns_pool_ready)
      return KB_HTTP_INTERNAL_ERROR;
   int64_t deadline = now + (int64_t)timeout_ms * 1000000LL;
   struct timespec until = {.tv_sec = deadline / 1000000000LL, .tv_nsec = deadline % 1000000000LL};
   pthread_mutex_lock(&dns_pool_mutex);
   int wait_result = 0;
   while (dns_slots_used && wait_result == 0)
      wait_result = pthread_cond_timedwait(&dns_slots_cond, &dns_pool_mutex, &until);
   *high_water = dns_slots_high_water;
   kb_http_result_t result = dns_slots_used ? KB_HTTP_TIMEOUT : KB_HTTP_OK;
   pthread_mutex_unlock(&dns_pool_mutex);
   return result;
}

__attribute__((visibility("hidden"))) kb_http_result_t
kb_http_client_test__dns_slots(size_t *used, size_t *high_water)
{
   if (!used || !high_water || pthread_once(&dns_pool_once, dns_pool_init) != 0 || !dns_pool_ready)
      return KB_HTTP_INVALID_ARGUMENT;
   if (pthread_mutex_lock(&dns_pool_mutex) != 0)
      return KB_HTTP_INTERNAL_ERROR;
   *used = dns_slots_used;
   *high_water = dns_slots_high_water;
   pthread_mutex_unlock(&dns_pool_mutex);
   return KB_HTTP_OK;
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
         if (pfd.revents & (POLLERR | POLLNVAL))
            return KB_HTTP_IO_ERROR;
         if (pfd.revents & events)
            return KB_HTTP_OK;
         if (pfd.revents & POLLHUP)
            return KB_HTTP_IO_ERROR;
         continue;
      }
      if (rc == 0)
         return KB_HTTP_TIMEOUT;
      if (errno != EINTR)
         return KB_HTTP_IO_ERROR;
   }
}

__attribute__((visibility("hidden"))) kb_http_result_t kb_http_client_test__wait_fd(int fd,
                                                                                    short events,
                                                                                    int timeout_ms)
{
   int64_t now = now_ns();
   if (fd < 0 || timeout_ms <= 0 || now < 0 || now > INT64_MAX - (int64_t)timeout_ms * 1000000LL)
      return KB_HTTP_INVALID_ARGUMENT;
   return wait_fd(fd, events, now + (int64_t)timeout_ms * 1000000LL);
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

typedef struct
{
   int fd;
} nosigpipe_bio_state_t;

static int nosigpipe_bio_create(BIO *bio)
{
   nosigpipe_bio_state_t *state = calloc(1, sizeof(*state));
   if (!state)
      return 0;
   state->fd = -1;
   BIO_set_data(bio, state);
   BIO_set_init(bio, 0);
   BIO_set_shutdown(bio, BIO_NOCLOSE);
   return 1;
}

static int nosigpipe_bio_destroy(BIO *bio)
{
   if (!bio)
      return 0;
   nosigpipe_bio_state_t *state = BIO_get_data(bio);
   if (state)
   {
      if (BIO_get_shutdown(bio) == BIO_CLOSE && BIO_get_init(bio) && state->fd >= 0)
         close(state->fd);
      free(state);
   }
   BIO_set_data(bio, NULL);
   BIO_set_init(bio, 0);
   return 1;
}

static int nosigpipe_bio_read(BIO *bio, char *bytes, int length)
{
   nosigpipe_bio_state_t *state = BIO_get_data(bio);
   if (!state || state->fd < 0 || !bytes || length <= 0)
      return 0;
   BIO_clear_retry_flags(bio);
   int result = (int)recv(state->fd, bytes, (size_t)length, 0);
   if (result < 0 && BIO_sock_should_retry(result))
      BIO_set_retry_read(bio);
   return result;
}

static int nosigpipe_bio_write(BIO *bio, const char *bytes, int length)
{
   nosigpipe_bio_state_t *state = BIO_get_data(bio);
   if (!state || state->fd < 0 || !bytes || length <= 0)
      return 0;
   BIO_clear_retry_flags(bio);
   int result = (int)send(state->fd, bytes, (size_t)length, MSG_NOSIGNAL);
   if (result < 0 && BIO_sock_should_retry(result))
      BIO_set_retry_write(bio);
   return result;
}

static long nosigpipe_bio_ctrl(BIO *bio, int command, long argument, void *pointer)
{
   nosigpipe_bio_state_t *state = BIO_get_data(bio);
   switch (command)
   {
   case BIO_C_SET_FD:
      if (!state || !pointer)
         return 0;
      if (BIO_get_shutdown(bio) == BIO_CLOSE && BIO_get_init(bio) && state->fd >= 0)
         close(state->fd);
      state->fd = *(int *)pointer;
      BIO_set_shutdown(bio, (int)argument);
      BIO_set_init(bio, 1);
      return 1;
   case BIO_C_GET_FD:
      if (!state || !BIO_get_init(bio))
         return -1;
      if (pointer)
         *(int *)pointer = state->fd;
      return state->fd;
   case BIO_CTRL_GET_CLOSE:
      return BIO_get_shutdown(bio);
   case BIO_CTRL_SET_CLOSE:
      BIO_set_shutdown(bio, (int)argument);
      return 1;
   case BIO_CTRL_FLUSH:
      return 1;
   case BIO_CTRL_EOF:
      return !state || !BIO_get_init(bio);
   case BIO_CTRL_PENDING:
   {
      int pending = 0;
      return state && state->fd >= 0 && ioctl(state->fd, FIONREAD, &pending) == 0 && pending > 0
                 ? pending
                 : 0;
   }
   case BIO_CTRL_WPENDING:
      return 0;
   case BIO_CTRL_DUP:
   {
      BIO *target = pointer;
      nosigpipe_bio_state_t *target_state = target ? BIO_get_data(target) : NULL;
      if (!state || !target_state)
         return 0;
      target_state->fd = state->fd;
      BIO_set_init(target, BIO_get_init(bio));
      BIO_set_shutdown(target, BIO_NOCLOSE); /* A duplicate never gains fd ownership. */
      return 1;
   }
   default:
      return 0;
   }
}

static BIO_METHOD *nosigpipe_bio_method;
static pthread_once_t nosigpipe_bio_once = PTHREAD_ONCE_INIT;

static void nosigpipe_bio_method_init(void)
{
   int type = BIO_get_new_index();
   if (type < 0)
      return;
   BIO_METHOD *method =
       BIO_meth_new(type | BIO_TYPE_SOURCE_SINK | BIO_TYPE_DESCRIPTOR, "aimee MSG_NOSIGNAL socket");
   if (!method || BIO_meth_set_create(method, nosigpipe_bio_create) != 1 ||
       BIO_meth_set_destroy(method, nosigpipe_bio_destroy) != 1 ||
       BIO_meth_set_read(method, nosigpipe_bio_read) != 1 ||
       BIO_meth_set_write(method, nosigpipe_bio_write) != 1 ||
       BIO_meth_set_ctrl(method, nosigpipe_bio_ctrl) != 1)
   {
      BIO_meth_free(method);
      return;
   }
   nosigpipe_bio_method = method; /* Process-lifetime OpenSSL method. */
}

static BIO *nosigpipe_socket_bio(int fd)
{
   if (fd < 0 || pthread_once(&nosigpipe_bio_once, nosigpipe_bio_method_init) != 0 ||
       !nosigpipe_bio_method)
      return NULL;
   BIO *bio = BIO_new(nosigpipe_bio_method);
   if (!bio || BIO_ctrl(bio, BIO_C_SET_FD, BIO_NOCLOSE, &fd) != 1)
   {
      BIO_free(bio);
      return NULL;
   }
   return bio;
}

__attribute__((visibility("hidden"))) int kb_http_client_test__nosigpipe_bio_write(int fd)
{
   BIO *bio = nosigpipe_socket_bio(fd);
   if (!bio)
   {
      errno = EINVAL;
      return -1;
   }
   int result = BIO_write(bio, "x", 1);
   int saved_errno = errno;
   BIO_free(bio);
   errno = saved_errno;
   return result;
}

__attribute__((visibility("hidden"))) int kb_http_client_test__nosigpipe_ssl_fd(int fd)
{
   SSL_CTX *context = SSL_CTX_new(TLS_client_method());
   SSL *ssl = context ? SSL_new(context) : NULL;
   BIO *bio = ssl ? nosigpipe_socket_bio(fd) : NULL;
   if (!ssl || !bio)
   {
      BIO_free(bio);
      SSL_free(ssl);
      SSL_CTX_free(context);
      return -1;
   }
   SSL_set_bio(ssl, bio, bio);
   int result = SSL_get_fd(ssl);
   SSL_free(ssl);
   SSL_CTX_free(context);
   return result;
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
       r->total_timeout_ms <= 0 || strcmp(r->method ? r->method : "", "POST") != 0 ||
       !origin_path_valid(r->target) || !r->headers || !r->header_count ||
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
   /* Do not consult SSL_CERT_FILE/SSL_CERT_DIR: egress trust is anchored only in
    * an administrator-managed Linux system bundle at a fixed location. */
   static const char *const system_bundles[] = {
       "/etc/ssl/certs/ca-certificates.crt",                /* Debian/Ubuntu */
       "/etc/pki/tls/certs/ca-bundle.crt",                  /* RHEL/Fedora */
       "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* RHEL alternate */
       "/etc/ssl/ca-bundle.pem"                             /* SUSE */
   };
   int trust_loaded = 0;
   for (size_t i = 0; i < sizeof(system_bundles) / sizeof(system_bundles[0]); i++)
   {
      ERR_clear_error();
      if (SSL_CTX_load_verify_locations(ctx, system_bundles[i], NULL) == 1)
      {
         trust_loaded = 1;
         break;
      }
   }
   if (!trust_loaded)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   return ctx;
}

__attribute__((visibility("hidden"))) int
kb_http_client_test__tls_eof_is_authenticated(int ssl_error)
{
   return ssl_error == SSL_ERROR_ZERO_RETURN;
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
                                      kb_http_response_t *response, kb_http_headers_fn headers_cb,
                                      kb_http_body_fn body_cb, void *context)
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
   kb_http_result_t result;
   char service[16];
   snprintf(service, sizeof(service), "%d", 443);
   struct addrinfo *addresses = NULL;
   result = resolve_deadline(request->authority, service, connect_deadline_ns, &addresses);
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
   BIO *transport_bio = ssl ? nosigpipe_socket_bio(fd) : NULL;
   if (!ssl || !transport_bio || SSL_set_tlsext_host_name(ssl, request->authority) != 1 ||
       SSL_set1_host(ssl, request->authority) != 1)
   {
      BIO_free(transport_bio);
      result = KB_HTTP_TLS_ERROR;
      goto done;
   }
   SSL_set_bio(ssl, transport_bio, transport_bio); /* SSL owns the single shared BIO reference. */
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
      if (kb_http_client_test__tls_eof_is_authenticated(error))
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
   resolved_addresses_free(addresses);
   if (ssl)
   {
      if (deadline_poll_ms(deadline))
         (void)SSL_shutdown(ssl);
      SSL_free(ssl);
   }
   SSL_CTX_free(ctx);
   if (fd >= 0)
      close(fd);
   return result;
}
