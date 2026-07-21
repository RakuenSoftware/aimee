#include "kb_mgmt_endpoint.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int dns_name(const char *s)
{
   size_t n = s ? strlen(s) : 0;
   if (!n || n > 253 || s[n - 1] == '.')
      return 0;
   size_t label = 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (c == '.')
      {
         if (!label || label > 63 || s[i - 1] == '-')
            return 0;
         label = 0;
         continue;
      }
      if (!(isalnum(c) || c == '-') || (!label && c == '-'))
         return 0;
      label++;
   }
   return label > 0 && label <= 63 && s[n - 1] != '-';
}

int kb_mgmt_endpoint_parse(const char *e, kb_mgmt_endpoint_t *out)
{
   if (!e || !out || strncmp(e, "https://", 8) != 0)
      return -1;
   memset(out, 0, sizeof(*out));
   out->port = 443;
   const char *p = e + 8;
   size_t n = strlen(p);
   if (!n || n > 319 || strpbrk(p, "/?#@") || strpbrk(p, "\r\n\t "))
      return -1;
   if (*p == '[')
   {
      const char *end = strchr(p + 1, ']');
      if (!end || end == p + 1 || strchr(p + 1, '%') ||
          (end[1] && end[1] != ':') || strlen(p + 1) >= sizeof(out->host))
         return -1;
      size_t hn = (size_t)(end - p - 1);
      if (hn >= sizeof(out->host))
         return -1;
      memcpy(out->host, p + 1, hn);
      out->host[hn] = '\0';
      struct in6_addr v6;
      if (inet_pton(AF_INET6, out->host, &v6) != 1)
         return -1;
      if (end[1] == ':')
      {
         char tail = 0;
         if (sscanf(end + 2, "%d%c", &out->port, &tail) != 1 || out->port < 1 ||
             out->port > 65535)
            return -1;
      }
      snprintf(out->host_header, sizeof(out->host_header),
               out->port == 443 ? "[%s]" : "[%s]:%d", out->host, out->port);
      return 0;
   }
   const char *colon = strrchr(p, ':');
   size_t hn = colon ? (size_t)(colon - p) : n;
   if (!hn || hn >= sizeof(out->host) || (colon && strchr(p, ':') != colon))
      return -1;
   memcpy(out->host, p, hn);
   out->host[hn] = '\0';
   if (colon)
   {
      char tail = 0;
      if (sscanf(colon + 1, "%d%c", &out->port, &tail) != 1 || out->port < 1 ||
          out->port > 65535)
         return -1;
   }
   struct in_addr v4;
   if (inet_pton(AF_INET, out->host, &v4) != 1 && !dns_name(out->host))
      return -1;
   snprintf(out->host_header, sizeof(out->host_header), out->port == 443 ? "%s" : "%s:%d",
            out->host, out->port);
   return 0;
}

int kb_mgmt_endpoint_validate(const char *e)
{
   kb_mgmt_endpoint_t ep;
   return kb_mgmt_endpoint_parse(e, &ep);
}

static int v4_permitted(uint32_t n)
{
   uint32_t a = ntohl(n);
   unsigned o1 = a >> 24, o2 = (a >> 16) & 255;
   return !(o1 == 0 || o1 == 10 || o1 == 127 || o1 >= 224 ||
            (o1 == 100 && o2 >= 64 && o2 <= 127) ||
            (o1 == 169 && o2 == 254) || (o1 == 172 && o2 >= 16 && o2 <= 31) ||
            (o1 == 192 && o2 == 168) || (o1 == 198 && (o2 == 18 || o2 == 19)));
}

int kb_mgmt_sockaddr_permitted(const struct sockaddr *addr, socklen_t len)
{
   if (!addr)
      return 0;
   if (addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in))
      return v4_permitted(((const struct sockaddr_in *)addr)->sin_addr.s_addr);
   if (addr->sa_family != AF_INET6 || len < sizeof(struct sockaddr_in6))
      return 0;
   const unsigned char *b = ((const struct sockaddr_in6 *)addr)->sin6_addr.s6_addr;
   static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
   if (memcmp(b, mapped, sizeof(mapped)) == 0)
   {
      uint32_t v4;
      memcpy(&v4, b + 12, sizeof(v4));
      return v4_permitted(v4);
   }
   int all_zero = 1;
   for (int i = 0; i < 16; i++)
      all_zero &= b[i] == 0;
   return !all_zero && !(b[0] == 0xff || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) ||
                         (b[0] & 0xfe) == 0xfc ||
                         (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 16) == 0));
}

int kb_mgmt_endpoint_connect(const kb_mgmt_endpoint_t *ep)
{
   if (!ep || !ep->host[0] || ep->port < 1 || ep->port > 65535)
      return -1;
   char service[16];
   snprintf(service, sizeof(service), "%d", ep->port);
   struct addrinfo hints = {0}, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(ep->host, service, &hints, &res) != 0)
      return -1;
   int fd = -1;
   for (const struct addrinfo *a = res; a; a = a->ai_next)
   {
      if (!kb_mgmt_sockaddr_permitted(a->ai_addr, a->ai_addrlen))
         continue;
      fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) == 0)
         break;
      if (fd >= 0)
         close(fd);
      fd = -1;
   }
   freeaddrinfo(res);
   return fd;
}
