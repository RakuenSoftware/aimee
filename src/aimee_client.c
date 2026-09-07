/* aimee_client.c: transport-agnostic client to aimee-server's /v1 HTTP API.
 *
 * See headers/aimee_client.h for the contract. This file contains no socket
 * headers — the TCP path goes through libaimee-core-connection, and the local UDS
 * path delegates to http_uds_request (POSIX only). It therefore builds
 * identically on Linux, macOS, and Windows.
 */
#include "aimee_client.h"
#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/endpoint.h>
#include <aimee/core/connection/http1.h>
#include <aimee/core/connection/socket.h>
#include "http_content_encoding.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef AIMEE_POSIX
#include "http_uds_client.h"
#endif

#ifdef WITH_TLS
#include "aimee_tls.h"
#else
/* No TLS in this build: the handle type still exists (always NULL) so the I/O
 * helpers below compile unchanged; https:// is refused in tcp_request. */
typedef void aimee_tls_t;
#endif

/* Remote target configured for this process. Set once at startup (e.g. after
 * parsing a `--server` flag) before any request is issued, so plain statics
 * suffice — aimee_client_set_remote is documented as not thread-safe. */
static char g_remote_url[512];
static char g_remote_token[256];
/* Learned from an authenticated response. Request compression is deliberately
 * withheld until the selected server advertises support, preserving rolling
 * upgrades with older servers that would otherwise parse compressed JSON. */
static int g_remote_request_gzip;
static void client_keepalive_close(void);

static _Thread_local int g_keepalive_fd = -1;
static _Thread_local aimee_tls_t *g_keepalive_tls;
static _Thread_local char g_keepalive_host[256];
static _Thread_local char g_keepalive_port[16];
static _Thread_local time_t g_keepalive_created;
static _Thread_local time_t g_keepalive_used;
static _Thread_local unsigned g_keepalive_requests;

static int thinclient_gzip_enabled(void)
{
   const char *value = getenv("AIMEE_TRANSPORT_THINCLIENT_GZIP_ENABLED");
   return http_content_encoding_available() && value &&
          (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
}

static int thinclient_gzip_route(const char *path)
{
   static const char *const allowed[] = {"/v1/responses", "/v1/completions", "/v1/embeddings",
                                         "/v1/messages", NULL};
   for (int i = 0; allowed[i]; i++)
      if (strcmp(path, allowed[i]) == 0)
         return 1;
   return 0;
}

static int body_requests_stream(const char *body)
{
   const char *p = body ? strstr(body, "\"stream\"") : NULL;
   if (!p)
      return 0;
   p += 8;
   while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
   if (*p++ != ':')
      return 0;
   while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
   return strncmp(p, "true", 4) == 0;
}

static int thinclient_keepalive_enabled(const char *path, const char *body, int is_https)
{
   const char *value = getenv("AIMEE_TRANSPORT_SERVER_KEEPALIVE_ENABLED");
   /* Default on after live validation. An explicit value other than 1/true is
    * the process-local rollback; old servers safely answer Connection: close. */
   if (!is_https || (value && strcmp(value, "1") != 0 && strcmp(value, "true") != 0))
      return 0;
   if (strstr(path, "/events") || strstr(path, "/stream") || strstr(path, "/live"))
      return 0;
   return !body_requests_stream(body);
}

static int ascii_equal_ci(const char *left, const char *right, size_t n)
{
   for (size_t i = 0; i < n; i++)
   {
      unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
      if (a >= 'A' && a <= 'Z')
         a = (unsigned char)(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z')
         b = (unsigned char)(b + ('a' - 'A'));
      if (a != b)
         return 0;
   }
   return 1;
}

static int response_is_gzip(const char *response, size_t header_len)
{
   static const char name[] = "content-encoding:";
   for (size_t i = 0; i + sizeof(name) - 1 < header_len; i++)
      if ((i == 0 || response[i - 1] == '\n') &&
          ascii_equal_ci(response + i, name, sizeof(name) - 1))
      {
         const char *value = response + i + sizeof(name) - 1;
         const char *end = memchr(value, '\n', header_len - (size_t)(value - response));
         if (!end)
            return 0;
         while (value < end && (*value == ' ' || *value == '\t'))
            value++;
         return end && (size_t)(end - value) >= 4 && ascii_equal_ci(value, "gzip", 4);
      }
   return 0;
}

static int response_accepts_gzip_requests(const char *response, size_t header_len)
{
   static const char name[] = "accept-request-encoding:";
   for (size_t i = 0; i + sizeof(name) - 1 < header_len; i++)
      if ((i == 0 || response[i - 1] == '\n') &&
          ascii_equal_ci(response + i, name, sizeof(name) - 1))
      {
         const char *value = response + i + sizeof(name) - 1;
         const char *end = memchr(value, '\n', header_len - (size_t)(value - response));
         if (!end)
            return 0;
         while (value < end && (*value == ' ' || *value == '\t'))
            value++;
         return (size_t)(end - value) >= 4 && ascii_equal_ci(value, "gzip", 4);
      }
   return 0;
}

/* When set, the transport suppresses its connection-failure diagnostics on
 * stderr. `aimee remote set` uses this around the pre-pin reachability probe
 * (which is EXPECTED to fail against a not-yet-pinned self-signed server) so a
 * successful set does not print a misleading "TLS handshake failed" line. */
static int g_suppress_conn_errors = 0;

void aimee_client_suppress_conn_errors(int on)
{
   g_suppress_conn_errors = on ? 1 : 0;
}

void aimee_client_set_remote(const char *url, const char *token)
{
   client_keepalive_close();
   g_remote_request_gzip = 0;
   if (url && *url)
      snprintf(g_remote_url, sizeof(g_remote_url), "%s", url);
   else
      g_remote_url[0] = '\0';
   if (token && *token)
      snprintf(g_remote_token, sizeof(g_remote_token), "%s", token);
   else
      g_remote_token[0] = '\0';
}

/* True when a remote aimee-server target is configured for this process — either
 * programmatically (aimee_client_set_remote, e.g. from --server) or via
 * AIMEE_SERVER_URL. Mirrors resolve_remote()'s precedence. On Windows (no UDS)
 * the dispatcher uses this to skip the local-socket preflight. */
int aimee_client_has_remote(void)
{
   if (g_remote_url[0])
      return 1;
   const char *env = getenv("AIMEE_SERVER_URL");
   return (env && *env) ? 1 : 0;
}

/* A short, credential-free description of where this process actually sends
 * requests. Failures print it, because "no remote configured" is otherwise
 * invisible: with remote.conf absent the client silently uses the local socket
 * and every command still answers — from a DIFFERENT aimee than the operator
 * believes they are talking to. A whole debugging session was spent
 * instrumenting a remote server that the failing commands were never reaching.
 *
 * Any userinfo in the URL is stripped: a credential must not reach a terminal,
 * a log, or a pasted bug report. */
const char *aimee_client_transport_label(void)
{
   static char label[sizeof(g_remote_url) + 32];
   const char *url = g_remote_url[0] ? g_remote_url : getenv("AIMEE_SERVER_URL");
   if (!url || !*url)
      return "local Unix socket (no remote server configured)";

   const char *scheme_end = strstr(url, "://");
   const char *authority = scheme_end ? scheme_end + 3 : url;
   const char *at = strchr(authority, '@');
   if (!at)
   {
      snprintf(label, sizeof(label), "%s", url);
      return label;
   }
   /* Keep the scheme, drop everything up to and including the '@'. */
   int scheme_len = scheme_end ? (int)(scheme_end + 3 - url) : 0;
   snprintf(label, sizeof(label), "%.*s%s", scheme_len, url, at + 1);
   return label;
}

int aimee_client_parse_flag(char **argv, int *i, int argc, const char **url, const char **token)
{
   const char *a = argv[*i];
   if (strncmp(a, "--server=", 9) == 0)
   {
      *url = a + 9;
      return 1;
   }
   if (strcmp(a, "--server") == 0 && *i + 1 < argc)
   {
      *url = argv[++(*i)];
      return 1;
   }
   if (strncmp(a, "--server-token=", 15) == 0)
   {
      *token = a + 15;
      return 1;
   }
   return 0;
}

void aimee_client_apply_override(const char *url, const char *token)
{
   if (!url && !token)
      return;
   if (!url)
      url = getenv("AIMEE_SERVER_URL");
   if (!token)
      token = getenv("AIMEE_SERVER_TOKEN");
   aimee_client_set_remote(url, token);
}

/* Resolve the effective remote URL + token: explicit set_remote wins, else env.
 * Returns 1 if a remote is in effect (url_out filled), 0 if none. */
static int resolve_remote(char *url_out, size_t url_sz, char *tok_out, size_t tok_sz)
{
   if (g_remote_url[0])
   {
      snprintf(url_out, url_sz, "%s", g_remote_url);
      snprintf(tok_out, tok_sz, "%s", g_remote_token);
      return 1;
   }
   const char *env = getenv("AIMEE_SERVER_URL");
   if (env && *env)
   {
      snprintf(url_out, url_sz, "%s", env);
      const char *t = getenv("AIMEE_SERVER_TOKEN");
      snprintf(tok_out, tok_sz, "%s", (t && *t) ? t : "");
      return 1;
   }
   return 0;
}

/* Read a full HTTP response (Connection: close) from |fd| into a heap buffer.
 * Returns the buffer (caller frees) and sets *out_len, or NULL on alloc error. */
/* Transport I/O over either a plain socket (tls == NULL) or a TLS session. */
static int io_write_all(int fd, aimee_tls_t *tls, const void *buf, size_t len)
{
#ifdef WITH_TLS
   if (tls)
      return aimee_tls_write_all(tls, buf, len);
#endif
   (void)tls;
   return aimee_core_socket_write_all(fd, buf, len);
}

static long io_read(int fd, aimee_tls_t *tls, void *buf, size_t len)
{
#ifdef WITH_TLS
   if (tls)
      return aimee_tls_read(tls, buf, len);
#endif
   (void)tls;
   return aimee_core_socket_read(fd, buf, len);
}

int aimee_client_proxy_request(const char *method, const char *path, const char *headers,
                               const void *body, unsigned long body_len, const char *session_id,
                               int (*write_response)(const void *, unsigned long, void *),
                               void (*on_socket)(int, void *), void *context)
{
   char url[512], token[256], auth_token[512], authorization[528];
   aimee_core_endpoint_t endpoint;
   if (!write_response || !resolve_remote(url, sizeof(url), token, sizeof(token)) ||
       aimee_core_endpoint_parse(url, &endpoint) != 0 ||
       aimee_client_would_leak_cleartext(endpoint.secure, endpoint.host, token))
      return -1;
#ifndef WITH_TLS
   if (endpoint.secure)
      return -1;
#endif
   snprintf(auth_token, sizeof(auth_token), "%s%s%s", token[0] ? token : "aimee-local",
            session_id && *session_id ? ".aimee-session." : "",
            session_id && *session_id ? session_id : "");
   if (aimee_core_bearer_value(authorization, sizeof(authorization), auth_token) != 0)
      return -1;
   int fd = aimee_core_socket_connect(endpoint.host, endpoint.port, 10000);
   if (fd < 0)
      return -1;
   if (on_socket)
      on_socket(fd, context);
   aimee_core_socket_set_timeouts(fd, 300000, 30000);
   aimee_tls_t *tls = NULL;
   int rc = -1;
#ifdef WITH_TLS
   if (endpoint.secure && !(tls = aimee_tls_connect(fd, endpoint.host)))
      goto done;
#endif
   char head[16384];
   int hlen = snprintf(head, sizeof(head),
                       "%s %s HTTP/1.1\r\nHost: %s%s%s:%s\r\nAuthorization: %s\r\n"
                       "Content-Length: %lu\r\nConnection: close\r\n%s\r\n",
                       method, path, strchr(endpoint.host, ':') ? "[" : "", endpoint.host,
                       strchr(endpoint.host, ':') ? "]" : "", endpoint.port, authorization,
                       body_len, headers ? headers : "");
   if (hlen <= 0 || hlen >= (int)sizeof(head) || io_write_all(fd, tls, head, (size_t)hlen) != 0 ||
       (body_len && io_write_all(fd, tls, body, body_len) != 0))
      goto done;
   char buffer[16384];
   long count;
   while ((count = io_read(fd, tls, buffer, sizeof(buffer))) > 0)
      if (write_response(buffer, (unsigned long)count, context) != 0)
         goto done;
   rc = count == 0 ? 0 : -1;
done:
   if (on_socket)
      on_socket(-1, context);
#ifdef WITH_TLS
   if (tls)
      aimee_tls_free(tls);
#endif
   aimee_core_socket_close(fd);
   return rc;
}

typedef struct
{
   int fd;
   aimee_tls_t *tls;
} client_http_io_t;

static long client_http_read(void *context, void *buffer, size_t length)
{
   client_http_io_t *io = context;
   return io_read(io->fd, io->tls, buffer, length);
}

static void client_keepalive_close(void)
{
   if (g_keepalive_fd < 0)
      return;
#ifdef WITH_TLS
   if (g_keepalive_tls)
      aimee_tls_free(g_keepalive_tls);
#endif
   aimee_core_socket_close(g_keepalive_fd);
   g_keepalive_fd = -1;
   g_keepalive_tls = NULL;
   g_keepalive_host[0] = '\0';
   g_keepalive_port[0] = '\0';
   g_keepalive_created = 0;
   g_keepalive_used = 0;
   g_keepalive_requests = 0;
}

static char *read_response(int fd, aimee_tls_t *tls, size_t *out_len, int want_reuse,
                           int *reusable_out)
{
   if (reusable_out)
      *reusable_out = 0;
   client_http_io_t transport = {.fd = fd, .tls = tls};
   aimee_core_http1_io_t io = {.context = &transport, .read = client_http_read};
   aimee_core_http1_response_t response;
   if (aimee_core_http1_response_read(&io, 64u * 1024u, 64u * 1024u + (1u << 20), want_reuse,
                                      &response) != 0)
      return NULL;
   if (reusable_out)
      *reusable_out = want_reuse && response.has_content_length && !response.connection_close;
   if (out_len)
      *out_len = response.length;
   return response.data;
}

/* Security guard (also exposed for tests): 1 when sending |token| to a server at
 * (|is_https|, |host|) would put the credential on the wire in cleartext — a
 * non-empty bearer over plaintext http:// to a non-loopback host. tcp_request
 * refuses such requests rather than leak the bearer. */
int aimee_client_would_leak_cleartext(int is_https, const char *host, const char *token)
{
   return aimee_core_would_leak_credential(is_https, host, token);
}

static char *tcp_request(const char *url, const char *token, const char *method, const char *path,
                         const char *body, int *status_out)
{
   aimee_core_endpoint_t endpoint;
   if (aimee_core_endpoint_parse(url, &endpoint) != 0)
   {
      fprintf(stderr, "aimee: invalid server URL: %s\n", url);
      return NULL;
   }
   const char *host = endpoint.host;
   const char *port = endpoint.port;
   int is_https = endpoint.secure;

   /* Fail closed: never transmit a bearer credential in cleartext to a
    * non-loopback host. A plaintext http:// connection to a remote server would
    * expose the bearer (and every payload) on the wire — the client refuses
    * rather than leak it. Loopback plaintext is fine (local tooling / a
    * co-located TLS-terminating proxy); remote access must use https://. */
   if (aimee_client_would_leak_cleartext(is_https, host, token))
   {
      fprintf(stderr,
              "aimee: refusing to send credentials over plaintext http:// to non-loopback host "
              "'%s'. Use https:// (the server's TLS port) — `aimee remote set` pins a self-signed "
              "certificate automatically.\n",
              host);
      return NULL;
   }
#ifndef WITH_TLS
   if (is_https)
   {
      /* This build has no TLS: front remote servers with a TLS-terminating
       * proxy and point at its http:// address. */
      fprintf(stderr, "aimee: https:// is not supported in this build; use http:// "
                      "behind a TLS-terminating proxy\n");
      return NULL;
   }
#endif

   int keepalive = thinclient_keepalive_enabled(path, body, is_https);
   time_t now = time(NULL);
   if (g_keepalive_fd >= 0 && (!keepalive || strcmp(g_keepalive_host, host) ||
                               strcmp(g_keepalive_port, port) || now - g_keepalive_used > 30 ||
                               now - g_keepalive_created > 600 || g_keepalive_requests >= 1000))
      client_keepalive_close();

   int fd = g_keepalive_fd;
   aimee_tls_t *tls = NULL;
   if (fd >= 0)
      tls = g_keepalive_tls;
   else
   {
      fd = aimee_core_socket_connect(host, port, 10000);
      if (fd < 0)
         return NULL;
#ifdef WITH_TLS
      if (is_https)
      {
         tls = aimee_tls_connect(fd, host);
         if (!tls)
         {
            if (!g_suppress_conn_errors)
               fprintf(stderr,
                       "aimee: TLS handshake with %s failed (untrusted/self-signed cert, or the "
                       "pinned cert was rotated). Run `aimee remote trust` to (re)pin this "
                       "server's certificate.\n",
                       host);
            aimee_core_socket_close(fd);
            return NULL;
         }
      }
#endif
      if (keepalive)
      {
         g_keepalive_fd = fd;
         g_keepalive_tls = tls;
         snprintf(g_keepalive_host, sizeof(g_keepalive_host), "%s", host);
         snprintf(g_keepalive_port, sizeof(g_keepalive_port), "%s", port);
         g_keepalive_created = now;
         g_keepalive_used = now;
         g_keepalive_requests = 0;
      }
   }

   int gzip_enabled =
       thinclient_gzip_enabled() && thinclient_gzip_route(path) && !body_requests_stream(body);
   size_t body_len = body ? strlen(body) : 0;
   unsigned char *compressed = NULL;
   size_t wire_len = body_len;
   const void *wire_body = body;
   if (gzip_enabled && g_remote_request_gzip && body_len >= 4096 &&
       http_gzip_compress(body, body_len, &compressed, &wire_len) == 0 &&
       (wire_len >= body_len || (body_len > 1024 && (body_len - 1024 + 49) / 50 > wire_len)))
   {
      free(compressed);
      compressed = NULL;
      wire_len = body_len;
   }
   if (compressed)
      wire_body = compressed;
   char head[1024];
   int hlen;
   const char *accept_encoding = gzip_enabled ? "Accept-Encoding: gzip\r\n" : "";
   const char *content_encoding = compressed ? "Content-Encoding: gzip\r\n" : "";
   char authorization[sizeof(g_remote_token) + 16] = "";
   int invalid_authorization =
       token && *token && aimee_core_bearer_value(authorization, sizeof(authorization), token) != 0;
   if (invalid_authorization)
      hlen = -1;
   else if (authorization[0])
      hlen = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: %s\r\n"
                      "Content-Type: application/json\r\n%s%sContent-Length: %zu\r\n"
                      "Connection: %s\r\n\r\n",
                      method, path, host, authorization, accept_encoding, content_encoding,
                      wire_len, keepalive ? "keep-alive" : "close");
   else
      hlen = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                      "%s%sContent-Length: %zu\r\nConnection: %s\r\n\r\n",
                      method, path, host, accept_encoding, content_encoding, wire_len,
                      keepalive ? "keep-alive" : "close");
   if (hlen <= 0 || hlen >= (int)sizeof(head))
   {
      free(compressed);
      if (keepalive)
         client_keepalive_close();
      else
      {
#ifdef WITH_TLS
         if (tls)
            aimee_tls_free(tls);
#endif
         aimee_core_socket_close(fd);
      }
      return NULL;
   }

   if (io_write_all(fd, tls, head, (size_t)hlen) != 0 ||
       (wire_len > 0 && io_write_all(fd, tls, wire_body, wire_len) != 0))
   {
      free(compressed);
      if (keepalive)
         client_keepalive_close();
      else
      {
#ifdef WITH_TLS
         if (tls)
            aimee_tls_free(tls);
#endif
         aimee_core_socket_close(fd);
      }
      return NULL;
   }
   free(compressed);

   size_t resp_len = 0;
   int reusable = 0;
   char *resp = read_response(fd, tls, &resp_len, keepalive, &reusable);
   if (keepalive && reusable)
   {
      g_keepalive_used = time(NULL);
      g_keepalive_requests++;
   }
   else if (keepalive)
      client_keepalive_close();
   else
   {
#ifdef WITH_TLS
      if (tls)
         aimee_tls_free(tls);
#endif
      aimee_core_socket_close(fd);
   }
   if (!resp)
      return NULL;

   int status = 0;
   if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
   {
      if (keepalive)
         client_keepalive_close();
      free(resp);
      return NULL;
   }
   if (status == 415)
      g_remote_request_gzip = 0;
   if (status_out)
      *status_out = status;
   char *bstart = strstr(resp, "\r\n\r\n");
   size_t header_len = bstart ? (size_t)(bstart + 4 - resp) : resp_len;
   if (gzip_enabled && response_accepts_gzip_requests(resp, header_len))
      g_remote_request_gzip = 1;
   char *out = NULL;
   if (bstart && response_is_gzip(resp, header_len))
   {
      unsigned char *decoded = NULL;
      size_t decoded_len = 0;
      if (http_gzip_decompress(resp + header_len, resp_len - header_len, 1u << 20, 50, &decoded,
                               &decoded_len) == 0)
         out = (char *)decoded;
      else if (keepalive)
         client_keepalive_close();
   }
   else
      out = bstart ? strdup(bstart + 4) : strdup("");
   free(resp);
   return out;
}

int aimee_client_remote_active_scheme(char *desc_out, unsigned long desc_sz, int *is_https_out)
{
   if (is_https_out)
      *is_https_out = 0;
   char url[512], tok[256];
   if (!resolve_remote(url, sizeof(url), tok, sizeof(tok)))
      return 0;
   aimee_core_endpoint_t endpoint;
   if (aimee_core_endpoint_parse(url, &endpoint) == 0)
   {
      if (desc_out && desc_sz)
         snprintf(desc_out, desc_sz, "%s:%s", endpoint.host, endpoint.port);
      if (is_https_out)
         *is_https_out = endpoint.secure;
   }
   else if (desc_out && desc_sz)
      snprintf(desc_out, desc_sz, "%s", url);
   return 1;
}

int aimee_client_remote_active(char *desc_out, unsigned long desc_sz)
{
   return aimee_client_remote_active_scheme(desc_out, desc_sz, NULL);
}

int aimee_client_remote_token(char *tok_out, unsigned long tok_sz)
{
   char url[512], tok[256];
   if (!resolve_remote(url, sizeof(url), tok, sizeof(tok)))
      return 0;
   if (tok_out && tok_sz)
      snprintf(tok_out, tok_sz, "%s", tok);
   return 1;
}

char *aimee_client_request(const char *method, const char *path, const char *body, int *status_out)
{
   if (status_out)
      *status_out = 0;
   if (!method || !path)
      return NULL;

   char url[512], tok[256];
   if (resolve_remote(url, sizeof(url), tok, sizeof(tok)))
      return tcp_request(url, tok, method, path, body, status_out);

#ifdef AIMEE_POSIX
   /* Default local transport: HTTP over the server's Unix domain socket. */
   return http_uds_request(method, path, body, status_out);
#else
   /* Windows has no UDS default — a remote target is required for connectivity. */
   fprintf(stderr, "aimee: no server configured; set AIMEE_SERVER_URL or use --server\n");
   return NULL;
#endif
}

int aimee_client_fetch_cert(const char *url, char **pem_out, char *fp_out, unsigned long fp_n)
{
   if (pem_out)
      *pem_out = NULL;
   if (fp_out && fp_n)
      fp_out[0] = '\0';
   if (!url || !*url || !pem_out)
      return -1;
   aimee_core_endpoint_t endpoint;
   if (aimee_core_endpoint_parse(url, &endpoint) != 0 || !endpoint.secure)
      return -1; /* only https:// servers present a cert to pin */
#ifdef WITH_TLS
   return aimee_tls_fetch_peer_cert(endpoint.host, endpoint.port, pem_out, fp_out, (size_t)fp_n);
#else
   return -1; /* no TLS in this build */
#endif
}
