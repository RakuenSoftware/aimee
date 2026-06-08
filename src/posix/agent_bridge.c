/* posix/agent_bridge.c: POSIX provider layer — HTTP client (libcurl) and SSH tunnel lifecycle */
#include "aimee.h"
#include "agent_exec.h"
#include "log.h"
#include "proxy_bootstrap.h"
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static SSL_CTX *s_ssl_ctx;

void agent_http_init(void)
{
   /* Initialize proxy bootstrap before setting up the SSL context so that
    * the CA bundle path (SSL_CERT_FILE / REQUESTS_CA_BUNDLE) is available. */
   proxy_bootstrap_init_global();

   s_ssl_ctx = SSL_CTX_new(TLS_client_method());
   if (s_ssl_ctx)
   {
      SSL_CTX_set_default_verify_paths(s_ssl_ctx);
      SSL_CTX_set_verify(s_ssl_ctx, SSL_VERIFY_PEER, NULL);
      SSL_CTX_set_min_proto_version(s_ssl_ctx, TLS1_2_VERSION);

      /* Load custom CA bundle when configured */
      const proxy_bootstrap_t *pb = proxy_get();
      if (pb && pb->ca_bundle[0])
      {
         if (SSL_CTX_load_verify_file(s_ssl_ctx, pb->ca_bundle) != 1)
            aimee_log(LOG_WARN, "proxy", "failed to load CA bundle: %s", pb->ca_bundle);
         else
            aimee_log(LOG_DEBUG, "proxy", "loaded CA bundle: %s", pb->ca_bundle);
      }
   }
}

void agent_http_cleanup(void)
{
   if (s_ssl_ctx)
   {
      SSL_CTX_free(s_ssl_ctx);
      s_ssl_ctx = NULL;
   }
}

#define HTTP_MAX_RESPONSE_SIZE (10 * 1024 * 1024) /* 10MB */

/* ---- URL parsing ---- */

typedef struct
{
   char host[256];
   char path[2048];
   int port;
   int use_ssl;
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *out)
{
   memset(out, 0, sizeof(*out));

   if (strncmp(url, "https://", 8) == 0)
   {
      out->use_ssl = 1;
      out->port = 443;
      url += 8;
   }
   else if (strncmp(url, "http://", 7) == 0)
   {
      out->use_ssl = 0;
      out->port = 80;
      url += 7;
   }
   else
      return -1;

   /* Extract host[:port] and path */
   const char *slash = strchr(url, '/');
   const char *colon = strchr(url, ':');
   size_t hostlen;

   if (colon && (!slash || colon < slash))
   {
      hostlen = (size_t)(colon - url);
      out->port = atoi(colon + 1);
   }
   else
      hostlen = slash ? (size_t)(slash - url) : strlen(url);

   if (hostlen == 0 || hostlen >= sizeof(out->host))
      return -1;

   memcpy(out->host, url, hostlen);
   out->host[hostlen] = '\0';

   if (slash)
      snprintf(out->path, sizeof(out->path), "%s", slash);
   else
      snprintf(out->path, sizeof(out->path), "/");

   return 0;
}

/* ---- Socket I/O with timeout ---- */

typedef struct
{
   int fd;
   SSL *ssl;
   /* Wall-clock deadline for the entire response body read, in CLOCK_MONOTONIC
    * nanoseconds. 0 means no deadline. Catches slow-trickle connections where
    * SO_RCVTIMEO never fires because the remote keeps sending small chunks. */
   int64_t deadline_ns;
} http_conn_t;

static int64_t conn_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int conn_past_deadline(const http_conn_t *conn)
{
   return conn->deadline_ns && conn_now_ns() > conn->deadline_ns;
}

static int connect_with_timeout(const char *host, int port, int timeout_ms)
{
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   struct addrinfo hints = {0}, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
      return -1;

   int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (fd < 0)
   {
      freeaddrinfo(res);
      return -1;
   }

   /* Set non-blocking for connect timeout */
   int flags = fcntl(fd, F_GETFL, 0);
   fcntl(fd, F_SETFL, flags | O_NONBLOCK);

   int rc = connect(fd, res->ai_addr, res->ai_addrlen);
   freeaddrinfo(res);

   if (rc < 0 && errno != EINPROGRESS)
   {
      close(fd);
      return -1;
   }

   if (rc < 0)
   {
      /* Wait for connect — capped well below the overall request timeout so an
       * unreachable host (e.g. a down kb behind a SYN-dropping gateway) fails
       * fast instead of blocking for the full request budget. See
       * agent_http_effective_connect_timeout_ms() in agent_exec.h. */
      struct pollfd pfd = {fd, POLLOUT, 0};
      int pr = poll(&pfd, 1, agent_http_effective_connect_timeout_ms(timeout_ms));
      if (pr <= 0)
      {
         close(fd);
         return -1;
      }
      int err = 0;
      socklen_t errlen = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
      if (err)
      {
         close(fd);
         return -1;
      }
   }

   /* Restore blocking */
   fcntl(fd, F_SETFL, flags);
   return fd;
}

/* Send HTTP CONNECT to establish a tunnel through `proxy_host:proxy_port` to
 * `target_host:target_port`.  The fd must already be connected to the proxy.
 * Returns 0 on success (proxy returned "200"), -1 on failure. */
static int proxy_connect_tunnel(int fd, const char *target_host, int target_port,
                                const char *proxy_auth)
{
   char req[1024];
   int req_len;

   if (proxy_auth && proxy_auth[0])
   {
      /* Base64-encode "user:pass" for Proxy-Authorization */
      /* For simplicity we skip auth encoding here — most enterprise proxies
       * accept CONNECT without credentials when network-level access control
       * is in place.  Auth support can be added when needed. */
      (void)proxy_auth;
   }

   req_len = snprintf(req, sizeof(req),
                      "CONNECT %s:%d HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Proxy-Connection: keep-alive\r\n"
                      "\r\n",
                      target_host, target_port, target_host, target_port);
   if (req_len <= 0 || req_len >= (int)sizeof(req))
      return -1;

   /* Send the CONNECT request */
   ssize_t written = 0;
   while (written < req_len)
   {
      ssize_t n = write(fd, req + written, (size_t)(req_len - written));
      if (n <= 0)
         return -1;
      written += n;
   }

   /* Read the response line (up to 512 bytes) */
   char resp[512];
   int resp_len = 0;
   while (resp_len < (int)sizeof(resp) - 1)
   {
      ssize_t n = read(fd, resp + resp_len, 1);
      if (n <= 0)
         return -1;
      resp_len++;
      resp[resp_len] = '\0';
      /* Detect end of response headers: \r\n\r\n */
      if (resp_len >= 4 && resp[resp_len - 4] == '\r' && resp[resp_len - 3] == '\n' &&
          resp[resp_len - 2] == '\r' && resp[resp_len - 1] == '\n')
         break;
   }

   /* Expect "HTTP/1.x 200 ..." */
   if (strncmp(resp, "HTTP/1.", 7) != 0 || resp[9] != '2' || resp[10] != '0' || resp[11] != '0')
   {
      aimee_log(LOG_WARN, "proxy", "CONNECT failed: %.80s", resp);
      return -1;
   }

   aimee_log(LOG_DEBUG, "proxy", "CONNECT tunnel established to %s:%d", target_host, target_port);
   return 0;
}

static int conn_open(http_conn_t *conn, const parsed_url_t *url, int timeout_ms)
{
   conn->ssl = NULL;
   /* Wall-clock deadline: total time allowed for this connection's response read.
    * Cap SO_RCVTIMEO at 30 s per recv call; the deadline catches slow-trickle
    * responses (e.g. models streaming long reasoning chains) that would
    * otherwise reset the per-recv timer on every small chunk. */
   conn->deadline_ns = timeout_ms > 0 ? conn_now_ns() + (int64_t)timeout_ms * 1000000LL : 0;
   /* Per-recv socket timeout: min(timeout_ms, 30000 ms). */
   int rcv_ms = timeout_ms > 0 ? (timeout_ms < 30000 ? timeout_ms : 30000) : 0;

   const proxy_bootstrap_t *pb = proxy_get();
   int use_proxy = pb && pb->has_proxy && url->use_ssl && proxy_should_use(pb, url->host);

   if (use_proxy)
   {
      /* Connect to the proxy, then tunnel to the target via CONNECT */
      conn->fd = connect_with_timeout(pb->https_proxy.host, pb->https_proxy.port, timeout_ms);
      if (conn->fd < 0)
      {
         aimee_log(LOG_WARN, "proxy", "failed to connect to proxy %s:%d", pb->https_proxy.host,
                   pb->https_proxy.port);
         return -1;
      }

      if (rcv_ms > 0)
      {
         struct timeval tv;
         tv.tv_sec = rcv_ms / 1000;
         tv.tv_usec = (rcv_ms % 1000) * 1000;
         setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
         setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      }

      if (proxy_connect_tunnel(conn->fd, url->host, url->port, pb->https_proxy.auth) != 0)
      {
         close(conn->fd);
         conn->fd = -1;
         return -1;
      }
   }
   else
   {
      conn->fd = connect_with_timeout(url->host, url->port, timeout_ms);
      if (conn->fd < 0)
         return -1;

      if (rcv_ms > 0)
      {
         struct timeval tv;
         tv.tv_sec = rcv_ms / 1000;
         tv.tv_usec = (rcv_ms % 1000) * 1000;
         setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
         setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      }
   }

   if (url->use_ssl)
   {
      if (!s_ssl_ctx)
      {
         close(conn->fd);
         conn->fd = -1;
         return -1;
      }
      conn->ssl = SSL_new(s_ssl_ctx);
      if (!conn->ssl)
      {
         close(conn->fd);
         conn->fd = -1;
         return -1;
      }
      SSL_set_fd(conn->ssl, conn->fd);
      SSL_set_tlsext_host_name(conn->ssl, url->host);

      /* Enable hostname verification */
      SSL_set1_host(conn->ssl, url->host);

      if (SSL_connect(conn->ssl) <= 0)
      {
         SSL_free(conn->ssl);
         close(conn->fd);
         conn->fd = -1;
         conn->ssl = NULL;
         return -1;
      }
   }
   return 0;
}

static void conn_close(http_conn_t *conn)
{
   if (conn->ssl)
   {
      SSL_shutdown(conn->ssl);
      SSL_free(conn->ssl);
      conn->ssl = NULL;
   }
   if (conn->fd >= 0)
   {
      close(conn->fd);
      conn->fd = -1;
   }
}

static ssize_t conn_write(http_conn_t *conn, const void *buf, size_t len)
{
   if (conn->ssl)
      return SSL_write(conn->ssl, buf, (int)len);
   return send(conn->fd, buf, len, 0);
}

static ssize_t conn_read(http_conn_t *conn, void *buf, size_t len)
{
   /* SO_RCVTIMEO is capped at 30 s per recv (see conn_open) so a wedged socket
    * can't hang forever, but a slow-but-alive peer -- e.g. a reasoning model
    * that pauses tens of seconds before emitting its first byte -- must not be
    * treated as dead. On a per-recv timeout, keep waiting until the overall
    * response deadline (conn->deadline_ns, the full configured timeout) is
    * actually past. Only a real EOF or error ends the read early. */
   for (;;)
   {
      if (conn->ssl)
      {
         ssize_t n = SSL_read(conn->ssl, buf, (int)len);
         if (n > 0)
            return n;
         int err = SSL_get_error(conn->ssl, (int)n);
         if (err == SSL_ERROR_ZERO_RETURN)
            return 0; /* clean TLS close */
         if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
             (err == SSL_ERROR_SYSCALL &&
              (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)))
         {
            if (conn_past_deadline(conn))
               return -1;
            continue; /* SO_RCVTIMEO fired; peer is slow, not gone */
         }
         return -1; /* real SSL/transport error */
      }
      ssize_t n = recv(conn->fd, buf, len, 0);
      if (n >= 0)
         return n;
      if (errno == EINTR ||
          ((errno == EAGAIN || errno == EWOULDBLOCK) && !conn_past_deadline(conn)))
         continue;
      return -1;
   }
}

/* Write all bytes, retrying short writes */
static int conn_write_all(http_conn_t *conn, const char *buf, size_t len)
{
   while (len > 0)
   {
      ssize_t n = conn_write(conn, buf, len);
      if (n <= 0)
         return -1;
      buf += n;
      len -= (size_t)n;
   }
   return 0;
}

/* ---- HTTP request/response ---- */

/* Build and send an HTTP request. Returns 0 on success, -1 on error. */
static int send_request(http_conn_t *conn, const char *method, const parsed_url_t *url,
                        const char *content_type, const char *auth_header,
                        const char *extra_headers, const char *user_agent, const char *body,
                        size_t body_len)
{
   /* Build request into a dynamic buffer */
   size_t cap = 4096 + body_len;
   char *req = malloc(cap);
   if (!req)
      return -1;

   int off = snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method,
                      url->path, url->host);
   if (off < 0 || (size_t)off >= cap)
      off = (int)(cap - 1);

   if (user_agent)
   {
      int n = snprintf(req + off, cap - (size_t)off, "User-Agent: %s\r\n", user_agent);
      if (n > 0 && (size_t)off + (size_t)n < cap)
         off += n;
   }

   if (content_type)
   {
      int n = snprintf(req + off, cap - (size_t)off, "%s\r\n", content_type);
      if (n > 0 && (size_t)off + (size_t)n < cap)
         off += n;
   }

   if (auth_header && auth_header[0])
   {
      int n = snprintf(req + off, cap - (size_t)off, "%s\r\n", auth_header);
      if (n > 0 && (size_t)off + (size_t)n < cap)
         off += n;
   }

   /* Append newline-separated extra headers */
   if (extra_headers && extra_headers[0])
   {
      char tmp[512];
      snprintf(tmp, sizeof(tmp), "%s", extra_headers);
      char *saveptr;
      char *line = strtok_r(tmp, "\n", &saveptr);
      while (line)
      {
         if (line[0])
         {
            int n = snprintf(req + off, cap - (size_t)off, "%s\r\n", line);
            if (n > 0 && (size_t)off + (size_t)n < cap)
               off += n;
         }
         line = strtok_r(NULL, "\n", &saveptr);
      }
   }

   {
      int n;
      if (body && body_len > 0)
         n = snprintf(req + off, cap - (size_t)off, "Content-Length: %zu\r\n\r\n", body_len);
      else
         n = snprintf(req + off, cap - (size_t)off, "\r\n");
      if (n > 0 && (size_t)off + (size_t)n < cap)
         off += n;
   }

   /* Append body */
   if (body && body_len > 0 && (size_t)off + body_len <= cap)
   {
      memcpy(req + off, body, body_len);
      off += (int)body_len;
   }

   int rc = conn_write_all(conn, req, (size_t)off);
   free(req);
   return rc;
}

/* Parse status line, return HTTP status code. Advances *buf past headers.
 * Sets *chunked=1 if Transfer-Encoding: chunked, else sets *content_length. */
static int parse_response_headers(char *buf, size_t len, size_t *header_end, int *chunked,
                                  size_t *content_length)
{
   *chunked = 0;
   *content_length = 0;
   *header_end = 0;

   /* Find end of headers */
   char *hdr_end = strstr(buf, "\r\n\r\n");
   if (!hdr_end)
      return -1;

   *header_end = (size_t)(hdr_end - buf) + 4;

   /* Parse status code from "HTTP/1.x NNN ..." */
   int status = 0;
   if (len < 12 || (strncmp(buf, "HTTP/1.0", 8) != 0 && strncmp(buf, "HTTP/1.1", 8) != 0))
      return -1;

   status = atoi(buf + 9);
   if (status < 100 || status > 999)
      return -1;

   /* Null-terminate headers for easy searching */
   *hdr_end = '\0';

   /* Check for Transfer-Encoding: chunked (case-insensitive scan) */
   for (char *p = buf; *p; p++)
   {
      if ((*p == 'T' || *p == 't') && strncasecmp(p, "Transfer-Encoding:", 18) == 0)
      {
         char *val = p + 18;
         while (*val == ' ')
            val++;
         if (strncasecmp(val, "chunked", 7) == 0)
            *chunked = 1;
         break;
      }
   }

   /* Check for Content-Length */
   for (char *p = buf; *p; p++)
   {
      if ((*p == 'C' || *p == 'c') && strncasecmp(p, "Content-Length:", 15) == 0)
      {
         *content_length = (size_t)atoll(p + 15);
         break;
      }
   }

   /* Restore the header terminator */
   *hdr_end = '\r';
   return status;
}

/* Read a full HTTP response (headers + body) into *out_body, returning status code.
 * For buffered (non-streaming) reads. */
static int http_read_response(http_conn_t *conn, char **out_body, size_t *out_len)
{
   *out_body = NULL;
   *out_len = 0;

   /* Read into a growing buffer until we have all headers + body */
   size_t cap = 8192, len = 0;
   char *buf = malloc(cap);
   if (!buf)
      return -1;

   /* Phase 1: read until we have complete headers */
   int headers_done = 0;
   size_t header_end = 0;
   int chunked = 0;
   size_t content_length = 0;
   int status = -1;

   while (!headers_done)
   {
      if (len + 4096 > cap)
      {
         cap *= 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
         {
            free(buf);
            return -1;
         }
         buf = tmp;
      }

      ssize_t n = conn_read(conn, buf + len, cap - len - 1);
      if (n <= 0)
      {
         if (len > 0)
            break; /* might have enough */
         free(buf);
         return -1;
      }
      len += (size_t)n;
      buf[len] = '\0';

      if (strstr(buf, "\r\n\r\n"))
         headers_done = 1;
   }

   status = parse_response_headers(buf, len, &header_end, &chunked, &content_length);
   if (status < 0)
   {
      free(buf);
      return -1;
   }

   /* Phase 2: read body */
   if (chunked)
   {
      /* Decode chunked transfer encoding */
      char *body = NULL;
      size_t body_len = 0;

      /* Start from data after headers */
      size_t pos = header_end;

      for (;;)
      {
         /* Ensure we have data to parse chunk size from */
         while (pos >= len || !strstr(buf + pos, "\r\n"))
         {
            if (len + 4096 > cap)
            {
               if (cap > HTTP_MAX_RESPONSE_SIZE)
               {
                  free(body);
                  free(buf);
                  return -1;
               }
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(body);
                  free(buf);
                  return -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               break;
            len += (size_t)n;
            buf[len] = '\0';
            if (conn_past_deadline(conn))
            {
               aimee_log(LOG_WARN, "agent_http", "response deadline exceeded reading chunk header");
               free(body);
               free(buf);
               return -1;
            }
         }

         /* Parse chunk size */
         char *crlf = strstr(buf + pos, "\r\n");
         if (!crlf)
            break;

         size_t chunk_size = (size_t)strtoul(buf + pos, NULL, 16);
         pos = (size_t)(crlf - buf) + 2;

         if (chunk_size == 0)
            break; /* final chunk */

         /* Overflow-safe combined size check: reject before reading.
          * chunk_size > MAX is the simple case; the subtraction form
          * body_len > MAX - chunk_size avoids body_len + chunk_size wrap. */
         if (chunk_size > HTTP_MAX_RESPONSE_SIZE || body_len > HTTP_MAX_RESPONSE_SIZE - chunk_size)
         {
            free(body);
            free(buf);
            return -1;
         }

         /* Read until we have the full chunk + its trailing CRLF.
          * chunk_size is now bounded by HTTP_MAX_RESPONSE_SIZE so
          * chunk_size + 2 cannot overflow. */
         while (len - pos < chunk_size + 2)
         {
            if (len + 4096 > cap)
            {
               if (cap > HTTP_MAX_RESPONSE_SIZE)
               {
                  free(body);
                  free(buf);
                  return -1;
               }
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(body);
                  free(buf);
                  return -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               break;
            len += (size_t)n;
            buf[len] = '\0';
            if (conn_past_deadline(conn))
            {
               aimee_log(LOG_WARN, "agent_http", "response deadline exceeded reading chunk body");
               free(body);
               free(buf);
               return -1;
            }
         }

         char *tmp = realloc(body, body_len + chunk_size + 1);
         if (!tmp)
         {
            free(body);
            free(buf);
            return -1;
         }
         body = tmp;
         memcpy(body + body_len, buf + pos, chunk_size);
         body_len += chunk_size;
         body[body_len] = '\0';

         pos += chunk_size + 2; /* skip chunk data + trailing CRLF */
      }

      free(buf);
      *out_body = body;
      *out_len = body_len;
   }
   else
   {
      /* Content-Length or read-until-close */
      size_t body_so_far = len - header_end;
      size_t target = content_length > 0 ? content_length : 0;

      if (content_length > 0)
      {
         /* Read remaining body bytes */
         while (body_so_far < target)
         {
            if (len + 4096 > cap)
            {
               if (cap > HTTP_MAX_RESPONSE_SIZE)
               {
                  free(buf);
                  return -1;
               }
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(buf);
                  return -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               break;
            len += (size_t)n;
            body_so_far += (size_t)n;
            buf[len] = '\0';
            if (conn_past_deadline(conn))
            {
               aimee_log(LOG_WARN, "agent_http", "response deadline exceeded reading body");
               free(buf);
               return -1;
            }
         }
      }
      else
      {
         /* Read until connection close */
         for (;;)
         {
            if (len + 4096 > cap)
            {
               if (cap > HTTP_MAX_RESPONSE_SIZE)
               {
                  free(buf);
                  return -1;
               }
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(buf);
                  return -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               break;
            len += (size_t)n;
            buf[len] = '\0';
            if (conn_past_deadline(conn))
            {
               aimee_log(LOG_WARN, "agent_http", "response deadline exceeded reading body");
               free(buf);
               return -1;
            }
         }
      }

      /* Extract body */
      size_t blen = len - header_end;
      char *body = malloc(blen + 1);
      if (!body)
      {
         free(buf);
         return -1;
      }
      memcpy(body, buf + header_end, blen);
      body[blen] = '\0';
      free(buf);

      *out_body = body;
      *out_len = blen;
   }

   return status;
}

/* Streaming response reader: reads headers, then delivers body chunks via callback.
 * Handles both chunked and content-length/close modes. */
static int http_read_response_stream(http_conn_t *conn, agent_http_stream_cb callback,
                                     void *userdata)
{
   size_t cap = 8192, len = 0;
   char *buf = malloc(cap);
   if (!buf)
      return -1;

   /* Read headers */
   int headers_done = 0;
   while (!headers_done)
   {
      if (len + 4096 > cap)
      {
         cap *= 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
         {
            free(buf);
            return -1;
         }
         buf = tmp;
      }
      ssize_t n = conn_read(conn, buf + len, cap - len - 1);
      if (n <= 0)
      {
         free(buf);
         return -1;
      }
      len += (size_t)n;
      buf[len] = '\0';
      if (strstr(buf, "\r\n\r\n"))
         headers_done = 1;
   }

   size_t header_end = 0;
   int chunked = 0;
   size_t content_length = 0;
   int status = parse_response_headers(buf, len, &header_end, &chunked, &content_length);
   if (status < 0)
   {
      free(buf);
      return -1;
   }

   /* Deliver any body data already read past the headers */
   size_t leftover = len - header_end;
   int aborted = 0;

   if (chunked)
   {
      /* For chunked streaming, we need to parse chunk boundaries and deliver decoded data */
      /* Move leftover to start of buffer */
      memmove(buf, buf + header_end, leftover);
      len = leftover;

      for (;;)
      {
         /* Ensure we have a chunk header */
         buf[len] = '\0';
         while (!strstr(buf, "\r\n"))
         {
            if (len + 4096 > cap)
            {
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(buf);
                  return aborted ? status : -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               goto stream_done;
            len += (size_t)n;
            buf[len] = '\0';
            if (conn_past_deadline(conn))
            {
               aimee_log(LOG_WARN, "agent_http", "stream deadline exceeded reading chunk header");
               goto stream_done;
            }
         }

         char *crlf = strstr(buf, "\r\n");
         size_t chunk_size = (size_t)strtoul(buf, NULL, 16);
         size_t hdr_len = (size_t)(crlf - buf) + 2;

         /* Remove chunk header from buffer */
         len -= hdr_len;
         memmove(buf, buf + hdr_len, len);

         if (chunk_size == 0)
            break;

         /* Read and deliver this chunk */
         size_t delivered = 0;
         while (delivered < chunk_size)
         {
            size_t avail = len < (chunk_size - delivered) ? len : (chunk_size - delivered);
            if (avail > 0)
            {
               if (callback(buf, avail, userdata) != 0)
               {
                  aborted = 1;
                  goto stream_done;
               }
               delivered += avail;
               len -= avail;
               memmove(buf, buf + avail, len);
            }
            if (delivered < chunk_size)
            {
               ssize_t n = conn_read(conn, buf + len, cap - len - 1);
               if (n <= 0)
                  goto stream_done;
               len += (size_t)n;
               if (conn_past_deadline(conn))
               {
                  aimee_log(LOG_WARN, "agent_http", "stream deadline exceeded reading chunk body");
                  goto stream_done;
               }
            }
         }

         /* Skip trailing CRLF after chunk data */
         while (len < 2)
         {
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               goto stream_done;
            len += (size_t)n;
         }
         len -= 2;
         memmove(buf, buf + 2, len);
      }
   }
   else
   {
      /* Deliver leftover from header read */
      if (leftover > 0)
      {
         if (callback(buf + header_end, leftover, userdata) != 0)
            aborted = 1;
      }

      if (!aborted)
      {
         /* Read remaining body and deliver */
         char chunk[8192];
         for (;;)
         {
            ssize_t n = conn_read(conn, chunk, sizeof(chunk));
            if (n <= 0)
               break;
            if (conn_past_deadline(conn))
            {
               aimee_log(LOG_WARN, "agent_http", "stream deadline exceeded reading body");
               aborted = 1;
               break;
            }
            if (callback(chunk, (size_t)n, userdata) != 0)
            {
               aborted = 1;
               break;
            }
         }
      }
   }

stream_done:
   free(buf);
   return status;
}

/* ---- Public API ---- */

int agent_http_get(const char *url, const char *extra_headers, char **response_buf, int timeout_ms)
{
   *response_buf = NULL;

   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "GET", &pu, NULL, NULL, extra_headers, "aimee/1.0", NULL, 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   size_t resp_len = 0;
   int status = http_read_response(&conn, response_buf, &resp_len);
   conn_close(&conn);

   if (status < 0)
   {
      free(*response_buf);
      *response_buf = NULL;
   }
   return status;
}

int agent_http_get_stream(const char *url, const char *extra_headers, agent_http_stream_cb callback,
                          void *userdata, int timeout_ms)
{
   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "GET", &pu, NULL, NULL, extra_headers, "aimee/1.0", NULL, 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   int status = http_read_response_stream(&conn, callback, userdata);
   conn_close(&conn);
   return status;
}

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   return agent_http_post_content_type(url, auth_header, "Content-Type: application/json", body,
                                       response_buf, timeout_ms, extra_headers);
}

int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers)
{
   *response_buf = NULL;

   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   aimee_log(LOG_DEBUG, "agent_http", "connecting to %s:%d (timeout %dms)", pu.host, pu.port,
             timeout_ms);

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
   {
      aimee_log(LOG_ERROR, "agent_http", "TCP connect failed: %s:%d", pu.host, pu.port);
      return -1;
   }

   aimee_log(LOG_DEBUG, "agent_http", "connected; sending request to %s:%d (%zu body bytes)",
             pu.host, pu.port, body ? strlen(body) : (size_t)0);

   const char *ct =
       content_type && content_type[0] ? content_type : "Content-Type: application/json";
   if (send_request(&conn, "POST", &pu, ct, auth_header, extra_headers, "aimee/1.0", body,
                    body ? strlen(body) : 0) < 0)
   {
      aimee_log(LOG_ERROR, "agent_http", "send_request failed: %s:%d", pu.host, pu.port);
      conn_close(&conn);
      return -1;
   }

   aimee_log(LOG_DEBUG, "agent_http", "request sent; waiting for response from %s:%d", pu.host,
             pu.port);

   size_t resp_len = 0;
   int status = http_read_response(&conn, response_buf, &resp_len);
   conn_close(&conn);

   if (status < 0)
   {
      free(*response_buf);
      *response_buf = NULL;
      aimee_log(LOG_ERROR, "agent_http", "read_response failed: %s:%d", pu.host, pu.port);
   }
   else
   {
      aimee_log(LOG_DEBUG, "agent_http", "response received: HTTP %d, %zu bytes from %s:%d", status,
                resp_len, pu.host, pu.port);
   }
   return status;
}

int agent_http_put(const char *url, const char *auth_header, const char *body, char **response_buf,
                   int timeout_ms, const char *extra_headers)
{
   *response_buf = NULL;

   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "PUT", &pu, "Content-Type: application/json", auth_header, extra_headers,
                    "aimee/1.0", body, body ? strlen(body) : 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   size_t resp_len = 0;
   int status = http_read_response(&conn, response_buf, &resp_len);
   conn_close(&conn);

   if (status < 0)
   {
      free(*response_buf);
      *response_buf = NULL;
      aimee_log(LOG_ERROR, "agent_http", "request failed");
   }
   return status;
}

int agent_http_post_form(const char *url, const char *body, char **response_buf, int timeout_ms)
{
   *response_buf = NULL;

   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "POST", &pu, "Content-Type: application/x-www-form-urlencoded", NULL,
                    "originator: codex_cli_rs", "codex_cli/1.0", body, body ? strlen(body) : 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   size_t resp_len = 0;
   int status = http_read_response(&conn, response_buf, &resp_len);
   conn_close(&conn);

   if (status < 0)
   {
      free(*response_buf);
      *response_buf = NULL;
   }
   return status;
}

int agent_http_post_stream(const char *url, const char *auth_header, const char *body,
                           agent_http_stream_cb callback, void *userdata, int timeout_ms,
                           const char *extra_headers)
{
   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "POST", &pu, "Content-Type: application/json", auth_header,
                    extra_headers, "aimee/1.0", body, body ? strlen(body) : 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   int status = http_read_response_stream(&conn, callback, userdata);
   conn_close(&conn);
   return status;
}

int agent_http_delete(const char *url, const char *auth_header, int timeout_ms)
{
   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "DELETE", &pu, NULL, auth_header, NULL, "aimee/1.0", NULL, 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   char *response = NULL;
   size_t resp_len = 0;
   int status = http_read_response(&conn, &response, &resp_len);
   conn_close(&conn);
   free(response);
   return status;
}

/* ================================================================
 * From: agent_tunnel.c
 * ================================================================ */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "agent_tunnel.h"
#include "cJSON.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef AIMEE_POSIX
#include <sys/wait.h>
#endif

/* --- State string conversion --- */

const char *agent_tunnel_state_str(agent_tunnel_state_t state)
{
   switch (state)
   {
   case TUNNEL_STATE_IDLE:
      return "idle";
   case TUNNEL_STATE_CONNECTING:
      return "connecting";
   case TUNNEL_STATE_ACTIVE:
      return "active";
   case TUNNEL_STATE_RECONNECTING:
      return "reconnecting";
   case TUNNEL_STATE_FAILED:
      return "failed";
   case TUNNEL_STATE_STOPPED:
      return "stopped";
   }
   return "unknown";
}

/* --- Internal: parse allocated port from ssh stderr --- */

static int parse_allocated_port(int fd, int *port_out, int timeout_ms)
{
   char buf[4096];
   size_t pos = 0;
   struct pollfd pfd = {.fd = fd, .events = POLLIN};

   while (pos < sizeof(buf) - 1)
   {
      int ret = poll(&pfd, 1, timeout_ms);
      if (ret <= 0)
         return -1; /* timeout or error */

      ssize_t n = read(fd, buf + pos, sizeof(buf) - 1 - pos);
      if (n <= 0)
         return -1;
      pos += (size_t)n;
      buf[pos] = '\0';

      /* Look for "Allocated port NNNNN for remote forward" */
      const char *marker = strstr(buf, "Allocated port ");
      if (marker)
      {
         int port = atoi(marker + 15);
         if (port > 0 && port < 65536)
         {
            *port_out = port;
            return 0;
         }
      }
   }
   return -1;
}

/* --- Internal: extract user@host from relay_ssh string --- */

static void extract_relay_target(const char *relay_ssh, char *user_host, size_t len)
{
   /* relay_ssh is like "ssh [-p PORT] [-i KEY] user@host" -- we want the last arg */
   const char *last_space = strrchr(relay_ssh, ' ');
   if (last_space)
      snprintf(user_host, len, "%s", last_space + 1);
   else
      snprintf(user_host, len, "%s", relay_ssh);
}

/* --- Internal: fork/exec ssh -R --- */

static int tunnel_fork_ssh(agent_tunnel_t *t)
{
   int stderr_pipe[2];
   if (pipe(stderr_pipe) < 0)
   {
      snprintf(t->error, sizeof(t->error), "pipe: %s", strerror(errno));
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      snprintf(t->error, sizeof(t->error), "fork: %s", strerror(errno));
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return -1;
   }

   if (pid == 0)
   {
      /* Child: exec ssh */
      close(stderr_pipe[0]);
      dup2(stderr_pipe[1], STDERR_FILENO);
      close(stderr_pipe[1]);

      /* Redirect stdout to /dev/null */
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0)
      {
         dup2(devnull, STDOUT_FILENO);
         dup2(devnull, STDIN_FILENO);
         if (devnull > 2)
            close(devnull);
      }

      /* Build argv */
      char remote_spec[256];
      snprintf(remote_spec, sizeof(remote_spec), "0:%s:%d", t->target_host, t->target_port);

      /* Parse relay_ssh to extract args. Simple approach: tokenize by space.
       * relay_ssh is like "ssh -p 22 relay@host" or "ssh relay@host" */
      char relay_copy[512];
      snprintf(relay_copy, sizeof(relay_copy), "%s", t->relay_ssh);

      const char *argv[32];
      int argc = 0;

      char *tok = strtok(relay_copy, " ");
      while (tok && argc < 20)
      {
         argv[argc++] = tok;
         tok = strtok(NULL, " ");
      }

      /* If first arg is "ssh", skip it (we'll use execvp("ssh", ...)) */
      int start = 0;
      if (argc > 0 && strcmp(argv[0], "ssh") == 0)
         start = 1;

      const char *exec_argv[48];
      int ei = 0;
      exec_argv[ei++] = "ssh";

      /* Copy relay args (skipping "ssh" if present) */
      for (int i = start; i < argc && ei < 32; i++)
         exec_argv[ei++] = argv[i];

      /* Add key if configured */
      if (t->relay_key[0])
      {
         exec_argv[ei++] = "-i";
         exec_argv[ei++] = t->relay_key;
      }

      /* Add tunnel options */
      exec_argv[ei++] = "-o";
      exec_argv[ei++] = "ServerAliveInterval=15";
      exec_argv[ei++] = "-o";
      exec_argv[ei++] = "ServerAliveCountMax=3";
      exec_argv[ei++] = "-o";
      exec_argv[ei++] = "ExitOnForwardFailure=yes";
      exec_argv[ei++] = "-o";
      exec_argv[ei++] = "StrictHostKeyChecking=accept-new";
      exec_argv[ei++] = "-v"; /* verbose -- needed for "Allocated port" message */
      exec_argv[ei++] = "-N"; /* no remote command */
      exec_argv[ei++] = "-R";
      exec_argv[ei++] = remote_spec;
      exec_argv[ei] = NULL;

      execvp("ssh", (char *const *)exec_argv);
      _exit(127);
   }

   /* Parent */
   close(stderr_pipe[1]);
   t->ssh_pid = pid;

   /* Wait for port allocation (15 second timeout) */
   int port = 0;
   if (parse_allocated_port(stderr_pipe[0], &port, 15000) == 0)
   {
      t->allocated_port = port;

      /* Build effective entry: "ssh -p PORT user@relay" */
      char relay_target[256];
      extract_relay_target(t->relay_ssh, relay_target, sizeof(relay_target));
      snprintf(t->effective_entry, sizeof(t->effective_entry), "ssh -p %d %s", port, relay_target);

      t->state = TUNNEL_STATE_ACTIVE;
      t->error[0] = '\0';
      close(stderr_pipe[0]);
      return 0;
   }

   /* Port discovery failed */
   close(stderr_pipe[0]);
   snprintf(t->error, sizeof(t->error), "port allocation timeout");
   kill(pid, SIGTERM);
   waitpid(pid, NULL, 0);
   t->ssh_pid = 0;
   return -1;
}

/* --- Monitor thread --- */

typedef struct
{
   agent_tunnel_mgr_t *mgr;
   int index;
} monitor_arg_t;

static void *tunnel_monitor_thread(void *arg)
{
   monitor_arg_t *ma = (monitor_arg_t *)arg;
   agent_tunnel_mgr_t *mgr = ma->mgr;
   int idx = ma->index;
   free(ma);

   while (!mgr->shutdown)
   {
      agent_tunnel_t *t = &mgr->tunnels[idx];

      if (t->ssh_pid > 0)
      {
         int status = 0;
         pid_t w = waitpid(t->ssh_pid, &status, WNOHANG);
         if (w > 0)
         {
            /* SSH process exited */
            pthread_mutex_lock(&mgr->lock);
            t->ssh_pid = 0;
            t->allocated_port = 0;
            t->effective_entry[0] = '\0';

            if (mgr->shutdown)
            {
               t->state = TUNNEL_STATE_STOPPED;
               pthread_mutex_unlock(&mgr->lock);
               break;
            }

            /* Attempt reconnect */
            t->reconnect_count++;
            if (t->max_reconnects > 0 && t->reconnect_count > t->max_reconnects)
            {
               t->state = TUNNEL_STATE_FAILED;
               snprintf(t->error, sizeof(t->error), "max reconnects exceeded (%d)",
                        t->max_reconnects);
               aimee_log(LOG_ERROR, "tunnel", "'%s': %s", t->name, t->error);
               pthread_mutex_unlock(&mgr->lock);
               break;
            }

            t->state = TUNNEL_STATE_RECONNECTING;
            aimee_log(LOG_INFO, "tunnel", "'%s': reconnecting (attempt %d)...", t->name,
                      t->reconnect_count);
            pthread_mutex_unlock(&mgr->lock);

            int delay = t->reconnect_delay_s > 0 ? t->reconnect_delay_s : 5;
            for (int i = 0; i < delay && !mgr->shutdown; i++)
               sleep(1);

            if (mgr->shutdown)
               break;

            pthread_mutex_lock(&mgr->lock);
            t->state = TUNNEL_STATE_CONNECTING;
            pthread_mutex_unlock(&mgr->lock);

            if (tunnel_fork_ssh(t) != 0)
            {
               pthread_mutex_lock(&mgr->lock);
               t->state = TUNNEL_STATE_FAILED;
               aimee_log(LOG_ERROR, "tunnel", "'%s': reconnect failed: %s", t->name, t->error);
               pthread_mutex_unlock(&mgr->lock);
            }
            else
            {
               pthread_mutex_lock(&mgr->lock);
               t->reconnect_count = 0; /* reset on success */
               aimee_log(LOG_INFO, "tunnel", "'%s': reconnected on port %d", t->name,
                         t->allocated_port);
               pthread_mutex_unlock(&mgr->lock);
            }
         }
      }

      /* Poll every 2 seconds */
      for (int i = 0; i < 4 && !mgr->shutdown; i++)
         usleep(500000);
   }

   return NULL;
}

/* --- Public API --- */

void agent_tunnel_mgr_init(agent_tunnel_mgr_t *mgr)
{
   pthread_mutex_init(&mgr->lock, NULL);
   mgr->shutdown = 0;
   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      mgr->tunnels[i].state = TUNNEL_STATE_IDLE;
      mgr->tunnels[i].ssh_pid = 0;
      mgr->tunnels[i].allocated_port = 0;
      mgr->tunnels[i].reconnect_count = 0;
      mgr->tunnels[i].error[0] = '\0';
      mgr->tunnels[i].effective_entry[0] = '\0';
   }
}

int agent_tunnel_start_all(agent_tunnel_mgr_t *mgr)
{
   if (mgr->tunnel_count == 0)
      return -1;

   int started = 0;
   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      agent_tunnel_t *t = &mgr->tunnels[i];
      t->state = TUNNEL_STATE_CONNECTING;
      aimee_log(LOG_INFO, "tunnel", "'%s': connecting to %s (target %s:%d)...", t->name,
                t->relay_ssh, t->target_host, t->target_port);

      if (tunnel_fork_ssh(t) == 0)
      {
         aimee_log(LOG_INFO, "tunnel", "'%s': active on port %d", t->name, t->allocated_port);

         /* Spawn monitor thread */
         monitor_arg_t *ma = malloc(sizeof(monitor_arg_t));
         if (ma)
         {
            ma->mgr = mgr;
            ma->index = i;
            pthread_create(&t->monitor_thread, NULL, tunnel_monitor_thread, ma);
         }
         started++;
      }
      else
      {
         t->state = TUNNEL_STATE_FAILED;
         aimee_log(LOG_ERROR, "tunnel", "'%s': failed: %s", t->name, t->error);
      }
   }

   return started > 0 ? 0 : -1;
}

void agent_tunnel_stop_all(agent_tunnel_mgr_t *mgr)
{
   mgr->shutdown = 1;

   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      agent_tunnel_t *t = &mgr->tunnels[i];
      if (t->ssh_pid > 0)
      {
         kill(t->ssh_pid, SIGTERM);
         waitpid(t->ssh_pid, NULL, 0);
         t->ssh_pid = 0;
      }
   }

   /* Join monitor threads */
   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      agent_tunnel_t *t = &mgr->tunnels[i];
      if (t->state != TUNNEL_STATE_IDLE && t->state != TUNNEL_STATE_FAILED)
         pthread_join(t->monitor_thread, NULL);
      t->state = TUNNEL_STATE_STOPPED;
      t->allocated_port = 0;
      t->effective_entry[0] = '\0';
   }
}

void agent_tunnel_mgr_destroy(agent_tunnel_mgr_t *mgr)
{
   agent_tunnel_stop_all(mgr);
   pthread_mutex_destroy(&mgr->lock);
}

agent_tunnel_t *agent_tunnel_find(agent_tunnel_mgr_t *mgr, const char *name)
{
   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      if (strcmp(mgr->tunnels[i].name, name) == 0)
         return &mgr->tunnels[i];
   }
   return NULL;
}

int agent_tunnel_resolve_entry(const agent_tunnel_mgr_t *mgr, const agent_network_t *network,
                               const agent_net_host_t *host, char *buf, size_t buf_len)
{
   if (mgr && host->tunnel[0])
   {
      for (int i = 0; i < mgr->tunnel_count; i++)
      {
         const agent_tunnel_t *t = &mgr->tunnels[i];
         if (strcmp(t->name, host->tunnel) == 0 && t->state == TUNNEL_STATE_ACTIVE &&
             t->effective_entry[0])
         {
            snprintf(buf, buf_len, "%s", t->effective_entry);
            return 1;
         }
      }
   }

   /* Fallback to network ssh_entry */
   if (network && network->ssh_entry[0])
      snprintf(buf, buf_len, "%s", network->ssh_entry);
   else
      buf[0] = '\0';
   return 0;
}

void agent_tunnel_print_status(const agent_tunnel_mgr_t *mgr, int json_output)
{
   if (json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < mgr->tunnel_count; i++)
      {
         const agent_tunnel_t *t = &mgr->tunnels[i];
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "name", t->name);
         cJSON_AddStringToObject(obj, "state", agent_tunnel_state_str(t->state));
         cJSON_AddStringToObject(obj, "relay", t->relay_ssh);
         cJSON_AddStringToObject(obj, "target_host", t->target_host);
         cJSON_AddNumberToObject(obj, "target_port", t->target_port);
         cJSON_AddNumberToObject(obj, "allocated_port", t->allocated_port);
         if (t->error[0])
            cJSON_AddStringToObject(obj, "error", t->error);
         cJSON_AddItemToArray(arr, obj);
      }
      char *json = cJSON_Print(arr);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(arr);
   }
   else
   {
      if (mgr->tunnel_count == 0)
      {
         printf("No tunnels configured.\n");
         return;
      }
      printf("%-12s %-12s %-30s %-20s %s\n", "TUNNEL", "STATE", "RELAY", "TARGET", "PORT");
      for (int i = 0; i < mgr->tunnel_count; i++)
      {
         const agent_tunnel_t *t = &mgr->tunnels[i];
         char target[80];
         snprintf(target, sizeof(target), "%s:%d", t->target_host, t->target_port);
         printf("%-12s %-12s %-30s %-20s %d\n", t->name, agent_tunnel_state_str(t->state),
                t->relay_ssh, target, t->allocated_port);
      }
   }
}
