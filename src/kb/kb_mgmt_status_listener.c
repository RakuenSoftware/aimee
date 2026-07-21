#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_mgmt_status_listener.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <openssl/x509v3.h>

typedef struct
{
   int fd;
   unsigned char address[16];
   unsigned char address_len;
} queued_conn_t;

typedef struct
{
   int listen_fd;
   uint16_t bound_port;
   SSL_CTX *tls;
   kb_mgmt_status_listener_handler_fn handle;
   void *opaque;
   pthread_t accept_thread;
   pthread_t workers[KB_MGMT_STATUS_LISTENER_WORKERS];
   pthread_mutex_t mutex;
   pthread_cond_t ready;
   queued_conn_t queue[KB_MGMT_STATUS_LISTENER_QUEUE];
   size_t head;
   size_t count;
   atomic_int stopping;
   int initialized;
} listener_state_t;

static listener_state_t g_listener = {.listen_fd = -1};

static int address_of(const struct sockaddr_storage *ss, queued_conn_t *out)
{
   if (ss->ss_family == AF_INET)
   {
      const struct sockaddr_in *v4 = (const struct sockaddr_in *)ss;
      memcpy(out->address, &v4->sin_addr, 4);
      out->address_len = 4;
      return 0;
   }
   if (ss->ss_family == AF_INET6)
   {
      const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)ss;
      memcpy(out->address, &v6->sin6_addr, 16);
      out->address_len = 16;
      return 0;
   }
   return -1;
}

static int header_name_char(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int ows_value(const unsigned char *begin, const unsigned char *end, const char *expected)
{
   while (begin < end && (*begin == ' ' || *begin == '\t'))
      ++begin;
   while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
      --end;
   size_t n = (size_t)(end - begin);
   return strlen(expected) == n && memcmp(begin, expected, n) == 0;
}

kb_mgmt_status_http_result_t kb_mgmt_status_http_parse(const unsigned char *in, size_t len,
                                                       const char **body, size_t *body_len)
{
   static const char request_line[] = "POST /v1/management/status HTTP/1.1\r\n";
   const unsigned char *header_end = NULL;
   size_t header_len = 0, content_len = 0, header_count = 0;
   int hosts = 0, lengths = 0, content_types = 0;
   if (body)
      *body = NULL;
   if (body_len)
      *body_len = 0;
   if ((!in && len) || !body || !body_len)
      return KB_MGMT_STATUS_HTTP_BAD;
   if (len > KB_MGMT_STATUS_HTTP_HEADER_MAX + 4 + KB_MGMT_STATUS_HTTP_BODY_MAX)
      return KB_MGMT_STATUS_HTTP_TOO_LARGE;
   for (size_t i = 0; i < len; ++i)
   {
      if (in[i] == 0 || in[i] == 0x7f ||
          (in[i] < 0x20 && in[i] != '\r' && in[i] != '\n' && in[i] != '\t'))
         return KB_MGMT_STATUS_HTTP_BAD;
      if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r'))
         return KB_MGMT_STATUS_HTTP_BAD;
      if (i >= 3 && in[i - 3] == '\r' && in[i - 2] == '\n' && in[i - 1] == '\r' && in[i] == '\n')
      {
         header_end = in + i + 1;
         header_len = i + 1;
         break;
      }
   }
   if (!header_end)
      return len > KB_MGMT_STATUS_HTTP_HEADER_MAX ? KB_MGMT_STATUS_HTTP_TOO_LARGE
                                                  : KB_MGMT_STATUS_HTTP_MORE;
   if (header_len > KB_MGMT_STATUS_HTTP_HEADER_MAX || header_len < sizeof(request_line) - 1 ||
       memcmp(in, request_line, sizeof(request_line) - 1) != 0)
      return KB_MGMT_STATUS_HTTP_BAD;

   const unsigned char *p = in + sizeof(request_line) - 1;
   const unsigned char *last = header_end - 2;
   while (p < last)
   {
      const unsigned char *e = NULL;
      for (const unsigned char *q = p; q + 1 < header_end; ++q)
         if (q[0] == '\r' && q[1] == '\n')
         {
            e = q;
            break;
         }
      if (!e || e == p || p[0] == ' ' || p[0] == '\t' || ++header_count > 32)
         return KB_MGMT_STATUS_HTTP_BAD;
      const unsigned char *colon = memchr(p, ':', (size_t)(e - p));
      if (!colon || colon == p)
         return KB_MGMT_STATUS_HTTP_BAD;
      for (const unsigned char *q = p; q < colon; ++q)
         if (!header_name_char(*q))
            return KB_MGMT_STATUS_HTTP_BAD;
      const unsigned char *value = colon + 1;
      for (const unsigned char *q = value; q < e; ++q)
         if ((*q < 0x20 && *q != '\t') || *q == 0x7f)
            return KB_MGMT_STATUS_HTTP_BAD;
      size_t name_len = (size_t)(colon - p);
      if (name_len == 4 && strncasecmp((const char *)p, "Host", 4) == 0)
      {
         if (++hosts != 1)
            return KB_MGMT_STATUS_HTTP_BAD;
         const unsigned char *a = value, *b = e;
         while (a < b && (*a == ' ' || *a == '\t'))
            ++a;
         while (b > a && (b[-1] == ' ' || b[-1] == '\t'))
            --b;
         if (a == b)
            return KB_MGMT_STATUS_HTTP_BAD;
      }
      else if (name_len == 14 && strncasecmp((const char *)p, "Content-Length", 14) == 0)
      {
         if (++lengths != 1)
            return KB_MGMT_STATUS_HTTP_BAD;
         const unsigned char *a = value, *b = e;
         while (a < b && (*a == ' ' || *a == '\t'))
            ++a;
         while (b > a && (b[-1] == ' ' || b[-1] == '\t'))
            --b;
         if (a == b || (b - a > 1 && *a == '0'))
            return KB_MGMT_STATUS_HTTP_BAD;
         for (const unsigned char *q = a; q < b; ++q)
         {
            if (*q < '0' || *q > '9' || content_len > (SIZE_MAX - 9) / 10)
               return KB_MGMT_STATUS_HTTP_BAD;
            content_len = content_len * 10 + (size_t)(*q - '0');
         }
         if (content_len > KB_MGMT_STATUS_HTTP_BODY_MAX)
            return KB_MGMT_STATUS_HTTP_TOO_LARGE;
      }
      else if (name_len == 12 && strncasecmp((const char *)p, "Content-Type", 12) == 0)
      {
         if (++content_types != 1 || !ows_value(value, e, "application/json"))
            return KB_MGMT_STATUS_HTTP_BAD;
      }
      else if ((name_len == 17 && strncasecmp((const char *)p, "Transfer-Encoding", 17) == 0) ||
               (name_len == 6 && strncasecmp((const char *)p, "Expect", 6) == 0) ||
               (name_len == 7 && strncasecmp((const char *)p, "Upgrade", 7) == 0) ||
               (name_len == 10 && strncasecmp((const char *)p, "Connection", 10) == 0))
         return KB_MGMT_STATUS_HTTP_BAD;
      p = e + 2;
   }
   if (p != last || hosts != 1 || lengths != 1 || content_types != 1)
      return KB_MGMT_STATUS_HTTP_BAD;
   if (len < header_len + content_len)
      return KB_MGMT_STATUS_HTTP_MORE;
   if (len != header_len + content_len)
      return KB_MGMT_STATUS_HTTP_BAD;
   *body = (const char *)(in + header_len);
   *body_len = content_len;
   return KB_MGMT_STATUS_HTTP_COMPLETE;
}

static int64_t monotonic_ms(void)
{
   struct timespec ts;
   return clock_gettime(CLOCK_MONOTONIC, &ts) == 0
              ? (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000
              : -1;
}

static int wait_fd(int fd, short events, int64_t deadline)
{
   int64_t now = monotonic_ms();
   if (now < 0 || now >= deadline)
      return -1;
   int remain = (int)(deadline - now);
   struct pollfd pfd = {.fd = fd, .events = events};
   int rc;
   do
      rc = poll(&pfd, 1, remain);
   while (rc < 0 && errno == EINTR);
   return rc == 1 && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) ? 0 : -1;
}

static int ssl_handshake(SSL *ssl, int fd, int64_t deadline)
{
   for (;;)
   {
      int rc = SSL_accept(ssl);
      if (rc == 1)
         return 0;
      int error = SSL_get_error(ssl, rc);
      short events = error == SSL_ERROR_WANT_READ    ? POLLIN
                     : error == SSL_ERROR_WANT_WRITE ? POLLOUT
                                                     : 0;
      if (!events || wait_fd(fd, events, deadline) != 0)
         return -1;
   }
}

static int ssl_has_http11_alpn(SSL *ssl)
{
   static const unsigned char expected[] = "http/1.1";
   const unsigned char *selected = NULL;
   unsigned int selected_len = 0;
   SSL_get0_alpn_selected(ssl, &selected, &selected_len);
   return selected && selected_len == sizeof(expected) - 1 &&
          CRYPTO_memcmp(selected, expected, sizeof(expected) - 1) == 0;
}

static int ssl_read_request(SSL *ssl, int fd, int64_t deadline, unsigned char *buffer, size_t cap,
                            const char **body, size_t *body_len)
{
   size_t used = 0;
   for (;;)
   {
      kb_mgmt_status_http_result_t parsed = kb_mgmt_status_http_parse(buffer, used, body, body_len);
      if (parsed == KB_MGMT_STATUS_HTTP_COMPLETE)
         return SSL_pending(ssl) == 0 ? 0 : -1;
      if (parsed != KB_MGMT_STATUS_HTTP_MORE || used == cap)
         return -1;
      int rc = SSL_read(ssl, buffer + used, (int)(cap - used));
      if (rc > 0)
      {
         used += (size_t)rc;
         continue;
      }
      int error = SSL_get_error(ssl, rc);
      short events = error == SSL_ERROR_WANT_READ    ? POLLIN
                     : error == SSL_ERROR_WANT_WRITE ? POLLOUT
                                                     : 0;
      if (!events || wait_fd(fd, events, deadline) != 0)
         return -1;
   }
}

static int ssl_write_all(SSL *ssl, int fd, int64_t deadline, const char *data, size_t len)
{
   size_t sent = 0;
   while (sent < len)
   {
      int rc = SSL_write(ssl, data + sent, (int)(len - sent));
      if (rc > 0)
      {
         sent += (size_t)rc;
         continue;
      }
      int error = SSL_get_error(ssl, rc);
      short events = error == SSL_ERROR_WANT_READ    ? POLLIN
                     : error == SSL_ERROR_WANT_WRITE ? POLLOUT
                                                     : 0;
      if (!events || wait_fd(fd, events, deadline) != 0)
         return -1;
   }
   return 0;
}

static void send_response(SSL *ssl, int fd, int64_t deadline, int status, const char *reason,
                          const char *body)
{
   char header[256];
   size_t body_len = strlen(body);
   int n = snprintf(header, sizeof(header),
                    "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                    "Connection: close\r\n\r\n",
                    status, reason, body_len);
   if (n > 0 && (size_t)n < sizeof(header) &&
       ssl_write_all(ssl, fd, deadline, header, (size_t)n) == 0)
      (void)ssl_write_all(ssl, fd, deadline, body, body_len);
}

static void release_address(const queued_conn_t *conn)
{
   (void)conn;
   /* Counts are derived from the bounded queue plus four active slots while
    * holding the mutex; workers retain their entry in active[] below. */
}

typedef struct
{
   listener_state_t *state;
   size_t worker_id;
} worker_arg_t;
static worker_arg_t g_worker_args[KB_MGMT_STATUS_LISTENER_WORKERS];
static queued_conn_t g_active[KB_MGMT_STATUS_LISTENER_WORKERS];
static unsigned char g_active_valid[KB_MGMT_STATUS_LISTENER_WORKERS];

static size_t address_count_locked(const listener_state_t *s, const queued_conn_t *candidate)
{
   size_t count = 0;
   for (size_t i = 0; i < s->count; ++i)
   {
      const queued_conn_t *q = &s->queue[(s->head + i) % KB_MGMT_STATUS_LISTENER_QUEUE];
      if (q->address_len == candidate->address_len &&
          memcmp(q->address, candidate->address, q->address_len) == 0)
         ++count;
   }
   for (size_t i = 0; i < KB_MGMT_STATUS_LISTENER_WORKERS; ++i)
      if (g_active_valid[i] && g_active[i].address_len == candidate->address_len &&
          memcmp(g_active[i].address, candidate->address, candidate->address_len) == 0)
         ++count;
   return count;
}

static void *worker_main(void *opaque)
{
   worker_arg_t *arg = opaque;
   listener_state_t *s = arg->state;
   for (;;)
   {
      pthread_mutex_lock(&s->mutex);
      while (!s->count && !atomic_load(&s->stopping))
         pthread_cond_wait(&s->ready, &s->mutex);
      if (!s->count && atomic_load(&s->stopping))
      {
         pthread_mutex_unlock(&s->mutex);
         break;
      }
      queued_conn_t conn = s->queue[s->head];
      s->head = (s->head + 1) % KB_MGMT_STATUS_LISTENER_QUEUE;
      --s->count;
      g_active[arg->worker_id] = conn;
      g_active_valid[arg->worker_id] = 1;
      pthread_mutex_unlock(&s->mutex);

      int flags = fcntl(conn.fd, F_GETFL, 0);
      if (flags >= 0)
         (void)fcntl(conn.fd, F_SETFL, flags | O_NONBLOCK);
      SSL *ssl = SSL_new(s->tls);
      int64_t deadline = monotonic_ms() + 5000;
      if (ssl && SSL_set_fd(ssl, conn.fd) == 1 && ssl_handshake(ssl, conn.fd, deadline) == 0 &&
          ssl_has_http11_alpn(ssl))
      {
         kb_mgmt_status_peer_t peer;
         memset(&peer, 0, sizeof(peer));
         if (kb_mgmt_status_peer_verify(ssl, &peer) == 1)
         {
            unsigned char input[KB_MGMT_STATUS_HTTP_HEADER_MAX + 4 + KB_MGMT_STATUS_HTTP_BODY_MAX];
            const char *body = NULL;
            size_t body_len = 0;
            if (ssl_read_request(ssl, conn.fd, deadline, input, sizeof(input), &body, &body_len) ==
                0)
            {
               char response[4096];
               memset(response, 0, sizeof(response));
               kb_mgmt_status_listener_result_t result = s->handle(
                   arg->worker_id, &peer, body, body_len, response, sizeof(response), s->opaque);
               if (result == KB_MGMT_STATUS_LISTENER_OK && memchr(response, 0, sizeof(response)))
                  send_response(ssl, conn.fd, deadline, 200, "OK", response);
               else if (result == KB_MGMT_STATUS_LISTENER_INVALID)
                  send_response(ssl, conn.fd, deadline, 400, "Bad Request",
                                "{\"error\":\"bad_request\"}");
               else if (result == KB_MGMT_STATUS_LISTENER_DENIED)
                  send_response(ssl, conn.fd, deadline, 403, "Forbidden", "{\"error\":\"denied\"}");
               else if (result == KB_MGMT_STATUS_LISTENER_CONFLICT)
                  send_response(ssl, conn.fd, deadline, 409, "Conflict",
                                "{\"error\":\"conflict\"}");
               else
                  send_response(ssl, conn.fd, deadline, 503, "Service Unavailable",
                                "{\"error\":\"unavailable\"}");
               OPENSSL_cleanse(response, sizeof(response));
            }
            else
               send_response(ssl, conn.fd, deadline, 400, "Bad Request",
                             "{\"error\":\"bad_request\"}");
         }
         OPENSSL_cleanse(&peer, sizeof(peer));
      }
      if (ssl)
      {
         (void)SSL_shutdown(ssl);
         SSL_free(ssl);
      }
      close(conn.fd);
      release_address(&conn);
      pthread_mutex_lock(&s->mutex);
      g_active_valid[arg->worker_id] = 0;
      memset(&g_active[arg->worker_id], 0, sizeof(g_active[arg->worker_id]));
      pthread_mutex_unlock(&s->mutex);
   }
   return NULL;
}

static void *accept_main(void *opaque)
{
   listener_state_t *s = opaque;
   while (!atomic_load(&s->stopping))
   {
      struct sockaddr_storage peer;
      socklen_t peer_len = sizeof(peer);
      int fd = accept4(s->listen_fd, (struct sockaddr *)&peer, &peer_len, SOCK_CLOEXEC);
      if (fd < 0)
      {
         if (errno == EINTR)
            continue;
         if (atomic_load(&s->stopping))
            break;
         continue;
      }
      queued_conn_t conn = {.fd = fd};
      if (address_of(&peer, &conn) != 0)
      {
         close(fd);
         continue;
      }
      pthread_mutex_lock(&s->mutex);
      if (s->count == KB_MGMT_STATUS_LISTENER_QUEUE || address_count_locked(s, &conn) >= 4 ||
          atomic_load(&s->stopping))
         close(fd);
      else
      {
         size_t tail = (s->head + s->count) % KB_MGMT_STATUS_LISTENER_QUEUE;
         s->queue[tail] = conn;
         ++s->count;
         pthread_cond_signal(&s->ready);
      }
      pthread_mutex_unlock(&s->mutex);
   }
   return NULL;
}

static int alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                       const unsigned char *in, unsigned int inlen, void *opaque)
{
   (void)ssl;
   (void)opaque;
   static const unsigned char h1[] = "http/1.1";
   const unsigned char *p = in;
   while (p < in + inlen)
   {
      unsigned int n = *p++;
      if (n > (unsigned int)(in + inlen - p))
         return SSL_TLSEXT_ERR_ALERT_FATAL;
      if (n == sizeof(h1) - 1 && memcmp(p, h1, n) == 0)
      {
         *out = h1;
         *outlen = sizeof(h1) - 1;
         return SSL_TLSEXT_ERR_OK;
      }
      p += n;
   }
   return SSL_TLSEXT_ERR_ALERT_FATAL;
}

SSL_CTX *kb_mgmt_status_listener_tls_ctx(const char *ca, const char *cert, const char *key)
{
   if (!ca || !cert || !key)
      return NULL;
   SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
   if (!ctx)
      return NULL;
   long options = SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION;
#ifdef SSL_OP_NO_RENEGOTIATION
   options |= SSL_OP_NO_RENEGOTIATION;
#endif
   SSL_CTX_set_options(ctx, options);
   SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
   SSL_CTX_set_verify_depth(ctx, 6);
   SSL_CTX_set_security_level(ctx, 2);
   SSL_CTX_set_post_handshake_auth(ctx, 0);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
   SSL_CTX_set_max_early_data(ctx, 0);
#endif
   if (X509_VERIFY_PARAM_set_purpose(SSL_CTX_get0_param(ctx), X509_PURPOSE_SSL_CLIENT) != 1)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   SSL_CTX_set_alpn_select_cb(ctx, alpn_select, NULL);
   if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) ||
       SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1 ||
       SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
       SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
       SSL_CTX_check_private_key(ctx) != 1)
   {
      SSL_CTX_free(ctx);
      return NULL;
   }
   return ctx;
}

int kb_mgmt_status_listener_start(const kb_mgmt_status_listener_config_t *cfg)
{
   listener_state_t *s = &g_listener;
   if (!cfg || !cfg->tls || !cfg->handle || s->initialized)
      return -1;
   const char *host = cfg->bind_address && cfg->bind_address[0] ? cfg->bind_address : "0.0.0.0";
   char service[6];
   snprintf(service, sizeof(service), "%u", cfg->port);
   struct addrinfo hints = {
       .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
   struct addrinfo *addresses = NULL;
   if (getaddrinfo(host, service, &hints, &addresses) != 0)
      return -1;
   int fd = -1;
   for (struct addrinfo *a = addresses; a; a = a->ai_next)
   {
      fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
      if (fd < 0)
         continue;
      int one = 1;
      (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      if (bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 32) == 0)
         break;
      close(fd);
      fd = -1;
   }
   freeaddrinfo(addresses);
   if (fd < 0)
      return -1;
   struct sockaddr_storage bound;
   socklen_t bound_len = sizeof(bound);
   if (getsockname(fd, (struct sockaddr *)&bound, &bound_len) != 0)
   {
      close(fd);
      return -1;
   }
   memset(g_active_valid, 0, sizeof(g_active_valid));
   s->listen_fd = fd;
   s->bound_port = bound.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&bound)->sin_port)
                                              : ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
   s->tls = cfg->tls;
   s->handle = cfg->handle;
   s->opaque = cfg->opaque;
   s->head = s->count = 0;
   atomic_store(&s->stopping, 0);
   if (pthread_mutex_init(&s->mutex, NULL) != 0)
   {
      close(fd);
      memset(s, 0, sizeof(*s));
      s->listen_fd = -1;
      return -1;
   }
   if (pthread_cond_init(&s->ready, NULL) != 0)
   {
      pthread_mutex_destroy(&s->mutex);
      close(fd);
      memset(s, 0, sizeof(*s));
      s->listen_fd = -1;
      return -1;
   }
   s->initialized = 1;
   size_t made = 0;
   for (; made < KB_MGMT_STATUS_LISTENER_WORKERS; ++made)
   {
      g_worker_args[made] = (worker_arg_t){.state = s, .worker_id = made};
      if (pthread_create(&s->workers[made], NULL, worker_main, &g_worker_args[made]) != 0)
         break;
   }
   if (made != KB_MGMT_STATUS_LISTENER_WORKERS ||
       pthread_create(&s->accept_thread, NULL, accept_main, s) != 0)
   {
      atomic_store(&s->stopping, 1);
      pthread_cond_broadcast(&s->ready);
      close(s->listen_fd);
      s->listen_fd = -1;
      for (size_t i = 0; i < made; ++i)
         pthread_join(s->workers[i], NULL);
      pthread_cond_destroy(&s->ready);
      pthread_mutex_destroy(&s->mutex);
      memset(s, 0, sizeof(*s));
      s->listen_fd = -1;
      return -1;
   }
   return 0;
}

uint16_t kb_mgmt_status_listener_bound_port(void)
{
   return g_listener.initialized ? g_listener.bound_port : 0;
}

void kb_mgmt_status_listener_stop(void)
{
   listener_state_t *s = &g_listener;
   if (!s->initialized)
      return;
   atomic_store(&s->stopping, 1);
   if (s->listen_fd >= 0)
   {
      shutdown(s->listen_fd, SHUT_RDWR);
      close(s->listen_fd);
      s->listen_fd = -1;
   }
   pthread_join(s->accept_thread, NULL);
   pthread_mutex_lock(&s->mutex);
   while (s->count)
   {
      close(s->queue[s->head].fd);
      s->head = (s->head + 1) % KB_MGMT_STATUS_LISTENER_QUEUE;
      --s->count;
   }
   pthread_cond_broadcast(&s->ready);
   pthread_mutex_unlock(&s->mutex);
   for (size_t i = 0; i < KB_MGMT_STATUS_LISTENER_WORKERS; ++i)
      pthread_join(s->workers[i], NULL);
   pthread_cond_destroy(&s->ready);
   pthread_mutex_destroy(&s->mutex);
   memset(s, 0, sizeof(*s));
   s->listen_fd = -1;
}
