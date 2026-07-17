/* sandbox_pkg_proxy.c — the socket I/O of the delegate-sandbox package proxy: the
 * CONNECT tunnel and absolute-form forwarder that use the pure decision core in
 * sandbox_pkg_proxy_core.c. Kept separate so the security-critical pure functions
 * unit-test without pulling logging/sockets. */

#include "sandbox_pkg_proxy.h"

#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* --- socket I/O ---------------------------------------------------------------
 * The functions above are pure and unit-tested; the plumbing below wires them to
 * real sockets and is exercised by the integration test (docker). */

static int write_all(int fd, const char *p, size_t n)
{
   size_t off = 0;
   while (off < n)
   {
      ssize_t w = write(fd, p + off, n - off);
      if (w < 0 && errno == EINTR)
         continue;
      if (w <= 0)
         return -1;
      off += (size_t)w;
   }
   return 0;
}

/* getaddrinfo(host,port), then for each candidate re-apply the SSRF guard to the
 * exact address we are about to dial (no rebinding window: the checked sockaddr IS
 * the connected one) and connect to the first that passes. Writes the dialed IP into
 * ipbuf. Returns the connected fd, or -1 (with *why set). */
static int proxy_dial(const char *host, int port, char *ipbuf, size_t ipcap, const char **why)
{
   *why = "resolve-failed";
   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   char portstr[8];
   snprintf(portstr, sizeof(portstr), "%d", port);
   struct addrinfo *res = NULL;
   if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
      return -1;

   int fd = -1;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
   {
      if (sandbox_pkg_ip_is_blocked(ai->ai_addr))
      {
         *why = "ssrf-blocked";
         continue;
      }
      fd = socket(ai->ai_family, SOCK_STREAM, 0);
      if (fd < 0)
      {
         *why = "socket-failed";
         continue;
      }
      /* Bound the blocking connect so a blackholed allowlisted host cannot pin a
       * server connection worker indefinitely. */
      struct timeval ctv = {.tv_sec = 15, .tv_usec = 0};
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
      if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
      {
         if (ipbuf && ipcap)
         {
            const void *a = ai->ai_family == AF_INET
                                ? (const void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr
                                : (const void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
            if (!inet_ntop(ai->ai_family, a, ipbuf, (socklen_t)ipcap))
               ipbuf[0] = '\0';
         }
         *why = NULL;
         break;
      }
      *why = "connect-failed";
      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);
   return fd;
}

/* Bidirectional byte pump, bounded by BOTH a wall-clock deadline and a total byte cap
 * so a compromised delegate cannot pin a worker or push unbounded data through an
 * (opaque, TLS) tunnel to exfiltrate/DoS. Returns bytes each way via the out params. */
#define PROXY_PUMP_DEADLINE_SEC 600
#define PROXY_PUMP_MAX_BYTES    (2UL * 1024 * 1024 * 1024)
static void proxy_pump(int a, int b, unsigned long *a2b, unsigned long *b2a)
{
   struct pollfd pf[2];
   pf[0].fd = a;
   pf[1].fd = b;
   char buf[65536];
   time_t deadline = time(NULL) + PROXY_PUMP_DEADLINE_SEC;
   unsigned long total = 0;
   for (;;)
   {
      if (time(NULL) >= deadline)
         return; /* wall-clock cap: applies even to a stalled/idle tunnel */
      pf[0].events = pf[1].events = POLLIN;
      pf[0].revents = pf[1].revents = 0;
      int pr = poll(pf, 2, 30000); /* short poll so the deadline is re-checked */
      if (pr < 0)
         return;
      if (pr == 0)
         continue; /* idle tick — loop re-checks the wall-clock deadline */
      for (int i = 0; i < 2; i++)
      {
         if (pf[i].revents & (POLLIN | POLLHUP | POLLERR))
         {
            int src = i == 0 ? a : b, dst = i == 0 ? b : a;
            ssize_t n = read(src, buf, sizeof(buf));
            if (n <= 0)
               return;
            if (write_all(dst, buf, (size_t)n) != 0)
               return;
            total += (unsigned long)n;
            if (total > PROXY_PUMP_MAX_BYTES)
               return; /* byte cap */
            if (i == 0 && a2b)
               *a2b += (unsigned long)n;
            else if (i == 1 && b2a)
               *b2a += (unsigned long)n;
         }
      }
   }
}

/* Headers the proxy must NOT forward upstream: hop-by-hop headers (tunnel/keepalive
 * correctness) AND credential-bearing headers (never leak a delegate secret to a
 * registry). */
static int header_should_strip(const char *name, size_t len)
{
   static const char *const h[] = {"connection",
                                   "proxy-connection",
                                   "keep-alive",
                                   "transfer-encoding",
                                   "te",
                                   "trailer",
                                   "upgrade",
                                   "authorization",       /* credential-bearing */
                                   "proxy-authorization", /* credential-bearing */
                                   "cookie",              /* credential-bearing */
                                   NULL};
   for (int i = 0; h[i]; i++)
      if (strlen(h[i]) == len && strncasecmp(name, h[i], len) == 0)
         return 1;
   return 0;
}

/* Rewrite an absolute-form request head to origin-form for the upstream: replace the
 * `http://host[:port]/path` target with just `/path`, drop hop-by-hop headers, force
 * `Connection: close`. Writes the rewritten head to `up`. Returns 0 on success. */
static int proxy_forward_head(int up, const char *head)
{
   const char *eol = strstr(head, "\n");
   if (!eol)
      return -1;
   /* first line: METHOD SP http://host/path SP HTTP/x.y */
   const char *sp1 = strchr(head, ' ');
   const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
   if (!sp1 || !sp2 || sp2 > eol)
      return -1;
   const char *target = sp1 + 1;
   /* skip scheme://authority to the path */
   const char *path = NULL;
   if (strncmp(target, "http://", 7) == 0)
   {
      const char *p = target + 7;
      while (p < sp2 && *p != '/')
         p++;
      path = (p < sp2 && *p == '/') ? p : "/";
   }
   if (!path)
      return -1;
   size_t pathlen = (size_t)(sp2 - path);
   if (path[0] != '/')
      pathlen = 1, path = "/";

   /* The version at sp2+1 is exactly "HTTP/1.0" or "HTTP/1.1" (validated by classify,
    * which runs before this path), so take its 8 chars and always append CRLF — no
    * fragile trailing-CR trimming. */
   char firstline[2100];
   int fl = snprintf(firstline, sizeof(firstline), "%.*s %.*s %.8s\r\n", (int)(sp1 - head), head,
                     (int)pathlen, path, sp2 + 1);
   if (fl <= 0 || (size_t)fl >= sizeof(firstline))
      return -1;
   if (write_all(up, firstline, (size_t)fl) != 0)
      return -1;

   /* headers: forward each well-formed, non-stripped header line verbatim */
   const char *ln = eol + 1;
   while (*ln)
   {
      const char *nl = strchr(ln, '\n');
      size_t linelen = nl ? (size_t)(nl - ln + 1) : strlen(ln);
      if (linelen <= 2) /* blank line — end of headers */
         break;
      const char *colon = memchr(ln, ':', linelen);
      if (!colon)
         return -1; /* a header line without a colon is malformed — refuse the request */
      size_t namelen = (size_t)(colon - ln);
      if (namelen == 0)
         return -1;
      /* the field name must be a token: no space, control byte, or CR/LF injection */
      for (size_t i = 0; i < namelen; i++)
      {
         unsigned char c = (unsigned char)ln[i];
         if (c <= 0x20 || c == 0x7F)
            return -1;
      }
      if (!header_should_strip(ln, namelen))
         if (write_all(up, ln, linelen) != 0)
            return -1;
      if (!nl)
         break;
      ln = nl + 1;
   }
   return write_all(up, "Connection: close\r\n\r\n", 21);
}

int sandbox_pkg_proxy_serve(int client_fd, int is_uds, const char *head, const char *allowlist,
                            const char *tag)
{
   /* Hard refusal on any non-UDS caller: the proxy must never be reachable from the
    * public TCP/TLS listener, independent of the caller's own listener check. */
   if (!is_uds)
   {
      aimee_log(LOG_ERROR, "sandbox-proxy",
                "refusing proxy request on a non-UDS socket (would expose egress on the "
                "public listener)");
      return 0;
   }
   if (!allowlist)
      allowlist = sandbox_pkg_default_allowlist();
   if (!tag)
      tag = "sandbox";
   if (!head)
      return 0;

   char host[256];
   int port = 0;
   sbx_req_kind_t kind = sandbox_pkg_classify_request_line(head, host, sizeof(host), &port);

   if (kind == SBX_REQ_INVALID || kind == SBX_REQ_API)
   {
      (void)write_all(client_fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", 46);
      return 0;
   }
   if (!sandbox_pkg_port_allowed(port) || !sandbox_pkg_host_allowed(host, allowlist))
   {
      aimee_log(LOG_WARN, "sandbox-proxy", "%s: DENY host=%s port=%d (allowlist/port)", tag, host,
                port);
      (void)write_all(client_fd, "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n", 46);
      return 0;
   }

   char ip[64] = "";
   const char *why = NULL;
   int up = proxy_dial(host, port, ip, sizeof(ip), &why);
   if (up < 0)
   {
      aimee_log(LOG_WARN, "sandbox-proxy", "%s: DENY host=%s port=%d reason=%s", tag, host, port,
                why ? why : "?");
      (void)write_all(client_fd, "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n", 46);
      return 0;
   }

   unsigned long up_bytes = 0, down_bytes = 0;
   if (kind == SBX_REQ_CONNECT)
   {
      if (write_all(client_fd, "HTTP/1.1 200 Connection Established\r\n\r\n", 38) == 0)
         proxy_pump(client_fd, up, &up_bytes, &down_bytes);
   }
   else /* SBX_REQ_ABSOLUTE */
   {
      if (proxy_forward_head(up, head) == 0)
         proxy_pump(client_fd, up, &up_bytes, &down_bytes);
   }
   close(up);
   aimee_log(LOG_INFO, "sandbox-proxy", "%s: OK host=%s port=%d ip=%s up=%lu down=%lu kind=%s", tag,
             host, port, ip[0] ? ip : "?", up_bytes, down_bytes,
             kind == SBX_REQ_CONNECT ? "connect" : "http");
   return 0;
}
