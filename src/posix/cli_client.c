/* cli_client.c: shared client library. aimee-server is reached over its /v1 HTTP
 * surface (cli_http_request / cli_v1_rpc_local); the cli_connect/cli_request
 * NDJSON primitives remain only for the aimee-kb sidecar socket. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cli_client.h"
#include "aimee_home.h"
#include "aimee_version.h"
#include "cli_server_compat.h"
#include "platform_path.h"
#include "cJSON.h"
#define RPC_PROTOCOL_VERSION 1
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef AIMEE_POSIX
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

/* macOS / BSD lack the Linux-only SOCK_CLOEXEC socket() flag. Fall back to 0 and
 * set close-on-exec explicitly via cli_fd_cloexec() after each socket(). On
 * Linux (where SOCK_CLOEXEC already applied it atomically) the post-hoc set is a
 * harmless no-op. */
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
static void cli_fd_cloexec(int fd)
{
   if (fd < 0)
      return;
   int flags = fcntl(fd, F_GETFD);
   if (flags >= 0)
      (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

#define SERVER_READY_TIMEOUT_MS          30000
#define STALE_SERVER_SHUTDOWN_TIMEOUT_MS 30000
#else
#include <io.h>
#include <process.h>
#endif

static const char *cli_config_dir(void)
{
   static char dir[4096];
   if (dir[0])
      return dir;
   const char *base = aimee_home();
   if (!base)
      base = "/tmp/.config/aimee";
   snprintf(dir, sizeof(dir), "%s", base);
   return dir;
}

const char *cli_default_socket_path(void)
{
   static char path[4096];
   if (path[0])
      return path;

   snprintf(path, sizeof(path), "%s/%s", cli_config_dir(), "aimee.sock");
   return path;
}

/* The co-located /v1 HTTP UDS (config_dir/aimee-http.sock) — the client's sole
 * transport to a local aimee-server now that the NDJSON socket is retired. */
static const char *cli_http_sock_path(void)
{
   static char path[4096];
   if (path[0])
      return path;
   snprintf(path, sizeof(path), "%s/%s", cli_config_dir(), "aimee-http.sock");
   return path;
}

#ifdef AIMEE_POSIX
/* True iff a co-located aimee-server answers GET /v1/health over the local HTTP
 * UDS within timeout_ms. The local UDS is filesystem-trusted (no token); a 2xx
 * response means the server is up and serving /v1. This is the liveness probe
 * that replaced the NDJSON `server.info` handshake — health-only by design (the
 * /v1 surface + /v1/rpc bridge make per-method/version gating unnecessary, and
 * strict version matching historically caused dev-vs-installed restart loops). */
static int cli_http_health_ok(int timeout_ms)
{
   char endpoint[4200];
   snprintf(endpoint, sizeof(endpoint), "unix:%s", cli_http_sock_path());
   int status = 0;
   cJSON *resp = cli_http_request(endpoint, "GET", "/v1/health", NULL, NULL,
                                  timeout_ms > 0 ? timeout_ms : CLIENT_CONNECT_TIMEOUT_MS, &status);
   int ok = 0;
   if (resp)
   {
      if (status >= 200 && status < 300)
      {
         cJSON *st = cJSON_GetObjectItemCaseSensitive(resp, "status");
         ok = !st || (cJSON_IsString(st) && strcmp(st->valuestring, "ok") == 0);
      }
      cJSON_Delete(resp);
   }
   return ok;
}
#endif

static int last_connect_errno = 0;
static char last_connect_path[4096] = "";

static void cli_record_connect_errno(const char *socket_path, int err)
{
   last_connect_errno = err;
   snprintf(last_connect_path, sizeof(last_connect_path), "%s", socket_path ? socket_path : "");
}

static void cli_clear_connect_errno(void)
{
   last_connect_errno = 0;
   last_connect_path[0] = '\0';
}

int cli_last_connect_errno(void)
{
   return last_connect_errno;
}

const char *cli_last_connect_path(void)
{
   return last_connect_path;
}

int cli_connect_errno_is_permission_denied(int err)
{
   return err == EACCES || err == EPERM;
}

int cli_connect_timeout(cli_conn_t *conn, const char *socket_path, int timeout_ms)
{
   if (!socket_path)
      socket_path = cli_default_socket_path();

   cli_clear_connect_errno();
   memset(conn, 0, sizeof(*conn));
   conn->fd = -1;

   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (fd < 0)
   {
      cli_record_connect_errno(socket_path, errno);
      return -1;
   }
   cli_fd_cloexec(fd);

   /* Set non-blocking for connect timeout */
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
   {
      int err = errno;
      close(fd);
      cli_record_connect_errno(socket_path, err);
      return -1;
   }

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

   int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
   if (rc < 0 && errno != EINPROGRESS)
   {
      int err = errno;
      close(fd);
      cli_record_connect_errno(socket_path, err);
      return -1;
   }

   if (rc < 0)
   {
      /* Wait for connect with timeout */
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      rc = poll(&pfd, 1, timeout_ms);
      if (rc <= 0)
      {
         int err = rc == 0 ? ETIMEDOUT : errno;
         close(fd);
         cli_record_connect_errno(socket_path, err);
         return -1;
      }

      /* Check for connect error */
      int err = 0;
      socklen_t elen = sizeof(err);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0)
      {
         err = errno;
         close(fd);
         cli_record_connect_errno(socket_path, err);
         return -1;
      }
      if (err != 0)
      {
         close(fd);
         cli_record_connect_errno(socket_path, err);
         return -1;
      }
   }

   /* Set back to blocking */
   fcntl(fd, F_SETFL, flags);

   conn->fd = fd;
   conn->read_len = 0;
   return 0;
}

int cli_connect(cli_conn_t *conn, const char *socket_path)
{
   return cli_connect_timeout(conn, socket_path, CLIENT_CONNECT_TIMEOUT_MS);
}

/* ── /v1 HTTP transport ─────────────────────────────────────────────────────
 * A minimal HTTP/1.1 client for aimee-server's /v1 surface, over UDS (same
 * host) or 127.0.0.1:port. This is the thin client's only transport to
 * aimee-server; the cli_connect/cli_request primitives above survive solely for
 * the aimee-kb NDJSON sidecar socket (server/kb_client.c). */

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
 * cli_rpc_routes.inc (included by both posix/ and windows/cli_client.c) so the
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

/* cli_v1_pathid_route_for_method moved to the shared cli_rpc_routes.inc. */

int cli_http_build_request(const char *method, const char *path, const char *host,
                           const char *bearer, const char *body, char *buf, size_t cap)
{
   if (!method || !path || !buf || cap == 0)
      return -1;
   if (!host || !host[0])
      host = "localhost";
   size_t body_len = body ? strlen(body) : 0;

   int n;
   if (bearer && bearer[0])
      n = snprintf(buf, cap,
                   "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
                   "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\n\r\n",
                   method, path, host, bearer, body_len);
   else
      n = snprintf(buf, cap,
                   "%s %s HTTP/1.1\r\nHost: %s\r\n"
                   "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\n\r\n",
                   method, path, host, body_len);
   if (n < 0 || (size_t)n >= cap)
      return -1;
   if (body_len)
   {
      if ((size_t)n + body_len >= cap)
         return -1;
      memcpy(buf + n, body, body_len);
      n += (int)body_len;
      buf[n] = '\0';
   }
   return n;
}

/* Connect to `endpoint` (UDS path / "unix:<path>" or "[tcp:]host:port") and
 * return a blocking fd, or -1. The TCP host may be a DNS name, an IPv4 literal,
 * or a bracketed IPv6 literal ("[::1]:8740"); resolution is via getaddrinfo.
 * connect_timeout_ms bounds each TCP connect attempt (<=0 ⇒
 * CLIENT_DEFAULT_TIMEOUT_MS) so an unreachable/filtered host fails fast instead
 * of blocking on the kernel default (~2 min). Writes the HTTP Host header value
 * into host_out. */
static int cli_http_connect(const char *endpoint, char *host_out, size_t host_n,
                            int connect_timeout_ms)
{
   if (host_out && host_n)
      snprintf(host_out, host_n, "localhost");
   if (!endpoint || !endpoint[0])
      return -1;
   if (connect_timeout_ms <= 0)
      connect_timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

   const char *uds = NULL;
   if (strncmp(endpoint, "unix:", 5) == 0)
      uds = endpoint + 5;
   else if (endpoint[0] == '/')
      uds = endpoint;

   if (uds)
   {
      int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0)
         return -1;
      cli_fd_cloexec(fd);
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", uds);
      if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
      {
         close(fd);
         return -1;
      }
      return fd;
   }

   /* TCP: "[tcp:]host:port". Split host (DNS / IPv4 / bracketed IPv6) and port. */
   const char *hp = endpoint;
   if (strncmp(hp, "tcp:", 4) == 0)
      hp += 4;
   char host[256];
   const char *port_str;
   if (hp[0] == '[')
   {
      const char *close_br = strchr(hp, ']');
      if (!close_br || close_br[1] != ':')
         return -1;
      size_t hl = (size_t)(close_br - hp - 1);
      if (hl == 0 || hl >= sizeof(host))
         return -1;
      memcpy(host, hp + 1, hl);
      host[hl] = '\0';
      port_str = close_br + 2;
   }
   else
   {
      const char *colon = strrchr(hp, ':');
      if (!colon)
         return -1;
      size_t hl = (size_t)(colon - hp);
      if (hl == 0 || hl >= sizeof(host))
         return -1;
      memcpy(host, hp, hl);
      host[hl] = '\0';
      port_str = colon + 1;
   }
   /* Require a numeric, in-range port (getaddrinfo would otherwise accept
    * service names like "http"). */
   if (!port_str[0])
      return -1;
   {
      int port = atoi(port_str);
      if (port <= 0 || port > 65535)
         return -1;
   }

   struct addrinfo hints, *res = NULL;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC; /* IPv4 or IPv6 */
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
      return -1;

   int out_fd = -1;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
   {
      int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
      if (fd < 0)
         continue;
      cli_fd_cloexec(fd);
      int rc;
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0)
         fcntl(fd, F_SETFL, flags | O_NONBLOCK); /* non-blocking connect for the timeout */
      rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
      if (rc != 0 && errno == EINPROGRESS)
      {
         struct pollfd pfd = {.fd = fd, .events = POLLOUT};
         rc = -1;
         if (poll(&pfd, 1, connect_timeout_ms) == 1 && (pfd.revents & POLLOUT))
         {
            int err = 0;
            socklen_t el = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
               rc = 0;
         }
      }
      if (rc == 0)
      {
         if (flags >= 0)
            fcntl(fd, F_SETFL, flags); /* restore blocking for the read/write loop */
         out_fd = fd;
         break;
      }
      close(fd);
   }
   freeaddrinfo(res);
   if (out_fd >= 0 && host_out && host_n)
      snprintf(host_out, host_n, "%s", host);
   return out_fd;
}

cJSON *cli_http_request(const char *endpoint, const char *method, const char *path,
                        const char *body_json, const char *bearer, int timeout_ms, int *http_status)
{
   if (http_status)
      *http_status = 0;
   if (!endpoint || !method || !path)
      return NULL;
   if (timeout_ms <= 0)
      timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

   char host[256];
   int fd = cli_http_connect(endpoint, host, sizeof(host), timeout_ms);
   if (fd < 0)
      return NULL;

   /* Build the request (sized for headers + body + slack). */
   size_t blen = body_json ? strlen(body_json) : 0;
   size_t reqcap = blen + strlen(path) + strlen(host) + (bearer ? strlen(bearer) : 0) + 256;
   char *req = malloc(reqcap);
   if (!req)
   {
      close(fd);
      return NULL;
   }
   int reqlen = cli_http_build_request(method, path, host, bearer, body_json, req, reqcap);
   if (reqlen < 0)
   {
      free(req);
      close(fd);
      return NULL;
   }

   size_t total = 0;
   while (total < (size_t)reqlen)
   {
      ssize_t n = write(fd, req + total, (size_t)reqlen - total);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         free(req);
         close(fd);
         return NULL;
      }
      total += (size_t)n;
   }
   free(req);

   /* Read the full response (server sends Connection: close, so read to EOF). */
   size_t cap = 8192, len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      close(fd);
      return NULL;
   }
   for (;;)
   {
      struct pollfd pfd = {.fd = fd, .events = POLLIN};
      int rc = poll(&pfd, 1, timeout_ms);
      if (rc <= 0)
      {
         free(buf);
         close(fd);
         return NULL;
      }
      if (len + 4096 > cap)
      {
         if (cap >= CLIENT_MAX_RESPONSE_SIZE)
         {
            free(buf);
            close(fd);
            return NULL;
         }
         size_t next = cap * 2;
         char *grown = realloc(buf, next);
         if (!grown)
         {
            free(buf);
            close(fd);
            return NULL;
         }
         buf = grown;
         cap = next;
      }
      ssize_t n = read(fd, buf + len, cap - len - 1);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         free(buf);
         close(fd);
         return NULL;
      }
      if (n == 0)
         break; /* EOF */
      len += (size_t)n;
   }
   close(fd);
   buf[len] = '\0';

   /* Status line: "HTTP/1.1 <code> <reason>". */
   int status = 0;
   if (strncmp(buf, "HTTP/", 5) == 0)
   {
      const char *sp = strchr(buf, ' ');
      if (sp)
         status = atoi(sp + 1);
   }
   /* JSON body follows the blank line. */
   const char *body = strstr(buf, "\r\n\r\n");
   cJSON *parsed = body ? cJSON_Parse(body + 4) : NULL;
   free(buf);
   if (http_status)
      *http_status = status;
   return parsed;
}

/* Streaming counterpart to cli_http_request(): sends the same HTTP request, then
 * parses a Server-Sent Events response body ("data: <json>\n", events separated
 * by blank lines, terminated by "data: [DONE]"). cb(event, userdata) is invoked
 * for each parsed event (return nonzero from cb to stop early). Returns the last
 * parsed event (caller cJSON_Delete()s it) or NULL on connect/transport failure;
 * *http_status is set from the status line like cli_http_request(). */
cJSON *cli_http_request_stream(const char *endpoint, const char *method, const char *path,
                               const char *body_json, const char *bearer, int timeout_ms,
                               int *http_status, cli_stream_cb cb, void *userdata)
{
   if (http_status)
      *http_status = 0;
   if (!endpoint || !method || !path)
      return NULL;
   if (timeout_ms <= 0)
      timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

   char host[256];
   int fd = cli_http_connect(endpoint, host, sizeof(host), timeout_ms);
   if (fd < 0)
      return NULL;

   size_t blen = body_json ? strlen(body_json) : 0;
   size_t reqcap = blen + strlen(path) + strlen(host) + (bearer ? strlen(bearer) : 0) + 256;
   char *req = malloc(reqcap);
   if (!req)
   {
      close(fd);
      return NULL;
   }
   int reqlen = cli_http_build_request(method, path, host, bearer, body_json, req, reqcap);
   if (reqlen < 0)
   {
      free(req);
      close(fd);
      return NULL;
   }
   size_t total = 0;
   while (total < (size_t)reqlen)
   {
      ssize_t n = write(fd, req + total, (size_t)reqlen - total);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         free(req);
         close(fd);
         return NULL;
      }
      total += (size_t)n;
   }
   free(req);

   size_t cap = 8192, len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      close(fd);
      return NULL;
   }
   int headers_done = 0;
   size_t scan = 0; /* offset of the next unparsed byte */
   cJSON *last = NULL;
   int stop = 0;
   while (!stop)
   {
      struct pollfd pfd = {.fd = fd, .events = POLLIN};
      if (poll(&pfd, 1, timeout_ms) <= 0)
         break;
      if (len + 4096 > cap)
      {
         if (cap >= CLIENT_MAX_RESPONSE_SIZE)
            break;
         size_t next = cap * 2;
         char *grown = realloc(buf, next);
         if (!grown)
            break;
         buf = grown;
         cap = next;
      }
      ssize_t n = read(fd, buf + len, cap - len - 1);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (n == 0)
         break; /* EOF */
      len += (size_t)n;
      buf[len] = '\0';

      if (!headers_done)
      {
         char *sep = strstr(buf, "\r\n\r\n");
         if (!sep)
            continue; /* headers incomplete */
         if (http_status && strncmp(buf, "HTTP/", 5) == 0)
         {
            const char *sp = strchr(buf, ' ');
            if (sp)
               *http_status = atoi(sp + 1);
         }
         headers_done = 1;
         scan = (size_t)(sep - buf) + 4;
      }

      for (;;)
      {
         char *nl = memchr(buf + scan, '\n', len - scan);
         if (!nl)
            break;
         char *line = buf + scan;
         size_t line_len = (size_t)(nl - line);
         scan = (size_t)(nl - buf) + 1;
         if (line_len > 0 && line[line_len - 1] == '\r')
            line[--line_len] = '\0';
         else
            *nl = '\0';
         if (strncmp(line, "data:", 5) != 0)
            continue;
         const char *payload = line + 5;
         while (*payload == ' ')
            payload++;
         if (strcmp(payload, "[DONE]") == 0)
         {
            stop = 1;
            break;
         }
         cJSON *ev = cJSON_Parse(payload);
         if (!ev)
            continue;
         int rc = cb ? cb(ev, userdata) : 0;
         if (last)
            cJSON_Delete(last);
         last = ev;
         if (rc != 0)
         {
            stop = 1;
            break;
         }
      }
   }
   free(buf);
   close(fd);
   return last;
}

/* NDJSON streaming counterpart to cli_http_request_stream(): for endpoints whose
 * response body is the native aimee event stream — one JSON object per line,
 * intermediate events carry an "event" field and the final message carries a
 * "status" field (exactly like the legacy NDJSON socket via cli_request_stream).
 * Used to consume POST /v1/chat/stream. cb(event,userdata) is invoked for each
 * intermediate event (return nonzero to abort); the final "status" object is the
 * return value (caller cJSON_Delete()s it). on_open(fd,on_open_ud), when
 * non-NULL, is called once with the live socket fd right after connect so an
 * external thread can force-close it to interrupt the turn (TUI abort); it is
 * called again with -1 just before the fd is closed. Returns NULL on
 * connect/transport failure; *http_status is set from the status line. */
cJSON *cli_http_request_stream_ndjson(const char *endpoint, const char *method, const char *path,
                                      const char *body_json, const char *bearer, int timeout_ms,
                                      int *http_status, cli_stream_cb cb, void *userdata,
                                      void (*on_open)(int fd, void *ud), void *on_open_ud)
{
   if (http_status)
      *http_status = 0;
   if (!endpoint || !method || !path)
      return NULL;
   /* poll(2) semantics: timeout_ms == -1 waits indefinitely (a chat turn has no
    * idle bound), so only a 0 timeout falls back to the default. Connect always
    * uses a finite bound. */
   int connect_timeout_ms = timeout_ms > 0 ? timeout_ms : CLIENT_CONNECT_TIMEOUT_MS;
   if (timeout_ms == 0)
      timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

   char host[256];
   int fd = cli_http_connect(endpoint, host, sizeof(host), connect_timeout_ms);
   if (fd < 0)
      return NULL;
   if (on_open)
      on_open(fd, on_open_ud);

   size_t blen = body_json ? strlen(body_json) : 0;
   size_t reqcap = blen + strlen(path) + strlen(host) + (bearer ? strlen(bearer) : 0) + 256;
   char *req = malloc(reqcap);
   if (!req)
   {
      if (on_open)
         on_open(-1, on_open_ud);
      close(fd);
      return NULL;
   }
   int reqlen = cli_http_build_request(method, path, host, bearer, body_json, req, reqcap);
   if (reqlen < 0)
   {
      free(req);
      if (on_open)
         on_open(-1, on_open_ud);
      close(fd);
      return NULL;
   }
   size_t total = 0;
   while (total < (size_t)reqlen)
   {
      ssize_t n = write(fd, req + total, (size_t)reqlen - total);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         free(req);
         if (on_open)
            on_open(-1, on_open_ud);
         close(fd);
         return NULL;
      }
      total += (size_t)n;
   }
   free(req);

   size_t cap = CLIENT_READ_BUF_SIZE, len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      if (on_open)
         on_open(-1, on_open_ud);
      close(fd);
      return NULL;
   }
   int headers_done = 0;
   size_t scan = 0; /* offset of the next unparsed body byte */
   cJSON *final = NULL;
   for (;;)
   {
      /* Parse any complete newline-delimited objects already buffered (body
       * only — scan starts after the header terminator once seen). */
      while (headers_done)
      {
         char *nl = memchr(buf + scan, '\n', len - scan);
         if (!nl)
            break;
         *nl = '\0';
         cJSON *msg = cJSON_Parse(buf + scan);
         scan = (size_t)(nl - buf) + 1;
         if (!msg)
            continue; /* skip unparseable lines */
         if (cJSON_GetObjectItem(msg, "status"))
         {
            final = msg;
            free(buf);
            close(fd);
            if (on_open)
               on_open(-1, on_open_ud);
            return final;
         }
         if (cb)
         {
            int rc = cb(msg, userdata);
            cJSON_Delete(msg);
            if (rc != 0)
            {
               free(buf);
               close(fd);
               if (on_open)
                  on_open(-1, on_open_ud);
               return NULL; /* callback requested abort */
            }
         }
         else
         {
            cJSON_Delete(msg);
         }
      }

      /* Compact consumed bytes so a long turn doesn't grow the buffer without
       * bound (each parsed line advances `scan`; the unparsed tail moves front). */
      if (headers_done && scan > 0)
      {
         memmove(buf, buf + scan, len - scan);
         len -= scan;
         scan = 0;
      }

      if (len + 4096 > cap)
      {
         if (cap >= CLIENT_MAX_RESPONSE_SIZE)
            break;
         size_t next = cap * 2;
         if (next > CLIENT_MAX_RESPONSE_SIZE)
            next = CLIENT_MAX_RESPONSE_SIZE;
         char *grown = realloc(buf, next);
         if (!grown)
            break;
         buf = grown;
         cap = next;
      }

      struct pollfd pfd = {.fd = fd, .events = POLLIN};
      int prc;
      do
      {
         prc = poll(&pfd, 1, timeout_ms);
      } while (prc < 0 && errno == EINTR);
      if (prc <= 0)
         break;
      ssize_t n = read(fd, buf + len, cap - 1 - len);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (n == 0)
         break; /* EOF */
      len += (size_t)n;
      buf[len] = '\0';

      if (!headers_done)
      {
         char *sep = strstr(buf, "\r\n\r\n");
         if (!sep)
            continue; /* headers incomplete */
         if (http_status && strncmp(buf, "HTTP/", 5) == 0)
         {
            const char *sp = strchr(buf, ' ');
            if (sp)
               *http_status = atoi(sp + 1);
         }
         headers_done = 1;
         scan = (size_t)(sep - buf) + 4;
      }
   }

   free(buf);
   close(fd);
   if (on_open)
      on_open(-1, on_open_ud);
   return NULL;
}

cJSON *cli_request(cli_conn_t *conn, cJSON *request, int timeout_ms)
{
   if (!conn || conn->fd < 0 || !request)
      return NULL;

   /* Serialize request */
   char *json_str = cJSON_PrintUnformatted(request);
   if (!json_str)
      return NULL;

   size_t json_len = strlen(json_str);

   /* Write request + newline */
   size_t total = 0;
   while (total < json_len)
   {
      ssize_t n = write(conn->fd, json_str + total, json_len - total);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         free(json_str);
         return NULL;
      }
      total += (size_t)n;
   }
   free(json_str);

   /* Write newline delimiter */
   char nl = '\n';
   if (write(conn->fd, &nl, 1) != 1)
      return NULL;

   /* Read response until newline. Some report-style RPCs can legitimately
    * exceed the small per-connection scratch buffer, so grow a bounded
    * request-local buffer for non-streaming responses. */
   conn->read_len = 0;
   size_t cap = CLIENT_READ_BUF_SIZE;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t len = 0;
   for (;;)
   {
      /* Check for newline in buffer */
      for (size_t i = 0; i < len; i++)
      {
         if (buf[i] == '\n')
         {
            buf[i] = '\0';
            cJSON *resp = cJSON_Parse(buf);
            free(buf);
            conn->read_len = 0;
            return resp;
         }
      }

      /* Buffer full without newline */
      if (len >= cap - 1)
      {
         if (cap >= CLIENT_MAX_RESPONSE_SIZE)
         {
            free(buf);
            return NULL;
         }
         size_t next = cap * 2;
         if (next > CLIENT_MAX_RESPONSE_SIZE)
            next = CLIENT_MAX_RESPONSE_SIZE;
         char *grown = realloc(buf, next);
         if (!grown)
         {
            free(buf);
            return NULL;
         }
         buf = grown;
         cap = next;
      }

      /* Wait for data */
      struct pollfd pfd = {.fd = conn->fd, .events = POLLIN};
      int rc = poll(&pfd, 1, timeout_ms);
      if (rc <= 0)
      {
         free(buf);
         return NULL;
      }

      ssize_t n = read(conn->fd, buf + len, cap - 1 - len);
      if (n <= 0)
      {
         free(buf);
         return NULL;
      }
      len += (size_t)n;
      buf[len] = '\0';
   }
}

int cli_server_available(const char *socket_path)
{
   (void)socket_path; /* the co-located server is reached over the /v1 HTTP UDS */
   return cli_http_health_ok(CLIENT_CONNECT_TIMEOUT_MS);
}

/* cli_v1_rpc_local now lives in the shared cli_rpc_routes.inc — it resolves the
 * method's first-class /v1 route (no more POST /v1/rpc bridge). */

void cli_close(cli_conn_t *conn)
{
   if (conn && conn->fd >= 0)
   {
      close(conn->fd);
      conn->fd = -1;
   }
}

/* --- Auto-start server --- */

/* Check if a Unix socket has a live server behind it.
 * Returns 1 if live, 0 if stale (ECONNREFUSED), -1 on error. */
static int socket_is_live(const char *path)
{
   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (fd < 0)
      return -1;
   cli_fd_cloexec(fd);
   fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

   int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
   int err = errno;
   close(fd);

   if (rc == 0 || err == EINPROGRESS)
      return 1; /* live */
   if (err == ECONNREFUSED)
      return 0; /* stale */
   return -1;   /* error */
}

/* Remove a stale socket file. Returns 1 if removed, 0 if not stale or not a socket. */
static int cleanup_stale_socket(const char *path)
{
   struct stat st;
   if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
      return 0;

   if (socket_is_live(path) == 0)
   {
      unlink(path);
      return 1;
   }
   return 0;
}

/* Acquire a spawn lock to prevent concurrent CLI invocations from double-spawning.
 * Returns the lock fd (>= 0) on success, -1 if another process holds the lock. */
static int acquire_spawn_lock(void)
{
   char lock_path[4096];
   snprintf(lock_path, sizeof(lock_path), "%s/server.lock", cli_config_dir());
   int fd = open(lock_path, O_CREAT | O_RDWR, 0600);
   if (fd < 0)
      return -1;

   int flags = fcntl(fd, F_GETFD, 0);
   if (flags >= 0)
      (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

   if (flock(fd, LOCK_EX | LOCK_NB) != 0)
   {
      /* Another process is spawning. Wait briefly for it to finish, but do
       * not block indefinitely if a daemon inherited the descriptor. */
      int waited_ms = 0;
      while (waited_ms < 3000)
      {
         usleep(50000);
         waited_ms += 50;
         if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            break;
      }

      /* By the time we get the lock, the other process has spawned.
       * Release immediately and signal caller to retry connect. */
      close(fd);
      return -1;
   }
   return fd;
}

static void release_spawn_lock(int fd)
{
   if (fd >= 0)
      close(fd); /* flock is released on close */
}

/* Try to reach a server at socket_path with a short timeout.
 * Returns 1 if available, 0 if not. */
/* Get the PID of the peer process on a Unix socket connection (Linux only). */
static pid_t get_peer_pid(int fd)
{
#ifdef __linux__
   struct ucred cred;
   socklen_t len = sizeof(cred);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
      return cred.pid;
#else
   (void)fd;
#endif
   return 0;
}

static int wait_for_stale_server_shutdown(pid_t pid, const char *socket_path, int timeout_ms)
{
   int elapsed = 0;
   int backoff = 25;

   while (elapsed < timeout_ms)
   {
      int pid_live = 0;
      if (pid > 1)
      {
         if (kill(pid, 0) == 0 || errno == EPERM)
            pid_live = 1;
      }

      int socket_live = socket_path && socket_is_live(socket_path) == 1;
      if (!pid_live && !socket_live)
         return 1;

      usleep((unsigned)(backoff * 1000));
      elapsed += backoff;
      if (backoff < 250)
         backoff *= 2;
   }

   return 0;
}

/* Diagnostic-only: connect, ask server.info, return 1 iff the running
 * server is compatible with this client. NEVER signals the peer.
 *
 * Earlier revisions SIGTERM'd the running server when versions did not
 * match. That made every aimee CLI invocation a potential restart-the-
 * world event — dev-built and installed binaries would mutually kill
 * each other's servers, ordinary hooks would page during routine
 * version drift, and the box accumulated zombie listeners. PR #1652
 * scoped the kill to same-path peers; this PR removes the kill from
 * the diagnostic path entirely. The orchestration layer in
 * cli_existing_server_for_method handles the "incompatible existing
 * server" case by returning NULL and surfacing a clear message; the
 * explicit `aimee server restart` command (or update.sh wrapping it)
 * is the only path that signals the peer. */
static int try_server(const char *socket_path, int timeout_ms, const char *required_method)
{
   (void)socket_path;
   (void)required_method; /* health-only liveness over the /v1 HTTP UDS */
   return cli_http_health_ok(timeout_ms);
}

/* Same as try_server but returns 0/1 AND populates *restart_reason
 * (caller-allocated, may be NULL) when the server is alive but
 * incompatible. Used by the orchestration layer to distinguish
 * "no server" from "old server" so the user gets a helpful message
 * pointing at `aimee server restart`.
 *
 * NULL-response semantics: if connect succeeds but server.info doesn't
 * reply within timeout_ms, return ok=1 (tentatively compatible). The
 * server is busy — most likely its accept/dispatch loop is mid-handler
 * for a long forwarded job (e.g. `make build-integrity` from a delegate
 * verify). Treating that as "alive but rejecting" was wrong: the real
 * request below uses CLIENT_DEFAULT_TIMEOUT_MS (5s) and will either
 * succeed once the server frees up, or fail with a specific error.
 * Telling the user to `aimee server restart` for transient backpressure
 * was misleading and noisy in PreToolUse hooks. */
static int try_server_diag(const char *socket_path, int timeout_ms, const char *required_method,
                           int *out_alive, char *restart_reason, size_t reason_len)
{
   (void)socket_path;
   (void)required_method;
   if (restart_reason && reason_len > 0)
      restart_reason[0] = '\0';
   /* Health-only: lifecycle is owned by systemd/launchd/SCM and version-drift
    * auto-detection is gone (it caused dev-vs-installed restart loops). A server
    * that answers /v1/health is usable; otherwise it is simply not running. */
   int ok = cli_http_health_ok(timeout_ms);
   if (out_alive)
      *out_alive = ok;
   return ok;
}

/* Wait for a server to become ready at sock_path with exponential backoff.
 * Returns 1 if ready, 0 on timeout. */
static int wait_for_ready(const char *sock_path, int timeout_ms, const char *required_method)
{
   (void)sock_path;
   (void)required_method;
   int elapsed = 0;
   int backoff = 10; /* start at 10ms */

   while (elapsed < timeout_ms)
   {
      if (cli_http_health_ok(500))
         return 1;
      usleep((unsigned)(backoff * 1000));
      elapsed += backoff;
      if (backoff < 200)
         backoff *= 2;
   }
   return 0;
}

static int wait_for_ready_or_startup_failure(const char *sock_path, int timeout_ms,
                                             const char *required_method, int notify_fd,
                                             char *failure, size_t failure_len)
{
   (void)sock_path;
   (void)required_method;
   int elapsed = 0;
   int backoff = 10;
   int notified_ok = 0;

   if (failure && failure_len > 0)
      failure[0] = '\0';
   if (notify_fd >= 0)
   {
      int flags = fcntl(notify_fd, F_GETFL, 0);
      if (flags >= 0)
         (void)fcntl(notify_fd, F_SETFL, flags | O_NONBLOCK);
   }

   while (elapsed < timeout_ms)
   {
      if (cli_http_health_ok(500))
         return 1;

      if (notify_fd >= 0)
      {
         struct pollfd pfd = {.fd = notify_fd, .events = POLLIN | POLLHUP | POLLERR};
         int rc = poll(&pfd, 1, backoff);
         elapsed += backoff;
         if (rc > 0)
         {
            char msg[512];
            ssize_t n = read(notify_fd, msg, sizeof(msg) - 1);
            if (n > 0)
            {
               msg[n] = '\0';
               if (strncmp(msg, "ok", 2) == 0)
               {
                  notified_ok = 1;
                  if (cli_http_health_ok(1000))
                     return 1;
                  continue;
               }
               if (failure && failure_len > 0)
                  snprintf(failure, failure_len, "%s", msg);
               return -1;
            }
            if (n == 0 && !notified_ok && (pfd.revents & (POLLHUP | POLLERR)))
            {
               if (failure && failure_len > 0)
                  snprintf(failure, failure_len, "error: server exited before readiness\n");
               return -1;
            }
         }
      }
      else
      {
         usleep((unsigned)(backoff * 1000));
         elapsed += backoff;
      }

      if (backoff < 200)
         backoff *= 2;
   }
   return 0;
}

/* Spawn an aimee-server on the well-known socket.
 * The server outlives this process but shuts down after idle timeout. */
static const char *spawn_server(const char *required_method)
{
   static char sock_path[4096];
   int notify_pipe[2] = {-1, -1};
   snprintf(sock_path, sizeof(sock_path), "%s", cli_default_socket_path());

   /* Ensure config directory exists before acquiring lock or watching for socket */
   const char *cfg = cli_config_dir();
   platform_mkdir_p(cfg, 0700);

   /* Acquire spawn lock to prevent concurrent double-spawn */
   int lock_fd = acquire_spawn_lock();
   if (lock_fd < 0)
   {
      /* Another process is spawning; by the time we get the lock, the server
       * may already be up. Try the well-known socket before giving up. */
      const char *well_known = cli_default_socket_path();
      if (wait_for_ready(well_known, SERVER_READY_TIMEOUT_MS, required_method))
         return well_known;
      return NULL;
   }

   if (pipe(notify_pipe) == 0)
   {
      int flags = fcntl(notify_pipe[0], F_GETFD, 0);
      if (flags >= 0)
         (void)fcntl(notify_pipe[0], F_SETFD, flags | FD_CLOEXEC);
   }

   /* Double-fork to fully daemonize the server.  The intermediate child
    * exits immediately so the parent can reap it with waitpid(), and the
    * grandchild (the actual server) is reparented to PID 1 — which always
    * reaps, preventing zombies when the server is later killed. */
   pid_t pid = fork();
   if (pid < 0)
   {
      release_spawn_lock(lock_fd);
      return NULL;
   }

   if (pid == 0)
   {
      /* Intermediate child */
      close(lock_fd);
      if (notify_pipe[0] >= 0)
         close(notify_pipe[0]);
      setsid();

      pid_t pid2 = fork();
      if (pid2 < 0)
      {
         if (notify_pipe[1] >= 0)
         {
            const char *msg = "error: fork failed\n";
            (void)write(notify_pipe[1], msg, strlen(msg));
         }
         _exit(127);
      }

      if (pid2 > 0)
         _exit(0); /* Intermediate exits immediately */

      /* Grandchild: the actual daemon (reparented to PID 1) */

      /* Redirect stdio to /dev/null */
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0)
      {
         dup2(devnull, STDIN_FILENO);
         dup2(devnull, STDOUT_FILENO);
         dup2(devnull, STDERR_FILENO);
         if (devnull > 2)
            close(devnull);
      }

      char sock_arg[4112];
      snprintf(sock_arg, sizeof(sock_arg), "--socket=%s", sock_path);
      if (notify_pipe[1] >= 0)
      {
         char fd_env[32];
         snprintf(fd_env, sizeof(fd_env), "%d", notify_pipe[1]);
         setenv("AIMEE_SERVER_STARTUP_FD", fd_env, 1);
      }

      /* aimee-server's worker threads need a large stack; the systemd unit
       * runs it with LimitSTACK=67108864 (64 MB). When the CLI fork-execs the
       * server directly (no service manager managing it), the child would
       * otherwise inherit the shell's default soft stack limit (often 8 MB),
       * which overflows and SIGSEGVs the server at startup. Raise the soft
       * limit to match the unit before exec — best-effort, never fatal.
       * RLIMIT_STACK sets the new image's main-thread stack and the default
       * size for threads that don't request their own. */
      {
         struct rlimit rl;
         const rlim_t want = 67108864; /* 64 MB; matches systemd LimitSTACK */
         if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur < want &&
             (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= want))
         {
            rl.rlim_cur = want;
            (void)setrlimit(RLIMIT_STACK, &rl);
         }
      }

      /* Try same directory as this binary first.  This ensures a dev build
       * (e.g. running ../aimee from the repo) always finds the matching
       * aimee-server in the same directory rather than a stale installed
       * version that PATH might resolve to.  Without this, a successful
       * `make` that fails `make install` (permission denied) leaves the
       * client and server at different build IDs, causing an infinite
       * restart loop as the client kills the PATH-resolved old server and
       * keeps re-spawning it. */
      char self_path[4096];
      ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
      if (len > 0)
      {
         self_path[len] = '\0';
         char *slash = strrchr(self_path, '/');
         if (slash)
         {
            snprintf(slash + 1, sizeof(self_path) - (size_t)(slash - self_path) - 1,
                     "aimee-server");
            execl(self_path, "aimee-server", sock_arg, NULL);
         }
      }

      /* Fallback: search PATH (standard installed deployment) */
      execlp("aimee-server", "aimee-server", sock_arg, NULL);
      if (notify_pipe[1] >= 0)
      {
         const char *msg = "error: exec aimee-server failed\n";
         (void)write(notify_pipe[1], msg, strlen(msg));
      }
      _exit(127);
   }

   /* Parent: reap the intermediate child immediately */
   waitpid(pid, NULL, 0);
   if (notify_pipe[1] >= 0)
      close(notify_pipe[1]);

   /* Parent: wait for server readiness with exponential backoff. */
   char startup_failure[512] = "";
   int ready =
       wait_for_ready_or_startup_failure(sock_path, SERVER_READY_TIMEOUT_MS, required_method,
                                         notify_pipe[0], startup_failure, sizeof(startup_failure));
   if (notify_pipe[0] >= 0)
      close(notify_pipe[0]);
   if (ready > 0)
   {
      release_spawn_lock(lock_fd);
      return sock_path;
   }
   if (ready < 0 && startup_failure[0])
      fprintf(stderr, "aimee: server failed to start: %s", startup_failure);
   else
      fprintf(stderr, "aimee: server did not become ready (30s timeout)\n");

   release_spawn_lock(lock_fd);
   return NULL;
}

const char *cli_ensure_server(void)
{
   return cli_ensure_server_for_method(NULL);
}

const char *cli_ensure_server_for_method(const char *method)
{
   /* As of #1660 the CLI no longer spawns aimee-server on demand. Five
    * iterations of orphan-listener fixes (#1648, #1652, #1656, #1657,
    * #1659) chased symptoms that all stemmed from the same architectural
    * choice: every CLI invocation was a potential server-spawner. Now
    * lifecycle is owned by systemd (Linux), launchd (macOS, follow-up),
    * and SCM (Windows, follow-up). The CLI is a pure client.
    *
    * If no server is running, callers get NULL and surface the platform-
    * appropriate remediation ("systemctl --user start aimee-server", or
    * `aimee server start` on platforms without a service manager wired). */
   return cli_existing_server_for_method(method);
}

const char *cli_existing_server(void)
{
   return cli_existing_server_for_method(NULL);
}

const char *cli_existing_server_for_method(const char *method)
{
   /* 1. Try AIMEE_SOCK (session-scoped server) with short timeout */
   const char *env_sock = getenv("AIMEE_SOCK");
   if (env_sock && env_sock[0])
   {
      if (try_server(env_sock, 100, method))
         return env_sock;
      /* Session server is dead — clean up stale socket if needed */
      cleanup_stale_socket(env_sock);
   }

   /* 2. Try well-known socket. Use try_server_diag so we can tell
    * "no server" (alive=0 → fall through to spawn) from
    * "old/incompatible server" (alive=1 → surface a clear message and
    * return NULL). The latter case used to silently SIGTERM the peer;
    * now we tell the user to run `aimee server restart` explicitly. */
   const char *well_known = cli_default_socket_path();
   int alive = 0;
   char restart_reason[256] = "";
   if (try_server_diag(well_known, 200, method, &alive, restart_reason, sizeof(restart_reason)))
      return well_known;

   if (alive)
   {
      if (restart_reason[0])
         fprintf(stderr, "aimee: server is incompatible (%s); run: aimee server restart\n",
                 restart_reason);
      else
         fprintf(stderr,
                 "aimee: server responded but rejected this client; run: aimee server restart\n");
      return NULL;
   }

   /* Server isn't alive — clean stale socket before attempting spawn */
   cleanup_stale_socket(well_known);
   return NULL;
}

int cli_restart_server(void)
{
   const char *tui_session = getenv("AIMEE_TUI_SESSION");
   if (tui_session && tui_session[0])
   {
      fprintf(stderr, "aimee: refusing to restart the server from inside the TUI; "
                      "run `aimee server restart` from a separate terminal after the active turn "
                      "finishes\n");
      return 2;
   }

   const char *sock = cli_http_sock_path();
   cli_conn_t conn;
   pid_t pid = 0;
   if (cli_connect_timeout(&conn, sock, 500) == 0)
   {
      pid = get_peer_pid(conn.fd);
      cli_close(&conn);
   }

   if (pid > 1)
   {
      fprintf(stderr, "aimee: terminating server pid=%d\n", (int)pid);
      kill(pid, SIGTERM);
      if (!wait_for_stale_server_shutdown(pid, sock, STALE_SERVER_SHUTDOWN_TIMEOUT_MS))
      {
         fprintf(stderr,
                 "aimee: server pid=%d is still shutting down after %dms; "
                 "forcing shutdown\n",
                 (int)pid, STALE_SERVER_SHUTDOWN_TIMEOUT_MS);
         kill(pid, SIGKILL);
         if (!wait_for_stale_server_shutdown(pid, sock, 5000))
         {
            fprintf(stderr,
                    "aimee: server pid=%d did not exit after SIGKILL; not spawning a second "
                    "server\n",
                    (int)pid);
            return 1;
         }
      }
   }
   else
   {
      fprintf(stderr, "aimee: no running server detected; spawning fresh one\n");
      cleanup_stale_socket(sock);
   }

   /* spawn_server is the only place the CLI signals a fork+exec since
    * #1660; cli_ensure_server is now a pure existing-server lookup. */
   const char *fresh = spawn_server(NULL);
   if (!fresh)
   {
      fprintf(stderr, "aimee: failed to spawn fresh server\n");
      return 1;
   }
   fprintf(stdout, "aimee: server restarted (%s)\n", fresh);
   return 0;
}

int cli_start_server(void)
{
   const char *sock = cli_default_socket_path();
   if (cli_existing_server() != NULL)
   {
      fprintf(stdout, "aimee: server already running (%s)\n", sock);
      return 0;
   }
   fprintf(stderr, "aimee: starting aimee-server\n");
   const char *fresh = spawn_server(NULL);
   if (!fresh)
   {
      fprintf(stderr, "aimee: failed to spawn aimee-server (check %s/server.log)\n",
              cli_config_dir());
      return 1;
   }
   fprintf(stdout, "aimee: server started (%s)\n", fresh);
   return 0;
}

#include "../cli_rpc_routes.inc"
