/* sandbox_pkg_proxy.c — the pure decision core of the delegate-sandbox package proxy.
 * Socket I/O (tunnel/forward) is layered on top; the functions here are the
 * security-critical, side-effect-free parts and are exhaustively unit-tested. */

#include "sandbox_pkg_proxy.h"

#include "log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

const char *sandbox_pkg_default_allowlist(void)
{
   return "deb.debian.org,*.debian.org,*.archive.ubuntu.com,security.ubuntu.com,"
          "registry.npmjs.org,pypi.org,files.pythonhosted.org";
}

int sandbox_pkg_port_allowed(int port)
{
   return port == 80 || port == 443;
}

/* --- SSRF guard --------------------------------------------------------------- */

/* 1 if the host-order IPv4 address falls in a blocked (non-public) range. */
static int ipv4_blocked(uint32_t a)
{
   /* a is host byte order. Prefix tests: (a & mask) == net. */
#define IN4(net, bits) ((a & (bits == 0 ? 0u : (0xFFFFFFFFu << (32 - (bits))))) == (net))
   if (IN4(0x00000000u, 8))
      return 1; /* 0.0.0.0/8 (this-network / unspecified) */
   if (IN4(0x0A000000u, 8))
      return 1; /* 10.0.0.0/8 */
   if (IN4(0x64400000u, 10))
      return 1; /* 100.64.0.0/10 CGNAT */
   if (IN4(0x7F000000u, 8))
      return 1; /* 127.0.0.0/8 loopback */
   if (IN4(0xA9FE0000u, 16))
      return 1; /* 169.254.0.0/16 link-local incl. 169.254.169.254 metadata */
   if (IN4(0xAC100000u, 12))
      return 1; /* 172.16.0.0/12 */
   if (IN4(0xC0000000u, 24))
      return 1; /* 192.0.0.0/24 IETF protocol assignments */
   if (IN4(0xC0A80000u, 16))
      return 1; /* 192.168.0.0/16 */
   if (IN4(0xC6120000u, 15))
      return 1; /* 198.18.0.0/15 benchmarking */
   if (IN4(0xE0000000u, 4))
      return 1; /* 224.0.0.0/4 multicast */
   if (IN4(0xF0000000u, 4))
      return 1; /* 240.0.0.0/4 reserved (incl. 255.255.255.255) */
#undef IN4
   return 0;
}

int sandbox_pkg_ip_is_blocked(const struct sockaddr *sa)
{
   if (!sa)
      return 1; /* fail closed */
   if (sa->sa_family == AF_INET)
   {
      const struct sockaddr_in *s = (const struct sockaddr_in *)sa;
      return ipv4_blocked(ntohl(s->sin_addr.s_addr));
   }
   if (sa->sa_family == AF_INET6)
   {
      const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)sa;
      const uint8_t *b = s->sin6_addr.s6_addr;

      /* v4-mapped (::ffff:a.b.c.d) and v4-compatible (::a.b.c.d, deprecated): apply
       * the IPv4 policy to the embedded address so a mapped private v4 can't slip in. */
      static const uint8_t v4mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
      static const uint8_t v4compat[12] = {0}; /* ::/96 with a nonzero tail */
      if (memcmp(b, v4mapped, 12) == 0 ||
          (memcmp(b, v4compat, 12) == 0 && (b[12] | b[13] | b[14] | b[15])))
      {
         uint32_t a = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) |
                      (uint32_t)b[15];
         return ipv4_blocked(a);
      }

      /* unspecified :: and loopback ::1 */
      int all_zero = 1;
      for (int i = 0; i < 15; i++)
         if (b[i])
         {
            all_zero = 0;
            break;
         }
      if (all_zero && (b[15] == 0 || b[15] == 1))
         return 1;

      if ((b[0] & 0xFE) == 0xFC)
         return 1; /* fc00::/7 unique-local */
      if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80)
         return 1; /* fe80::/10 link-local */
      if (b[0] == 0xFF)
         return 1; /* ff00::/8 multicast */
      return 0;
   }
   return 1; /* unknown family — fail closed */
}

/* --- host allowlist ----------------------------------------------------------- */

static int host_eq_ci(const char *a, size_t alen, const char *b)
{
   size_t blen = strlen(b);
   if (alen != blen)
      return 0;
   for (size_t i = 0; i < alen; i++)
      if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
         return 0;
   return 1;
}

/* True if `host` ends with ".suffix" (case-insensitive). */
static int host_has_suffix_ci(const char *host, const char *suffix)
{
   size_t hl = strlen(host), sl = strlen(suffix);
   if (sl == 0 || hl <= sl + 1)
      return 0;
   if (host[hl - sl - 1] != '.')
      return 0;
   for (size_t i = 0; i < sl; i++)
      if (tolower((unsigned char)host[hl - sl + i]) != tolower((unsigned char)suffix[i]))
         return 0;
   return 1;
}

int sandbox_pkg_host_allowed(const char *host, const char *allowlist)
{
   if (!host || !host[0] || !allowlist)
      return 0;
   const char *p = allowlist;
   while (*p)
   {
      while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
         p++;
      const char *start = p;
      while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
         p++;
      size_t len = (size_t)(p - start);
      if (len == 0)
         continue;
      if (len >= 2 && start[0] == '*' && start[1] == '.')
      {
         /* "*.suffix": matches the suffix itself or any label beneath it. */
         char suffix[256];
         size_t sl = len - 2;
         if (sl < sizeof(suffix))
         {
            memcpy(suffix, start + 2, sl);
            suffix[sl] = '\0';
            if (host_eq_ci(host, strlen(host), suffix) || host_has_suffix_ci(host, suffix))
               return 1;
         }
      }
      else
      {
         /* exact host (case-insensitive): compare the entry (start,len) to host */
         if (host_eq_ci(start, len, host))
            return 1;
      }
   }
   return 0;
}

/* --- request-line classification ---------------------------------------------- */

/* Copy a host token (lowercased) into out[cap]; strip an optional [..] IPv6 bracket.
 * Returns 0 on success. */
static int copy_host(const char *s, size_t n, char *out, size_t cap)
{
   if (n == 0 || n >= cap)
      return -1;
   size_t o = 0;
   int bracket = (s[0] == '[');
   for (size_t i = 0; i < n; i++)
   {
      char c = s[i];
      if (bracket && (c == '[' || c == ']'))
         continue;
      if (c == '/' || c == ' ' || c == '\r' || c == '\n')
         return -1;
      if (o + 1 >= cap)
         return -1;
      out[o++] = (char)tolower((unsigned char)c);
   }
   out[o] = '\0';
   return o ? 0 : -1;
}

sbx_req_kind_t sandbox_pkg_classify_request_line(const char *line, char *host, size_t hostcap,
                                                 int *port)
{
   if (!line || !host || hostcap == 0 || !port)
      return SBX_REQ_INVALID;
   host[0] = '\0';
   *port = 0;

   /* METHOD SP target SP HTTP/x.y */
   const char *sp = strchr(line, ' ');
   if (!sp || sp == line)
      return SBX_REQ_INVALID;
   size_t methlen = (size_t)(sp - line);
   const char *target = sp + 1;
   const char *sp2 = strchr(target, ' ');
   if (!sp2 || sp2 == target)
      return SBX_REQ_INVALID;
   size_t tlen = (size_t)(sp2 - target);

   if (methlen == 7 && strncmp(line, "CONNECT", 7) == 0)
   {
      /* authority-form: host:port  (port required). Use the LAST colon so a
       * bracketed IPv6 authority [::1]:443 splits at the port, not inside the host. */
      const char *colon = NULL;
      for (const char *q = target + tlen - 1; q >= target; q--)
         if (*q == ':')
         {
            colon = q;
            break;
         }
      if (!colon || colon == target)
         return SBX_REQ_INVALID;
      size_t hlen = (size_t)(colon - target);
      int p = 0;
      for (const char *q = colon + 1; q < target + tlen; q++)
      {
         if (*q < '0' || *q > '9')
            return SBX_REQ_INVALID;
         p = p * 10 + (*q - '0');
         if (p > 65535)
            return SBX_REQ_INVALID;
      }
      if (p == 0 || copy_host(target, hlen, host, hostcap) != 0)
         return SBX_REQ_INVALID;
      *port = p;
      return SBX_REQ_CONNECT;
   }

   if (target[0] == '/')
      return SBX_REQ_API; /* origin-form — the existing /v1 stack owns this */

   /* absolute-form: scheme://host[:port]/path — only http:// is forwarded. */
   if (tlen > 7 && strncmp(target, "http://", 7) == 0)
   {
      const char *hoststart = target + 7;
      size_t rem = tlen - 7;
      /* host runs to the first '/' , ':' or end. */
      size_t hlen = 0;
      while (hlen < rem && hoststart[hlen] != '/' && hoststart[hlen] != ':')
         hlen++;
      int p = 80;
      if (hlen < rem && hoststart[hlen] == ':')
      {
         p = 0;
         size_t i = hlen + 1;
         for (; i < rem && hoststart[i] != '/'; i++)
         {
            if (hoststart[i] < '0' || hoststart[i] > '9')
               return SBX_REQ_INVALID;
            p = p * 10 + (hoststart[i] - '0');
            if (p > 65535)
               return SBX_REQ_INVALID;
         }
         if (p == 0)
            return SBX_REQ_INVALID;
      }
      if (copy_host(hoststart, hlen, host, hostcap) != 0)
         return SBX_REQ_INVALID;
      *port = p;
      return SBX_REQ_ABSOLUTE;
   }

   return SBX_REQ_INVALID;
}

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

/* Bidirectional byte pump with an idle timeout. Returns total bytes each way via the
 * out params (best-effort, for the audit log). */
static void proxy_pump(int a, int b, unsigned long *a2b, unsigned long *b2a)
{
   struct pollfd pf[2];
   pf[0].fd = a;
   pf[1].fd = b;
   char buf[65536];
   for (;;)
   {
      pf[0].events = pf[1].events = POLLIN;
      pf[0].revents = pf[1].revents = 0;
      int pr = poll(pf, 2, 300000); /* 5-min idle timeout */
      if (pr <= 0)
         return;
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
            if (i == 0 && a2b)
               *a2b += (unsigned long)n;
            else if (i == 1 && b2a)
               *b2a += (unsigned long)n;
         }
      }
   }
}

static int hop_by_hop(const char *name, size_t len)
{
   static const char *const h[] = {
       "connection", "proxy-connection",    "keep-alive", "transfer-encoding", "te", "trailer",
       "upgrade",    "proxy-authorization", NULL};
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

   char firstline[2100];
   int fl = snprintf(firstline, sizeof(firstline), "%.*s %.*s %.*s\r\n", (int)(sp1 - head), head,
                     (int)pathlen, path, (int)(eol - sp2 - 1), sp2 + 1);
   if (fl <= 0 || (size_t)fl >= sizeof(firstline))
      return -1;
   /* strip a trailing \r the version field may carry */
   if (fl >= 3 && firstline[fl - 3] == '\r')
   {
      firstline[fl - 3] = '\r';
      firstline[fl - 2] = '\n';
      firstline[fl - 1] = '\0';
      fl -= 1;
   }
   if (write_all(up, firstline, (size_t)fl) != 0)
      return -1;

   /* headers: copy each non-hop-by-hop header line verbatim */
   const char *ln = eol + 1;
   while (*ln)
   {
      const char *nl = strchr(ln, '\n');
      size_t linelen = nl ? (size_t)(nl - ln + 1) : strlen(ln);
      if (linelen <= 2) /* blank line — end of headers */
         break;
      const char *colon = memchr(ln, ':', linelen);
      size_t namelen = colon ? (size_t)(colon - ln) : 0;
      if (!colon || !hop_by_hop(ln, namelen))
         if (write_all(up, ln, linelen) != 0)
            return -1;
      if (!nl)
         break;
      ln = nl + 1;
   }
   return write_all(up, "Connection: close\r\n\r\n", 21);
}

int sandbox_pkg_proxy_serve(int client_fd, const char *head, const char *allowlist, const char *tag)
{
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
