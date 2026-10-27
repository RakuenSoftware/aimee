/* The shared thinclient HTTP owner. Domain payloads remain opaque bytes. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cli_client.h"
#include "aimee_home.h"
#include "cJSON.h"
#include "json_wire.h"
#include <aimee/ir/aimee_ir.h>
#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/endpoint.h>
#include <aimee/core/connection/http1.h>
#include <aimee/core/connection/socket.h>
#ifdef WITH_TLS
#include <aimee/core/connection/native_tls.h>
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#endif

cli_transport_t cli_transport_parse(const char *s)
{
   if (!s || !s[0])
      return CLI_TRANSPORT_SOCKET;
   if (strcmp(s, "http") == 0)
      return CLI_TRANSPORT_HTTP;
   if (strcmp(s, "auto") == 0)
      return CLI_TRANSPORT_AUTO;
   return CLI_TRANSPORT_SOCKET; /* "socket" and any unrecognized value */
}

/* cli_v1_route_for_method / cli_v1_pathid_route_for_method now live in the shared
 * cli_v1_routes.inc (included by both posix/ and windows/cli_client.c) so the
 * Windows thin client uses the same first-class /v1 routing. */

/* Percent-encode `in` into `out` (cap incl. NUL): RFC3986 unreserved bytes pass
 * through, everything else (incl. '/') is %XX-encoded so a workspace path
 * survives as one URL segment. The server's ws_pct_decode reverses it. Returns 0
 * on success, -1 if the result would overflow. */
int cli_v1_pct_encode(const char *in, char *out, size_t cap)
{
   static const char *hex = "0123456789ABCDEF";
   size_t o = 0;
   for (size_t i = 0; in && in[i]; i++)
   {
      unsigned char ch = (unsigned char)in[i];
      int unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                       (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
      if (unreserved)
      {
         if (o + 1 >= cap)
            return -1;
         out[o++] = (char)ch;
      }
      else
      {
         if (o + 3 >= cap)
            return -1;
         out[o++] = '%';
         out[o++] = hex[ch >> 4];
         out[o++] = hex[ch & 0xF];
      }
   }
   if (o >= cap)
      return -1;
   out[o] = '\0';
   return 0;
}

/* cli_v1_pathid_route_for_method moved to the shared cli_v1_routes.inc. */

char *cli_http_endpoint_normalize(const char *ep)
{
   if (!ep || !ep[0])
      return NULL;
   const char *scheme = NULL;
   const char *rest = NULL;
   if (strncmp(ep, "https://", 8) == 0)
   {
      scheme = "tls:";
      rest = ep + 8;
   }
   else if (strncmp(ep, "http://", 7) == 0)
   {
      scheme = "tcp:";
      rest = ep + 7;
   }
   if (!scheme)
      return strdup(ep); /* tcp:/tls:/unix:/host:port already */
   /* Take host[:port], dropping any trailing path; default the port by scheme. */
   char hostport[300];
   size_t n = strcspn(rest, "/");
   if (n >= sizeof(hostport))
      n = sizeof(hostport) - 1;
   memcpy(hostport, rest, n);
   hostport[n] = '\0';
   /* Strip any userinfo ("user:pass@host") so it can't corrupt the host:port the
    * TLS/connect layer parses; credentials belong in the bearer, not the URL. */
   char *at = strrchr(hostport, '@');
   char *hp = at ? at + 1 : hostport;
   char out[320];
   if (strchr(hp, ':'))
      snprintf(out, sizeof(out), "%s%s", scheme, hp);
   else
      snprintf(out, sizeof(out), "%s%s:%s", scheme, hp, strcmp(scheme, "tls:") == 0 ? "443" : "80");
   return strdup(out);
}

#define HEADER_LIMIT (64u * 1024u)
struct cli_http_stream
{
   _Atomic int fd, cancelled;
   int opened, timeout, done, chunked;
   void *tls;
   char *headers;
   int status;
   uint64_t remaining, chunk_remaining;
   int has_length, chunk_crlf;
   unsigned char pending[8192];
   size_t at, end;
};

int cli_remote_read_at_home(const char *home, char *url, size_t url_size, char *token,
                            size_t token_size)
{
   if (url_size)
      url[0] = 0;
   if (token_size)
      token[0] = 0;
   char path[4096];
   if (!home || snprintf(path, sizeof(path), "%s/remote.conf", home) >= (int)sizeof(path))
      return 0;
   FILE *file = fopen(path, "r");
   if (!file)
      return 0;
   if (url_size && fgets(url, (int)url_size, file))
      url[strcspn(url, "\r\n")] = 0;
   if (token_size && fgets(token, (int)token_size, file))
      token[strcspn(token, "\r\n")] = 0;
   fclose(file);
   return url_size && url[0];
}
int cli_http_profile_resolve(const char *client_home, const char *profile, cli_http_profile_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   const char *home = client_home ? client_home : aimee_home_profile(profile);
   if (!home || !(out->tls_home = strdup(home)))
      return -1;
   char url[512], token[256];
   cli_remote_read_at_home(home, url, sizeof(url), token, sizeof(token));
   const char *endpoint = getenv("AIMEE_API_ENDPOINT");
   const char *server = getenv("AIMEE_SERVER_URL");
   const char *bearer = getenv("AIMEE_API_BEARER");
   const char *server_token = getenv("AIMEE_SERVER_TOKEN");
   if (!endpoint || !endpoint[0])
      endpoint = server && server[0] ? server : url;
   if (!bearer || !bearer[0])
      bearer = server_token && server_token[0] ? server_token : token;
   if (endpoint && endpoint[0])
      out->endpoint = cli_http_endpoint_normalize(endpoint);
   if (bearer && bearer[0])
      out->bearer = strdup(bearer);
   if ((endpoint && endpoint[0] && !out->endpoint) || (bearer && bearer[0] && !out->bearer))
   {
      cli_http_profile_free(out);
      return -1;
   }
   return 0;
}
void cli_http_profile_free(cli_http_profile_t *profile)
{
   if (!profile)
      return;
   if (profile->bearer)
      for (volatile char *p = profile->bearer; *p; ++p)
         *p = 0;
   free(profile->endpoint);
   free(profile->bearer);
   free(profile->tls_home);
   memset(profile, 0, sizeof(*profile));
}
cli_http_stream_t *cli_http_stream_new(void)
{
   cli_http_stream_t *s = calloc(1, sizeof(*s));
   if (s)
   {
      atomic_init(&s->fd, -1);
      atomic_init(&s->cancelled, 0);
   }
   return s;
}
static int cancelled(void *context)
{
   return atomic_load(&((cli_http_stream_t *)context)->cancelled);
}
void cli_http_stream_cancel(cli_http_stream_t *s)
{
   if (!s)
      return;
   atomic_store(&s->cancelled, 1);
   int fd = atomic_load(&s->fd);
   if (fd >= 0)
      aimee_core_socket_shutdown(fd);
}
void cli_http_stream_free(cli_http_stream_t *s)
{
   if (!s)
      return;
   cli_http_stream_cancel(s);
#ifdef WITH_TLS
   if (s->tls)
      aimee_tls_free(s->tls);
#endif
   int fd = atomic_exchange(&s->fd, -1);
   if (fd >= 0)
      aimee_core_socket_close(fd);
   free(s->headers);
   free(s);
}
int cli_http_stream_status(const cli_http_stream_t *s)
{
   return s ? s->status : 0;
}
const char *cli_http_stream_headers(const cli_http_stream_t *s)
{
   return s ? s->headers : NULL;
}
static long receive(cli_http_stream_t *s, void *buffer, size_t length)
{
   if (cancelled(s))
      return -1;
#ifdef WITH_TLS
   if (s->tls)
      return aimee_tls_read(s->tls, buffer, length);
#endif
   aimee_core_control_t control;
   if (aimee_core_control_init_timeout(&control, s->timeout, 50, cancelled, s))
      return -1;
   if (aimee_core_wait_fd(atomic_load(&s->fd), AIMEE_CORE_WAIT_READ, &control))
      return -1;
   return aimee_core_socket_read(atomic_load(&s->fd), buffer, length);
}
static long buffered(cli_http_stream_t *s, void *buffer, size_t length)
{
   if (s->at < s->end)
   {
      size_t n = s->end - s->at;
      if (n > length)
         n = length;
      memcpy(buffer, s->pending + s->at, n);
      s->at += n;
      return (long)n;
   }
   return receive(s, buffer, length);
}
static int byte(cli_http_stream_t *s)
{
   if (s->at == s->end)
   {
      long n = receive(s, s->pending, sizeof(s->pending));
      if (n <= 0)
         return -1;
      s->at = 0;
      s->end = (size_t)n;
   }
   return s->pending[s->at++];
}
static int line(cli_http_stream_t *s, char *out, size_t maximum)
{
   for (size_t n = 0; n < maximum; ++n)
   {
      int c = byte(s);
      if (c < 0 || c == 0)
         return -1;
      if (c == '\r')
      {
         if (byte(s) != '\n')
            return -1;
         out[n] = 0;
         return (int)n;
      }
      if (c == '\n')
         return -1;
      out[n] = (char)c;
   }
   return -1;
}
static int safe_header(const char *name, const char *value)
{
   if (!name || !name[0] || !value)
      return 0;
   for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
      if (*p <= 32 || *p >= 127 || strchr("()<>@,;:\\\"/[]?={}", *p))
         return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
      if ((*p < 32 && *p != '\t') || *p == 127)
         return 0;
   return 1;
}
static int same_name(const char *a, const char *b)
{
   while (*a && *b)
   {
      unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
      if (x >= 'A' && x <= 'Z')
         x += 32;
      if (y >= 'A' && y <= 'Z')
         y += 32;
      if (x != y)
         return 0;
   }
   return *a == *b;
}
static int safe_request_line(const cli_http_options_t *o)
{
   if (!safe_header(o->method, "") || !o->path || o->path[0] != '/')
      return 0;
   for (const unsigned char *p = (const unsigned char *)o->path; *p; ++p)
      if (*p <= 32 || *p == 127)
         return 0;
   return 1;
}
static char *request_bytes(const cli_http_options_t *o, const char *host, size_t *length)
{
   if (!safe_request_line(o) || (o->body_length && !o->body) || o->header_count > 256 ||
       (o->header_count && !o->headers))
      return NULL;
   size_t extra = 0;
   int have_host = 0, have_type = 0, have_auth = 0;
   for (size_t i = 0; i < o->header_count; ++i)
   {
      const cli_http_header_t *h = o->headers + i;
      if (!safe_header(h->name, h->value) || same_name(h->name, "Content-Length") ||
          same_name(h->name, "Transfer-Encoding") || same_name(h->name, "Connection"))
         return NULL;
      have_host += same_name(h->name, "Host");
      have_type += same_name(h->name, "Content-Type");
      have_auth += same_name(h->name, "Authorization");
      if (strlen(h->name) > HEADER_LIMIT || strlen(h->value) > HEADER_LIMIT ||
          extra > HEADER_LIMIT - strlen(h->name) ||
          extra + strlen(h->name) > HEADER_LIMIT - strlen(h->value))
         return NULL;
      extra += strlen(h->name) + strlen(h->value) + 4;
   }
   if (have_host > 1 || have_auth > 1 || (have_auth && o->bearer && o->bearer[0]))
      return NULL;
   char auth[4096] = "";
   if (o->bearer && o->bearer[0] && aimee_core_bearer_value(auth, sizeof(auth), o->bearer))
      return NULL;
   size_t capacity = 256;
   size_t pieces[] = {o->body_length, strlen(o->method), strlen(o->path),
                      strlen(host),   strlen(auth),      extra};
   for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); ++i)
   {
      if (pieces[i] > SIZE_MAX - capacity)
         return NULL;
      capacity += pieces[i];
   }
   char *request = malloc(capacity);
   if (!request)
      return NULL;
   int first = snprintf(request, capacity, "%s %s HTTP/1.1\r\n", o->method, o->path);
   if (first < 0 || (size_t)first >= capacity)
   {
      free(request);
      return NULL;
   }
   size_t n = (size_t)first;
#define ADD(...)                                                                                   \
   do                                                                                              \
   {                                                                                               \
      int k = snprintf(request + n, capacity - (size_t)n, __VA_ARGS__);                            \
      if (k < 0 || (size_t)k >= capacity - (size_t)n)                                              \
      {                                                                                            \
         free(request);                                                                            \
         return NULL;                                                                              \
      }                                                                                            \
      n += (size_t)k;                                                                              \
   } while (0)
   if (!have_host)
      ADD("Host: %s\r\n", host);
   if (auth[0])
      ADD("Authorization: %s\r\n", auth);
   for (size_t i = 0; i < o->header_count; i++)
      ADD("%s: %s\r\n", o->headers[i].name, o->headers[i].value);
   if (!have_type)
      ADD("Content-Type: application/json\r\n");
   ADD("Content-Length: %zu\r\nConnection: close\r\n\r\n", o->body_length);
#undef ADD
   if (o->body_length)
      memcpy(request + n, o->body, o->body_length);
   *length = (size_t)n + o->body_length;
   return request;
}
int cli_http_stream_open(cli_http_stream_t *s, const cli_http_options_t *o)
{
   if (!s || !o || !o->endpoint || !safe_request_line(o) || s->opened || cancelled(s) ||
       o->header_count > 256 || (o->header_count && !o->headers))
      return -1;
   const char *credential = o->bearer;
   int authorization = 0;
   for (size_t i = 0; i < o->header_count; ++i)
   {
      const cli_http_header_t *h = o->headers + i;
      if (!safe_header(h->name, h->value))
         return -1;
      if (same_name(h->name, "Authorization"))
      {
         if (++authorization > 1 || (o->bearer && o->bearer[0]))
            return -1;
         credential = h->value;
      }
   }
   s->opened = 1;
   s->timeout = o->io_timeout_ms;
   int budget = o->connect_timeout_ms > 0 ? o->connect_timeout_ms : CLIENT_CONNECT_TIMEOUT_MS;
   aimee_core_control_t control;
   if (aimee_core_control_init_timeout(&control, budget, 50, cancelled, s))
      return -1;
   int fd = -1, secure = !strncmp(o->endpoint, "tls:", 4);
   char host[512] = "localhost", peer[256] = "localhost";
   const char *unix_path = !strncmp(o->endpoint, "unix:", 5) ? o->endpoint + 5
                           : o->endpoint[0] == '/'           ? o->endpoint
                                                             : NULL;
   if (unix_path)
   {
#ifdef _WIN32
      return -1;
#else
      struct sockaddr_un address = {.sun_family = AF_UNIX};
      if (!unix_path[0] || strlen(unix_path) >= sizeof(address.sun_path))
         return -1;
      memcpy(address.sun_path, unix_path, strlen(unix_path) + 1);
      fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0)
         return -1;
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      int flags = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      atomic_store(&s->fd, fd);
      if (cancelled(s))
         return -1;
      if (connect(fd, (void *)&address, sizeof(address)) < 0)
      {
         if (errno != EINPROGRESS || aimee_core_wait_fd(fd, AIMEE_CORE_WAIT_WRITE, &control))
            return -1;
         int error = 0;
         socklen_t size = sizeof(error);
         if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error)
            return -1;
      }
      if (fcntl(fd, F_SETFL, flags) < 0)
         return -1;
#endif
   }
   else
   {
      const char *authority = o->endpoint;
      if (!strncmp(authority, "tcp:", 4) || secure)
         authority += 4;
      aimee_core_endpoint_t endpoint;
      if (aimee_core_endpoint_parse(authority, &endpoint))
         return -1;
      secure |= endpoint.secure;
      if (aimee_core_would_leak_credential(secure, endpoint.host, credential))
         return -1;
      if (aimee_core_socket_connect_controlled(endpoint.host, endpoint.port, 0, &control, &fd))
         return -1;
      atomic_store(&s->fd, fd);
      snprintf(peer, sizeof(peer), "%s", endpoint.host);
      int n = strchr(endpoint.host, ':')
                  ? snprintf(host, sizeof(host), "[%s]:%s", endpoint.host, endpoint.port)
                  : snprintf(host, sizeof(host), "%s:%s", endpoint.host, endpoint.port);
      if (n < 0 || (size_t)n >= sizeof(host))
         return -1;
   }
   if (cancelled(s))
      return -1;
   if (aimee_core_socket_set_timeouts(fd, budget, budget))
      return -1;
   if (secure)
   {
#ifdef WITH_TLS
      s->tls = aimee_tls_connect_home(fd, peer, o->tls_home);
      if (!s->tls)
         return -1;
#else
      return -1;
#endif
   }
   if (cancelled(s) || aimee_core_socket_set_timeouts(fd, s->timeout, s->timeout))
      return -1;
   size_t length = 0;
   char *request = request_bytes(o, host, &length);
   if (!request)
      return -1;
   int failed;
#ifdef WITH_TLS
   if (s->tls)
      failed = aimee_tls_write_all(s->tls, request, length);
   else
#endif
      failed = aimee_core_socket_write_all(fd, request, length);
   free(request);
   if (failed)
      return -1;
   s->headers = malloc(HEADER_LIMIT + 1);
   if (!s->headers)
      return -1;
   size_t used = 0;
   for (;;)
   {
      int c = byte(s);
      if (c <= 0 || used == HEADER_LIMIT)
         return -1;
      s->headers[used++] = (char)c;
      s->headers[used] = 0;
      if (used >= 4 && !memcmp(s->headers + used - 4, "\r\n\r\n", 4))
      {
         aimee_core_http1_response_t response = {.data = s->headers, .header_length = used};
         if (aimee_core_http1_headers_parse(&response, 1))
            return -1;
         if (response.status >= 100 && response.status < 200)
         {
            if (response.status == 101)
               return -1;
            used = 0;
            continue;
         }
         s->status = response.status;
         s->chunked = response.transfer_chunked;
         s->has_length = response.has_content_length;
         s->remaining = response.content_length;
         if (!strcmp(o->method, "HEAD") || s->status == 204 || s->status == 304)
         {
            s->has_length = 1;
            s->remaining = 0;
            s->chunked = 0;
         }
         return 0;
      }
   }
}
long cli_http_stream_read(cli_http_stream_t *s, void *buffer, size_t length)
{
   if (!s || !buffer || !s->status || cancelled(s))
      return -1;
   if (!length || s->done)
      return 0;
   if (s->chunked)
   {
      if (!s->chunk_remaining)
      {
         if (s->chunk_crlf && (byte(s) != '\r' || byte(s) != '\n'))
            return -1;
         char value[HEADER_LIMIT + 1];
         int n = line(s, value, HEADER_LIMIT);
         if (n <= 0)
            return -1;
         uint64_t size = 0;
         int digits = 0;
         for (const char *p = value; *p && *p != ';'; ++p)
         {
            unsigned c = (unsigned char)*p;
            int d = c >= '0' && c <= '9'   ? (int)(c - '0')
                    : c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10)
                    : c >= 'A' && c <= 'F' ? (int)(c - 'A' + 10)
                                           : -1;
            if (d < 0 || size > (UINT64_MAX - (unsigned)d) / 16)
               return -1;
            size = size * 16 + (unsigned)d;
            digits++;
         }
         if (!digits)
            return -1;
         s->chunk_remaining = size;
         s->chunk_crlf = 1;
         if (!size)
         {
            size_t trailers = 0;
            do
            {
               n = line(s, value, HEADER_LIMIT);
               if (n < 0 || trailers > HEADER_LIMIT - (size_t)n)
                  return -1;
               trailers += (size_t)n;
               if (n && !strchr(value, ':'))
                  return -1;
            } while (n);
            s->done = 1;
            return 0;
         }
      }
      if ((uint64_t)length > s->chunk_remaining)
         length = (size_t)s->chunk_remaining;
      long n = buffered(s, buffer, length);
      if (n <= 0)
         return -1;
      s->chunk_remaining -= (uint64_t)n;
      return n;
   }
   if (s->has_length)
   {
      if (!s->remaining)
      {
         s->done = 1;
         return 0;
      }
      if ((uint64_t)length > s->remaining)
         length = (size_t)s->remaining;
   }
   long n = buffered(s, buffer, length);
   if (n == 0 && s->has_length && s->remaining)
      return -1;
   if (n > 0 && s->has_length)
      s->remaining -= (uint64_t)n;
   if (!n)
      s->done = 1;
   return n;
}

cJSON *cli_http_request(const char *endpoint, const char *method, const char *path,
                        const char *body, const char *bearer, int timeout, int *status)
{
   if (status)
      *status = 0;
   cli_http_stream_t *s = cli_http_stream_new();
   cli_http_options_t o = {.endpoint = endpoint,
                           .method = method,
                           .path = path,
                           .bearer = bearer,
                           .body = body,
                           .body_length = body ? strlen(body) : 0,
                           .connect_timeout_ms = timeout > 0 ? timeout : CLIENT_DEFAULT_TIMEOUT_MS,
                           .io_timeout_ms = timeout > 0 ? timeout : CLIENT_DEFAULT_TIMEOUT_MS};
   char *buffer = NULL;
   size_t used = 0, capacity = 8192;
   if (!s || cli_http_stream_open(s, &o))
      goto done;
   if (status)
      *status = s->status;
   buffer = malloc(capacity);
   if (!buffer)
      goto done;
   for (;;)
   {
      if (used + 1 == capacity)
      {
         size_t next = capacity * 2;
         if (next > CLIENT_MAX_RESPONSE_SIZE + 1u)
            next = CLIENT_MAX_RESPONSE_SIZE + 1u;
         if (next <= capacity)
         {
            free(buffer);
            buffer = NULL;
            break;
         }
         char *grown = realloc(buffer, next);
         if (!grown)
         {
            free(buffer);
            buffer = NULL;
            break;
         }
         buffer = grown;
         capacity = next;
      }
      long n = cli_http_stream_read(s, buffer + used, capacity - used - 1);
      if (n < 0)
      {
         free(buffer);
         buffer = NULL;
         break;
      }
      if (!n)
      {
         buffer[used] = 0;
         break;
      }
      used += (size_t)n;
   }
done:
   cli_http_stream_free(s);
   cJSON *result = buffer
                       ? (!strncmp(path, "/v1/memory/", 11) ? json_wire_parse_exact_integers(buffer)
                                                            : cJSON_Parse(buffer))
                       : NULL;
   free(buffer);
   return result;
}
int cli_http_request_stream_ndjson_on(cli_http_stream_t *s, const cli_http_options_t *o,
                                      int *status, cli_stream_cb cb, void *context)
{
   if (status)
      *status = 0;
   if (!s || !cb || cli_http_stream_open(s, o))
      return -1;
   if (status)
      *status = s->status;
   size_t used = 0, capacity = 8192;
   char *buffer = malloc(capacity);
   if (!buffer)
      return -1;
   int result = -1;
   for (;;)
   {
      unsigned char bytes[8192];
      long n = cli_http_stream_read(s, bytes, sizeof(bytes));
      if (n <= 0)
      {
         if (used || n < 0)
            (void)cb(NULL, context);
         result = n == 0 && !used ? 0 : -1;
         break;
      }
      for (long i = 0; i < n; i++)
      {
         if (bytes[i] == '\n')
         {
            cJSON *value = aimee_ir_json_parse_frame(buffer, used);
            if (!value)
            {
               (void)cb(NULL, context);
               goto done;
            }
            int rc = cb(value, context);
            cJSON_Delete(value);
            used = 0;
            if (rc)
            {
               result = rc > 0 ? 0 : -1;
               goto done;
            }
         }
         else
         {
            if (used == CLIENT_MAX_RESPONSE_SIZE)
               goto done;
            if (used == capacity)
            {
               size_t next = capacity * 2;
               if (next > CLIENT_MAX_RESPONSE_SIZE)
                  next = CLIENT_MAX_RESPONSE_SIZE;
               char *grown = realloc(buffer, next);
               if (!grown)
                  goto done;
               buffer = grown;
               capacity = next;
            }
            buffer[used++] = (char)bytes[i];
         }
      }
   }
done:
   free(buffer);
   return result;
}
int cli_http_request_stream_ndjson(const char *endpoint, const char *method, const char *path,
                                   const char *body, const char *bearer, int timeout, int *status,
                                   cli_stream_cb cb, void *context)
{
   cli_http_stream_t *s = cli_http_stream_new();
   cli_http_options_t o = {.endpoint = endpoint,
                           .method = method,
                           .path = path,
                           .bearer = bearer,
                           .body = body,
                           .body_length = body ? strlen(body) : 0,
                           .connect_timeout_ms = timeout > 0 ? timeout : CLIENT_CONNECT_TIMEOUT_MS,
                           .io_timeout_ms = timeout < 0 ? 0
                                            : timeout   ? timeout
                                                        : CLIENT_DEFAULT_TIMEOUT_MS};
   int result = cli_http_request_stream_ndjson_on(s, &o, status, cb, context);
   cli_http_stream_free(s);
   return result;
}
