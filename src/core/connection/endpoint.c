#include <aimee/core/connection/endpoint.h>

#include <stdio.h>
#include <string.h>

static int copy_part(char *out, size_t out_size, const char *start, const char *end)
{
   size_t length = (size_t)(end - start);
   if (length == 0 || length >= out_size)
      return -1;
   memcpy(out, start, length);
   out[length] = '\0';
   return 0;
}

static int port_valid(const char *port)
{
   unsigned value = 0;
   if (!port || !*port)
      return 0;
   for (const unsigned char *p = (const unsigned char *)port; *p; p++)
   {
      if (*p < '0' || *p > '9')
         return 0;
      value = value * 10u + (unsigned)(*p - '0');
      if (value > 65535u)
         return 0;
   }
   return value > 0;
}

int aimee_core_endpoint_parse(const char *value, aimee_core_endpoint_t *out)
{
   if (!value || !*value || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   const char *authority = value;
   if (strncmp(authority, "https://", 8) == 0)
   {
      out->secure = 1;
      authority += 8;
   }
   else if (strncmp(authority, "http://", 7) == 0)
   {
      authority += 7;
   }
   else if (strstr(authority, "://"))
   {
      return -1;
   }

   const char *authority_end = authority + strcspn(authority, "/?#");
   if (authority == authority_end || memchr(authority, '@', (size_t)(authority_end - authority)))
      return -1;

   const char *port = NULL;
   const char *host_start = authority;
   const char *host_end = authority_end;
   if (*host_start == '[')
   {
      const char *close = memchr(host_start + 1, ']', (size_t)(authority_end - host_start - 1));
      if (!close)
         return -1;
      host_start++;
      host_end = close;
      if (close + 1 < authority_end)
      {
         if (close[1] != ':')
            return -1;
         port = close + 2;
      }
   }
   else
   {
      const char *colon = memchr(authority, ':', (size_t)(authority_end - authority));
      if (colon)
      {
         /* Bare IPv6 is ambiguous with a port and is deliberately refused. */
         if (memchr(colon + 1, ':', (size_t)(authority_end - colon - 1)))
            return -1;
         host_end = colon;
         port = colon + 1;
      }
   }

   if (copy_part(out->host, sizeof(out->host), host_start, host_end) != 0)
      return -1;
   if (port)
   {
      if (copy_part(out->port, sizeof(out->port), port, authority_end) != 0)
         return -1;
   }
   else
   {
      snprintf(out->port, sizeof(out->port), "%s", out->secure ? "443" : "80");
   }
   return port_valid(out->port) ? 0 : -1;
}
