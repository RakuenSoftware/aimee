/* posix/agent_bridge.c: POSIX provider layer — HTTP client (libcurl) and SSH tunnel lifecycle */
#include "aimee.h"
#include "agent_exec.h"
#include "log.h"
#include "proxy_bootstrap.h"
#include <aimee/core/connection/control.h>
#include <aimee/core/connection/socket.h>
#include <aimee/core/connection/tls_openssl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* The HTTP client is also used by small standalone binaries that do not link
 * the agent runtime. In the server this resolves to the parallel worker's
 * thread-local cancellation flag; elsewhere the weak symbol is absent. */
int agent_request_cancelled(void) __attribute__((weak));

static SSL_CTX *s_ssl_ctx;

/* THE SYNTHESIS SIDECAR IS THE ONE HOP THAT NEEDS A CLIENT CERTIFICATE.
 *
 * s_ssl_ctx verifies against the system trust store and presents nothing. That is
 * right for every public provider and wrong for exactly one peer: aimee-llm, whose
 * certificate is issued by the kb's own CA and whose stunnel terminator sets
 * `verifyChain = yes` and therefore REQUIRES a client certificate.
 *
 * Without this the deploy layer's SYNTHESIS_CA_FILE / SYNTHESIS_CERT_FILE /
 * SYNTHESIS_KEY_FILE were read by nothing at all. Every synthesis call left the kb
 * with the default context, the sidecar's certificate chained to a CA the client had
 * never heard of, and the handshake died with "tlsv1 alert unknown ca" -- surfacing
 * to the operator as `provider HTTP -1` on a permanently failed curator job, and in
 * the log as "TCP connect failed", which is what a connection that connected fine
 * looks like from a caller that cannot tell a handshake from a connect.
 *
 * A SECOND CONTEXT, NOT A RELAXED FIRST ONE. Loading our CA into s_ssl_ctx would
 * make a certificate the kb issued to itself acceptable for api.anthropic.com, and
 * attaching the client certificate there would hand the kb's identity to every
 * endpoint it talks to. So the identity is bound to the one host:port that
 * SYNTHESIS_ENDPOINT names, and nothing else can reach it. */
static SSL_CTX *s_synth_ssl_ctx;
static char s_synth_host[256];
static int s_synth_port;
static pthread_mutex_t s_synth_lock = PTHREAD_MUTEX_INITIALIZER;

/* Defined below parse_url, which it needs; the signature keeps parsed_url_t out of
 * the forward declaration. Called from agent_http_init, once, before any worker
 * thread exists -- so no locking here or at the point of use. */
static void synth_ssl_ctx_init(void);

void agent_http_init(void)
{
   /* Initialize proxy bootstrap before setting up the SSL context so that
    * the CA bundle path (SSL_CERT_FILE / REQUESTS_CA_BUNDLE) is available. */
   proxy_bootstrap_init_global();

   s_ssl_ctx = aimee_core_tls_client_context();
   if (s_ssl_ctx)
   {
      SSL_CTX_set_default_verify_paths(s_ssl_ctx);
      SSL_CTX_set_verify(s_ssl_ctx, SSL_VERIFY_PEER, NULL);

      /* Load custom CA bundle when configured */
      const proxy_bootstrap_t *pb = proxy_get();
      if (pb && pb->ca_bundle[0])
      {
         if (aimee_core_tls_trust_file(s_ssl_ctx, pb->ca_bundle) != 0)
            aimee_log(LOG_WARN, "proxy", "failed to load CA bundle: %s", pb->ca_bundle);
         else
            aimee_log(LOG_DEBUG, "proxy", "loaded CA bundle: %s", pb->ca_bundle);
      }
   }

   synth_ssl_ctx_init();
}

void agent_http_cleanup(void)
{
   if (s_ssl_ctx)
   {
      SSL_CTX_free(s_ssl_ctx);
      s_ssl_ctx = NULL;
   }
   if (s_synth_ssl_ctx)
   {
      SSL_CTX_free(s_synth_ssl_ctx);
      s_synth_ssl_ctx = NULL;
   }
   s_synth_host[0] = '\0';
   s_synth_port = 0;
}

#define HTTP_MAX_RESPONSE_SIZE (10 * 1024 * 1024) /* 10MB */

/* ---- URL parsing ---- */

typedef struct
{
   char host[256];
   char path[2048];
   int port;
   int use_ssl;
   /* SSRF pinning: when non-empty, the connection is made to this exact
    * pre-validated numeric IP (no DNS re-resolution), while TLS SNI and the
    * Host header still use `host`. Closes the DNS-rebinding TOCTOU. */
   char pinned_ip[64];
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

/* Build the synthesis-sidecar client context from the deploy layer's three files.
 *
 * All four inputs are required together. SYNTHESIS_ENDPOINT alone is the ordinary
 * external-provider case (the operator points synthesis at a public endpoint with a
 * public certificate), and the three files without it name no peer to trust, so both
 * partial states correctly leave the default context in charge.
 *
 * FAIL LOUD, NOT QUIET. If the files are named but unusable this logs an error and
 * leaves s_synth_ssl_ctx NULL, so the hop falls back to the default context and
 * fails the handshake -- the same outcome as before, but now with a line that says
 * which file could not be loaded instead of a bare "unknown ca" from OpenSSL.
 *
 * THE FILES DO NOT EXIST YET AT INIT, which is why the load is deferred to the
 * first request rather than done here. In the kb, agent_http_init() runs during
 * startup and kb_synthesis_identity_ensure() mints this material LATER in the same
 * startup -- 81 seconds later on a first boot of CT 302, because Postgres has to
 * come up in between. Loading eagerly read three files that did not exist, left the
 * context NULL, and disabled synthesis for the life of the process; it only looked
 * right when the container was restarted with the material already on disk.
 *
 * So init records WHERE the sidecar is, and the first request to that host:port
 * loads the material. By then the kb has issued it. */
static void synth_ssl_ctx_load_locked(void)
{
   const char *ca = getenv("SYNTHESIS_CA_FILE");
   const char *cert = getenv("SYNTHESIS_CERT_FILE");
   const char *key = getenv("SYNTHESIS_KEY_FILE");
   if (!ca || !cert || !key)
      return;

   SSL_CTX *ctx = aimee_core_tls_client_context();
   if (!ctx)
      return;
   /* Deliberately NO SSL_CTX_set_default_verify_paths: on this hop our CA REPLACES
    * the system store. A public CA has no business vouching for the sidecar, and
    * accepting one would turn a private hop into a publicly impersonable one. */
   if (aimee_core_tls_trust_file(ctx, ca) != 0 ||
       aimee_core_tls_use_identity_files(ctx, cert, key) != 0)
   {
      char ebuf[256] = "";
      unsigned long e = ERR_peek_last_error();
      if (e)
         ERR_error_string_n(e, ebuf, sizeof(ebuf));
      aimee_log(LOG_ERROR, "synthesis_mtls",
                "could not load the synthesis client identity (ca=%s cert=%s key=%s)%s%s", ca, cert,
                key, ebuf[0] ? ": " : "", ebuf);
      SSL_CTX_free(ctx);
      return;
   }
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
   s_synth_ssl_ctx = ctx;
   aimee_log(LOG_INFO, "synthesis_mtls", "client identity loaded for %s:%d", s_synth_host,
             s_synth_port);
}

/* Record which peer the identity is for. The material itself is loaded on first use
 * (see synth_ssl_ctx_load_locked): at this point in startup it does not exist yet. */
static void synth_ssl_ctx_init(void)
{
   const char *endpoint = getenv("SYNTHESIS_ENDPOINT");
   const char *ca = getenv("SYNTHESIS_CA_FILE");
   const char *cert = getenv("SYNTHESIS_CERT_FILE");
   const char *key = getenv("SYNTHESIS_KEY_FILE");
   if (!endpoint || !endpoint[0] || !ca || !ca[0] || !cert || !cert[0] || !key || !key[0])
      return;

   parsed_url_t pu;
   if (parse_url(endpoint, &pu) != 0 || !pu.use_ssl)
   {
      aimee_log(LOG_WARN, "synthesis_mtls",
                "SYNTHESIS_ENDPOINT (%s) is not an https URL; the client identity in "
                "SYNTHESIS_CERT_FILE will not be used",
                endpoint);
      return;
   }
   snprintf(s_synth_host, sizeof(s_synth_host), "%s", pu.host);
   s_synth_port = pu.port;
}

/* The sidecar identity is for the sidecar, and for nothing else. Host AND port must
 * both match what SYNTHESIS_ENDPOINT named: matching on host alone would present the
 * kb's client certificate to any other service that happens to share the name.
 *
 * The load happens here, once, under a lock: worker threads call this concurrently,
 * and the curator runs several. A failed load is retried on the next request rather
 * than latched -- the material appears partway through startup, so "not there yet"
 * is a normal state to pass through, not a permanent verdict. */
static SSL_CTX *ssl_ctx_for(const parsed_url_t *url)
{
   if (!s_synth_host[0] || url->port != s_synth_port || strcasecmp(url->host, s_synth_host) != 0)
      return s_ssl_ctx;

   pthread_mutex_lock(&s_synth_lock);
   if (!s_synth_ssl_ctx)
      synth_ssl_ctx_load_locked();
   SSL_CTX *ctx = s_synth_ssl_ctx;
   pthread_mutex_unlock(&s_synth_lock);

   /* No context means the material is unreadable, and the error above says which
    * file. Falling back to the default context here would present no certificate
    * and fail the handshake anyway, so return it and let the TLS error stand. */
   return ctx ? ctx : s_ssl_ctx;
}

/* ---- Socket I/O with timeout ---- */

typedef struct
{
   int fd;
   SSL *ssl;
   aimee_core_control_t control;
} http_conn_t;

static int core_agent_cancelled(void *unused)
{
   (void)unused;
   return agent_request_cancelled && agent_request_cancelled();
}

static int conn_past_deadline(const http_conn_t *conn)
{
   return aimee_core_control_check(&conn->control) == AIMEE_CORE_TIMEOUT;
}

/* A panel deadline is shorter than an individual provider timeout. Once a
 * connection exists, poll blocking socket/TLS operations frequently enough for
 * the parallel worker's cancellation flag to interrupt the provider call before
 * pthread_join can extend the panel past its wall-clock budget. */
#define HTTP_CANCEL_POLL_MS 100

static int connect_controlled(const char *host, int port, int timeout_ms, unsigned flags)
{
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);
   aimee_core_control_t control;
   int connect_timeout = agent_http_effective_connect_timeout_ms(timeout_ms);
   if (aimee_core_control_init_timeout(&control, connect_timeout, HTTP_CANCEL_POLL_MS,
                                       agent_request_cancelled ? core_agent_cancelled : NULL,
                                       NULL) != AIMEE_CORE_OK)
      return -1;
   int fd = -1;
   if (aimee_core_socket_connect_controlled(host, port_str, flags | AIMEE_CORE_CONNECT_NONBLOCKING,
                                            &control, &fd) != AIMEE_CORE_OK)
      return -1;
   return fd;
}

/* Send HTTP CONNECT to establish a tunnel through `proxy_host:proxy_port` to
 * `target_host:target_port`.  The fd must already be connected to the proxy.
 * Returns 0 on success (proxy returned "200"), -1 on failure. */
static int proxy_connect_tunnel(http_conn_t *conn, const char *target_host, int target_port,
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
   size_t written = 0;
   if (aimee_core_socket_write_all_controlled(conn->fd, req, (size_t)req_len, &conn->control,
                                              &written) != AIMEE_CORE_OK)
      return -1;

   /* Read the response line (up to 512 bytes) */
   char resp[512];
   int resp_len = 0;
   while (resp_len < (int)sizeof(resp) - 1)
   {
      size_t received = 0;
      if (aimee_core_socket_read_controlled(conn->fd, resp + resp_len, 1, &conn->control,
                                            &received) != AIMEE_CORE_OK)
         return -1;
      resp_len += (int)received;
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
   conn->fd = -1;
   conn->ssl = NULL;
   if (aimee_core_control_init_timeout(
           &conn->control, timeout_ms > 0 ? timeout_ms : 0, HTTP_CANCEL_POLL_MS,
           agent_request_cancelled ? core_agent_cancelled : NULL, NULL) != AIMEE_CORE_OK)
      return -1;

   const proxy_bootstrap_t *pb = proxy_get();
   /* A pinned (SSRF-validated) fetch connects directly to the checked IP and
    * bypasses any forward proxy — the egress decision was made on that IP. */
   int use_proxy =
       !url->pinned_ip[0] && pb && pb->has_proxy && url->use_ssl && proxy_should_use(pb, url->host);

   if (url->pinned_ip[0])
   {
      conn->fd = connect_controlled(url->pinned_ip, url->port, timeout_ms,
                                    AIMEE_CORE_CONNECT_NUMERIC_HOST);
      if (conn->fd < 0)
         return -1;
   }
   else if (use_proxy)
   {
      /* Connect to the proxy, then tunnel to the target via CONNECT */
      conn->fd = connect_controlled(pb->https_proxy.host, pb->https_proxy.port, timeout_ms, 0);
      if (conn->fd < 0)
      {
         aimee_log(LOG_WARN, "proxy", "failed to connect to proxy %s:%d", pb->https_proxy.host,
                   pb->https_proxy.port);
         return -1;
      }

      if (proxy_connect_tunnel(conn, url->host, url->port, pb->https_proxy.auth) != 0)
      {
         aimee_core_socket_close(conn->fd);
         conn->fd = -1;
         return -1;
      }
   }
   else
   {
      conn->fd = connect_controlled(url->host, url->port, timeout_ms, 0);
      if (conn->fd < 0)
         return -1;
   }

   if (url->use_ssl)
   {
      SSL_CTX *ctx = ssl_ctx_for(url);
      if (!ctx)
      {
         aimee_core_socket_close(conn->fd);
         conn->fd = -1;
         return -1;
      }
      conn->ssl = SSL_new(ctx);
      if (!conn->ssl)
      {
         aimee_core_socket_close(conn->fd);
         conn->fd = -1;
         return -1;
      }
      if (SSL_set_fd(conn->ssl, conn->fd) != 1 ||
          aimee_core_tls_configure_client_session(conn->ssl, url->host, 1) != 0)
      {
         SSL_free(conn->ssl);
         aimee_core_socket_close(conn->fd);
         conn->fd = -1;
         conn->ssl = NULL;
         return -1;
      }

      if (aimee_core_tls_handshake_client_controlled(conn->ssl, &conn->control) != AIMEE_CORE_OK)
      {
         /* Name the handshake. The caller's only message is "TCP connect failed",
          * which is actively misleading here: the TCP connect succeeded and TLS is
          * what failed. Debugging the sidecar hop cost an hour to this one line --
          * OpenSSL knew it was "tlsv1 alert unknown ca" the whole time. */
         char ebuf[256] = "";
         unsigned long e = ERR_peek_last_error();
         if (e)
            ERR_error_string_n(e, ebuf, sizeof(ebuf));
         aimee_log(LOG_ERROR, "agent_http", "TLS handshake failed with %s:%d%s%s", url->host,
                   url->port, ebuf[0] ? ": " : "", ebuf);
         SSL_free(conn->ssl);
         aimee_core_socket_close(conn->fd);
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
      aimee_core_tls_session_free(conn->ssl);
      conn->ssl = NULL;
   }
   if (conn->fd >= 0)
   {
      aimee_core_socket_close(conn->fd);
      conn->fd = -1;
   }
}

static ssize_t conn_write(http_conn_t *conn, const void *buf, size_t len)
{
   size_t written = 0;
   aimee_core_result_t result =
       conn->ssl
           ? aimee_core_tls_write_all_controlled(conn->ssl, buf, len, &conn->control, &written)
           : aimee_core_socket_write_all_controlled(conn->fd, buf, len, &conn->control, &written);
   return result == AIMEE_CORE_OK ? (ssize_t)written : -1;
}

static ssize_t conn_read(http_conn_t *conn, void *buf, size_t len)
{
   size_t received = 0;
   aimee_core_result_t result =
       conn->ssl ? aimee_core_tls_read_controlled(conn->ssl, buf, len, &conn->control, &received)
                 : aimee_core_socket_read_controlled(conn->fd, buf, len, &conn->control, &received);
   return result == AIMEE_CORE_OK ? (ssize_t)received : (result == AIMEE_CORE_EOF ? 0 : -1);
}

/* Write all bytes, retrying short writes */
static int conn_write_all(http_conn_t *conn, const char *buf, size_t len)
{
   return conn_write(conn, buf, len) == (ssize_t)len ? 0 : -1;
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

   if (content_type && content_type[0])
   {
      /* Accept either a full header line ("Content-Type: application/json") or a
       * bare media type ("application/json"): callers pass both conventions. A
       * bare value emitted verbatim is a header line with no field name — an
       * invalid HTTP header that some servers (e.g. api.github.com) reject with
       * 400 before parsing the body. Prefix the field name when it is absent. */
      const char *prefix = strchr(content_type, ':') ? "" : "Content-Type: ";
      int n = snprintf(req + off, cap - (size_t)off, "%s%s\r\n", prefix, content_type);
      if (n > 0 && (size_t)off + (size_t)n < cap)
         off += n;
   }

   if (auth_header && auth_header[0])
   {
      int n = snprintf(req + off, cap - (size_t)off, "%s\r\n", auth_header);
      if (n > 0 && (size_t)off + (size_t)n < cap)
         off += n;
   }

   /* Append extra headers, one per line.
    *
    * Callers use BOTH conventions for the separator: some pass "A: x\nB: y",
    * others pass "A: x\r\n". Splitting on '\n' alone leaves a trailing '\r' on
    * every line of the second form, which is then re-terminated here and emits
    * "A: x\r\r\n" -- a bare CR inside the header block.
    *
    * Lenient origins ignore that; CDNs do not. Measured live: with a trailing
    * "\r\n" Accept header, example.com returned 200 while
    * raw.githubusercontent.com and en.wikipedia.org both returned 400. That is
    * why page reads failed against real sites while search (which happened to
    * use bare '\n') worked. Trim the CR so both conventions are accepted. */
   if (extra_headers && extra_headers[0])
   {
      char tmp[512];
      snprintf(tmp, sizeof(tmp), "%s", extra_headers);
      char *saveptr;
      char *line = strtok_r(tmp, "\n", &saveptr);
      while (line)
      {
         size_t ll = strlen(line);
         while (ll > 0 && (line[ll - 1] == '\r' || line[ll - 1] == ' '))
            line[--ll] = '\0';
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
/* Retry-After (seconds) parsed from the most recent buffered HTTP response on
 * this thread, or 0 if it carried none. Set on every parse_response_headers call.
 * Read by the Anthropic ingress to relay an upstream 429/529 Retry-After to the
 * client for exact parity. */
static __thread int g_last_retry_after = 0;

int agent_http_last_retry_after(void)
{
   return g_last_retry_after;
}

static int parse_response_headers(char *buf, size_t len, size_t *header_end, int *chunked,
                                  size_t *content_length)
{
   *chunked = 0;
   *content_length = 0;
   *header_end = 0;
   g_last_retry_after = 0;

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

   /* Check for Retry-After (seconds; the provider APIs send a delta, not a date).
    * Match at a header-line start (line begin or just after a "\r\n") so it does
    * not false-match a substring inside another header's value. */
   for (char *p = buf; *p; p++)
   {
      if ((p == buf || (p[-1] == '\n')) && (*p == 'R' || *p == 'r') &&
          strncasecmp(p, "Retry-After:", 12) == 0)
      {
         char *val = p + 12;
         while (*val == ' ' || *val == '\t')
            val++;
         g_last_retry_after = atoi(val);
         if (g_last_retry_after < 0)
            g_last_retry_after = 0;
         break;
      }
   }

   /* Restore the header terminator */
   *hdr_end = '\r';
   return status;
}

/* Read a full HTTP response (headers + body) into *out_body, returning status code.
 * For buffered (non-streaming) reads. */
/* Copy one response header's value out of the raw header block. Case-insensitive
 * on the name, as HTTP requires; the block is `buf[0..header_end)` and is still
 * intact at the point this is called. */
static void http_header_value(const char *buf, size_t header_end, const char *name, char *out,
                              size_t out_cap)
{
   out[0] = '\0';
   size_t name_len = strlen(name);
   for (size_t i = 0; i + name_len + 1 < header_end; i++)
   {
      if (i && buf[i - 1] != '\n')
         continue; /* only at the start of a header line */
      if (strncasecmp(buf + i, name, name_len) != 0 || buf[i + name_len] != ':')
         continue;
      const char *v = buf + i + name_len + 1;
      while (*v == ' ' || *v == '\t')
         v++;
      size_t n = 0;
      while (v[n] && v[n] != '\r' && v[n] != '\n' && (size_t)(v - buf) + n < header_end &&
             n + 1 < out_cap)
         n++;
      memcpy(out, v, n);
      out[n] = '\0';
      return;
   }
}

/* As http_read_response, plus one header captured for the caller. Exists so a
 * caller can act on a redirect (Location) itself rather than have this layer
 * follow it: a 302 to pre-signed storage must NOT carry the Authorization header
 * onward, and only the caller knows which of its headers are host-bound. */
static int http_read_response_hdr(http_conn_t *conn, char **out_body, size_t *out_len,
                                  const char *want_header, char *hdr_out, size_t hdr_cap)
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
   if (want_header && hdr_out && hdr_cap)
      http_header_value(buf, header_end, want_header, hdr_out, hdr_cap);

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

static int http_read_response(http_conn_t *conn, char **out_body, size_t *out_len)
{
   return http_read_response_hdr(conn, out_body, out_len, NULL, NULL, 0);
}

/* ---- Public API ---- */

/* GET that REPORTS a redirect instead of following it: on a 3xx the target lands
 * in `location` and the caller decides what to re-send. Deliberately not a
 * following GET — a forge's log endpoint redirects to pre-signed third-party
 * storage, and blindly replaying the request there would hand that host the
 * Authorization header (the forge token). Only the caller knows which of its
 * headers are host-bound, so only the caller can safely follow. */
int agent_http_get_location(const char *url, const char *extra_headers, char *location,
                            size_t location_cap, char **response_buf, int timeout_ms)
{
   *response_buf = NULL;
   if (location && location_cap)
      location[0] = '\0';

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
   int status =
       http_read_response_hdr(&conn, response_buf, &resp_len, "Location", location, location_cap);
   conn_close(&conn);
   if (status < 0)
   {
      free(*response_buf);
      *response_buf = NULL;
   }
   return status;
}

/* SSRF-safe GET: connect to `pinned_ip` (a numeric IP the caller already
 * validated against the egress deny-list) while keeping Host/SNI = the URL host.
 * No DNS re-resolution, so a rebinding resolver cannot swap in a private IP
 * between check and connect. */
int agent_http_get_pinned(const char *url, const char *pinned_ip, const char *extra_headers,
                          char **response_buf, int timeout_ms)
{
   *response_buf = NULL;
   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;
   if (pinned_ip && pinned_ip[0])
      snprintf(pu.pinned_ip, sizeof(pu.pinned_ip), "%s", pinned_ip);

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
   return agent_http_post_bytes(url, auth_header, body, body ? strlen(body) : 0, response_buf,
                                timeout_ms, extra_headers);
}

int agent_http_post_bytes(const char *url, const char *auth_header, const void *body,
                          size_t body_len, char **response_buf, int timeout_ms,
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
             pu.host, pu.port, body_len);
   if (send_request(&conn, "POST", &pu, "Content-Type: application/json", auth_header,
                    extra_headers, "aimee/1.0", body, body_len) < 0)
   {
      aimee_log(LOG_ERROR, "agent_http", "send_request failed: %s:%d", pu.host, pu.port);
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
      aimee_log(LOG_ERROR, "agent_http", "read_response failed: %s:%d", pu.host, pu.port);
   }
   return status;
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

int agent_http_patch(const char *url, const char *auth_header, const char *body,
                     char **response_buf, int timeout_ms, const char *extra_headers)
{
   *response_buf = NULL;

   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "PATCH", &pu, "Content-Type: application/json", auth_header,
                    extra_headers, "aimee/1.0", body, body ? strlen(body) : 0) < 0)
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
      aimee_log(LOG_ERROR, "agent_http", "PATCH request failed");
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
   return agent_http_post_stream_bytes(url, auth_header, body, body ? strlen(body) : 0, callback,
                                       userdata, timeout_ms, extra_headers);
}

int agent_http_post_stream_bytes(const char *url, const char *auth_header, const void *body,
                                 size_t body_len, agent_http_stream_cb callback, void *userdata,
                                 int timeout_ms, const char *extra_headers)
{
   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "POST", &pu, "Content-Type: application/json", auth_header,
                    extra_headers, "aimee/1.0", body, body_len) < 0)
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
