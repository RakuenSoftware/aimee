/* posix/web_egress.c -- see headers/web_egress.h.
 *
 * The deny-list, resolver and URL splitter below are a VERBATIM lift from
 * posix/web_read.c, where they were static and therefore reachable by exactly
 * one caller. Nothing about their logic changed in the move; what changed is
 * that search can now reach them too, which is the entire point.
 *
 * The policy split and web_egress_fetch() are new. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"

#include "agent_exec.h"
#include "ip_transition.h"
#include "log.h"
#include "web_egress.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* ---------------- SSRF egress deny-list ---------------- */

/* 1 if this IPv4 (host byte order) is in a private/reserved/link-local range. */
/* Every range a model- or ranking-chosen destination has no business reaching.
 * Kept deliberately in step with blockedIPv4Nets in
 * server-go/modules/delegates/proxyguard.go and the table in
 * server-go/modules/sandbox/proxy_policy.go: the same destination must not be
 * refused on one plane and dialed on another. */
static int ipv4_blocked(uint32_t a)
{
   static const struct
   {
      uint32_t network;
      unsigned bits;
   } blocked[] = {
       {0x00000000u, 8},  /* 0.0.0.0/8 this-network / unspecified */
       {0x0a000000u, 8},  /* 10/8 private */
       {0x64400000u, 10}, /* 100.64/10 CGNAT */
       {0x7f000000u, 8},  /* 127/8 loopback */
       {0xa9fe0000u, 16}, /* 169.254/16 link-local incl. 169.254.169.254 metadata */
       {0xac100000u, 12}, /* 172.16/12 private */
       {0xc0000000u, 24}, /* 192.0.0/24 IETF protocol assignments */
       {0xc0000200u, 24}, /* 192.0.2/24 TEST-NET-1 */
       {0xc0586300u, 24}, /* 192.88.99/24 6to4 relay anycast (deprecated) */
       {0xc0a80000u, 16}, /* 192.168/16 private */
       {0xc6120000u, 15}, /* 198.18/15 benchmarking */
       {0xc6336400u, 24}, /* 198.51.100/24 TEST-NET-2 */
       {0xcb007100u, 24}, /* 203.0.113/24 TEST-NET-3 */
       {0xe0000000u, 4},  /* 224/4 multicast */
       {0xf0000000u, 4},  /* 240/4 reserved incl. 255.255.255.255 */
   };
   for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++)
   {
      uint32_t mask = blocked[i].bits == 0 ? 0u : (~0u << (32 - blocked[i].bits));
      if ((a & mask) == blocked[i].network)
         return 1;
   }
   return 0;
}

static int ipv6_blocked(const struct in6_addr *a)
{
   const uint8_t *b = a->s6_addr;
   /* :: (unspecified) and ::1 (loopback) */
   int all_zero = 1;
   for (int i = 0; i < 15; i++)
      if (b[i])
      {
         all_zero = 0;
         break;
      }
   if (all_zero && (b[15] == 0 || b[15] == 1))
      return 1;
   if ((b[0] & 0xfe) == 0xfc)
      return 1; /* fc00::/7 unique-local */
   if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
      return 1; /* fe80::/10 link-local */
   if (b[0] == 0xff)
      return 1; /* ff00::/8 multicast */
   /* A transition address names an IPv4 destination; judge it as that IPv4.
    * Covers v4-mapped, NAT64, 6to4, Teredo and v4-compatible -- see
    * headers/ip_transition.h for why checking only v4-mapped is a bypass. */
   uint32_t embedded = 0;
   if (ip_embedded_ipv4(a, &embedded))
      return ipv4_blocked(embedded);
   return 0;
}

/* 1 if the sockaddr targets a private/reserved address (blocked). */
int web_egress_addr_blocked(const struct sockaddr *sa)
{
   if (sa->sa_family == AF_INET)
      return ipv4_blocked(ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr));
   if (sa->sa_family == AF_INET6)
      return ipv6_blocked(&((const struct sockaddr_in6 *)sa)->sin6_addr);
   return 1; /* unknown family: block */
}

/* Resolve `host` once, validate the chosen address, and write its numeric IP to
 * pinned_ip. Returns 0 on success, -1 (with *err set) if unresolved or blocked. */
static int egress_resolve_validate(const char *host, char pinned_ip[64], const char **err)
{
   struct addrinfo hints = {0}, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
   {
      *err = "host did not resolve";
      return -1;
   }
   int ok = 0;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
   {
      if (web_egress_addr_blocked(ai->ai_addr))
         continue;
      void *addr = NULL;
      if (ai->ai_family == AF_INET)
         addr = &((struct sockaddr_in *)ai->ai_addr)->sin_addr;
      else if (ai->ai_family == AF_INET6)
         addr = &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
      if (addr && inet_ntop(ai->ai_family, addr, pinned_ip, 64))
      {
         ok = 1;
         break;
      }
   }
   freeaddrinfo(res);
   if (!ok)
   {
      *err = "host resolves only to private/reserved addresses (blocked)";
      return -1;
   }
   return 0;
}

/* Split "scheme://host[:port]/..." — scheme must be http/https. */
static int split_url(const char *url, char *scheme, char *host, int *port, const char **err)
{
   const char *p;
   int ssl;
   if (strncmp(url, "https://", 8) == 0)
   {
      strcpy(scheme, "https");
      p = url + 8;
      ssl = 1;
   }
   else if (strncmp(url, "http://", 7) == 0)
   {
      strcpy(scheme, "http");
      p = url + 7;
      ssl = 0;
   }
   else
   {
      *err = "only http/https URLs are allowed";
      return -1;
   }
   const char *slash = strchr(p, '/');
   const char *colon = strchr(p, ':');
   size_t hlen;
   *port = ssl ? 443 : 80;
   if (colon && (!slash || colon < slash))
   {
      hlen = (size_t)(colon - p);
      *port = atoi(colon + 1);
   }
   else
      hlen = slash ? (size_t)(slash - p) : strlen(p);
   if (hlen == 0 || hlen >= 255)
   {
      *err = "malformed host";
      return -1;
   }
   memcpy(host, p, hlen);
   host[hlen] = '\0';
   return 0;
}

/* ---------------- HTML -> text ---------------- */

/* ---------------- policy ---------------- */

int web_egress_private_endpoint_allowed(void)
{
   /* Deliberately an environment variable, not a config key. config.set is
    * capability-gated but still reachable from inside the running system; an
    * environment variable is set when the deployment is built. Permitting a
    * private destination should require touching the deployment, not the
    * running config. */
   const char *v = getenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT");
   return (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
}

char *web_egress_fetch_pinned(const char *url, web_egress_policy_t policy,
                              const char *extra_headers, int timeout_ms, size_t max_bytes,
                              char *pinned_out, size_t pinned_out_len, const char **err)
{
   if (pinned_out && pinned_out_len)
      pinned_out[0] = '\0';
   const char *local_err = NULL;
   if (!err)
      err = &local_err;
   *err = NULL;

   char scheme[8], host[256];
   int port = 0;
   if (split_url(url, scheme, host, &port, err) != 0)
      return NULL;

   char pinned[64];
   if (egress_resolve_validate(host, pinned, err) != 0)
   {
      /* A configured endpoint on a private address is permitted only when the
       * deployment opted in. Everything else stays denied, including every
       * untrusted destination. */
      if (policy == WEB_EGRESS_CONFIGURED && web_egress_private_endpoint_allowed())
      {
         aimee_log(LOG_INFO, "web_egress",
                   "configured endpoint %s resolves to a private address; permitted by "
                   "AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT",
                   host);
         *err = NULL;
         /* Resolve again WITHOUT the address check, but still pin to the result
          * so the connect target cannot change after this point. */
         struct addrinfo hints, *ai = NULL;
         memset(&hints, 0, sizeof(hints));
         hints.ai_family = AF_UNSPEC;
         hints.ai_socktype = SOCK_STREAM;
         if (getaddrinfo(host, NULL, &hints, &ai) != 0 || !ai)
         {
            *err = "dns resolution failed";
            return NULL;
         }
         const void *src = NULL;
         if (ai->ai_family == AF_INET)
            src = &((struct sockaddr_in *)ai->ai_addr)->sin_addr;
         else if (ai->ai_family == AF_INET6)
            src = &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
         if (!src || !inet_ntop(ai->ai_family, src, pinned, 64))
         {
            freeaddrinfo(ai);
            *err = "dns resolution failed";
            return NULL;
         }
         freeaddrinfo(ai);
      }
      else
      {
         return NULL; /* *err already set by the validator */
      }
   }

   if (pinned_out && pinned_out_len)
      snprintf(pinned_out, pinned_out_len, "%s", pinned);
   char *resp = NULL;
   int status = agent_http_get_pinned(url, pinned, extra_headers, &resp, timeout_ms);
   if (status < 0 || !resp)
   {
      free(resp);
      *err = "fetch failed";
      return NULL;
   }
   if (status >= 300 && status < 400)
   {
      free(resp);
      *err = "redirected; pass the final URL (redirects are not followed for egress safety)";
      return NULL;
   }
   if (status != 200)
   {
      free(resp);
      *err = "non-200 response";
      return NULL;
   }
   if (max_bytes && strlen(resp) > max_bytes)
      resp[max_bytes] = '\0';
   return resp;
}

char *web_egress_fetch(const char *url, web_egress_policy_t policy, const char *extra_headers,
                       int timeout_ms, size_t max_bytes, const char **err)
{
   return web_egress_fetch_pinned(url, policy, extra_headers, timeout_ms, max_bytes, NULL, 0, err);
}
