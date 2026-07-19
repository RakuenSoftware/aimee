/* sandbox_pkg_proxy_core.c — the PURE, side-effect-free decision core of the
 * delegate-sandbox package proxy: request classification, the SSRF IP guard, and the
 * host allowlist. No logging, no sockets — so it links into the unit test without the
 * I/O/logging object. The socket I/O that uses these lives in sandbox_pkg_proxy.c. */

#include "sandbox_pkg_proxy.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

const char *sandbox_pkg_default_allowlist(void)
{
   /* Specific registry hosts only — no broad wildcards. The `*.archive.ubuntu.com`
    * entry is the one wildcard, for the many geo mirrors under it. */
   return "deb.debian.org,security.debian.org,*.archive.ubuntu.com,security.ubuntu.com,"
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
   if (IN4(0xC0000200u, 24))
      return 1; /* 192.0.2.0/24 TEST-NET-1 */
   if (IN4(0xC0A80000u, 16))
      return 1; /* 192.168.0.0/16 */
   if (IN4(0xC6120000u, 15))
      return 1; /* 198.18.0.0/15 benchmarking */
   if (IN4(0xC6336400u, 24))
      return 1; /* 198.51.100.0/24 TEST-NET-2 */
   if (IN4(0xCB007100u, 24))
      return 1; /* 203.0.113.0/24 TEST-NET-3 */
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

      /* Any IPv6 form that embeds an IPv4 address must inherit the IPv4 policy, or a
       * malicious resolver could map a private v4 into v6 to slip past the check:
       *   - v4-mapped   ::ffff:a.b.c.d      (embedded at bytes 12..15)
       *   - v4-compat   ::a.b.c.d           (deprecated; bytes 12..15)
       *   - NAT64       64:ff9b::a.b.c.d    (bytes 12..15)
       *   - 6to4        2002:AABB:CCDD::    (embedded at bytes 2..5) */
      static const uint8_t v4mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
      static const uint8_t v4compat[12] = {0};
      static const uint8_t nat64[12] = {0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0};
      if (memcmp(b, v4mapped, 12) == 0 || memcmp(b, nat64, 12) == 0 ||
          (memcmp(b, v4compat, 12) == 0 && (b[12] | b[13] | b[14] | b[15])))
      {
         uint32_t a = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) |
                      (uint32_t)b[15];
         return ipv4_blocked(a);
      }
      if (b[0] == 0x20 && b[1] == 0x02) /* 6to4: 2002:V4:V4:: — v4 at bytes 2..5 */
      {
         uint32_t a = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) |
                      (uint32_t)b[5];
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
      /* Reject userinfo ('@') and any framing/control byte in the host. */
      if (c == '/' || c == ' ' || c == '\r' || c == '\n' || c == '@' || c == '\0')
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

   /* Reject anything but HTTP/1.0 or HTTP/1.1 (request-smuggling / parser-confusion
    * hardening). classify only runs on proxy-bound requests, so this never gates /v1. */
   const char *ver = sp2 + 1;
   if (strncmp(ver, "HTTP/1.", 7) != 0 || (ver[7] != '0' && ver[7] != '1') ||
       (ver[8] != '\0' && ver[8] != '\r' && ver[8] != '\n' && ver[8] != ' '))
      return SBX_REQ_INVALID;

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
